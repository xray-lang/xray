#!/usr/bin/env python3
"""Audit zero-cost feature coverage against current AOT plan/filetest evidence.

Each inventory row claims a zero-cost family is either backed by a pattern that
really occurs somewhere in the tree, or is explicitly named as a gap. That keeps
the TODO surface executable: a family cannot quietly lose its evidence, and an
unfinished one has to say so rather than being silently absent.

Searching is done in-process rather than by shelling out to rg/grep, so there is
one matching semantics on every host instead of two (the shell version picked
whichever tool was installed).

Usage: plan_inventory.py [inventory.tsv]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import List, Optional

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parents[3]
DEFAULT_INVENTORY = SCRIPT_DIR / "plan_inventory.tsv"

FIELDS = ("feature", "status", "pattern", "scope", "note")

# Binary and build artifacts would only slow the scan down and never carry the
# source-level evidence these rows point at.
SKIP_DIRS = {".git", "build", "build-asan", "build-tsan", "build-lsan",
             "build-release", "build-fast", ".cache", "node_modules"}


def search_pattern(pattern: str, scope: str) -> bool:
    """Whether `pattern` (an extended regex) occurs anywhere under `scope`."""
    root = PROJECT_DIR / scope
    if not root.exists():
        return False
    try:
        regex = re.compile(pattern)
    except re.error:
        return False

    if root.is_file():
        return _file_matches(root, regex)

    for path in root.rglob("*"):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.is_file() and _file_matches(path, regex):
            return True
    return False


def _file_matches(path: Path, regex) -> bool:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return bool(regex.search(text))


def main(argv: List[str]) -> int:
    inventory = Path(argv[1]) if len(argv) > 1 else DEFAULT_INVENTORY
    if not inventory.is_file():
        sys.stderr.write(f"FAIL: inventory not found: {inventory}\n")
        return 1

    passed = gaps = failed = 0
    for number, raw in enumerate(inventory.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.rstrip("\r")
        if not line or line.startswith("#"):
            continue

        parts = line.split("|")
        if len(parts) != len(FIELDS) or any(p == "" for p in parts):
            print(f"FAIL line {number}: expected {'|'.join(FIELDS)}")
            failed += 1
            continue

        feature, status, pattern, scope, note = parts
        if status == "covered":
            if search_pattern(pattern, scope):
                print(f"PASS {feature}: {note}")
                passed += 1
            else:
                print(f"FAIL {feature}: missing pattern '{pattern}' under {scope}")
                failed += 1
        elif status == "gap":
            print(f"GAP  {feature}: {note}")
            gaps += 1
        else:
            print(f"FAIL {feature}: unknown status '{status}'")
            failed += 1

    print(f"=== zero-cost inventory: {passed} covered, {gaps} gaps, {failed} failed ===")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
