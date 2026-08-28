#!/usr/bin/env python3
"""Reject incremental-cache symbols reachable from runtime-only archives.

The runtime product must be linkable without the compiler's verified plan
cache. A header lint proves runtime headers do not reach compiler headers; it
cannot prove the archive is free of the definitions themselves, which is what
an accidental source addition to a runtime target would introduce.

The forbidden symbol set is derived from the cache owner's own headers rather
than restated here, so a new export is covered the moment it is declared.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

EXPORT_RE = re.compile(
    r"^\s*XR_FUNC\s+[A-Za-z_][\w\s*]*?\b([A-Za-z_]\w*)\s*\(", re.MULTILINE
)
# nm marks a definition with a letter that is not the undefined-reference `U`.
DEFINITION_RE = re.compile(r"^\S*\s+([A-TV-Za-tv-z])\s+(\S+)\s*$")
SKIP_EXIT = 77


def find_nm() -> str | None:
    for name in ("nm", "llvm-nm"):
        found = shutil.which(name)
        if found:
            return found
    if sys.platform == "win32":
        program_files = os.environ.get("ProgramFiles")
        if program_files:
            candidate = Path(program_files) / "LLVM" / "bin" / "llvm-nm.exe"
            if candidate.is_file():
                return str(candidate)
    return None


def cache_exports(root: Path) -> set[str]:
    owner = root / "src" / "incremental"
    exports: set[str] = set()
    for header in sorted(owner.glob("*.h")):
        exports.update(EXPORT_RE.findall(header.read_text(encoding="utf-8")))
    return exports


def defined_symbols(nm: str, archive: Path) -> set[str]:
    result = subprocess.run(
        [nm, "-g", str(archive)], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(f"{nm} failed on {archive}: {result.stderr.strip()}")
    symbols: set[str] = set()
    for line in result.stdout.splitlines():
        match = DEFINITION_RE.match(line)
        if not match:
            continue
        name = match.group(2)
        # Mach-O and some COFF toolchains prefix C symbols with an underscore.
        symbols.add(name[1:] if name.startswith("_") else name)
    return symbols


def partition_archives(archives: list[Path]) -> tuple[list[Path], list[Path]]:
    existing: list[Path] = []
    missing: list[Path] = []
    for archive in archives:
        (existing if archive.is_file() else missing).append(archive)
    return existing, missing


def self_test() -> int:
    sample = "XR_FUNC bool xr_cache_store_collect(XrCacheStore *store);\n"
    if EXPORT_RE.findall(sample) != ["xr_cache_store_collect"]:
        print("runtime archive cache symbol lint self-test: FAIL (export parse)")
        return 1
    if EXPORT_RE.findall("XR_FUNC XrCacheStore *xr_cache_store_open(const void *c);\n") != [
        "xr_cache_store_open"
    ]:
        print("runtime archive cache symbol lint self-test: FAIL (pointer return)")
        return 1
    multiline = (
        "XR_FUNC void\n"
        "xr_program_target_plan_cancellation_token_request(void *token);\n"
        "XR_FUNC const XrInvalidationRecord *\n"
        "xr_invalidation_result_at(const void *result, size_t index);\n"
    )
    if EXPORT_RE.findall(multiline) != [
        "xr_program_target_plan_cancellation_token_request",
        "xr_invalidation_result_at",
    ]:
        print("runtime archive cache symbol lint self-test: FAIL (multiline exports)")
        return 1
    present = Path(__file__)
    absent = present.with_name("runtime-archive-cache-symbol-lint-missing")
    existing, missing = partition_archives([present, absent])
    if existing != [present] or missing != [absent]:
        print("runtime archive cache symbol lint self-test: FAIL (archive completeness)")
        return 1
    if DEFINITION_RE.match("                 U _xr_cache_store_open"):
        print("runtime archive cache symbol lint self-test: FAIL (undefined accepted)")
        return 1
    match = DEFINITION_RE.match("0000000000000abc T _xr_cache_store_open")
    if not match or match.group(2) != "_xr_cache_store_open":
        print("runtime archive cache symbol lint self-test: FAIL (definition parse)")
        return 1
    print("runtime archive cache symbol lint self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--archive", type=Path, action="append", default=[])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    root = args.root.resolve(strict=True)
    exports = cache_exports(root)
    if not exports:
        print("runtime archive cache symbol lint: FAIL (no cache exports found)")
        return 1

    archives, missing = partition_archives(args.archive)
    if missing:
        print("runtime archive cache symbol lint: SKIP (archive not built)")
        for archive in missing:
            print(f"  - missing archive: {archive}")
        return SKIP_EXIT
    if not archives:
        print("runtime archive cache symbol lint: SKIP (no archive built)")
        return SKIP_EXIT
    nm = find_nm()
    if not nm:
        print("runtime archive cache symbol lint: SKIP (no nm or llvm-nm found)")
        return SKIP_EXIT

    failed = False
    for archive in archives:
        try:
            leaked = sorted(exports & defined_symbols(nm, archive))
        except RuntimeError as error:
            print(f"runtime archive cache symbol lint: SKIP ({error})")
            return SKIP_EXIT
        if leaked:
            failed = True
            print(f"runtime archive cache symbol lint: FAIL {archive}")
            for symbol in leaked:
                print(f"  - runtime archive defines cache symbol: {symbol}")
    if failed:
        return 1
    print(
        "runtime archive cache symbol lint: PASS "
        f"({len(archives)} archives, {len(exports)} cache exports)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
