#!/usr/bin/env python3
"""Three-way reconciliation for flow-sensitive type narrowing.

  1. every rule N-x declared in the spec section is cited by at least one
     conformance test;
  2. every rule is cited by the analyzer source that implements it, so a rule
     can never be silently dropped from the compiler;
  3. the spec section, the MCP knowledge card, and the E0379 error-code row all
     exist and reference each other.

Narrowing feeds code generation (the narrowed type is what lowering sees), so
an unimplemented or untested rule is a VM/AOT divergence risk, not a
documentation nit. That is why this is fail-closed.

Usage: check_narrowing_rules.py    (run from the project root)
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import List, Sequence

PROJECT_ROOT = Path(__file__).resolve().parent.parent

SPEC_SECTION = Path("spec/source/sections/003-2-type-system.md")
CARD = Path("spec/source/cards/topics/narrowing.json")
ERROR_CODES = Path("spec/source/sections/019-18-error-codes.md")

TEST_DIRS = (Path("tests/compile_errors/narrowing"),
             Path("tests/regression/16_narrowing"))

IMPL_FILES = (
    Path("src/frontend/analyzer/xanalyzer_flow.c"),
    Path("src/frontend/analyzer/xanalyzer_flow.h"),
    Path("src/frontend/analyzer/xanalyzer_visitor.c"),
    Path("src/frontend/analyzer/xanalyzer_visitor_expr.c"),
    Path("src/frontend/analyzer/xanalyzer_visitor_call.c"),
    Path("src/frontend/analyzer/xanalyzer_visitor_stmt.c"),
    Path("src/frontend/analyzer/xanalyzer_visitor_internal.h"),
    Path("src/frontend/parser/xparse_decl.c"),
    Path("src/frontend/parser/xast_nodes_expr.h"),
)

MIN_RULES = 10
RULE_MARKER = re.compile(r"\*\*(N-[0-9][0-9.]*)")

USE_COLOR = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
RED = "\033[0;31m" if USE_COLOR else ""
GREEN = "\033[0;32m" if USE_COLOR else ""
NC = "\033[0m" if USE_COLOR else ""


class Report:
    def __init__(self) -> None:
        self.errors = 0

    def ok(self, message: str) -> None:
        print(f"  {GREEN}PASS{NC}: {message}")

    def bad(self, message: str) -> None:
        print(f"  {RED}FAIL{NC}: {message}")
        self.errors += 1


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def contains(paths: Sequence[Path], needle: str) -> bool:
    """True when any readable path holds the literal needle."""
    for path in paths:
        if path.is_file() and needle in read(path):
            return True
    return False


def files_under(dirs: Sequence[Path]) -> List[Path]:
    found: List[Path] = []
    for directory in dirs:
        if directory.is_dir():
            found.extend(p for p in directory.rglob("*") if p.is_file())
    return found


def file_prefix(rule: str) -> str:
    """The `nNN` filename prefix a test may use instead of citing the rule.

    N-4 -> n04, N-11.1 -> n11: the major number only, zero-padded to two.
    """
    major = rule[len("N-"):].split(".")[0]
    try:
        return "n%02d" % int(major)
    except ValueError:
        return f"n{major}"


def main(argv: List[str]) -> int:
    os.chdir(PROJECT_ROOT)
    report = Report()

    print("============================================")
    print("Narrowing rule reconciliation (spec §2.13)")
    print("============================================")

    for path in (SPEC_SECTION, CARD, ERROR_CODES):
        if not path.is_file():
            report.bad(f"missing source of truth: {path}")
            print(f"aborting: cannot reconcile without {path}")
            return 1

    spec_text = read(SPEC_SECTION)
    # sorted() matches the shell's `sort -u`, so the rules are reported in the
    # same lexicographic order (N-1, N-10, N-11, N-11.1, N-12, N-2, ...).
    rules = sorted(set(RULE_MARKER.findall(spec_text)))
    if len(rules) < MIN_RULES:
        report.bad(f"expected at least {MIN_RULES} narrowing rules in "
                   f"{SPEC_SECTION}, found {len(rules)}")
    else:
        report.ok(f"{len(rules)} narrowing rules declared in {SPEC_SECTION}")

    print("")
    print("-- rule -> conformance test")
    for directory in TEST_DIRS:
        if not directory.is_dir():
            report.bad(f"missing conformance test directory: {directory}")

    test_files = files_under(TEST_DIRS)
    test_names = [p.name for p in test_files]
    for rule in rules:
        # A test cites a rule either in prose ("N-4") or through its file name
        # (n04_..., n11_1_...). Both forms count.
        prefix = file_prefix(rule)
        if contains(test_files, rule):
            report.ok(f"{rule} cited by a conformance test")
        elif any(name.startswith(prefix) for name in test_names):
            report.ok(f"{rule} covered by a {prefix}* test file")
        else:
            dirs = " ".join(str(d) for d in TEST_DIRS)
            report.bad(f"{rule} has no conformance test under: {dirs}")

    print("")
    print("-- rule -> analyzer implementation")
    for rule in rules:
        if contains(IMPL_FILES, rule):
            report.ok(f"{rule} cited by the implementation")
        else:
            report.bad(f"{rule} is not referenced by any implementation file; "
                       "either implement it or drop the rule")

    print("")
    print("-- cross references")
    if "E0379" in spec_text:
        report.ok("§2.13 references E0379")
    else:
        report.bad("§2.13 must state which error code its null rules raise (E0379)")

    if "2.13" in read(ERROR_CODES):
        report.ok("E0379 row points back at §2.13")
    else:
        report.bad(f"the E0379 row in {ERROR_CODES} must point at §2.13")

    card_text = read(CARD)
    if "narrowing" in card_text and "E0379" in card_text:
        report.ok("MCP knowledge card covers narrowing and E0379")
    else:
        report.bad(f"{CARD} must cover narrowing and cite E0379")

    # The diagnostic text is the user-facing half of N-12; keep it discoverable.
    analyzer = Path("src/frontend/analyzer/xanalyzer_visitor_expr.c")
    if contains((analyzer,), "possibly-null value"):
        report.ok("E0379 diagnostic text present in the analyzer")
    else:
        report.bad("E0379 diagnostic text missing from the analyzer")

    print("")
    print("============================================")
    if report.errors == 0:
        print(f"{GREEN}All narrowing reconciliation checks passed{NC}")
        return 0
    print(f"{RED}{report.errors} narrowing reconciliation failure(s){NC}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
