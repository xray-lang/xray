#!/usr/bin/env python3
"""Paired comparison for the direct inherited-stdio process spawn path."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import random
import statistics
import subprocess


def percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int((len(ordered) - 1) * fraction + 0.5)))
    return float(ordered[index])


def run(binary: Path, samples: int) -> list[int]:
    completed = subprocess.run(
        [str(binary), "--samples", str(samples)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{binary} failed with {completed.returncode}: stderr bytes={completed.stderr!r}"
        )
    try:
        values = [int(line) for line in completed.stdout.splitlines()]
    except ValueError as exc:
        raise RuntimeError(f"{binary} emitted non-numeric bytes: {completed.stdout!r}") from exc
    if len(values) != samples or any(value <= 0 for value in values):
        raise RuntimeError(f"{binary} emitted {len(values)} invalid samples")
    return values


def bootstrap_interval(ratios: list[float], seed: int) -> tuple[float, float]:
    rng = random.Random(seed)
    estimates = []
    for _ in range(5000):
        draw = [ratios[rng.randrange(len(ratios))] for _ in ratios]
        estimates.append(statistics.median(draw))
    estimates.sort()
    return estimates[int(len(estimates) * 0.025)], estimates[int(len(estimates) * 0.975)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=9)
    parser.add_argument("--samples", type=int, default=160)
    parser.add_argument("--max-median-percent", type=float, default=1.0)
    parser.add_argument("--max-p95-percent", type=float, default=3.0)
    args = parser.parse_args()
    if args.rounds < 5 or args.samples < 40:
        parser.error("use at least 5 rounds and 40 samples")

    rng = random.Random(257)
    median_ratios: list[float] = []
    p95_ratios: list[float] = []
    baseline_all: list[int] = []
    candidate_all: list[int] = []
    for _ in range(args.rounds):
        order = ["baseline", "candidate"]
        rng.shuffle(order)
        samples: dict[str, list[int]] = {}
        for name in order:
            samples[name] = run(
                args.baseline if name == "baseline" else args.candidate, args.samples
            )
        baseline = samples["baseline"]
        candidate = samples["candidate"]
        baseline_all.extend(baseline)
        candidate_all.extend(candidate)
        median_ratios.append(statistics.median(candidate) / statistics.median(baseline))
        p95_ratios.append(percentile(candidate, 0.95) / percentile(baseline, 0.95))

    median_ratio = statistics.median(median_ratios)
    p95_ratio = statistics.median(p95_ratios)
    median_ci = bootstrap_interval(median_ratios, 25701)
    p95_ci = bootstrap_interval(p95_ratios, 25795)
    result = {
        "schema": 1,
        "rounds": args.rounds,
        "samplesPerRound": args.samples,
        "baselineMedianNs": statistics.median(baseline_all),
        "candidateMedianNs": statistics.median(candidate_all),
        "medianDeltaPercent": (median_ratio - 1.0) * 100.0,
        "medianRatio95Ci": median_ci,
        "baselineP95Ns": percentile(baseline_all, 0.95),
        "candidateP95Ns": percentile(candidate_all, 0.95),
        "p95DeltaPercent": (p95_ratio - 1.0) * 100.0,
        "p95Ratio95Ci": p95_ci,
    }
    print(json.dumps(result, sort_keys=True))
    median_regression = median_ci[0] > 1.0 and median_ratio > 1.0 + args.max_median_percent / 100.0
    p95_regression = p95_ci[0] > 1.0 and p95_ratio > 1.0 + args.max_p95_percent / 100.0
    if median_regression or p95_regression:
        print("process spawn performance gate: FAIL")
        return 1
    print("process spawn performance gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
