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

import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from . import platform, proc

_probe_cache: "dict[str, str | None]" = {}

CC_DRIVER_GNU = "gnu"
CC_DRIVER_ZIG = "zig"
CC_DRIVER_MSVC = "msvc"

SYMBOL_DUMPER_DUMPBIN = "dumpbin"
SYMBOL_DUMPER_LLVM_NM = "llvm-nm"
SYMBOL_DUMPER_NM = "nm"


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


_DUMPBIN_SYMBOL_RE = re.compile(
    r"^\s*[0-9A-F]+\s+[0-9A-F]+\s+"
    r"(?P<section>UNDEF|SECT[0-9A-F]+|ABS)\s+.*?\|\s*(?P<symbol>\S.*)$",
    re.IGNORECASE,
)
_POSIX_NM_SYMBOL_RE = re.compile(
    r"^(?:.*?:\s+)?(?P<symbol>\S+)\s+(?P<kind>[A-Za-z?])"
    r"(?:\s+(?:[0-9A-Fa-f]+|-))?(?:\s+(?:[0-9A-Fa-f]+|-))?\s*$"
)


def _normalize_dumpbin_symbols(raw: str) -> "tuple[str | None, str]":
    symbols: "list[str]" = []
    for line in raw.splitlines():
        if "|" not in line:
            continue
        match = _DUMPBIN_SYMBOL_RE.match(line)
        if not match:
            return None, "unrecognized dumpbin symbol row"
        if match.group("section").upper() == "UNDEF":
            continue
        symbols.append(match.group("symbol").split(None, 1)[0])
    if not symbols:
        return None, "dumpbin output contains no defined symbols"
    return "\n".join(symbols), ""


def _normalize_posix_nm_symbols(raw: str) -> "tuple[str | None, str]":
    symbols: "list[str]" = []
    for line in raw.splitlines():
        stripped = line.strip()
        if not stripped or stripped.endswith(":"):
            continue
        match = _POSIX_NM_SYMBOL_RE.match(stripped)
        if not match:
            return None, "unrecognized nm symbol row"
        if match.group("kind").upper() == "U":
            return None, "defined-only nm output contains an undefined symbol"
        symbols.append(match.group("symbol"))
    if not symbols:
        return None, "nm output contains no defined symbols"
    return "\n".join(symbols), ""


@dataclass(frozen=True)
class SymbolDumper:
    """A probed provider that returns only normalized, defined symbol names."""

    path: str
    driver: str

    def dump_argv(self, artifact: Path) -> "list[str]":
        if self.driver == SYMBOL_DUMPER_DUMPBIN:
            return [self.path, "/nologo", "/symbols", str(artifact)]
        if self.driver == SYMBOL_DUMPER_NM and platform.IS_DARWIN:
            return [self.path, "-U", "-P", str(artifact)]
        return [self.path, "--defined-only", "--format=posix", str(artifact)]

    def dump_defined_symbols(self, artifact: Path) -> "tuple[bool, str]":
        result = proc.run(self.dump_argv(artifact), timeout=120)
        if not result.ok:
            state = "timed out" if result.timed_out else f"exit {result.returncode}"
            detail = result.combined_text().strip()
            suffix = f": {detail}" if detail else ""
            return False, f"{self.path} {state}{suffix}"

        raw = result.stdout.decode("utf-8", "replace")
        if self.driver == SYMBOL_DUMPER_DUMPBIN:
            normalized, error = _normalize_dumpbin_symbols(raw)
        else:
            normalized, error = _normalize_posix_nm_symbols(raw)
        if normalized is None:
            detail = result.combined_text().strip()
            suffix = f": {detail}" if detail else ""
            return False, f"{self.path}: {error}{suffix}"
        return True, normalized


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


def _symbol_dumper_driver(path: str) -> "str | None":
    name = Path(path).name.lower()
    if name in ("dumpbin", "dumpbin.exe"):
        return SYMBOL_DUMPER_DUMPBIN
    if name in ("llvm-nm", "llvm-nm.exe"):
        return SYMBOL_DUMPER_LLVM_NM
    if name in ("nm", "nm.exe"):
        return SYMBOL_DUMPER_NM
    return None


def _symbol_dumper_probe(path: str, driver: str) -> bool:
    if driver == SYMBOL_DUMPER_DUMPBIN:
        # DUMPBIN's help path returns 1100. Parsing its own PE headers is a
        # real, zero-exit capability probe with an output shape an impostor
        # cannot satisfy merely by accepting the command line.
        result = proc.run([path, "/nologo", "/headers", path], timeout=30)
        text = result.combined_text().lower()
        return (result.ok and "pe signature found" in text and
                "file type: executable image" in text)

    probes = [[path, "--version"]]
    if driver == SYMBOL_DUMPER_NM and platform.IS_DARWIN:
        probes.append([path, "-V"])
    for argv in probes:
        result = proc.run(argv, timeout=30)
        text = result.combined_text().lower()
        if (result.ok and "nm" in text and
                ("gnu" in text or "llvm" in text or "apple" in text)):
            return True
    return False


def find_symbol_dumper() -> "SymbolDumper | None":
    """Find a verified provider of defined symbols for binaries and archives.

    A resolved executable is not enough: the probe validates both its identity
    and a successful provider-specific command. Once selected, a dump failure
    is final for that artifact and never falls through to a different tool.
    """
    if "symbol_dumper" in _probe_cache:
        cached = _probe_cache["symbol_dumper"]
        return _SYMBOL_DUMPER_OBJS.get(cached) if cached else None

    candidates = (["dumpbin", "llvm-nm", "nm"] if platform.IS_WINDOWS
                  else ["nm", "llvm-nm"])
    for name in candidates:
        resolved = shutil.which(name)
        if not resolved:
            continue
        driver = _symbol_dumper_driver(resolved)
        if driver is None or not _symbol_dumper_probe(resolved, driver):
            continue
        dumper = SymbolDumper(path=resolved, driver=driver)
        _probe_cache["symbol_dumper"] = resolved
        _SYMBOL_DUMPER_OBJS[resolved] = dumper
        return dumper

    _probe_cache["symbol_dumper"] = None
    return None


_CC_OBJS: "dict[str, CCompiler]" = {}
_SYMBOL_DUMPER_OBJS: "dict[str, SymbolDumper]" = {}


def reset_probe_cache() -> None:
    """Clear cached probe results. For tests that vary the environment."""
    _probe_cache.clear()
    _CC_OBJS.clear()
    _SYMBOL_DUMPER_OBJS.clear()
