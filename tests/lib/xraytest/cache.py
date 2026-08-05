"""Content-addressed cache keys, cache directories, and directory locks.

A cache key answers one question: "would recomputing this produce a different
result?" If not, a warm entry is reused. The danger is a key that misses an
input -- it then reports a hit for a state it never actually established, which
is how a stale cache masks a real failure. Two design choices guard against it:

  - The AOT toolchain key walks the *transitive include closure* of the runtime
    header, not a directory glob. The closure reaches 87 headers, 54 of them
    outside src/aot (src/base, src/os, src/shared). A glob of src/aot silently
    dropped those 54 -- exactly the kind of miss that produces a false hit.

  - Absent files hash to the marker "missing" rather than being skipped, so a
    file appearing or disappearing changes the key.

Absorbed the former scripts/test_cache_key.py, which existed only so shell
suites could compute keys in one process; with no shell callers left, this is
the single implementation.
"""

from __future__ import annotations

import hashlib
import os
import re
import time
from pathlib import Path
from typing import Iterable, Sequence

from . import platform

# Local includes only: <system> headers belong to the toolchain, not the tree.
_INCLUDE_RE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.MULTILINE)

# The header generated C includes. Everything it reaches is compiled into every
# generated translation unit, so it all belongs in the toolchain key.
AOT_RUNTIME_ROOT = "src/aot/xrt.h"

# Sources that decide what C is emitted (as opposed to what the emitted C
# includes). Directory-scoped: every file under them compiles into the generator.
AOT_GENERATOR_DIRS = ("src/aot",)
AOT_GENERATOR_FILES = ("src/ir/xi_method_sym.def",)
AOT_SOURCE_SUFFIXES = (".h", ".c", ".inc.c", ".def")

# Static libraries linked into the running xray, part of "what actually runs".
LINKED_LIBS = ("xray_aot_core", "xray_rt_coro", "xray_core")

MISSING = "missing"


def file_digest(path: Path) -> str:
    """Content digest of one file, or MISSING when it is absent/unreadable."""
    try:
        with path.open("rb") as handle:
            digest = hashlib.blake2b(digest_size=16)
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
            return digest.hexdigest()
    except OSError:
        return MISSING


def mix(parts: Iterable[str]) -> str:
    """Order-sensitive digest of labeled parts. Callers pre-sort file lists."""
    digest = hashlib.blake2b(digest_size=8)
    for part in parts:
        digest.update(part.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()


def _rel(path: Path, base: Path) -> str:
    try:
        return path.relative_to(base).as_posix()
    except ValueError:
        return path.as_posix()


def include_closure(root: Path, project_dir: Path) -> "list[Path]":
    """Every local header reachable from `root` through #include "...".

    Quoted-include resolution close enough for a key: relative to the including
    file first, then to the project root. An include resolving to neither is a
    system or generated header and not part of the tree's identity.
    """
    seen: "set[Path]" = set()
    pending = [root]
    while pending:
        current = pending.pop()
        try:
            current = current.resolve()
        except OSError:
            continue
        if current in seen or not current.is_file():
            continue
        seen.add(current)
        try:
            body = current.read_bytes()
        except OSError:
            continue
        for match in _INCLUDE_RE.finditer(body):
            name = match.group(1).decode("utf-8", "replace")
            for base in (current.parent, project_dir):
                candidate = base / name
                if candidate.is_file():
                    pending.append(candidate)
                    break
    return sorted(seen)


def _collect_generator_sources(project_dir: Path) -> "list[Path]":
    out: "list[Path]" = []
    for rel in AOT_GENERATOR_DIRS:
        directory = project_dir / rel
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.name.endswith(AOT_SOURCE_SUFFIXES):
                out.append(path)
    for rel in AOT_GENERATOR_FILES:
        path = project_dir / rel
        if path.is_file():
            out.append(path)
    return sorted(set(out))


def toolchain_key(xray_bin: Path, project_dir: Path) -> str:
    """Identity of the AOT pipeline: the binary, its libs, generator, runtime.

    A change to any of the three independent inputs -- what runs, what decides
    the emitted C, what that C includes -- yields a different key.
    """
    xray_bin = Path(xray_bin)
    project_dir = Path(project_dir).resolve()
    bin_dir = xray_bin.parent

    parts = [f"xray {file_digest(xray_bin)}"]
    for stem in LINKED_LIBS:
        name = platform.static_lib_name(stem)
        parts.append(f"{name} {file_digest(bin_dir / name)}")

    for path in _collect_generator_sources(project_dir):
        parts.append(f"src {_rel(path, project_dir)} {file_digest(path)}")

    runtime_root = project_dir / AOT_RUNTIME_ROOT
    if runtime_root.is_file():
        for path in include_closure(runtime_root, project_dir):
            parts.append(f"rt {_rel(path, project_dir)} {file_digest(path)}")

    return mix(parts)


def files_key(paths: Sequence) -> str:
    """Key over an explicit, order-preserving list of files."""
    return mix(f"{p} {file_digest(Path(p))}" for p in paths)


def dir_key(directory: Path, globs: Sequence | None = None) -> str:
    """Key over the files a directory holds, by name and content.

    Case-directory identity for suites whose fixtures live beside the case:
    sources, args sidecars, and manifests.
    """
    directory = Path(directory)
    patterns = list(globs) if globs else ["*.xr", "*.args", "*.toml"]
    found: "set[Path]" = set()
    for pattern in patterns:
        found.update(p for p in directory.glob(pattern) if p.is_file())
    return mix(f"{p.name} {file_digest(p)}" for p in sorted(found))


def cache_root(project_dir: Path) -> Path:
    """Root for all suite caches, overridable for isolation in CI shards."""
    override = os.environ.get("XRAY_TEST_CACHE_ROOT")
    if override:
        return Path(override)
    return Path(project_dir) / ".cache" / "xray-test"


def stable_cache_dir(project_dir: Path, suite: str, xray_bin: Path) -> Path:
    """Per-suite cache directory keyed by toolchain identity.

    Distinct toolchains never share a directory, so a rebuilt compiler warms a
    fresh cache instead of colliding with the old one's entries.
    """
    key = toolchain_key(xray_bin, project_dir)
    return cache_root(project_dir) / suite / key


def _lock_timeout_ticks() -> int:
    raw = os.environ.get("XRAY_TEST_LOCK_TIMEOUT", "3000")
    try:
        value = int(raw)
    except ValueError:
        return 3000
    return value if value >= 0 else 3000


class DirLock:
    """Cross-process lock via atomic mkdir, matching the shell lock's contract.

    mkdir is the portable atomic primitive: it succeeds for exactly one racer
    and fails with FileExistsError for the rest. Held as a context manager so
    the lock is released on the exception path too. Times out to avoid a
    deadlocked run wedging a whole CI lane.
    """

    def __init__(self, path: Path) -> None:
        self._path = Path(path)
        self._held = False

    def acquire(self) -> bool:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        waited = 0
        timeout = _lock_timeout_ticks()
        while True:
            try:
                self._path.mkdir()
                self._held = True
                return True
            except FileExistsError:
                if waited >= timeout:
                    return False
                waited += 1
                time.sleep(0.1)
            except OSError:
                return False

    def release(self) -> None:
        if not self._held:
            return
        self._held = False
        try:
            self._path.rmdir()
        except OSError:
            pass

    def __enter__(self) -> "DirLock":
        if not self.acquire():
            raise TimeoutError(f"could not lock {self._path}")
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        self.release()
        return False
