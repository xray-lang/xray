from pathlib import Path
import os
import subprocess
import sys
import tempfile
exe, stdlib = map(Path, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="semantic-inputs #", dir=exe.resolve().parent) as directory:
    root = Path(directory)
    (root / "xray.toml").write_text('[project]\nname="lsp-semantic"\nversion="1.0.0"\nmain="root.xr"\n', encoding="utf-8")
    entry = root / "root.xr"
    assert not entry.exists()
    result = subprocess.run([str(exe.resolve()), entry.as_uri(), sys.argv[3]], env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())), timeout=25)
    assert not entry.exists()
sys.exit(result.returncode)
