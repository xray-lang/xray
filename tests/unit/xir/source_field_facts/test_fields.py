from pathlib import Path
import os
import subprocess
import sys
import tempfile
exe, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='source-field-facts-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="field-facts"\nversion="1.0.0"\nmain="root.xr"\n', encoding='utf-8')
    for mode in range(6):
        subprocess.run([str(exe.resolve()), (root / 'root.xr').as_uri(), str(mode)],
                       env={**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())}, check=True, timeout=25)
