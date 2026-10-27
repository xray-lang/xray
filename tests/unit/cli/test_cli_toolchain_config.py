"""Preference consumers with independent TOML inputs and exact error oracles."""
from pathlib import Path
import subprocess
import sys
import tempfile

exe, parent = map(lambda value: Path(value).resolve(), sys.argv[1:3])
normal_only = sys.argv[3:] == ["--normal"]
parent.mkdir(parents=True, exist_ok=True)
valid = '''schema = 1
[native]
provider = "msvc"
compiler = "C:/owned/编译器/cl.exe"
[target."aarch64-linux-gnu"]
provider = "zig"
zig = "C:/owned/zig.exe"
[policy]
allow_fallback = false
prefer_external_native = true
auto_probe = true
'''
cases = [
    ("valid", valid, "faults"),
    ("missing", None, "0"),
    ("empty", "", "1"),
    ("schema-type", 'schema = "1"\n', "1"),
    ("unknown", 'schema = 1\nunknown = 42\n', "1"),
    ("native-type", 'schema = 1\nnative = 42\n', "1"),
    ("policy-type", 'schema = 1\n[policy]\nauto_probe = "true"\n', "1"),
    ("provider-nul", 'schema = 1\n[native]\nprovider = "msvc\\u0000zig"\n', "1"),
    ("compiler-nul", 'schema = 1\n[native]\nprovider = "msvc"\ncompiler = "cl\\u0000.exe"\n', "1"),
    ("wrong-provider-path", 'schema = 1\n[native]\nprovider = "msvc"\nzig = "zig.exe"\n', "1"),
    ("wrong-target", 'schema = 1\n[target."not-a-real-target"]\nprovider = "auto"\n', "1"),
    ("valid-arm-msvc-target", 'schema = 1\n[target."aarch64-windows-msvc"]\nprovider = "msvc"\n', "0"),
    ("duplicate", 'schema = 1\nschema = 1\n', "1"),
    ("depth-limit", 'schema = 1\nbogus = [[[[[[[[[1]]]]]]]]]\n', "2"),
]
with tempfile.TemporaryDirectory(prefix="owned-中-", dir=parent) as directory:
    root = Path(directory)
    for name, text, expected in cases:
        if normal_only and expected == "faults":
            expected = "0"
        path = root / f"{name}.toml"
        if text is not None:
            path.write_text(text, encoding="utf-8", newline="\n")
        result = subprocess.run([str(exe), str(path), expected], capture_output=True, timeout=90)
        print(name, result.returncode, result.stdout.decode("utf-8", errors="replace"), flush=True)
        if result.returncode:
            sys.stderr.buffer.write(result.stderr)
            raise SystemExit(result.returncode)
        if text is not None:
            assert path.read_text(encoding="utf-8") == text
