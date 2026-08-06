#!/usr/bin/env python3
"""Fail closed on drift in committed task-262 sort performance evidence."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tomllib


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    budget = tomllib.loads(
        (ROOT / "contracts" / "semantic-performance-budget.toml").read_text(encoding="utf-8")
    )
    evidence = json.loads(
        (ROOT / "contracts" / "semantic-performance-baseline.json").read_text(encoding="utf-8")
    )
    runtime = json.loads(
        (ROOT / "contracts" / "semantic-runtime-benchmark.json").read_text(encoding="utf-8")
    )
    errors: list[str] = []
    if evidence.get("schema") != 1 or evidence.get("task") != 262:
        errors.append("semantic performance evidence schema/task mismatch")
    profiles = {row.get("opt"): row for row in evidence.get("profiles", [])}
    for opt in (0, 2):
        row = profiles.get(opt)
        limits = budget.get(f"o{opt}", {})
        if not row:
            errors.append(f"missing O{opt} evidence")
            continue
        for metric in (
            "generated_c_bytes", "object_bytes", "binary_bytes", "codegen_ns",
            "object_compile_ns", "native_build_ns", "runtime_p99_ns",
        ):
            value = row.get(metric)
            limit = limits.get("max_" + metric)
            if not isinstance(value, int) or value <= 0:
                errors.append(f"O{opt} {metric} is not positive integer evidence")
            elif not isinstance(limit, int) or value > limit:
                errors.append(f"O{opt} {metric}={value} exceeds budget {limit}")
        if row.get("canonical_sort_kernel_mentions", 0) <= 0:
            errors.append(f"O{opt} generated C lost the canonical sort kernel")
        if row.get("retired_private_sort_mentions") != 0:
            errors.append(f"O{opt} generated C revived a private sort kernel")
    benches = runtime.get("benchmarks", [])
    match = next((row for row in benches if row.get("name") == "runtime/sort_i64_random"), None)
    minimum = budget.get("runtime", {}).get("min_c_ratio")
    if not match or match.get("status") != "pass" or not match.get("audit_pass"):
        errors.append("sort runtime benchmark/audit did not pass")
    elif not isinstance(minimum, (int, float)) or match.get("ratio", 0) < minimum:
        errors.append(f"sort C ratio {match.get('ratio')} is below {minimum}")
    if errors:
        print("semantic performance evidence gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("semantic performance evidence gate: PASS (O0/O2 shape, object, runtime)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
