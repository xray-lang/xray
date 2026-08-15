#!/usr/bin/env python3
"""Mutation tests for governed full-validation evidence collection."""

from __future__ import annotations

import importlib.util
import json
import sys
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

BASELINE_SPEC = importlib.util.spec_from_file_location(
    "target_machine_baseline_runner",
    ROOT / "tests/target-machine/phase0/run_baseline.py",
)
assert BASELINE_SPEC is not None and BASELINE_SPEC.loader is not None
baseline_runner = importlib.util.module_from_spec(BASELINE_SPEC)
sys.modules[BASELINE_SPEC.name] = baseline_runner
BASELINE_SPEC.loader.exec_module(baseline_runner)


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


def write_configured_build(build: Path, source: Path, *, fastpaths: str = "OFF",
                           duplicate_generator: bool = False) -> None:
    build.mkdir(parents=True, exist_ok=True)
    lines = [
        "CMAKE_BUILD_TYPE:STRING=Release",
        "CMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON",
        "CMAKE_GENERATOR:INTERNAL=Ninja",
        f"CMAKE_HOME_DIRECTORY:INTERNAL={source.resolve()}",
        f"XRAY_STDLIB_VM_FASTPATHS:BOOL={fastpaths}",
    ]
    if duplicate_generator:
        lines.append("CMAKE_GENERATOR:INTERNAL=Ninja")
    (build / "CMakeCache.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (build / "build.ninja").write_text("# fixture\n", encoding="utf-8")


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
        source = Path(directory) / "source"
        source.mkdir()
        build = source / "build-relocated"
        output = build / "evidence"
        captured: list[tuple[list[str], str]] = []
        original_step = baseline_runner.run_preflight_step

        def capture_step(command, root, selected_build, evidence, name, timeout, policy):
            captured.append((command, name))
            return {"name": name, "status": "passed"}

        baseline_runner.run_preflight_step = capture_step
        try:
            steps = baseline_runner.run_freshness_preflight(
                source, build, output,
                {"identity": {
                    "configure_preset": "default",
                    "configure_cache_variables": {
                        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
                        "XRAY_STDLIB_VM_FASTPATHS": "OFF",
                    },
                    "configure_timeout_seconds": 600,
                    "build_timeout_seconds": 3600,
                    "build_parallelism": 4,
                }},
            )
        finally:
            baseline_runner.run_preflight_step = original_step
        expected = [
            (["cmake", "--preset", "default", "-B", str(build),
              "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
              "-DXRAY_STDLIB_VM_FASTPATHS=OFF"], "configure"),
            (["cmake", "--build", str(build), "--parallel", "4"], "build"),
        ]
        if steps != [{"name": "configure", "status": "passed"},
                     {"name": "build", "status": "passed"}] or captured != expected:
            raise AssertionError("non-default build root is not governed exactly")
        results.append("relocatable-build-root")

        relocated_source = Path(directory) / "relocated-source"
        relocated_build = relocated_source / "another-build-name"
        relocated_output = relocated_build / "other-evidence"
        canonical = baseline_runner.command_text(
            captured[0][0], source, output, build
        )
        relocated_command = [
            "cmake", "--preset", "default", "-B", str(relocated_build),
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DXRAY_STDLIB_VM_FASTPATHS=OFF",
        ]
        if canonical != baseline_runner.command_text(
                relocated_command, relocated_source, relocated_output,
                relocated_build):
            raise AssertionError("build preflight evidence retains a physical root")
        if baseline_runner.command_text(
                [str(build / "xray.exe")], source, output, build
        ) != ["${BUILD_ROOT}/xray.exe"]:
            raise AssertionError("build artifact evidence path is not canonical")
        results.append("relocatable-preflight-identity")

        try:
            baseline_runner.run_freshness_preflight(
                source, source.parent, output,
                {"identity": {
                    "configure_preset": "default",
                    "configure_cache_variables": {
                        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
                        "XRAY_STDLIB_VM_FASTPATHS": "OFF",
                    },
                    "configure_timeout_seconds": 600,
                    "build_timeout_seconds": 3600,
                    "build_parallelism": 4,
                }},
            )
        except RuntimeError:
            results.append("source-containing-build-root")
        else:
            raise AssertionError("build root containing the source tree was accepted")

        source_a = Path(directory) / "identity-source-a"
        source_b = Path(directory) / "identity-source-b"
        source_a.mkdir()
        source_b.mkdir()
        build_a = source_a / "build-a"
        build_b = source_b / "build-b"
        write_configured_build(build_a, source_a)
        write_configured_build(build_b, source_b)
        identity_a = producer.configured_build_identity(source_a, build_a)
        identity_b = producer.configured_build_identity(source_b, build_b)
        if identity_a != identity_b or identity_a["build_root"] != "${BUILD_ROOT}":
            raise AssertionError("configured build identity is not relocatable")
        results.append("relocatable-configured-build-identity")
        if producer.canonical_command(
                source_a, build_a, build_a / "raw", "fixture-owner", 321
        )[-2:] != ["--lane-timeout", "321"]:
            raise AssertionError("collector command identity omits the lane timeout")
        results.append("collector-timeout-identity")

        expect_failure(
            "wrong-source-root",
            lambda: producer.configured_build_identity(source_b, build_a),
            results,
        )

        for label, fastpaths, duplicate in (
            ("fastpaths-on", "ON", False),
            ("duplicate-build-field", "OFF", True),
        ):
            broken_build = source_a / f"build-{label}"
            write_configured_build(
                broken_build, source_a, fastpaths=fastpaths,
                duplicate_generator=duplicate,
            )
            expect_failure(
                label,
                lambda selected=broken_build: producer.configured_build_identity(
                    source_a, selected
                ),
                results,
            )

        policy = baseline_runner.load_policy(
            ROOT, ROOT / "contracts/target-machine/baseline-manifest.json"
        )
        for label, variables in (
            ("missing-configure-cache", {"XRAY_STDLIB_VM_FASTPATHS": "OFF"}),
            ("fastpaths-policy-on", {
                "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
                "XRAY_STDLIB_VM_FASTPATHS": "ON",
            }),
            ("extra-configure-cache", {
                "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
                "XRAY_STDLIB_VM_FASTPATHS": "OFF",
                "UNOWNED_OPTION": "ON",
            }),
        ):
            mutated_policy = json.loads(json.dumps(policy))
            mutated_policy["identity"]["configure_cache_variables"] = variables
            if not baseline_runner.validate_policy(ROOT, mutated_policy):
                raise AssertionError(f"{label}: policy mutation was accepted")
            results.append(label)
        legacy_policy = json.loads(json.dumps(policy))
        legacy_policy["identity"]["build_directory"] = "build"
        if not baseline_runner.validate_policy(ROOT, legacy_policy):
            raise AssertionError("legacy build-directory policy was accepted")
        results.append("legacy-build-directory")

        build = Path(directory) / "build"
        build.mkdir()
        (build / "CTestTestfile.cmake").write_text("# fixture\n", encoding="utf-8")
        write_configured_build(build, ROOT)
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
