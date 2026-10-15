from pathlib import Path
import os
import subprocess
import sys

exe, stdlib = map(Path, sys.argv[1:3])
root = exe.resolve().parent / "rename-inputs #😀"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text(
    '[project]\nname="lsp-rename"\nversion="1.0.0"\nmain="root.xr"\n', encoding="utf-8"
)
entry, library, closed = root / "root.xr", root / "lib.xr", root / "closed.xr"
closed.write_text("export fn target(value:i64)->i64{return value}\n", encoding="utf-8")
closed_before = closed.read_bytes()
assert not entry.exists() and not library.exists()
subprocess.run(
    [str(exe.resolve()), entry.as_uri(), library.as_uri(), closed.as_uri(), *sys.argv[3:]],
    env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())), check=True, timeout=290,
)
assert not entry.exists() and not library.exists()

assert closed.read_bytes() == closed_before
