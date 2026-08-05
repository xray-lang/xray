#!/usr/bin/env python3
"""Cases that must be rejected by the COMPILER, not merely exit non-zero.

A non-zero exit alone does not count: a program that compiles and then panics
also exits non-zero. A recognisable compiler diagnostic must appear in the
output, which is what separates "rejected" from "crashed".

Sibling files:
  <case>.xr.expected          every non-empty line must appear in the output.
                              Pins the error code and wording.
  <case>.xr.expected-runtime  same matching, but declares that the compiler
                              does NOT reject this case and the program only
                              traps at run time. Reported separately as a
                              compile-time coverage gap, never as a pass.
                              Rename to .expected once the diagnostic moves to
                              compile time.

Writing a good case: make the missing diagnostic the ONLY defect, so the
program would otherwise compile and run to completion. If the body also blows
up at run time for an unrelated reason, the case can look "rejected" for the
wrong reason.

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
from typing import List, Optional, Tuple


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import diagnostics, platform, proc  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent

RED = "\033[0;31m"
GREEN = "\033[0;32m"
YELLOW = "\033[0;33m"
NC = "\033[0m"

# Accepted compiler-diagnostic shapes: `error[E0123]:` / `error:` from the
# analyzer, `Error:` from module resolution, and the `[xcompiler] ... failed at
# <stage>:` line from the Xi pipeline. A bare runtime panic matches none of
# these.
XR_DIAG_RE = re.compile(
    r"(^|[^A-Za-z])([Ee]rror(\[E[0-9]+\])?:|\[xcompiler\].*failed at )")

PASS, FAIL, RUNTIME = "PASS", "FAIL", "RUNTIME"


@dataclass
class Result:
    category: str
    filename: str
    verdict: str
    report: str


def expected_for(case: Path) -> Tuple[Path, str]:
    """The sibling that governs this case, and whether it is a coverage gap."""
    runtime_sibling = Path(str(case) + ".expected-runtime")
    if runtime_sibling.is_file():
        return runtime_sibling, "runtime"
    return Path(str(case) + ".expected"), "compile"


def run_one_case(xray: Path, case: Path, timeout: "float | None") -> Result:
    expected_file, kind = expected_for(case)

    env = dict(os.environ)
    inherited = os.environ.get("XRAY_TYPEPATH", "")
    env["XRAY_TYPEPATH"] = (f"{case.parent}{os.pathsep}{inherited}"
                            if inherited else str(case.parent))
    result = proc.run([xray, case], env=env, timeout=timeout)
    # xray emits plain, un-coloured diagnostics whenever stderr is not a
    # terminal, which is always the case here, so there is nothing to strip.
    # Trailing newlines go, matching the shell's `$(...)` capture: the report
    # adds its own, and keeping them doubled every blank line in the output.
    output = result.combined_text().rstrip("\n")
    missing = diagnostics.missing_lines(
        output, diagnostics.expected_lines(expected_file))

    name = case.name
    if result.returncode == 0:
        verdict, report = FAIL, (
            f"  {RED}✗{NC} {name} - should have failed but succeeded\n")
    elif kind == "compile" and not XR_DIAG_RE.search(output):
        verdict, report = FAIL, (
            f"  {RED}✗{NC} {name} - exited non-zero with no compiler diagnostic "
            "(a run-time panic is not a compile error)\n"
            f"    Output: {output}\n")
    elif missing:
        listed = "".join(f"\n      - {line}" for line in missing)
        verdict, report = FAIL, (
            f"  {RED}✗{NC} {name} - error message missing expected lines:{listed}\n"
            f"    Output: {output}\n")
    elif kind == "runtime":
        verdict, report = RUNTIME, (
            f"  {YELLOW}~{NC} {name} - rejected, but only at run time\n")
    else:
        verdict, report = PASS, f"  {GREEN}✓{NC} {name} - correctly rejected\n"

    return Result(case.parent.name, name, verdict, report)


def collect_cases() -> List[Path]:
    """One directory level of categories, `*.xr` directly inside each."""
    cases: List[Path] = []
    for directory in sorted(p for p in SCRIPT_DIR.iterdir() if p.is_dir()):
        cases.extend(sorted(directory.glob("*.xr")))
    return cases


def main(argv: List[str]) -> int:
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

    passed = failed = runtime_only = 0
    runtime_list: List[str] = []
    previous_category: Optional[str] = None

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

    print("")
    print("========================================")
    print("Compile Error Tests Summary")
    print("========================================")
    print(f"Passed: {GREEN}{passed}{NC}")
    print(f"Failed: {RED}{failed}{NC}")
    if runtime_only:
        print(f"Runtime-only (compile-time coverage gap): "
              f"{YELLOW}{runtime_only}{NC}")
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

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
