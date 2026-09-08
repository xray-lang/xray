#!/usr/bin/env python3
"""Report historical test scheduling costs and recorded build durations.

Two questions decide how long an edit-test cycle takes, and neither is answered
by a pass/fail summary:

  1. Which test lanes dominate?  Cost is never spread evenly -- a handful of
     lanes that rebuild a compiler or drive an external C toolchain decide the
     wall time, and every fast test scheduled beside one gets charged for the
     starvation.
  2. Which build steps cannot be parallelized?  A single long custom command
     sets a floor that no -j value can lower, so it stays invisible in a
     "total build time" number.

CTestCostData.txt holds scheduling estimates, not the last run's test times.
Only entries with previous runs are included; declared COST entries with zero
runs are not measurements. Ninja's .ninja_log retains the latest recorded
duration of each output, potentially from different builds. It has no invocation
identity, and compaction can reorder outputs. These files cannot establish the
latest invocation, its wall time, or its critical path. This report reads their
historical evidence without running tests or builds.

Usage:
    python3 scripts/test_profile.py                  # build/ tests + build
    python3 scripts/test_profile.py --build-dir build-asan
    python3 scripts/test_profile.py --top 30

Works on Windows and POSIX: it only reads text files.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path


def read_test_costs(build_dir: Path) -> list[tuple[str, float]]:
    """Historical scheduling costs with previous runs, not current wall times."""
    path = build_dir / "Testing" / "Temporary" / "CTestCostData.txt"
    if not path.is_file():
        return []
    costs: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip() == "---":
            break  # the remainder lists failed test names, not costs
        parts = line.split()
        if len(parts) != 3:
            continue
        try:
            runs = int(parts[1])
            cost = float(parts[2])
        except ValueError:
            continue
        if runs < 0 or not math.isfinite(cost) or cost < 0:
            continue
        if runs == 0:
            costs.pop(parts[0], None)
        else:
            costs[parts[0]] = cost
    return sorted(costs.items(), key=lambda row: (-row[1], row[0]))


def read_build_edges(build_dir: Path) -> list[tuple[str, float]]:
    """Last recorded duration per exact Ninja output key, across build history.

    Rows are '<start_ms> <end_ms> <mtime> <out> <hash>'. Ninja overwrites the
    record for the same output while loading its log and may recompact the
    resulting map in a different order. Cross-output row order, timestamps and
    command hashes therefore cannot identify an invocation or a unique edge.
    Preserve output spelling, including directories and absolute/relative keys.
    """
    path = build_dir / ".ninja_log"
    if not path.is_file():
        return []
    latest: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8").splitlines(keepends=True):
        if not line.endswith("\n"):
            continue  # a concurrent writer may not have finished the last row
        if line.startswith("#"):
            continue
        parts = line.rstrip("\r\n").split("\t")
        if len(parts) != 5 or not parts[3]:
            continue
        try:
            start = int(parts[0])
            end = int(parts[1])
            int(parts[2])
            int(parts[4], 16)
            seconds = (end - start) / 1000.0
        except (ValueError, OverflowError):
            continue
        if start < 0 or end < start or not math.isfinite(seconds):
            continue
        latest[parts[3]] = seconds
    return sorted(latest.items(), key=lambda row: (-row[1], row[0]))


def bar(fraction: float, width: int = 28) -> str:
    filled = int(round(fraction * width))
    return "#" * filled + "." * (width - filled)


def report_tests(rows: list[tuple[str, float]], top: int) -> None:
    if not rows:
        print("no recorded CTest costs with previous runs (missing, empty, or unrun data)\n")
        return
    total = sum(c for _, c in rows)
    print(f"TEST COST HISTORY  ({len(rows)} tests with previous runs)")
    print("  Scheduling estimates, not the latest run's measured wall times.")
    print("  Zero-run declarations are excluded.")
    print("-" * 72)
    shown = rows[:top]
    for name, cost in shown:
        if cost < 0.05:
            break
        print(f"  {cost:8.1f}s  {bar(cost / rows[0][1])}  {name}")
    head = sum(c for _, c in rows[:5])
    if total > 0:
        print(f"\n  top {min(5, len(rows))} lanes = {head:.0f}s = "
              f"{100 * head / total:.0f}% of summed historical cost")
        print(f"  the other {max(0, len(rows) - 5)} tests = {total - head:.0f}s")
    print()


def report_build(rows: list[tuple[str, float]], top: int) -> None:
    if not rows:
        print("no recorded Ninja durations (missing, empty, or incomplete log)\n")
        return
    print(f"BUILD OUTPUT HISTORY  ({len(rows)} output keys)")
    print("  Latest recorded duration per output, not the latest invocation.")
    print("  Multiple outputs may repeat one command's time; rows are not unique edges.")
    print("-" * 72)
    for name, cost in rows[:top]:
        if cost < 0.2:
            break
        print(f"  {cost:8.1f}s  {bar(cost / rows[0][1])}  {name}")
    slowest = rows[0][1] if rows else 0.0
    print(f"\n  longest recorded output duration = {slowest:.1f}s")
    print("  No invocation boundary or current critical path is inferred.\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build", help="build directory (default: build)")
    ap.add_argument("--top", type=int, default=20, help="rows per section (default: 20)")
    ap.add_argument("--tests-only", action="store_true")
    ap.add_argument("--build-only", action="store_true")
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_dir():
        print(f"no such build directory: {build_dir}", file=sys.stderr)
        return 1

    print()
    if not args.build_only:
        report_tests(read_test_costs(build_dir), args.top)
    if not args.tests_only:
        report_build(read_build_edges(build_dir), args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
