from pathlib import Path
import os
import subprocess
import sys
exe, stdlib = map(Path, sys.argv[1:3])
root = exe.resolve().parent / "semantic-inputs #"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text('[project]\nname="lsp-semantic"\nversion="1.0.0"\nmain="root.xr"\n', encoding="utf-8")
entry = root / "root.xr"
assert not entry.exists()
result = subprocess.run([str(exe.resolve()), entry.as_uri(), sys.argv[3]], env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())), timeout=25)
assert not entry.exists()
sys.exit(result.returncode)
