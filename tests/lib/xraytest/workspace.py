"""Temporary workspaces with cleanup that survives exceptions.

Replaces the shell tree's 57 hand-rolled `mktemp` calls and 53 `trap ... EXIT`
handlers with one context manager. A `trap EXIT` in shell fires once at script
exit; this fires at the end of its `with` block, so a suite can open and reclaim
many short-lived workspaces instead of accumulating everything until teardown.

KEEP_TMP is honored in one place: set it and the directory is preserved and its
path printed, exactly as the shell `--keep-tmp` did, for every migrated suite.
"""

from __future__ import annotations

import shutil
import sys
import tempfile
from pathlib import Path

from . import platform


class Workspace:
    """A scratch directory that cleans itself up unless asked to persist.

    Use as a context manager:

        with Workspace("aot_filetests") as ws:
            src = ws.write("case.xr", source_bytes)
            ...

    On exit the tree is removed, on the exception path too, unless `keep` was
    passed or XRAY_TEST_KEEP_TMP is set -- in which case the path is printed so
    a developer can inspect the failing artifacts.
    """

    def __init__(self, label: str = "xraytest", *, keep: bool | None = None) -> None:
        self._label = label
        self._keep = platform.env_flag("XRAY_TEST_KEEP_TMP") if keep is None else keep
        self._root: Path | None = None

    def __enter__(self) -> "Workspace":
        base = tempfile.mkdtemp(prefix=f"{self._label}.")
        self._root = Path(base)
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        root = self._root
        self._root = None
        if root is None:
            return False
        if self._keep:
            sys.stderr.write(f"[xraytest] kept workspace: {root}\n")
            return False
        shutil.rmtree(root, ignore_errors=True)
        return False

    def keep_on_exit(self, keep: bool) -> None:
        """Decide at the end of the run whether the tree survives.

        For suites whose artifacts only matter when something went wrong: a
        failing run keeps its logs, a green one leaves nothing behind. The
        outcome is not known when the workspace is created, so the decision has
        to be revisable rather than a constructor argument.
        """
        self._keep = keep

    @property
    def root(self) -> Path:
        if self._root is None:
            raise RuntimeError("Workspace used outside its 'with' block")
        return self._root

    def path(self, *parts: str) -> Path:
        """Absolute path inside the workspace, creating parent directories."""
        target = self.root.joinpath(*parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        return target

    def write(self, name: str, content: bytes | str) -> Path:
        """Materialize a file in the workspace and return its path."""
        target = self.path(name)
        if isinstance(content, str):
            target.write_text(content, encoding="utf-8", newline="\n")
        else:
            target.write_bytes(content)
        return target

    def subdir(self, name: str) -> Path:
        target = self.root / name
        target.mkdir(parents=True, exist_ok=True)
        return target
