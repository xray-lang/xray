"""Supply UTF-8 paths independently of the Windows narrow argv encoding."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="xray-path-boundary-") as temporary:
    scope = Path(temporary)
    root = scope / "工程 空格 😀"
    root.mkdir()
    (root / "main.xr").write_bytes(b"print(41)\n")
    locator = scope / "root.txt"
    locator.write_bytes(str(root).encode("utf-8"))
    raise SystemExit(subprocess.run([sys.argv[1], str(locator)], check=False).returncode)
