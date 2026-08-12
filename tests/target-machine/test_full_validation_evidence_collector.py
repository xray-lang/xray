#!/usr/bin/env python3
"""Mutation tests for governed full-validation evidence collection."""

from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "full_validation_producer",
    ROOT / "scripts/collect_target_machine_full_validation_evidence.py",
)
assert SPEC is not None and SPEC.loader is not None
producer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(producer)


def expect_failure(label: str, action, results: list[str]) -> None:
    try:
        action()
    except producer.CollectionError:
        results.append(label)
        return
    raise AssertionError(f"{label}: mutation was accepted")


def baseline(status: str = "passed") -> dict:
    correctness = [
        {
            "name": name,
            "status": status,
            "repeat_policy": repeat,
        }
        for name, repeat in (
            ("startup-debt", "three-or-more"),
            ("differential-modes", "three-or-more"),
            ("architecture-contracts", "three-or-more"),
            ("non-sanitizer-suite", "single-heavy-gate"),
            ("asan-ubsan", "single-heavy-gate"),
            ("tsan", "single-heavy-gate"),
        )
    ]
    performance = [{
        "name": "clean-cold-source-run", "status": status,
        "variance": {"status": status},
    }]
    return {
        "result": status,
        "correctness_lanes": correctness,
        "performance_lanes": performance,
    }


def main() -> int:
    results: list[str] = []
    discovery = json.dumps({
        "tests": [{"name": "second"}, {"name": "first"}],
    })
    if producer.parse_ctest_discovery(discovery) != ("first", "second"):
        raise AssertionError("CTest discovery is not canonical")
    expect_failure(
        "invalid-json",
        lambda: producer.parse_ctest_discovery("not json"), results,
    )
    expect_failure(
        "missing-tests",
        lambda: producer.parse_ctest_discovery("{}"), results,
    )
    expect_failure(
        "duplicate-test",
        lambda: producer.parse_ctest_discovery(json.dumps({
            "tests": [{"name": "same"}, {"name": "same"}],
        })), results,
    )
    expect_failure(
        "missing-test-name",
        lambda: producer.parse_ctest_discovery(json.dumps({"tests": [{}]})), results,
    )

    good = baseline()
    statuses = producer.baseline_lane_statuses(good)
    required = set(producer.LANE_REGEX) | {producer.BASELINE_ONLY_LANE}
    if set(statuses) != required or not all(statuses.values()):
        raise AssertionError("clean full baseline did not satisfy every lane")
    failed_sanitizer = baseline()
    failed_sanitizer["correctness_lanes"][-1]["status"] = "failed"
    statuses = producer.baseline_lane_statuses(failed_sanitizer)
    if statuses["tsan"] or not statuses["unit"]:
        raise AssertionError("sanitizer failure classification drifted")
    failed_variance = baseline()
    failed_variance["performance_lanes"][0]["variance"]["status"] = "failed"
    statuses = producer.baseline_lane_statuses(failed_variance)
    if statuses["performance"] or statuses[producer.BASELINE_ONLY_LANE]:
        raise AssertionError("performance variance failure was reclassified")
    results.extend(("sanitizer-failure", "variance-failure"))
    expect_failure(
        "missing-lane-collections",
        lambda: producer.baseline_lane_statuses({"result": "passed"}), results,
    )
    duplicated = baseline()
    duplicated["correctness_lanes"].append(
        dict(duplicated["correctness_lanes"][0])
    )
    expect_failure(
        "duplicate-baseline-lane",
        lambda: producer.baseline_selection(duplicated), results,
    )

    if producer.selection_sha256(("a", "b")) == producer.selection_sha256(("b", "a")):
        raise AssertionError("selection identity ignores execution order")
    results.append("selection-order")

    with tempfile.TemporaryDirectory(prefix="xray-full-validation-") as directory:
        build = Path(directory) / "build"
        build.mkdir()
        (build / "CTestTestfile.cmake").write_text("# fixture\n", encoding="utf-8")
        target = Path(directory) / "raw"
        target.mkdir()
        sentinel = target / "sentinel"
        sentinel.write_text("preserve\n", encoding="utf-8")
        try:
            producer.collect(ROOT, build, target, "fixture-owner", 1)
        except producer.CollectionError as error:
            if "never overwrites" not in str(error):
                raise AssertionError(f"unexpected overwrite error: {error}") from error
        else:
            raise AssertionError("existing output was overwritten")
        if sentinel.read_text(encoding="utf-8") != "preserve\n":
            raise AssertionError("existing output content changed")
        results.append("atomic-no-overwrite")

    if required != {
        "unit", "regression", "compile-error", "aot", "generated-c", "vm",
        "stdlib", "ffi", "coroutine", "concurrency", "contract", "asan-ubsan",
        "tsan", "fuzz", "model-stress", "portability", "performance",
        "runtime-embedding", "installer-package",
    }:
        raise AssertionError("mandatory validation lane catalog drifted")
    print(
        "target-machine full validation evidence self-test: PASS "
        f"({len(results)} mutations)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
