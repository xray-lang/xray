"""Publish the real standard library in an isolated, disposable source root."""
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    executable, source_root, generated = map(pathlib.Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="xir-stdlib-output-") as temporary:
        root = pathlib.Path(temporary).resolve()
        (root / "io").mkdir()
        shutil.copyfile(source_root / "stdlib/io/output.xr", root / "io/output.xr")
        shutil.copyfile(source_root / "tests/fixtures/xir_stdlib_output/root.xr", root / "root.xr")
        subprocess.run([str(executable.resolve()), str(root), str(generated.resolve())], check=True)
        if (root / "io/output.xr").exists() or not (root / "output.xrc").is_file():
            raise AssertionError("consumer did not retain a packet after deleting the copied source")


if __name__ == "__main__":
    main()
