#!/usr/bin/env python3
"""Measure Task-218 CGen verifier cost inside a real AOT compilation.

The verifier remains always-on.  XRAY_CGEN_VERIFY_TIMING only reports the CPU
time already spent in each verification call; it cannot disable or alter the
check.  This script compares the summed verifier time with end-to-end compiler
wall time and fails when the median share reaches one percent.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


TIMING_RE = re.compile(r"^\[cgen-verify\] cpu_us=(\d+) bytes=(\d+) tu=(.*)$", re.MULTILINE)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, default=root / "build" / "xray")
    parser.add_argument(
        "--main",
        type=Path,
        default=root.parent.parent / "xray-ports" / "ports" / "xxhash" / "src" / "main.xr",
    )
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--max-percent", type=float, default=1.0)
    args = parser.parse_args()

    xray = args.xray.resolve()
    main_file = args.main.resolve()
    if not xray.is_file():
        parser.error(f"xray binary not found: {xray}")
    if not main_file.is_file():
        parser.error(f"real workload not found: {main_file}")
    if args.samples < 3:
        parser.error("--samples must be at least 3")

    shares: list[float] = []
    walls_ms: list[float] = []
    verifier_ms: list[float] = []
    env = os.environ.copy()
    env["XRAY_CGEN_VERIFY_TIMING"] = "1"

    with tempfile.TemporaryDirectory(prefix="xray-cgen-verify-") as tmp:
        for index in range(args.samples):
            output = Path(tmp) / f"sample-{index}.c"
            started = time.perf_counter()
            run = subprocess.run(
                [str(xray), "build", str(main_file), "--native", "--c-only", "-o", str(output)],
                cwd=main_file.parent.parent,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            wall_us = (time.perf_counter() - started) * 1_000_000.0
            if run.returncode != 0:
                print(run.stdout, end="")
                print(run.stderr, end="")
                return run.returncode
            timings = TIMING_RE.findall(run.stderr)
            if not timings:
                raise SystemExit("no [cgen-verify] timing records were emitted")
            verify_us = float(sum(int(row[0]) for row in timings))
            share = 100.0 * verify_us / wall_us
            walls_ms.append(wall_us / 1000.0)
            verifier_ms.append(verify_us / 1000.0)
            shares.append(share)
            print(
                f"sample {index + 1}: wall={wall_us / 1000.0:.2f}ms "
                f"verify={verify_us / 1000.0:.2f}ms share={share:.3f}% tus={len(timings)}"
            )

    median_share = statistics.median(shares)
    print(
        f"median: wall={statistics.median(walls_ms):.2f}ms "
        f"verify={statistics.median(verifier_ms):.2f}ms share={median_share:.3f}% "
        f"limit={args.max_percent:.3f}%"
    )
    if median_share >= args.max_percent:
        print("FAIL: CGen verifier overhead budget exceeded")
        return 1
    print("PASS: CGen verifier overhead is below budget")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
