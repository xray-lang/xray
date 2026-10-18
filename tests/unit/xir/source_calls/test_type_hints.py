from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
resources = sys.argv[3:] == ["--resources"]
assert not sys.argv[3:] or resources
with tempfile.TemporaryDirectory(prefix="type-hints-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="type-hints"\nmain="root.xr"\n', encoding="utf-8")
    result = subprocess.run([str(exe.resolve()), (root / "root.xr").as_uri(), *sys.argv[3:]],
        capture_output=True, timeout=25, env={**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())})
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    assert re.fullmatch(rb"Type hints sites=[1-9][0-9]* full_FI=" + (b"1" if resources else b"0") +
                        rb" generic=1 unicode=1 long=1 producer_dead=1 physical0/0\r?\n", result.stdout)
    print(result.stdout.decode(), end="")
