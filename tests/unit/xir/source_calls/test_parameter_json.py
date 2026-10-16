from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
mode = sys.argv[3]
assert mode in ("--normal", "--resources", "--before")
with tempfile.TemporaryDirectory(prefix="parameter-json-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="parameter-json"\nmain="root.xr"\n', encoding="utf-8")
    result = subprocess.run([str(exe.resolve()), *((root / name).as_uri() for name in ("root.xr", "lib.xr", "entry.xr")),
                             *(["--resources"] if mode == "--resources" else [])], capture_output=True, timeout=25,
                            env={**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())})
    if mode == "--before":
        assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
        assert b"PARAMETER_PROVENANCE accepted conflict=version status=0" in result.stderr, result.stderr
    else:
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert re.fullmatch(rb"Parameter JSON source=[1-9][0-9]* json=[1-9][0-9]* full_FI=" +
                            (b"1" if mode == "--resources" else b"0") +
                            rb" roots=2 conflicts=2 invalid=5 producer_dead=1 physical0/0 JSON0/0\r?\n", result.stdout)
        print(result.stdout.decode(), end="")
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
