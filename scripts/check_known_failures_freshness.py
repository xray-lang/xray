#!/usr/bin/env python3
"""Every suppression in tests/known_failures.txt must be young and attributable.

Reverse invariant E.4: a line that disables a test must carry ISSUE=, OWNER=
and ADDED=YYYY-MM-DD, and the date must be at most 30 days old. Older entries
fail CI unconditionally, which forces the owner to either fix the root cause or
re-justify the suppression. Permanent suppressions are not allowed -- that is
the whole point of the gate.

Line format (one per disabled test):
    <test path>  ISSUE=<url>  ADDED=YYYY-MM-DD  OWNER=<email>

Comment lines (starting with #) and blank lines are ignored.

Exit 0 when nothing is expired, 1 on any expired, malformed, or incomplete
entry.

Usage: check_known_failures_freshness.py
"""

from __future__ import annotations

import os
import re
import sys
from datetime import date, datetime
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parent.parent
FILE = Path("tests/known_failures.txt")
MAX_AGE_DAYS = 30

ADDED_RE = re.compile(r"ADDED=([0-9-]+)")

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[31m" if USE_COLOR else ""
GREEN = "\033[32m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


def red(message: str) -> None:
    print(f"{RED}{message}{NC}")


def green(message: str) -> None:
    print(f"{GREEN}{message}{NC}")


def main(argv: List[str]) -> int:
    os.chdir(REPO_ROOT)
    if not FILE.is_file():
        red(f"ERR: {FILE} missing.")
        return 1

    today = date.today()
    total = expired = malformed = 0

    for number, line in enumerate(
            FILE.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        total += 1

        if "ISSUE=" not in line:
            red(f"  {FILE}:{number}: missing ISSUE= field")
            malformed += 1
            continue
        if "OWNER=" not in line:
            red(f"  {FILE}:{number}: missing OWNER= field")
            malformed += 1
            continue

        match = ADDED_RE.search(line)
        if not match:
            red(f"  {FILE}:{number}: missing ADDED=YYYY-MM-DD field")
            malformed += 1
            continue
        try:
            # Whole days between two calendar dates. The shell measured epoch
            # seconds, which made the answer depend on the time of day the gate
            # ran -- and differently on GNU and BSD date, because BSD's
            # `date -j -f %Y-%m-%d` fills the time fields from the current
            # clock while GNU's `date -d` uses midnight.
            added = datetime.strptime(match.group(1), "%Y-%m-%d").date()
        except ValueError:
            red(f"  {FILE}:{number}: unparseable date '{match.group(1)}'")
            malformed += 1
            continue

        age = (today - added).days
        if age > MAX_AGE_DAYS:
            red(f"  {FILE}:{number}: entry is {age} days old "
                f"(limit {MAX_AGE_DAYS}):")
            print(f"      {line}")
            expired += 1

    if not expired and not malformed:
        if total == 0:
            green(f"OK: {FILE} contains 0 active suppressions.")
        else:
            green(f"OK: {total} active suppression(s), all <= {MAX_AGE_DAYS} "
                  "days old.")
        return 0

    red(f"{expired} expired, {malformed} malformed entries.")
    print("")
    print("  Either fix the underlying test or re-justify with a new ADDED= date.")
    print("  Permanent suppressions are not allowed.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
