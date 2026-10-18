from pathlib import Path
import os
import subprocess
import sys
import tempfile
exe, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='pattern-rename-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="pattern-rename"\nversion="1.0.0"\nmain="root.xr"\n', encoding='utf-8')
    subprocess.run([str(exe.resolve()), (root / 'root.xr').as_uri(), *sys.argv[3:]],
                   env={**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())}, check=True, timeout=290)
    assert not (root / 'root.xr').exists()
