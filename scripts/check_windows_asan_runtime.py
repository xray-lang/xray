#!/usr/bin/env python3
"""Fail unless this CTest process inherited the selected Windows ASan DLL."""

from __future__ import annotations

import argparse
import filecmp
import os
import sys
from pathlib import Path


DLL_NAME = "clang_rt.asan_dynamic-x86_64.dll"


def _same_directory(left: Path, right: Path) -> bool:
    try:
        return left.samefile(right)
    except OSError:
        return os.path.normcase(os.path.abspath(left)) == os.path.normcase(
            os.path.abspath(right)
        )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expected-dir", required=True, type=Path)
    parser.add_argument("--staged-dir", action="append", default=[], type=Path)
    args = parser.parse_args(argv[1:])

    expected = args.expected_dir.resolve()
    expected_dll = expected / DLL_NAME
    if not expected_dll.is_file():
        print(f"FAIL: selected ASan runtime is missing: {expected_dll}", file=sys.stderr)
        return 1

    path_entries = [
        Path(entry)
        for entry in os.environ.get("PATH", "").split(os.pathsep)
        if entry
    ]
    if not path_entries or not _same_directory(path_entries[0], expected):
        first = path_entries[0] if path_entries else "<empty>"
        print(
            f"FAIL: selected ASan runtime is not first on PATH: {first}",
            file=sys.stderr,
        )
        return 1

    # DLL is not an executable PATHEXT entry, so shutil.which() is not the
    # Windows loader oracle. Rebuild the loader's PATH file lookup directly.
    resolved = next(
        (entry / DLL_NAME for entry in path_entries if (entry / DLL_NAME).is_file()),
        None,
    )
    if resolved is None or not _same_directory(resolved.parent, expected):
        print(
            f"FAIL: Windows loader search resolves {DLL_NAME} to {resolved!r}",
            file=sys.stderr,
        )
        return 1

    for staged_dir in args.staged_dir:
        staged_dll = staged_dir.resolve() / DLL_NAME
        if not staged_dll.is_file():
            print(f"FAIL: staged ASan runtime is missing: {staged_dll}", file=sys.stderr)
            return 1
        if not filecmp.cmp(expected_dll, staged_dll, shallow=False):
            print(
                f"FAIL: staged ASan runtime differs from {expected_dll}: {staged_dll}",
                file=sys.stderr,
            )
            return 1

    print(f"PASS: Windows ASan runtime={expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
