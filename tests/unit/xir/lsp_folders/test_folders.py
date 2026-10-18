from pathlib import Path
import os
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='folders-inputs-', dir=exe.resolve().parent) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="folders-lifecycle"\nversion="1.0.0"\nmain="root.xr"\n', encoding='utf-8')
    entry = root / 'root.xr'
    entry.write_text("fn indexed()->i64{return 1}\n", encoding="utf-8")
    result = subprocess.run([str(exe.resolve()), entry.as_uri()],
                            env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())), timeout=25)
    entry.unlink(missing_ok=True)
    raise SystemExit(result.returncode)
