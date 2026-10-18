from pathlib import Path
import os,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="source-corpus-",dir=os.getcwd()) as folder:
    root=Path(folder)
    (root/"xray.toml").write_text('[project]\nname="source-corpus"\nmain="root.xr"\n',encoding="utf-8")
    paths=[root/name for name in ("root.xr","lib.xr","closed.xr","extra.xr")]
    assert all(not path.exists() for path in paths)
    subprocess.run([str(exe.resolve()),*map(str,paths)],env={**os.environ,"XRAY_STDLIB_PATH":str(stdlib.resolve())},check=True,timeout=60)
    assert all(not path.exists() for path in paths)
