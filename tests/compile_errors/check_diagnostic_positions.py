#!/usr/bin/env python3
"""Count diagnostics whose position is not a position, and refuse to let it grow.

`xray run` renders diagnostics through xdiag_fmt.h, which clamps an unset
column to 1 (`if (column <= 0) column = 1;`). That clamp is good for readers
and terrible for measurement: a diagnostic that never computed a column becomes
indistinguishable from one deliberately pointing at the start of a statement.
`xray check` prints the raw span, so this gate reads THAT and sees the zeros.

What it measures is therefore not "does the caret look plausible" -- the expected
files and their `misplaced` lines carry that human judgement -- but the strictly
mechanical question of whether the compiler computed a position at all.

The budget below is a ratchet. Fixing a position-losing emission point makes the
count fall, and the budget must fall with it; a new emission point that forgets
to pass a span makes the count rise, and this gate fails. Never raise the budget
to make a run green.

Usage: check_diagnostic_positions.py [--list]
Environment:
    XRAY / XRAY_BIN   the xray binary
    XRAY_TEST_JOBS    parallelism (default: number of CPUs)
"""

from __future__ import annotations

import os
import re
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc  # noqa: E402

platform.configure_utf8_stdio()

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

# `xray check` writes one line per diagnostic: `<file>:<line>:<col>: error: ...`
CHECK_LINE_RE = re.compile(
    r"^(?P<file>\S.*?):(?P<line>[0-9]+):(?P<col>[0-9]+): (?:error|warning|note): ",
    re.MULTILINE)

# The original 00f665c5c baseline across 882 cases / 903 diagnostics had 250
# diagnostics with a line but no column. The current 889-case corpus has 239;
# keep ratcheting this number down whenever a position-losing site is fixed.
# The 12 diagnostics with no line remain traced in
# blockers/9-diagnostic-positions-degenerate.md.
NO_COLUMN_BUDGET = 239
NO_LINE_BUDGET = 12


@dataclass(frozen=True)
class CasePositions:
    case: Path
    rows: list[tuple[str, int, int]]
    timed_out: bool
    output: str


def collect_cases() -> list[Path]:
    cases: list[Path] = []
    for directory in sorted(p for p in SCRIPT_DIR.iterdir() if p.is_dir()):
        cases.extend(sorted(directory.glob("*.xr")))
    return cases


def check_one(xray: Path, case: Path,
              timeout: float | None) -> CasePositions:
    env = dict(os.environ)
    inherited = os.environ.get("XRAY_TYPEPATH", "")
    env["XRAY_TYPEPATH"] = (f"{case.parent}{os.pathsep}{inherited}"
                            if inherited else str(case.parent))
    env["NO_COLOR"] = "1"
    result = proc.run([xray, "check", case], env=env, timeout=timeout)
    output = result.combined_text()
    return CasePositions(
        case=case,
        rows=[(case.name, int(m.group("line")), int(m.group("col")))
              for m in CHECK_LINE_RE.finditer(output)],
        timed_out=result.timed_out,
        output=output)


def main(argv: list[str]) -> int:
    listing = "--list" in argv
    xray = Path(os.environ.get("XRAY")
                or os.environ.get("XRAY_BIN")
                or str(PROJECT_DIR / "build" / platform.exe_name("xray")))
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"Error: xray not found at {xray}")
        return 1

    cases = collect_cases()
    jobs = platform.env_int("XRAY_TEST_JOBS", platform.cpu_count())
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 30)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        outcomes = list(pool.map(
            lambda c: check_one(xray, c, timeout), cases))
    emitted = [row for outcome in outcomes for row in outcome.rows]
    timed_out = [outcome for outcome in outcomes if outcome.timed_out]

    no_line = [row for row in emitted if row[1] < 1]
    no_column = [row for row in emitted if row[1] >= 1 and row[2] < 1]
    degenerate = {row[0] for row in no_line + no_column}

    print("Diagnostic position census (raw spans, via `xray check`)")
    print("========================================")
    print(f"cases:              {len(cases)}")
    print(f"diagnostics:        {len(emitted)}")
    print(f"complete positions: {len(emitted) - len(no_line) - len(no_column)}")
    print(f"line but no column: {len(no_column)}  (budget {NO_COLUMN_BUDGET})")
    print(f"no line at all:     {len(no_line)}  (budget {NO_LINE_BUDGET})")
    print(f"cases affected:     {len(degenerate)}")
    print(f"cases timed out:    {len(timed_out)}")

    if timed_out:
        print("")
        for outcome in timed_out:
            relative = outcome.case.relative_to(SCRIPT_DIR)
            seconds = "unbounded" if timeout is None else f"{timeout:g} seconds"
            print(f"FAIL: {relative} timed out after {seconds}; "
                  "process tree was terminated")
            partial = outcome.output.strip()
            if partial:
                print("Partial output:")
                print(partial[-2000:])
        return 1

    if listing:
        print("")
        for name, count in sorted(Counter(
                row[0] for row in no_line + no_column).items()):
            print(f"  {name}: {count}")

    over: list[str] = []
    if len(no_column) > NO_COLUMN_BUDGET:
        over.append(f"diagnostics with no column rose to {len(no_column)}, "
                    f"budget is {NO_COLUMN_BUDGET}")
    if len(no_line) > NO_LINE_BUDGET:
        over.append(f"diagnostics with no line rose to {len(no_line)}, "
                    f"budget is {NO_LINE_BUDGET}")
    if over:
        print("")
        for line in over:
            print(f"FAIL: {line}")
        print("A new emission point is dropping its span. Pass the source "
              "location through instead of raising the budget.")
        print("Run with --list to see which cases changed.")
        return 1

    slack = ((NO_COLUMN_BUDGET - len(no_column))
             + (NO_LINE_BUDGET - len(no_line)))
    if slack:
        print("")
        print(f"PASS, but {slack} diagnostic(s) below budget: positions were "
              "fixed and the budget should be lowered to match.")
        return 0

    print("")
    print("PASS: no new position losses")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
