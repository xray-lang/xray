from pathlib import Path
import os,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="xray-lsp-authorities-") as temporary:
    root=Path(temporary)
    for mode in ("project","script","stdlib","package"):
        folder=root/mode;folder.mkdir()
        if mode in ("project","package"):
            (folder/"xray.toml").write_text('[project]\nname="lsp-owned"\nversion="1.0.0"\nmain="root.xr"\n',encoding="utf8")
        if mode=="stdlib":
            selected=root/"selected-stdlib";(selected/"io").mkdir(parents=True)
            library=selected/"io"/"edited.xr";specifier="std/io/edited"
        elif mode=="package":
            selected=stdlib.resolve();home=root/"home";cache=home/".xray"/"cache";cache.mkdir(parents=True)
            (cache/"owner-package-1.2.3.tar.gz").write_bytes(b"abc")
            base=home/".xray"/"packages"/"owner"/"package"/"1.2.3"/"src";base.mkdir(parents=True)
            library=base/"main.xr";specifier="owner/package"
            (folder/"xray.lock").write_text('# Format version: 1\n[package.owner/package]\nversion="1.2.3"\nresolved="fixture.tar.gz"\nchecksum="sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"\n',encoding="utf8")
        else:
            selected=stdlib.resolve();(folder/"sub").mkdir();library=folder/"sub"/"lib.xr";specifier="./sub/lib"
        entry=folder/"root.xr"
        assert not entry.exists() and not library.exists()
        env=dict(os.environ,XRAY_STDLIB_PATH=str(selected))
        if mode=="package":env["HOME"]=str(home)
        result=subprocess.run([str(exe),entry.as_uri(),library.as_uri(),specifier],env=env,timeout=180)
        assert result.returncode==0,(mode,result.returncode)
        assert not entry.exists() and not library.exists()
