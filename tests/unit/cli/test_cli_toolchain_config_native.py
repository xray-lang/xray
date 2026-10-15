"""Real Source/native builds must respect a private persisted selection."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

exe, compiler, parent = map(lambda value: Path(value).resolve(), sys.argv[1:4])
parent.mkdir(parents=True, exist_ok=True)
records = []
with tempfile.TemporaryDirectory(prefix="selection-中-", dir=parent) as directory:
    root = Path(directory)
    environment = dict(os.environ, LOCALAPPDATA=str(root / "用户配置"))
    config = Path(environment["LOCALAPPDATA"]) / "Xray" / "toolchains.toml"
    config.parent.mkdir(parents=True)
    source = root / "main.xr"
    source.write_text("print(42)\n", encoding="utf-8")

    def run(name, text, expected, diagnostic=None, options=()):
        if text is None:
            config.unlink(missing_ok=True)
        else:
            config.write_text(text, encoding="utf-8", newline="\n")
        output = root / f"{name}.exe"
        if expected:
            output.write_bytes(b"preserved original output")
        command = [str(exe), "build", str(source), "-o", str(output), *options]
        result = subprocess.run(command, env=environment, capture_output=True, timeout=90)
        records.append({"case": name, "argv": command, "exit": result.returncode,
                        "stdout": result.stdout.decode("utf-8", errors="replace"),
                        "stderr": result.stderr.decode("utf-8", errors="replace")})
        assert result.returncode == expected, records[-1]
        if diagnostic:
            assert diagnostic.encode() in result.stderr, records[-1]
        if expected:
            assert output.read_bytes() == b"preserved original output"
        else:
            observed = subprocess.run([str(output)], capture_output=True, timeout=10)
            assert (observed.returncode, observed.stdout, observed.stderr) == (0, b"42\n", b""), records[-1]
        if text is not None:
            assert config.read_text(encoding="utf-8") == text
        print(name, result.returncode, flush=True)

    run("missing-default", None, 0)
    run("valid-other-target-does-not-block-native", 'schema = 1\n[target."aarch64-windows-msvc"]\nprovider = "msvc"\n', 0)
    run("unsupported-stored-provider", 'schema = 1\n[native]\nprovider = "zig"\n', 1, "XR_BUILD_6011")
    run("corrupt-preserved", 'schema = 1\n[native]\nprovider = 42\n', 1, "XR_BUILD_6010")
    missing = root / "does-not-exist" / "cl.exe"
    broken = 'schema = 1\n[native]\nprovider = "msvc"\ncompiler = ' + json.dumps(str(missing)) + '\n'
    run("explicit-stored-path-no-fallback", broken, 1, "XR_BUILD_6006")
    run("caller-path-overrides-stored-path", broken, 0, options=("--cc", str(compiler)))
    run("caller-provider-overrides-stored-provider", 'schema = 1\n[native]\nprovider = "zig"\n',
        0, options=("--toolchain", "msvc", "--cc", str(compiler)))
output = parent / "records.json"
output.write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
