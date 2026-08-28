#!/usr/bin/env python3
"""Cases that must be rejected by the COMPILER, at the RIGHT PLACE.

A non-zero exit alone does not count: a program that compiles and then panics
also exits non-zero. Neither does a diagnostic with the right words in it: one
pointing at the wrong line, the wrong column, or the wrong file used to satisfy
every case in this suite. Each expected file therefore pins a position, and the
runner matches assertions against individual emitted diagnostics rather than
against the run's combined text. See `expected_format.py` for the grammar.

Sibling files:
  <case>.xr.expected          the assertions this case requires.
  <case>.xr.expected-runtime  same grammar, but declares that the compiler does
                              NOT reject this case and the program only traps at
                              run time. Reported separately as a compile-time
                              coverage gap, never as a pass. Rename to
                              .expected once the diagnostic moves to compile
                              time.

Writing a good case: make the missing diagnostic the ONLY defect, so the
program would otherwise compile and run to completion. If the body also blows
up at run time for an unrelated reason, the case can look "rejected" for the
wrong reason.

A diagnostic that lands somewhere other than where it should is a compiler
defect this suite cannot fix -- `src/` is out of its reach. Such a case pins
the position the compiler currently reports and adds a `misplaced` line naming
the position it should report; the run then reports it as a diagnostic-position
gap instead of a pass, the same way `.expected-runtime` reports a compile-time
gap. Fixing the compiler makes the gate demand the corrected position.

Cases are independent, so they run concurrently. Results are collected and
reported in sorted (category, filename) order, so the report and the tallies
are byte-for-byte deterministic regardless of completion order.

Environment:
    XRAY / XRAY_BIN   the xray binary
    XRAY_TEST_JOBS    parallelism (default: number of CPUs)

Usage: run_compile_error_tests.py
"""

from __future__ import annotations

import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)


_bootstrap()
from expected_format import (  # noqa: E402
    ExpectedFormatError, expected_path_for, match_assertions, parse_diagnostics,
    parse_expected)
from xraytest import platform, proc  # noqa: E402

platform.configure_utf8_stdio()

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

RED = "\033[0;31m"
GREEN = "\033[0;32m"
YELLOW = "\033[0;33m"
BLUE = "\033[0;34m"
NC = "\033[0m"

# Accepted compiler-diagnostic shapes: `error[E0123]:` / `error:` from the
# analyzer, `Error:` from module resolution, and the `[xcompiler] ... failed at
# <stage>:` line from the Xi pipeline. A bare runtime panic matches none of
# these.
XR_DIAG_RE = re.compile(
    r"(^|[^A-Za-z])([Ee]rror(\[E[0-9]+\])?:|\[xcompiler\].*failed at )")

PASS, FAIL, RUNTIME, MISPLACED = "PASS", "FAIL", "RUNTIME", "MISPLACED"


@dataclass
class Result:
    category: str
    filename: str
    verdict: str
    report: str


def run_one_case(xray: Path, case: Path, timeout: float | None) -> Result:
    expected_file, kind = expected_path_for(case)
    category, name = case.parent.name, case.name

    env = dict(os.environ)
    inherited = os.environ.get("XRAY_TYPEPATH", "")
    env["XRAY_TYPEPATH"] = (f"{case.parent}{os.pathsep}{inherited}"
                            if inherited else str(case.parent))
    env["NO_COLOR"] = "1"
    result = proc.run([xray, case], env=env, timeout=timeout)
    # xray emits plain, un-coloured diagnostics whenever stderr is not a
    # terminal, which is always the case here, so there is nothing to strip.
    # Trailing newlines go, matching the shell's `$(...)` capture: the report
    # adds its own, and keeping them doubled every blank line in the output.
    output = result.combined_text().rstrip("\n")

    try:
        assertions = parse_expected(expected_file)
    except ExpectedFormatError as bad:
        return Result(category, name, FAIL, f"  {RED}✗{NC} {name} - {bad}\n")

    if result.returncode == 0:
        return Result(category, name, FAIL,
                      f"  {RED}✗{NC} {name} - should have failed but succeeded\n")
    if kind == "compile" and not XR_DIAG_RE.search(output):
        return Result(category, name, FAIL, (
            f"  {RED}✗{NC} {name} - exited non-zero with no compiler diagnostic "
            "(a run-time panic is not a compile error)\n"
            f"    Output: {output}\n"))

    failures = match_assertions(assertions, parse_diagnostics(output), name)
    if failures:
        listed = "".join(f"\n      - {line}" for line in failures)
        return Result(category, name, FAIL, (
            f"  {RED}✗{NC} {name} - diagnostics do not match the expected "
            f"assertions:{listed}\n"
            f"    Output: {output}\n"))

    if kind == "runtime":
        return Result(category, name, RUNTIME,
                      f"  {YELLOW}~{NC} {name} - rejected, but only at run time\n")

    gaps = [f"points at {a.position_text()}, should point at {a.misplaced_text()}"
            for a in assertions if a.misplaced]
    gaps += [a.where.gap_reason() for a in assertions if a.where.degenerate]
    if gaps:
        listed = "".join(f"\n      - {gap}" for gap in gaps)
        return Result(category, name, MISPLACED, (
            f"  {BLUE}?{NC} {name} - rejected, but the diagnostic is "
            f"misplaced:{listed}\n"))

    return Result(category, name, PASS,
                  f"  {GREEN}✓{NC} {name} - correctly rejected\n")


def collect_cases() -> list[Path]:
    """One directory level of categories, `*.xr` directly inside each."""
    cases: list[Path] = []
    for directory in sorted(p for p in SCRIPT_DIR.iterdir() if p.is_dir()):
        cases.extend(sorted(directory.glob("*.xr")))
    return cases


def main(argv: list[str]) -> int:
    xray = Path(os.environ.get("XRAY")
                or os.environ.get("XRAY_BIN")
                or str(PROJECT_DIR / "build" / platform.exe_name("xray")))
    if not (xray.is_file() and os.access(xray, os.X_OK)):
        print(f"Error: xray not found at {xray}")
        print("Build xray first or set XRAY environment variable")
        return 1

    jobs = platform.env_int("XRAY_TEST_JOBS", platform.cpu_count())
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    cases = collect_cases()
    if not cases:
        print(f"No compile-error cases found under {SCRIPT_DIR}")
        return 1

    print(f"Running compile error tests... ({jobs} parallel)")
    print("========================================")

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda c: run_one_case(xray, c, timeout), cases))
    # Sorted by (category, filename), matching the shell's sorted result walk,
    # so the report is identical however the cases actually finished.
    results.sort(key=lambda r: (r.category, r.filename))

    passed = failed = runtime_only = misplaced_only = 0
    runtime_list: list[str] = []
    misplaced_list: list[str] = []
    previous_category: str | None = None

    for item in results:
        if item.category != previous_category:
            print("")
            print(f"Category: {item.category}")
            print("----------------------------------------")
            previous_category = item.category
        sys.stdout.write(item.report)

        if item.verdict == PASS:
            passed += 1
        elif item.verdict == FAIL:
            failed += 1
        elif item.verdict == RUNTIME:
            runtime_only += 1
            runtime_list.append(f"  - {item.category}/{item.filename}")
        elif item.verdict == MISPLACED:
            misplaced_only += 1
            misplaced_list.append(f"  - {item.category}/{item.filename}")

    print("")
    print("========================================")
    print("Compile Error Tests Summary")
    print("========================================")
    print(f"Passed: {GREEN}{passed}{NC}")
    print(f"Failed: {RED}{failed}{NC}")
    if runtime_only:
        print(f"Runtime-only (compile-time coverage gap): "
              f"{YELLOW}{runtime_only}{NC}")
    if misplaced_only:
        print(f"Misplaced diagnostic (position gap): {BLUE}{misplaced_only}{NC}")
    print(f"Total:  {len(results)}")
    print("========================================")
    if runtime_only:
        print("")
        print("These cases are rejected only once the program runs; the compiler")
        print("accepts them. Each carries a .expected-runtime sibling saying so.")
        print("Rename it to .expected when the diagnostic moves to compile time:")
        print("")
        for line in runtime_list:
            print(line)
    if misplaced_only:
        print("")
        print("These cases are rejected at compile time, but the diagnostic points")
        print("somewhere other than the defect. Each expected file carries a")
        print("`misplaced` line naming where it should point. Fix the compiler,")
        print("then delete that line so the gate demands the right position:")
        print("")
        for line in misplaced_list:
            print(line)

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
