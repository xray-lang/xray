from pathlib import Path
import os
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='watch-inputs-', dir=exe.resolve().parent) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="watch-lifecycle"\nversion="1.0.0"\nmain="root.xr"\n', encoding='utf-8')
    entry = root / 'root.xr'
    result = subprocess.run([str(exe.resolve()), entry.as_uri()],
                            env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())), timeout=25)
    assert not entry.exists()
    raise SystemExit(result.returncode)
