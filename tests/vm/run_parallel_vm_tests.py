#!/usr/bin/env python3
"""Parallel VM lane semantics: 29 cases, each an exact observable result.

Every case here runs one `.xr` program under `xray run --workers N` and pins
down what the parallel VM is allowed to look like from the outside. Lane bugs
are not crashes; they are a line that appears twice, a line that never appears,
a cleanup that ran on the wrong thread, or a panic that arrived rewritten. So
each case asserts the whole stdout, not a substring, and treats any stderr at
all as a failure.

What the corpus covers:

  - Lane identity and fallback: `multilane_threadlocal` proves each lane owns
    its own thread-local state; `deterministic_single_lane` proves
    XRAY_CORO_DETERMINISTIC collapses the schedule to one lane; the
    `small_range_single_lane` and `plan_fallback_single_lane` cases prove a
    workload too small to split stays on one lane instead of paying for
    dispatch it cannot amortize.
  - Callback effect safety: a callback body may call a nested function, hold a
    const function value, use the channel try-operations, or make an ordinary
    stdlib call without any of that being reclassified as a lane effect.
  - Plan batching: `for_each`, `map`, `reduce`, and the aggregate and
    reference-accumulator forms of reduce all have to produce the same result
    batched across lanes as they would sequentially.
  - Teardown under failure: cleanup after a panic, cleanup after a plain and a
    typed error, close-during-dispatch, and close-after-panic for the plan,
    for_each, map, and reduce-combine paths. These are where a lane runtime
    leaks or double-frees, and the expected output is the cleanup trace.
  - Panic fidelity: `lane_panic_propagates_original` requires the payload that
    crosses the lane boundary to be the one that was thrown.
  - Scaling: `plan_vm_scaling_baseline` at 8 workers requires all eight lanes to
    report, the case a missing or duplicated line would betray.

Ported from run_parallel_vm_tests.sh, which was never registered anywhere and
so had never run. Two details of that port are load-bearing:

  - The shell compared `"$(cat "$out")"` against the expected string. Command
    substitution strips *trailing* newlines, so the assertion was really
    "stdout with its trailing newlines removed". `_matches` reproduces exactly
    that and nothing more: no leading strip, no whitespace folding, no CR
    removal, no `[sysmon]` filtering. A difference anywhere else in the byte
    stream is a failure.
  - The assertion was three-part -- exit status 0, that exact stdout, and an
    empty stderr -- and all three are kept. A warn-only diagnostic on stderr is
    still a failure here; that is deliberate, because these cases are supposed
    to be quiet.

Parallelism defaults to a quarter of the host CPUs rather than all of them:
each case is itself a parallel VM asking for up to 8 worker threads, so the
real thread count is roughly jobs x workers. Running one case per core would
oversubscribe the machine several times over, and these cases have genuine
timing behaviour -- lane dispatch decisions, scheduler watchdogs -- that a
thrashing host can perturb. Results are sorted by name before reporting so the
output is byte-for-byte identical regardless of completion order.

Usage: run_parallel_vm_tests.py [xray] [--jobs N] [--list] [-R REGEX]
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import sys
import textwrap
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import platform, proc, ratchet  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
BASELINE_FILE = Path(__file__).resolve().parent / "parallel_vm_known_failures.txt"

MAX_REPORTED_LINES = 40


def _expect(block: str) -> str:
    """Expected stdout from an indented triple-quoted block.

    The literal opens with a newline and carries the source indentation, so the
    block is dedented and the leading and trailing newlines are dropped. The
    result is the expected stdout *without* a trailing newline, which is the
    form `_matches` compares against. No case here expects a blank first or
    last line, so stripping those newlines cannot lose an expectation.
    """
    return textwrap.dedent(block).strip("\n")


@dataclass(frozen=True)
class Case:
    """One `.xr` program, the worker count to run it with, and its whole stdout."""

    name: str
    workers: int
    expected: str
    env: dict = field(default_factory=dict)

    @property
    def source(self) -> Path:
        return SCRIPT_DIR / f"{self.name}.xr"


# The 29 cases, in the order the shell script ran them. Each name is also the
# `.xr` file stem, so the table doubles as the inventory of what is covered:
# worker counts are 1 (2 cases), 2 (14), 4 (12), and 8 (1).
CASES: list[Case] = [
    Case(
        name="parallel_vm_multilane_threadlocal",
        workers=4,
        expected=_expect("""
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_shadowing_no_intrinsic",
        workers=1,
        expected=_expect("""
            3
        """),
    ),
    Case(
        name="parallel_vm_deterministic_single_lane",
        workers=4,
        expected=_expect("""
            true
            1
        """),
        env={"XRAY_CORO_DETERMINISTIC": "1"},
    ),
    Case(
        name="parallel_vm_small_range_single_lane",
        workers=4,
        expected=_expect("""
            1
            1
        """),
    ),
    Case(
        name="parallel_callback_nested_function_effect_not_lane",
        workers=2,
        expected=_expect("""
            6
        """),
    ),
    Case(
        name="parallel_callback_const_function_value_safe",
        workers=2,
        expected=_expect("""
            10
        """),
    ),
    Case(
        name="parallel_callback_channel_try_ops_safe",
        workers=2,
        expected=_expect("""
            4
        """),
    ),
    Case(
        name="parallel_callback_stdlib_normal_call_safe",
        workers=2,
        expected=_expect("""
            3
        """),
    ),
    Case(
        name="parallel_plan_for_each_vm_batch",
        workers=4,
        expected=_expect("""
            true
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_map_vm_batch",
        workers=4,
        expected=_expect("""
            true
            true
            true
            true
            true
            true
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_reduce_vm_batch",
        workers=4,
        expected=_expect("""
            true
            true
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_reduce_aggregate_vm_batch",
        workers=4,
        expected=_expect("""
            true
            true
            true
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_reduce_reference_accumulator",
        workers=2,
        expected=_expect("""
            40
            40
        """),
    ),
    Case(
        name="parallel_reduce_generic_failure_cleanup",
        workers=4,
        expected=_expect("""
            true
            true
            true
            1000
        """),
    ),
    Case(
        name="parallel_plan_reduce_generic_failure_cleanup",
        workers=4,
        expected=_expect("""
            true
            true
            true
            1000
            4
        """),
    ),
    Case(
        name="parallel_plan_reduce_combine_cleanup_after_panic",
        workers=4,
        expected=_expect("""
            caught
            true
        """),
    ),
    Case(
        name="parallel_plan_reduce_combine_close_after_panic_cleanup",
        workers=4,
        expected=_expect("""
            true
            true
            closed
            true
        """),
    ),
    Case(
        name="parallel_plan_cleanup_after_panic",
        workers=2,
        expected=_expect("""
            caught
            true
        """),
    ),
    Case(
        name="parallel_plan_for_each_close_after_panic_cleanup",
        workers=4,
        expected=_expect("""
            true
            true
            true
            closed
            true
        """),
    ),
    Case(
        name="parallel_plan_map_cleanup_after_panic",
        workers=2,
        expected=_expect("""
            true
            true
            true
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_map_close_after_panic_cleanup",
        workers=2,
        expected=_expect("""
            true
            true
            true
            map-closed
            true
            true
            true
            mapinto-closed
            true
        """),
    ),
    Case(
        name="parallel_plan_map_close_during_dispatch",
        workers=2,
        expected=_expect("""
            true
            map-closed
            true
            true
            mapinto-closed
            done
        """),
    ),
    Case(
        name="parallel_plan_close_during_dispatch",
        workers=2,
        expected=_expect("""
            true
            closed
            done
        """),
    ),
    Case(
        name="parallel_plan_close_after_error_cleanup",
        workers=2,
        expected=_expect("""
            true
            true
            closed
            true
        """),
    ),
    Case(
        name="parallel_plan_close_after_typed_error_cleanup",
        workers=2,
        expected=_expect("""
            closed-typed
            true
            closed
            true
        """),
    ),
    Case(
        name="parallel_lane_panic_propagates_original",
        workers=2,
        expected=_expect("""
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_nested_dispatch_gate",
        workers=2,
        expected=_expect("""
            seq-concurrent
            true
            vm-concurrent
            true
        """),
    ),
    Case(
        name="parallel_plan_vm_scaling_baseline",
        workers=8,
        expected=_expect("""
            true
            true
            true
            true
            true
            true
            true
            true
        """),
    ),
    Case(
        name="parallel_plan_fallback_single_lane",
        workers=1,
        expected=_expect("""
            0
            128
            0
            128
            0
        """),
    ),
]


@dataclass
class Result:
    """What one case did: the verdict plus everything a failure report needs."""

    case: Case
    passed: bool
    stdout: str
    stderr: str
    returncode: int
    timed_out: bool


def _matches(stdout: str, expected: str) -> bool:
    """Exact stdout comparison, minus trailing newlines only.

    The shell original compared `"$(cat out)"`, and command substitution eats
    trailing newlines, so an expected value never carried one. Dropping them
    here keeps that contract; everything else -- leading whitespace, interior
    blank lines, CR, trailing spaces on a line -- still has to match byte for
    byte.
    """
    return stdout.rstrip("\n") == expected


def run_case(xray: Path, case: Case, timeout: float | None) -> Result:
    """Run one case and judge it on exit status, exact stdout, and empty stderr."""
    env = dict(os.environ)
    env.update(case.env)
    result = proc.run(
        [xray, "run", "--workers", str(case.workers), case.source],
        env=env,
        timeout=timeout,
    )
    stdout = result.stdout.decode("utf-8", "replace")
    stderr = result.stderr.decode("utf-8", "replace")
    passed = result.ok and _matches(stdout, case.expected) and not result.stderr
    return Result(
        case=case,
        passed=passed,
        stdout=stdout,
        stderr=stderr,
        returncode=result.returncode,
        timed_out=result.timed_out,
    )


def report_failure(item: Result) -> None:
    """Print why one case failed: status, a stdout diff, and the stderr text."""
    case = item.case
    print(f"  FAIL: {case.name} (--workers {case.workers})")
    if case.env:
        settings = " ".join(f"{k}={v}" for k, v in sorted(case.env.items()))
        print(f"      env: {settings}")
    if item.timed_out:
        print("      status: timed out")
    elif item.returncode != 0:
        print(f"      status: exit {item.returncode}")

    actual = item.stdout.rstrip("\n")
    if actual != case.expected:
        diff = list(difflib.unified_diff(
            case.expected.splitlines(),
            actual.splitlines(),
            fromfile="expected",
            tofile="actual",
            lineterm="",
        ))
        if not diff:
            # The two differ only in characters splitlines() hides -- a CR, a
            # trailing space. Show the raw values instead of an empty diff.
            print(f"      expected: {case.expected!r}")
            print(f"      actual:   {actual!r}")
        for line in diff[:MAX_REPORTED_LINES]:
            print(f"      {line}")
    if item.stderr:
        for line in item.stderr.splitlines()[:MAX_REPORTED_LINES]:
            print(f"      stderr: {line}")


def print_listing(cases: list[Case]) -> None:
    """List the selected cases, their worker counts, and any env overrides."""
    for case in cases:
        settings = " ".join(f"{k}={v}" for k, v in sorted(case.env.items()))
        suffix = f"  env: {settings}" if settings else ""
        print(f"  {case.name}  --workers {case.workers}{suffix}")
    print("")
    print(f"{len(cases)} cases")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("xray", nargs="?", default=None,
                        help="xray binary (default: $XRAY_BIN, then build/xray)")
    parser.add_argument("--jobs", type=int, default=None,
                        help="cases to run concurrently (default: CPU count / 4)")
    parser.add_argument("--list", action="store_true",
                        help="list the cases and exit without running them")
    parser.add_argument("-R", dest="regex", default=None,
                        help="only run cases whose name matches this regex")
    parser.add_argument("--baseline", type=Path, default=BASELINE_FILE,
                        help=f"only-shrink ratchet baseline "
                             f"(default: {BASELINE_FILE.name})")
    parser.add_argument("--no-baseline", action="store_true",
                        help="judge every case directly, ignoring the "
                             "baseline. For finding out what is actually red.")
    return parser.parse_args(argv[1:])


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    cases = CASES
    if args.regex is not None:
        try:
            pattern = re.compile(args.regex)
        except re.error as exc:
            sys.stderr.write(f"FAIL: bad -R regex: {exc}\n")
            return 1
        cases = [case for case in CASES if pattern.search(case.name)]
        if not cases:
            # An empty selection is a mistyped filter, not a green run.
            sys.stderr.write(f"FAIL: no case matches -R {args.regex}\n")
            return 1

    if args.list:
        print_listing(cases)
        return 0

    xray = Path(args.xray if args.xray is not None
                else os.environ.get("XRAY_BIN", str(PROJECT_DIR / "build" / "xray")))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)
    jobs = args.jobs if args.jobs is not None else max(1, platform.cpu_count() // 4)
    if jobs < 1:
        sys.stderr.write(f"FAIL: --jobs must be at least 1, got {jobs}\n")
        return 1

    print("=== Parallel VM Lane Tests ===")
    print(f"Binary: {xray}")
    print(f"Cases: {len(cases)}  jobs: {jobs}")
    print("")

    if not (xray.is_file() and os.access(xray, os.X_OK)):
        sys.stderr.write(f"FAIL: xray binary not executable: {xray}\n")
        return 1

    # A renamed or deleted `.xr` would otherwise surface as a wall of identical
    # compile failures; name the missing sources instead.
    missing = [case.name for case in cases if not case.source.is_file()]
    if missing:
        sys.stderr.write("FAIL: missing case sources:\n")
        for name in missing:
            sys.stderr.write(f"  {name}.xr\n")
        return 1

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda c: run_case(xray, c, timeout), cases))
    # Sorted by name so the report is byte-for-byte deterministic regardless of
    # completion order.
    results.sort(key=lambda r: r.case.name)

    passed = 0
    for item in results:
        if item.passed:
            print(f"  PASS: {item.case.name}")
            passed += 1
        else:
            report_failure(item)

    failed = len(results) - passed
    print("")
    print(f"parallel VM: {passed} passed, {failed} failed ({len(results)} cases)")

    if args.no_baseline:
        return 1 if failed else 0

    # Only-shrink ratchet, the same policy every other suite here uses. Cases
    # `-R` filtered out are `skipped`, not `passing`: they produced no verdict,
    # so a baselined entry among them must not be reported as a line to delete.
    ran = {item.case.name for item in results}
    verdict = ratchet.evaluate(
        failed={item.case.name for item in results if not item.passed},
        baseline=ratchet.read_baseline(args.baseline),
        skipped={case.name for case in CASES} - ran,
    )
    if not verdict.ok:
        print("")
        print(ratchet.format_report(
            verdict, baseline_path=str(args.baseline.name)))
        return 1
    print(f"ratchet: {verdict.failing_count} failing, all "
          f"{verdict.baseline_count} baselined.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
