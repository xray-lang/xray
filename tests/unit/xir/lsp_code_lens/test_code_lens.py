from pathlib import Path
import os
import subprocess
import sys

exe, stdlib = map(Path, sys.argv[1:3])
root = exe.resolve().parent / "code-lens-inputs #"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text(
    '[project]\nname="lsp-code-lens"\nversion="1.0.0"\nmain="root.xr"\n',
    encoding="utf-8",
)
entry, library = root / "root.xr", root / "lib.xr"
assert not entry.exists() and not library.exists()
env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve()))
subprocess.run(
    [str(exe.resolve()), entry.as_uri(), library.as_uri(), *sys.argv[3:]],
    env=env,
    check=True,
    timeout=290,
)
assert not entry.exists() and not library.exists()
