"""Public preference commands persist facts used by the real Source/native build."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

exe, compiler, parent = (Path(x).resolve() for x in sys.argv[1:4])
parent.mkdir(parents=True, exist_ok=True)
records = []
with tempfile.TemporaryDirectory(prefix="public-偏好-", dir=parent) as directory:
    root = Path(directory)
    env = dict(os.environ, LOCALAPPDATA=str(root / "用户配置"))
    config = Path(env["LOCALAPPDATA"]) / "Xray/toolchains.toml"

    def invoke(name, args, code=0, json_reply=True):
        result = subprocess.run([str(exe), *args], env=env, capture_output=True, timeout=90)
        row = {"name": name, "argv": args, "exit": result.returncode,
               "stdout": result.stdout.decode("utf-8"), "stderr": result.stderr.decode("utf-8")}
        records.append(row)
        assert result.returncode == code, row
        if code:
            assert row["stderr"], row
        elif json_reply:
            assert not row["stderr"], row
            return json.loads(row["stdout"])
        return row

    def preference(name, *args, code=0):
        return invoke(name, ["toolchain", *args, "--json"], code)

    selected = preference("path-without-read", "config-path")
    assert Path(selected["path"]) == config and not config.exists()
    listed = preference("missing-is-empty", "list")
    assert listed["preferences"] == [] and listed["exists"] is False and not config.exists()
    preference("native-real-compiler", "use", "msvc", "--target", "native", "--cc", str(compiler))
    listed = preference("native-round-trip", "list", "--target", "native")
    assert listed["preferences"] == [{"target": "native", "provider": "msvc", "compiler": str(compiler), "zig": None}]
    preference("preserve-other-target", "use", "msvc", "--target", "aarch64-windows-msvc")
    assert len(preference("list-all", "list")["preferences"]) == 2
    source = root / "main.xr"
    source.write_text("print(42)\n", encoding="utf-8")
    output = root / "真实输出.exe"
    before = config.read_bytes()
    invoke("real-build-consumes-preference", ["build", str(source), "-o", str(output)], json_reply=False)
    assert config.read_bytes() == before
    result = subprocess.run([str(output)], capture_output=True, timeout=10)
    assert (result.returncode, result.stdout, result.stderr) == (0, b"42\n", b"")
    bad_cases = [
        ("use-needs-target", ["use", "msvc"], 2),
        ("unknown-provider", ["use", "unknown", "--target", "native"], 2),
        ("empty-provider", ["use", "", "--target", "native"], 2),
        ("bad-target", ["list", "--target", "invalid-target"], 2),
        ("empty-target", ["list", "--target", ""], 2),
        ("reset-forbids-path", ["reset", "--cc", str(compiler)], 2),
        ("list-forbids-path", ["list", "--zig", "zig"], 2),
        ("path-forbids-target", ["config-path", "--target", "native"], 2),
        ("cc-zig-mismatch", ["use", "zig", "--target", "native", "--cc", str(compiler)], 2),
        ("zig-msvc-mismatch", ["use", "msvc", "--target", "native", "--zig", "zig"], 2),
        ("unknown-action", ["missing"], 2),
        ("extra-argument", ["list", "extra"], 2),
    ]
    for name, args, code in bad_cases:
        before = config.read_bytes()
        preference(name, *args, code=code)
        assert config.read_bytes() == before, name
    escaped = 'C:\\中\\"quoted"\\compiler\t.exe'
    preference("escaped-owned-text", "use", "clang", "--target", "native", "--cc", escaped)
    assert preference("escaped-json-round-trip", "list", "--target", "native")["preferences"][0]["compiler"] == escaped
    missing = root / "missing/cl.exe"
    preference("store-fact-not-readiness", "use", "msvc", "--target", "native", "--cc", str(missing))
    output.write_bytes(b"original output")
    invoke("no-automatic-compiler-fallback", ["build", str(source), "-o", str(output)], code=1, json_reply=False)
    assert output.read_bytes() == b"original output"
    preference("targeted-reset", "reset", "--target", "native")
    rows = preference("other-target-survives", "list")["preferences"]
    assert [row["target"] for row in rows] == ["aarch64-windows-msvc"]
    config.write_text('schema=1\n[native]\nprovider=42\n', encoding="utf-8")
    corrupt = config.read_bytes()
    for name, args in [("strict-list", ["list"]), ("strict-use", ["use", "msvc", "--target", "native"]),
                       ("strict-targeted-reset", ["reset", "--target", "native"])]:
        preference(name, *args, code=1)
        assert config.read_bytes() == corrupt
    assert Path(preference("path-with-corrupt-file", "config-path")["path"]) == config
    preference("explicit-reset-all-repairs", "reset")
    assert not config.exists()
    preference("reset-missing-is-idempotent", "reset")
    assert preference("empty-after-reset", "list")["preferences"] == []
    assert config.with_suffix(config.suffix + ".lock").is_file()
    invoke("human-list", ["toolchain", "list"], json_reply=False)
    invoke("help", ["help", "toolchain"], json_reply=False)
    invoke("missing-action", ["toolchain"], code=2, json_reply=False)
(parent / "records.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Public toolchain {len(records)} cases; real native42; strict input and old output preserved")
