"""Probe compilers, interpreters, and emulators by running them.

`command -v` reports presence, not usability. Two cases in this tree bite:

  - A Visual Studio developer prompt puts a `python3` App Execution Alias on
    PATH that exits without executing anything. It "exists" and does nothing.

  - C compiler drivers do not share a syntax-only command line. GCC and Clang
    use `-fsyntax-only`, Zig emits a throwaway object, and MSVC uses `/Zs`.

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

CC_DRIVER_GNU = "gnu"
CC_DRIVER_ZIG = "zig"
CC_DRIVER_MSVC = "msvc"


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
    """A usable C11 compiler and its syntax-check driver contract.

    The driver family is captured during the executable probe, not guessed by
    each caller. Generated C always gets an explicit C11 language mode.
    """

    path: str
    driver: str

    def syntax_check_argv(self, source: Path, include_dirs: Sequence, out_obj: Path) -> "list[str]":
        if self.driver == CC_DRIVER_ZIG:
            argv = [self.path, "cc", "-std=c11", "-c", "-o", str(out_obj)]
        elif self.driver == CC_DRIVER_MSVC:
            argv = [
                self.path,
                "/nologo",
                "/TC",
                "/std:c11",
                "/experimental:c11atomics",
                "/utf-8",
                "/Zs",
            ]
        else:
            argv = [self.path, "-std=c11", "-fsyntax-only"]
        include_prefix = "/I" if self.driver == CC_DRIVER_MSVC else "-I"
        for inc in include_dirs:
            argv.append(f"{include_prefix}{inc}")
        argv.append(str(source))
        return argv


def _c_compiler_driver(path: str) -> str:
    name = Path(path).name.lower()
    if name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        return CC_DRIVER_MSVC
    if "zig" in name:
        return CC_DRIVER_ZIG
    return CC_DRIVER_GNU


def _c_compiler_probe_argv(path: str, driver: str) -> list[str]:
    if driver == CC_DRIVER_MSVC:
        return [path, "/nologo", "/?"]
    return [path, "--version"]


def _find_c_compiler(cache_key: str, candidates: Sequence) -> "CCompiler | None":
    if cache_key in _probe_cache:
        cached = _probe_cache[cache_key]
        return _CC_OBJS.get(cached) if cached else None

    for name in candidates:
        resolved = shutil.which(name)
        if not resolved:
            continue
        driver = _c_compiler_driver(resolved)
        probe = proc.run(_c_compiler_probe_argv(resolved, driver), timeout=30)
        if probe.ok:
            cc = CCompiler(path=resolved, driver=driver)
            _probe_cache[cache_key] = resolved
            _CC_OBJS[resolved] = cc
            return cc
    _probe_cache[cache_key] = None
    return None


def find_c_compiler() -> "CCompiler | None":
    """Find the general-purpose C driver used by build-and-run test fixtures."""
    import os

    candidates: "list[str]" = []
    if os.environ.get("CC"):
        candidates.append(os.environ["CC"])
    candidates.extend(["cc", "clang", "gcc"])
    return _find_c_compiler("cc", candidates)


def find_c_syntax_compiler() -> "CCompiler | None":
    """Find a verified driver capable of checking generated hosted C11.

    Windows developer environments expose MSVC as `cl`, not `cc`. Keep this
    capability separate from build-and-run fixture compilers: syntax-only C11
    has a precise MSVC contract, while freestanding/shared-library fixture
    construction requires different driver-specific link capabilities.
    """
    import os

    candidates: "list[str]" = []
    if os.environ.get("CC"):
        candidates.append(os.environ["CC"])
    if platform.IS_WINDOWS:
        candidates.extend(["cl", "clang-cl"])
    candidates.extend(["cc", "clang", "gcc"])
    return _find_c_compiler("c_syntax", candidates)


_CC_OBJS: "dict[str, CCompiler]" = {}


def reset_probe_cache() -> None:
    """Clear cached probe results. For tests that vary the environment."""
    _probe_cache.clear()
    _CC_OBJS.clear()
