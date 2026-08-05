"""xraytest -- the shared runtime for Xray's test and tooling scripts.

One public implementation of the things every shell suite re-invented: running
subprocesses, scratch workspaces, content-addressed cache keys, the only-shrink
ratchet, resource-tagged parallelism, toolchain probes, and result reporting.

A runner locates the package with `bootstrap()` and imports submodules:

    from xraytest import bootstrap
    bootstrap()
    from xraytest import proc, workspace, cache, ratchet, scheduler, report

No install step: bootstrap() puts tests/lib on sys.path. Python 3.9 is the floor
across the whole package.
"""

from __future__ import annotations

import sys
from pathlib import Path

__all__ = [
    "bootstrap",
    "platform",
    "proc",
    "workspace",
    "cache",
    "ratchet",
    "scheduler",
    "toolchain",
    "report",
]


def lib_dir() -> Path:
    """Directory that holds the xraytest package (tests/lib)."""
    return Path(__file__).resolve().parent.parent


def bootstrap() -> None:
    """Ensure `import xraytest` works from any runner without an install.

    Idempotent: prepends tests/lib to sys.path only if absent. A runner script
    calls this before importing submodules, or adds the path itself; either way
    there is no pip step and no PYTHONPATH requirement in CMake.
    """
    path = str(lib_dir())
    if path not in sys.path:
        sys.path.insert(0, path)


# Submodules import lazily through normal `from xraytest import proc`; they are
# not eagerly imported here so a runner pays only for what it uses.
from . import platform  # noqa: E402  (re-export after docstring/bootstrap defs)
