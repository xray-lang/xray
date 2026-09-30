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
        "main.xr": 'import { base } from "./部分"\nexport fn value()->i64 { return base() + 1 }\n',
        "部分.xr": 'export fn base()->i64 { return 40 }\n',
        "xray.toml": '[declarations]\nversion=1\n[[declarations.function]]\nmodule="main.xr"\nname="value"\nno_suspend=true\n',
    }
    padding = "#" + "x" * 131072 + "\n"
    sources["xray.toml"] += padding
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
    (root / "模块.xr").write_text('print(999)\nexport fn unreachable()->i64 { return 7 }\n'
        'export fn apply(callback:fn()->i64)->i64 { return callback() }\n', encoding="utf-8")
    manifest = ('[declarations]\nversion=1\n'
        '[[declarations.function]]\nmodule="root.xr"\nname="answer"\nno_suspend=true\n'
        '[[declarations.function]]\nmodule="模块.xr"\nname="unreachable"\nno_suspend=true\n'
        '[[declarations.function]]\nmodule="模块.xr"\nname="apply"\nno_suspend=true\n'
        'no_suspend_parameters=["callback"]\n')
    manifest += padding
    manifest_path = root / "xray.toml"
    manifest_path.write_text(manifest, encoding="utf-8")
    environment = dict(os.environ)
    # Only the child resolver sees this package fixture root.
    environment["HOME"] = str(root)
    environment["USERPROFILE"] = str(root)
    checksum = "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest()
    subprocess.run([sys.argv[1], str(root), str(entry), checksum, *sys.argv[2:]], env=environment, check=True)
    # Each manifest fits the allowance alone; their combined parse work does not.
    bounded = dict(environment, XR_TEST_PACKAGE_WORK="196608")
    result = subprocess.run([sys.argv[1], str(root), str(entry), checksum], env=bounded,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    if result.returncode == 0 or "declaration manifest admission failed" not in result.stderr:
        raise AssertionError(("cumulative package work", result.returncode, result.stdout, result.stderr))
    for content, expected in [
        (manifest.replace('["callback"]', '["missing"]'), "declaration parameter name is missing"),
        (manifest.replace('no_suspend_parameters=["callback"]\n', ''), "declared no_suspend"),
        (manifest.replace('name="unreachable"', 'name="missing"'), "promise target is missing"),
        (manifest.replace('version=1', 'version=2'), "manifest admission failed"),
    ]:
        manifest_path.write_text(content, encoding="utf-8")
        result = subprocess.run([sys.argv[1], str(root), str(entry), checksum], env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        if result.returncode == 0 or expected not in result.stderr:
            raise AssertionError((expected, result.returncode, result.stdout, result.stderr))
