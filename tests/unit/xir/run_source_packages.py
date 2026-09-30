"""Run package admission in a private child environment without touching installed packages."""
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

with tempfile.TemporaryDirectory(prefix="xir-locked-package-") as directory:
    root = Path(directory)
    package = root / ".xray/packages/fixture/math/1.0.0"
    package.mkdir(parents=True)
    sources = {
        "main.xr": 'import { base } from "./part"\nexport fn value()->i64 { return base() + 1 }\n',
        "part.xr": 'export fn base()->i64 { return 40 }\n',
    }
    cache = root / ".xray/cache"
    cache.mkdir()
    archive = cache / "fixture-math-1.0.0.tar.gz"
    with tarfile.open(archive, "w:gz") as output:
        for name, source in sources.items():
            data = source.encode("utf-8")
            (package / name).write_bytes(data)
            info = tarfile.TarInfo(name)
            info.size = len(data)
            output.addfile(info, io.BytesIO(data))
    entry = root / "root.xr"
    entry.write_text('import { value } from "fixture/math"\nexport fn answer()->i64 { return value() }\n', encoding="utf-8")
    environment = dict(os.environ)
    # Only the child resolver sees this package fixture root.
    environment["HOME"] = str(root)
    environment["USERPROFILE"] = str(root)
    checksum = "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest()
    subprocess.run([sys.argv[1], str(root), str(entry), checksum, *sys.argv[2:]], env=environment, check=True)
