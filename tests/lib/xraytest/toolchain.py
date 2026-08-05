"""Probe compilers, interpreters, and emulators by running them.

`command -v` reports presence, not usability. Two cases in this tree bite:

  - A Visual Studio developer prompt puts a `python3` App Execution Alias on
    PATH that exits without executing anything. It "exists" and does nothing.

  - Zig's `cc` rejects `-fsyntax-only`, so a syntax gate that assumes that flag
    silently degrades on hosts where the chosen compiler is Zig.

So every probe here launches the candidate and checks it did the thing. A probe
result is cached per-process: a suite asks the same question many times.
"""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from . import platform, proc

_probe_cache: "dict[str, str | None]" = {}


def find_python() -> "str | None":
    """A Python 3 that actually runs, not merely one that is on PATH.

    Executes a candidate before trusting it, defeating the App Execution Alias
    stub. XRAY_TEST_PYTHON overrides the search for pinned hosts.
    """
    if "python" in _probe_cache:
        return _probe_cache["python"]
    import os

    # An explicit override is authoritative: if the pinned interpreter does not
    # run, that is an error to surface, not a reason to silently pick another.
    # Falling back would defeat the point of pinning and could mask a broken CI
    # image behind a different Python than the one under test.
    override = os.environ.get("XRAY_TEST_PYTHON")
    if override:
        candidates: "list[str]" = [override]
    else:
        candidates = ["python3", "python", "py"]

    chosen: "str | None" = None
    for name in candidates:
        if not name:
            continue
        resolved = shutil.which(name) or (name if os.path.isabs(name) else None)
        if not resolved:
            continue
        result = proc.run([resolved, "-c", "import hashlib,sys;sys.exit(0)"], timeout=30)
        if result.ok:
            chosen = resolved
            break
    _probe_cache["python"] = chosen
    return chosen


@dataclass(frozen=True)
class CCompiler:
    """A usable C compiler and how to ask it for a syntax-only check.

    Zig cannot do a true syntax-only pass, so `syntax_check_args` compiles to a
    throwaway object there and to -fsyntax-only everywhere else. The difference
    is captured once, here, not re-decided in every suite.
    """

    path: str
    is_zig: bool

    def syntax_check_argv(self, source: Path, include_dirs: Sequence, out_obj: Path) -> "list[str]":
        argv = [self.path]
        if self.is_zig:
            argv = [self.path, "cc", "-c", "-o", str(out_obj)]
        else:
            argv = [self.path, "-fsyntax-only"]
        for inc in include_dirs:
            argv.append(f"-I{inc}")
        argv.append(str(source))
        return argv


def find_c_compiler() -> "CCompiler | None":
    """The C compiler to use for syntax gates, verified by a trivial compile.

    Honors CC, then falls back to cc/clang/gcc. Verification matters: a broken
    or wrong-arch compiler on PATH would otherwise turn every gate red with a
    misleading message.
    """
    if "cc" in _probe_cache:
        cached = _probe_cache["cc"]
        return _CC_OBJS.get(cached) if cached else None
    import os

    candidates: "list[str]" = []
    if os.environ.get("CC"):
        candidates.append(os.environ["CC"])
    candidates.extend(["cc", "clang", "gcc"])

    for name in candidates:
        resolved = shutil.which(name)
        if not resolved:
            continue
        is_zig = "zig" in Path(resolved).name.lower()
        probe = proc.run([resolved, "--version"], timeout=30)
        if probe.ok:
            cc = CCompiler(path=resolved, is_zig=is_zig)
            _probe_cache["cc"] = resolved
            _CC_OBJS[resolved] = cc
            return cc
    _probe_cache["cc"] = None
    return None


_CC_OBJS: "dict[str, CCompiler]" = {}


def reset_probe_cache() -> None:
    """Clear cached probe results. For tests that vary the environment."""
    _probe_cache.clear()
    _CC_OBJS.clear()
