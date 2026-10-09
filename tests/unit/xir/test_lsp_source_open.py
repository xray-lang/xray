from pathlib import Path
import os,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="xray-lsp-open-") as temporary:
    root=Path(temporary)
    (root/"xray.toml").write_text('[project]\nname="lsp-open"\nversion="1.0.0"\nmain="root.xr"\n',encoding="utf8")
    entry,library=root/"root.xr",root/"lib.xr"
    env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib.resolve()))
    result=subprocess.run([str(exe),entry.as_uri(),library.as_uri()],env=env,timeout=180)
    assert result.returncode==0,result.returncode
    assert not entry.exists() and not library.exists()
