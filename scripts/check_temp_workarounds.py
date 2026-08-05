#!/usr/bin/env python3
"""Reconcile DEFENSIVE-TEMP tags in source against their tracking table.

The table lives at tests/known_temp_workarounds.md rather than under docs/,
because docs/ is intentionally untracked; this file is a CI-enforced
engineering contract, peer to tests/known_failures.txt.

The check runs in both directions, and both are mandatory -- a one-way check
lets rot accumulate on the side it does not look at:

  1. Tag without row -> FAIL. Every source block of the form
         // DEFENSIVE-TEMP[NNN]: <summary>.
         //   Tracking row "<id>" in tests/known_temp_workarounds.md.
     must have a table row whose Tag column equals <id>.
  2. Row without tag -> FAIL. Every table row must be referenced from source.

Plus two structural checks: each row fills all eight columns, and each tag
header is immediately followed by its tracking line (a header with no tracking
line is a malformed tag, not a missing row).

Exit 0 when every check passes, 1 otherwise.

Usage: check_temp_workarounds.py
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import List, Sequence, Set, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent
TABLE_FILE = Path("tests/known_temp_workarounds.md")
SCAN_DIR = Path("src")
SOURCE_SUFFIXES = (".c", ".h")

# Splitting `|a|b|...|` yields a leading empty, the eight cells, and a trailing
# empty -- ten fields. The cells are therefore indices 1..8; index 9 is the
# trailing empty and must not be checked for emptiness.
ROW_FIELDS = 10
FIRST_CELL, LAST_CELL = 1, 8

TRACKING_REF = re.compile(r'Tracking row "([^"]+)"')
TAG_HEADER = re.compile(r"DEFENSIVE-TEMP\[[0-9A-Za-z_-]+\]:")
ALIGNMENT_ROW = re.compile(r"^-+$")

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[31m" if USE_COLOR else ""
GREEN = "\033[32m" if USE_COLOR else ""
YELLOW = "\033[33m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


def red(message: str) -> None:
    print(f"{RED}{message}{NC}")


def green(message: str) -> None:
    print(f"{GREEN}{message}{NC}")


def yellow(message: str) -> None:
    print(f"{YELLOW}{message}{NC}")


def section(title: str) -> None:
    print("")
    print(f"=== {title} ===")


def table_rows(text: str) -> List[List[str]]:
    """Data rows of the markdown table, cells trimmed, header/alignment gone."""
    rows: List[List[str]] = []
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.split("|")]
        if len(cells) < ROW_FIELDS:
            continue
        tag = cells[1]
        if tag == "Tag" or ALIGNMENT_ROW.match(tag) or tag == "":
            continue
        rows.append(cells)
    return rows


def source_files() -> List[Path]:
    root = REPO_ROOT / SCAN_DIR
    if not root.is_dir():
        return []
    return sorted(p for p in root.rglob("*")
                  if p.is_file() and p.suffix in SOURCE_SUFFIXES)


def main(argv: List[str]) -> int:
    os.chdir(REPO_ROOT)
    failed = False

    if not TABLE_FILE.is_file():
        red(f"FAIL: {TABLE_FILE} is missing.")
        return 1

    table_text = TABLE_FILE.read_text(encoding="utf-8")
    rows = table_rows(table_text)
    table_ids = sorted({row[1] for row in rows})

    section("table parse")
    if not table_ids:
        green(f"OK: {TABLE_FILE} has no data rows (table is clean).")
    else:
        green(f"Parsed {len(table_ids)} row(s) from {TABLE_FILE}.")

    section("row completeness")
    empty_cells = [f'row "{row[1]}" has empty cell #{index}'
                   for row in rows
                   for index in range(FIRST_CELL, LAST_CELL + 1)
                   if row[index] == ""]
    if empty_cells:
        red("FAIL: empty cells in table:")
        for entry in empty_cells:
            print(f"  {entry}")
        failed = True
    else:
        green("OK: every row has all eight cells filled.")

    files = source_files()
    tag_refs: Set[str] = set()
    for path in files:
        tag_refs.update(TRACKING_REF.findall(
            path.read_text(encoding="utf-8", errors="replace")))

    section("tag collection")
    if not tag_refs:
        yellow(f"Note: no DEFENSIVE-TEMP tracking refs found under {SCAN_DIR}.")

    section("tag block shape")
    malformed: List[str] = []
    for path in files:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for index, line in enumerate(lines):
            if not TAG_HEADER.search(line):
                continue
            following = lines[index + 1] if index + 1 < len(lines) else ""
            if not TRACKING_REF.search(following):
                malformed.append(
                    f'{path.relative_to(REPO_ROOT)}:{index + 1}  missing '
                    f'`Tracking row "..."` on line {index + 2}')
    if malformed:
        red("FAIL: malformed DEFENSIVE-TEMP block(s):")
        for entry in malformed:
            print(f"  {entry}")
        failed = True
    else:
        green("OK: every DEFENSIVE-TEMP header has a tracking line.")

    section("tag -> row reconciliation")
    if tag_refs:
        orphan_tags = sorted(tag_refs - set(table_ids))
        if orphan_tags:
            red(f"FAIL: source tags without a matching row in {TABLE_FILE}:")
            for entry in orphan_tags:
                print(f"  {entry}")
            print("")
            print(f"  Add the missing row(s) to {TABLE_FILE} with all eight "
                  "columns,")
            print("  or correct the tag id in source.")
            failed = True
        else:
            green("OK: every source tag has a matching table row.")

    section("row -> tag reconciliation")
    orphan_rows = sorted(set(table_ids) - tag_refs)
    if orphan_rows:
        red("FAIL: table rows without a matching source tag:")
        for entry in orphan_rows:
            print(f"  {entry}")
        print("")
        print("  Either re-add the DEFENSIVE-TEMP tag in source, or remove the")
        print(f"  stale row from {TABLE_FILE}.")
        failed = True
    else:
        green("OK: every table row has a matching source tag.")

    section("summary")
    if failed:
        red("DEFENSIVE-TEMP reconciliation: one or more checks failed.")
        return 1
    green("DEFENSIVE-TEMP reconciliation: all checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
