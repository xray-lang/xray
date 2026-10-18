"""Require fresh public probe observations and reject missing or changed inputs."""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

exe, compiler, parent = (Path(value).resolve() for value in sys.argv[1:4])
parent.mkdir(parents=True, exist_ok=True)
records = []
with tempfile.TemporaryDirectory(prefix="probe-中-", dir=parent) as directory:
    root = Path(directory)
    environment = dict(os.environ, LOCALAPPDATA=str(root / "用户配置"))
    def command(name, binary, arguments, expected):
        argv = [str(binary), "toolchain", *map(str, arguments), "--json"]
        result = subprocess.run(argv, env=environment, capture_output=True, timeout=120)
        row = dict(case=name, argv=argv, exit=result.returncode,
                   stdout=result.stdout.decode("utf-8"), stderr=result.stderr.decode("utf-8"))
        records.append(row)
        (parent / "records.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        assert result.returncode == expected and not result.stderr, row
        if expected == 2:
            assert not result.stdout, row
            return None
        return json.loads(result.stdout)

    configured = command("persisted-msvc", exe, ["use", "--target", "native", "msvc", "--cc", compiler], 0)
    config = Path(configured["configPath"])
    original = config.read_bytes()

    def probe(name, arguments=(), ready=True, binary=exe):
        row = command(name, binary, ["probe", "--target", "native", *arguments], 0 if ready else 3)
        assert set(row) == {"schema", "xray", "request", "selection", "capabilities", "diagnostics", "probe"}
        assert row["schema"] == 1
        assert set(row["xray"]) == {"version", "build", "sdkDigest"}
        assert set(row["request"]) == {"target", "normalizedTarget", "profile", "provider"}
        assert set(row["selection"]) == {"provider", "ownership", "compiler", "version", "targetAbi", "runtimeArtifact", "ready", "fallbackUsed"}
        assert set(row["capabilities"]) == {"cCompile", "sdkCompile", "runtimeLink", "nativeRun", "cross", "lto"}
        assert set(row["probe"]) == {"id", "fingerprint", "cache", "durationMs"}
        assert row["selection"]["ready"] is ready and row["probe"]["cache"] == "disabled"
        assert row["request"]["normalizedTarget"] == row["selection"]["targetAbi"] == "x86_64-windows-msvc"
        assert row["request"]["profile"] == ("freestanding" if "--profile" in arguments else "hosted")
        if ready:
            assert row["selection"]["provider"] == "msvc" and row["selection"]["ownership"] == "external"
            assert Path(row["selection"]["compiler"]) == compiler
            assert re.fullmatch(r"\d+\.\d+\.\d+\.\d+", row["selection"]["version"])
            assert re.fullmatch(r"[0-9a-f]{64}", row["selection"]["runtimeArtifact"])
            assert row["xray"]["sdkDigest"] == row["selection"]["runtimeArtifact"]
            assert re.fullmatch(r"[0-9a-f]{64}", row["probe"]["fingerprint"])
            assert row["probe"]["durationMs"] > 0 and not row["diagnostics"]
            assert all(row["capabilities"][key] == "ok" for key in ("cCompile", "sdkCompile", "runtimeLink", "nativeRun"))
        else:
            assert row["diagnostics"] and row["capabilities"]["nativeRun"] != "ok"
        assert config.read_bytes() == original
        return row

    probe("actual-persisted-native")
    probe("actual-explicit-refresh", ["--provider", "msvc", "--cc", compiler, "--refresh"])
    probe("actual-auto", ["--provider", "auto"])
    probe("missing-explicit-compiler", ["--provider", "msvc", "--cc", root / "missing-cl.exe"], False)
    probe("unsupported-provider", ["--provider", "zig"], False)
    probe("unsupported-profile", ["--profile", "freestanding"], False)
    command("invalid-target", exe, ["probe", "--target", "invented-target"], 2)
    moved = root / "isolated"
    moved.mkdir()
    copied = moved / "xray.exe"
    shutil.copy2(exe, copied)
    for dll in exe.parent.glob("*.dll"):
        shutil.copy2(dll, moved / dll.name)
    missing = probe("missing-sdk", ready=False, binary=copied)
    assert missing["diagnostics"][0]["stage"] == "runtime-sdk"
    source_sdk = exe.parent / "xir-runtime-sdk"
    assert source_sdk.is_dir()
    bundle = moved / "xir-runtime-sdk"
    shutil.copytree(source_sdk, bundle)
    manifest = json.loads((bundle / "sdk_manifest.json").read_text(encoding="utf-8"))
    candidates = [p for p in bundle.rglob("*.lib")]
    assert len(candidates) == 5 and manifest
    member = candidates[0]
    payload = bytearray(member.read_bytes())
    payload[-1] ^= 1
    member.write_bytes(payload)
    changed = probe("changed-sdk", ready=False, binary=copied)
    assert changed["diagnostics"][0]["stage"] == "runtime-sdk"
    command("reset-private-state", exe, ["reset"], 0)
print(json.dumps(dict(status="PASS", commands=len(records), actual_native_probes=3,
                      strict_installer_schema=True, missing_sdk_rejected=True, changed_sdk_rejected=True)))
