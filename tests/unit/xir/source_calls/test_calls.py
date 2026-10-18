from pathlib import Path
import os
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="source-calls-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="source-calls"\nversion="1.0.0"\nmain="root.xr"\n', encoding="utf-8")
    (root / "lib.xr").write_text("export fn visible(v:i64)->i64{return v}\n", encoding="utf-8")
    for mode in range(6):
        subprocess.run([str(exe.resolve()), (root / "root.xr").as_uri(), str(mode)],
                       env={**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())}, check=True, timeout=25)
