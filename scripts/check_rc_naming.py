#!/usr/bin/env python3
"""Legacy GC terminology must not survive in active RC runtime code.

Xray reclaims with reference counting plus a cycle collector; it has no tracing
GC. A name like XrGCHeader or xr_gc_* left in active code tells a reader the
runtime works in a way it does not, so this is a naming gate, not a style one.

Default mode is strict and fails on any legacy active name. `--allow-current`
prints the current distribution and exits 0, for recording a migration
baseline.

Usage: check_rc_naming.py [--allow-current]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import List, Tuple

PROJECT_ROOT = Path(__file__).resolve().parent.parent

TOKENS: Tuple[str, ...] = (
    "XrGCHeader", "XrCoroGC", "XrGC", "xr_gc_", "xr_coro_gc_",
    "g_type_ops", "XR_GC_", "XGC_", "GC_BARRIER",
)
# Word-bounded for the type names, bare for the prefixes: xr_gc_ is a prefix of
# longer identifiers and must match inside them.
PATTERN_TEXT = (r"\bXrGCHeader\b|\bXrCoroGC\b|\bXrGC\b|xr_gc_|xr_coro_gc_|"
                r"g_type_ops|XR_GC_|XGC_|GC_BARRIER")
PATTERN = re.compile(PATTERN_TEXT)

SCAN_DIRS = ("src", "include", "stdlib", "tests")
SUFFIXES = (".c", ".h", ".xr")

MAX_SHOWN = 80

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[0;31m" if USE_COLOR else ""
GREEN = "\033[0;32m" if USE_COLOR else ""
YELLOW = "\033[0;33m" if USE_COLOR else ""
CYAN = "\033[0;36m" if USE_COLOR else ""
RESET = "\033[0m" if USE_COLOR else ""


def scan() -> List[str]:
    """grep -RInE equivalent: `path:line:text` for every matching line."""
    hits: List[str] = []
    for directory in SCAN_DIRS:
        root = PROJECT_ROOT / directory
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in SUFFIXES:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            relative = path.relative_to(PROJECT_ROOT)
            for number, line in enumerate(text.splitlines(), start=1):
                if PATTERN.search(line):
                    hits.append(f"{relative}:{number}:{line}")
    return hits


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Scan active sources for legacy GC names.")
    parser.add_argument("--allow-current", action="store_true",
                        help="print matches and exit 0 even if legacy names "
                             "remain; use only for migration baselines")
    args = parser.parse_args(argv[1:])

    print("=== RC naming convergence check ===")
    print(f"pattern: {PATTERN_TEXT}")
    print(f"dirs: {' '.join(SCAN_DIRS)}")
    print("")

    hits = scan()
    if not hits:
        print(f"{GREEN}OK{RESET}: no legacy GC active names found.")
        return 0

    joined = "\n".join(hits)
    print(f"{CYAN}Legacy-name hits by token:{RESET}")
    for token in TOKENS:
        count = joined.count(token)
        if count:
            print(f"  {token:<18} {count}")

    print("")
    print(f"{CYAN}First matches:{RESET}")
    for line in hits[:MAX_SHOWN]:
        print(line)
    if len(hits) > MAX_SHOWN:
        print(f"... ({len(hits)} total matches, truncated)")

    print("")
    if args.allow_current:
        print(f"{YELLOW}ALLOW-CURRENT{RESET}: {len(hits)} legacy-name hit(s) remain.")
        return 0

    print(f"{RED}FAIL{RESET}: {len(hits)} legacy-name hit(s) remain.")
    print("Run with --allow-current only while recording a migration baseline.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
