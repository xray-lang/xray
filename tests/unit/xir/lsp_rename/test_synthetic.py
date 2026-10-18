from pathlib import Path
import hashlib
import os
import subprocess
import sys

exe, stdlib = (Path(value).resolve() for value in sys.argv[1:3])
root = exe.parent / "synthetic-inputs #😀"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text('[project]\nname="lsp-synthetic"\nversion="1.0.0"\nmain="root.xr"\n', encoding="utf-8")
entry = root / "root.xr"
assert not entry.exists()
image_before = hashlib.sha256(exe.read_bytes()).hexdigest()
result = subprocess.run([str(exe), entry.as_uri(), entry.as_uri(), entry.as_uri(), "--synthetic-required"],
    env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib)), capture_output=True, text=True, encoding="utf-8",
    errors="replace", timeout=25, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
assert hashlib.sha256(exe.read_bytes()).hexdigest() == image_before
assert not entry.exists()
raise SystemExit(result.returncode)
