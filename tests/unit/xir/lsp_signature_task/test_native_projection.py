from pathlib import Path
import os
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="projection-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="projection"\nmain="root.xr"\n', encoding="utf-8")
    result = subprocess.run([str(exe.resolve()), (root / "root.xr").as_uri(), (root / "lib.xr").as_uri()],
        capture_output=True, timeout=110, env={**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())})
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
    assert result.stdout.strip() == b"native projection monomorphic=7 reused_generic=1 cross_root=2 producer_dead=1 physical0/0"
    print(result.stdout.decode(), end="")
