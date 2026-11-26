"""Exercise the published Windows probe with real native compiler and runtime inputs."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "lib"))
from xraytest.toolchain import install_native_runtime

exe, compiler, parent = (Path(value).resolve() for value in sys.argv[1:4])
parent.mkdir(parents=True, exist_ok=True)
records = []
with tempfile.TemporaryDirectory(prefix="probe-中-", dir=parent) as directory:
    root = Path(directory)
    environment = dict(os.environ, LOCALAPPDATA=str(root / "用户配置"))

    def command(name, binary, arguments, expected, diagnostic=None):
        argv = [str(binary), "toolchain", *map(str, arguments), "--json"]
        result = subprocess.run(argv, env=environment, capture_output=True, timeout=120)
        row = dict(case=name, argv=argv, exit=result.returncode,
                   stdout=result.stdout.decode("utf-8"),
                   stderr=result.stderr.decode("utf-8", errors="backslashreplace"))
        records.append(row)
        (parent / "records.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        assert result.returncode == expected, row
        if diagnostic is None:
            assert not result.stderr, row
        else:
            assert diagnostic in result.stderr, row
        if expected == 2:
            assert not result.stdout, row
            return None
        return json.loads(result.stdout)

    configured = command("persisted-msvc", exe, ["use", "--target", "native", "msvc", "--cc", compiler], 0)
    config = Path(configured["configPath"])
    original = config.read_bytes()

    def probe(name, arguments=(), *, stage="complete", compiler_request=None, diagnostic=None, binary=exe):
        ready = stage == "complete"
        row = command(name, binary, ["probe", "--target", "native", *arguments], 0 if ready else 3, diagnostic)
        assert row == {"target": "x86_64-windows-msvc",
                       "compilerRequest": str(compiler_request) if compiler_request is not None else None,
                       "stage": stage, "ready": ready}, row
        assert row["ready"] is ready, row
        assert config.read_bytes() == original, name

    probe("actual-persisted-native")
    probe("actual-explicit-refresh", ["--provider", "msvc", "--cc", compiler, "--refresh"], compiler_request=compiler)
    probe("actual-auto", ["--provider", "auto"])
    missing_compiler = root / "missing-cl.exe"
    probe("missing-explicit-compiler", ["--provider", "msvc", "--cc", missing_compiler],
          stage="native compilation", compiler_request=missing_compiler, diagnostic=b"host compiler discovery failed")
    probe("unsupported-provider", ["--provider", "zig"],
          stage="native compilation", diagnostic=b"Zig automatic native compilation is temporarily unavailable")
    probe("unsupported-profile", ["--profile", "freestanding"], stage="target")
    command("invalid-target", exe, ["probe", "--target", "invented-target"], 2)
    assert config.read_bytes() == original

    prefix = root / "isolated"
    moved = prefix / "bin"
    moved.mkdir(parents=True)
    copied = moved / exe.name
    shutil.copy2(exe, copied)
    for dll in exe.parent.glob("*.dll"):
        shutil.copy2(dll, moved / dll.name)
    probe("missing-runtime", stage="native compilation",
          diagnostic=b"native runtime inputs are unavailable", binary=copied)

    runtime = install_native_runtime(exe.parent, prefix, parent)
    probe("installed-runtime", binary=copied)
    (runtime / "xray_native_runtime.lib").write_bytes(b"invalid native runtime archive\n")
    probe("corrupt-runtime", stage="native compilation", diagnostic=b"native link failed", binary=copied)

    command("reset-private-state", exe, ["reset"], 0)
    assert not config.exists()

print(json.dumps(dict(status="PASS", commands=len(records), actual_native_probes=4,
                      strict_probe_schema=True, missing_runtime_rejected=True, corrupt_runtime_rejected=True)))
