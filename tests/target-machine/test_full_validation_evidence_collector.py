#!/usr/bin/env python3
"""Focused tests for full-validation lane result classification."""

from __future__ import annotations

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "full_validation_producer",
    ROOT / "scripts/collect_target_machine_full_validation_evidence.py",
)
assert SPEC is not None and SPEC.loader is not None
producer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(producer)


def main() -> int:
    cases = (
        ((0, "Total Tests: 3", 0), ("passed", 3)),
        ((0, "Total Tests: 0", 0), ("failed", 0)),
        ((0, "Total Tests: 3", 1), ("failed", 3)),
        ((1, "Total Tests: 3", 0), ("failed", 3)),
        ((0, "No tests were found", 0), ("failed", 0)),
    )
    for inputs, expected in cases:
        actual = producer.lane_result(*inputs)
        if actual != expected:
            raise AssertionError(f"lane classification {inputs}: {actual} != {expected}")
    if set(producer.LANE_REGEX) != {
        "unit", "regression", "compile-error", "aot", "generated-c", "vm",
        "stdlib", "ffi", "coroutine", "concurrency", "contract", "asan-ubsan",
        "tsan", "fuzz", "model-stress", "portability", "performance",
        "runtime-embedding", "installer-package",
    }:
        raise AssertionError("mandatory validation lane catalog drifted")
    print(f"target-machine full validation evidence self-test: PASS ({len(cases)} mutations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
