"""Exercise the manifest loader with isolated real files and a root junction."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
from test_file_read_windows import junction


def main() -> int:
    workspace = Path.cwd().resolve()
    scope = Path(tempfile.mkdtemp(prefix="xray declaration file ", dir=workspace)).resolve()
    assert scope.parent == workspace and scope.name.startswith("xray declaration file ")
    alias = scope / "alias"
    try:
        root = scope / "root"
        root.mkdir()
        valid = ("[declarations]\nversion=1\n[[declarations.function]]\n"
                 "module='src/main.xr'\nname='apply'\nno_suspend=true\n"
                 "no_suspend_parameters=['callback']\n")
        (root / "xray.toml").write_text(valid, encoding="utf-8")
        for name in ["absent", "nosection", "syntax", "schema", "directory"]:
            (root / name).mkdir()
        (root / "nosection" / "xray.toml").write_text("[project]\nname='ordinary'\n", encoding="utf-8")
        (root / "syntax" / "xray.toml").write_text(valid.replace("version=1", "version=1+2"), encoding="utf-8")
        (root / "schema" / "xray.toml").write_text(valid.replace("version=1", "version=2"), encoding="utf-8")
        (root / "directory" / "xray.toml").mkdir()
        junction(alias, root)
        return subprocess.run([sys.argv[1], str(root), str(alias)], check=False).returncode
    finally:
        assert alias.parent == scope
        if os.path.lexists(alias):
            os.rmdir(alias)
        assert scope.parent == workspace and scope.name.startswith("xray declaration file ")
        shutil.rmtree(scope)


if __name__ == "__main__":
    raise SystemExit(main())
