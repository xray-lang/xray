from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
resources = sys.argv[3:] == ["--resources"]
assert not sys.argv[3:] or resources
with tempfile.TemporaryDirectory(prefix="parameter-hints-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="parameter-hints"\nmain="root.xr"\n', encoding="utf-8")
    result = subprocess.run([str(exe.resolve()), (root / "root.xr").as_uri(), (root / "lib.xr").as_uri(), *sys.argv[3:]], capture_output=True, timeout=25,
                            env={**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())})
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    assert re.fullmatch(rb"Parameter hints cases=16 sites=[1-9][0-9]* full_FI=" + (b"1" if resources else b"0") +
                        rb" stale=4 ref=1 variadic=1 core=1 unicode=1 cross_root=2 changed_provenance=1 native_shadow=1 long=97 producer_dead=1 physical0/0\r?\n", result.stdout)
    print(result.stdout.decode(), end="")
