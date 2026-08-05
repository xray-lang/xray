#!/usr/bin/env python3
"""Report where test and build wall time actually goes.

Two questions decide how long an edit-test cycle takes, and neither is answered
by a pass/fail summary:

  1. Which test lanes dominate?  Cost is never spread evenly -- a handful of
     lanes that rebuild a compiler or drive an external C toolchain decide the
     wall time, and every fast test scheduled beside one gets charged for the
     starvation.
  2. Which build steps cannot be parallelized?  A single long custom command
     sets a floor that no -j value can lower, so it stays invisible in a
     "total build time" number.

Both answers are already on disk after any run: CTest writes per-test cost to
CTestCostData.txt, and Ninja writes per-edge start/end times to .ninja_log.
This reads them, so it costs nothing to run and needs no instrumentation.

Usage:
    python3 scripts/test_profile.py                  # build/ tests + build
    python3 scripts/test_profile.py --build-dir build-asan
    python3 scripts/test_profile.py --top 30

Works on Windows and POSIX: it only reads text files.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def read_test_costs(build_dir: Path) -> list[tuple[str, float]]:
    """Per-test cost from CTestCostData.txt: '<name> <runs> <seconds>'."""
    path = build_dir / "Testing" / "Temporary" / "CTestCostData.txt"
    if not path.is_file():
        return []
    rows: list[tuple[str, float]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue  # the file ends with a non-cost section
        try:
            rows.append((parts[0], float(parts[2])))
        except ValueError:
            continue
    rows.sort(key=lambda r: r[1], reverse=True)
    return rows


def read_build_edges(build_dir: Path) -> list[tuple[str, float]]:
    """Per-edge duration from .ninja_log: '<start_ms> <end_ms> <mtime> <out> <hash>'.

    The log accumulates across builds, so the same output appears once per
    rebuild; the most recent entry wins. Absolute and relative spellings of one
    output are separate rows in the log but the same edge, so the longer-named
    duplicate is dropped.
    """
    path = build_dir / ".ninja_log"
    if not path.is_file():
        return []
    latest: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        try:
            seconds = (int(parts[1]) - int(parts[0])) / 1000.0
        except ValueError:
            continue
        if seconds <= 0:
            continue
        out = parts[3]
        key = Path(out).name
        # Several outputs share one custom command; they report identical
        # durations. Keep the shortest spelling as the representative.
        prev = latest.get(key)
        if prev is None or seconds > prev:
            latest[key] = seconds
    rows = sorted(latest.items(), key=lambda r: r[1], reverse=True)
    return rows


def bar(fraction: float, width: int = 28) -> str:
    filled = int(round(fraction * width))
    return "#" * filled + "." * (width - filled)


def report_tests(rows: list[tuple[str, float]], top: int) -> None:
    if not rows:
        print("no CTestCostData.txt -- run ctest once in this build dir first\n")
        return
    total = sum(c for _, c in rows)
    print(f"TEST LANES  ({len(rows)} tests, {total:.0f}s if run serially)")
    print("-" * 72)
    shown = rows[:top]
    for name, cost in shown:
        if cost < 0.05:
            break
        print(f"  {cost:8.1f}s  {bar(cost / rows[0][1])}  {name}")
    head = sum(c for _, c in rows[:5])
    if total > 0:
        print(f"\n  top 5 lanes = {head:.0f}s = {100 * head / total:.0f}% of serial cost")
        print(f"  the other {len(rows) - 5} tests = {total - head:.0f}s")
    print()


def report_build(rows: list[tuple[str, float]], top: int) -> None:
    if not rows:
        print("no .ninja_log -- this build dir is not a Ninja tree\n")
        print("  A Makefiles tree cannot report per-edge cost, and CMake's")
        print("  recursive make parallelizes far worse than Ninja. Reconfigure")
        print("  with -G Ninja if you care about build wall time.\n")
        return
    print(f"BUILD STEPS  (slowest edge of each output, {len(rows)} edges recorded)")
    print("-" * 72)
    for name, cost in rows[:top]:
        if cost < 0.2:
            break
        print(f"  {cost:8.1f}s  {bar(cost / rows[0][1])}  {name}")
    slowest = rows[0][1] if rows else 0.0
    print(f"\n  slowest single edge = {slowest:.1f}s")
    print("  A single edge is a floor no -j can lower: while it runs, nothing")
    print("  downstream of it can start. Edges far above the rest are the ones")
    print("  worth splitting or making conditional.\n")


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
