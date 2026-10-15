from pathlib import Path
import os
import subprocess
import sys

exe, stdlib = map(Path, sys.argv[1:3])
root = exe.resolve().parent / "document-links-inputs #😀"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text(
    '[project]\nname="lsp-document-links"\nversion="1.0.0"\nmain="root.xr"\n',
    encoding="utf-8",
)
entry, library, disk = root / "root.xr", root / "lib.xr", root / "disk.xr"
disk.write_text("fn unfinished(", encoding="utf-8")
long_relative = "./missing/" + "/".join(letter * 180 for letter in "abc")
long_file = root / (long_relative[2:] + ".xr")
bad = root / "invalid-project"
bad.mkdir(exist_ok=True)
(bad / "xray.toml").write_text("[project", encoding="utf-8")
assert not entry.exists() and not library.exists() and not long_file.exists()
env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve()))
subprocess.run(
    [str(exe.resolve()), entry.as_uri(), library.as_uri(), disk.as_uri(),
     long_file.as_uri(), long_relative, (stdlib.resolve() / "math/math.xr").as_uri(),
     (bad / "root.xr").as_uri(), *sys.argv[3:]],
    env=env, check=True, timeout=290,
)
assert not entry.exists() and not library.exists() and not long_file.exists()
