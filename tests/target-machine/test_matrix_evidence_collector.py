#!/usr/bin/env python3
"""Self-test the identity-bound target-machine matrix evidence collector."""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import collect_target_machine_matrix_evidence as collector  # noqa: E402


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def run(arguments: list[str], cwd: Path) -> str:
    result = subprocess.run(arguments, cwd=cwd, check=False, capture_output=True,
                            text=True, encoding="utf-8")
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()


def initialize(root: Path, local_command: str | None = None) -> dict[str, Any]:
    (root / "scripts").mkdir(parents=True)
    (root / "src").mkdir()
    (root / "contracts/target-machine").mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
    (root / "src/authority.h").write_text("#define FIXTURE 1\n", encoding="utf-8")
    baseline = root / "contracts/target-machine/baseline.json"
    write_json(baseline, {"fixture": True})
    dimensions = [
        "target", "provider", "artifact_route", "executor_or_generation",
        "build_or_sanitizer",
    ]
    command = "fixture-run --exact"
    target = "fixture-target"
    if local_command is not None:
        target = collector.local_target()
        if local_command == "ctest":
            command = "ctest --test-dir build --output-on-failure"
        elif local_command != "plain":
            raise AssertionError(f"unknown local command fixture {local_command}")
    matrix = {"schema": 1, "rows": [{
        "id": "TM-MATRIX-FIXTURE", "support_tier": "supported",
        "target": target, "provider": "fixture-provider",
        "artifact_route": "source+native", "executor_or_generation": "vm+aot",
        "build_or_sanitizer": "Release", "command": command,
        "owner": "fixture-owner", "oracle": "fixture independent oracle",
        "baseline": "contracts/target-machine/baseline.json",
        "timeout_seconds": 30,
    }, {
        "id": "TM-MATRIX-UNSUPPORTED", "support_tier": "unsupported",
        "target": "unsupported-target", "provider": "any",
        "artifact_route": "any", "executor_or_generation": "any",
        "build_or_sanitizer": "any", "command": "compile-time rejection",
        "owner": "fixture-owner", "oracle": "rejection oracle",
        "baseline": "contracts/target-machine/baseline.json",
        "timeout_seconds": 30,
    }]}
    write_json(root / "contracts/target-machine/validation-matrix.json", matrix)
    for name, value in {
        "semantic-owner.json": {"operation_count": 0, "operations": []},
        "legacy-vm.json": {"opcode_count": 0, "opcodes": [],
                           "tagged_frame_sites": [], "vm_public_api_symbols": [],
                           "legacy_artifact_symbols": [], "artifact": {}},
        "legacy-product.json": {"total": 0, "owner_count": 0},
        "aot-plan.json": {"row_count": 0, "rows": [],
                          "mixed_representation_types": {}},
    }.items():
        write_json(root / "contracts/target-machine" / name, value)
    subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
    run(["git", "config", "user.email", "fixture@example.invalid"], root)
    run(["git", "config", "user.name", "Fixture"], root)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "fixture"], root)
    policy = {
        "schema": 2, "checker": "target-machine-completion-governance/2",
        "policy": {"accepted_evidence_status": ["passed", "unsupported"],
                   "compatibility_or_fallback": "forbidden",
                   "missing_or_unclassified": "error", "residue_count": 0,
                   "self_certifying_write": "forbidden",
                   "skip_or_allowlist": "forbidden"},
        "input_identity": {"algorithm": "sha256", "files": ["CMakeLists.txt"],
                           "sha256": assembler.completion.framed_tree_hash(
                               root, ["CMakeLists.txt"])},
        "authorities": [{"id": "fixture", "path": "src/authority.h",
                         "required_regex": ["FIXTURE"]}],
        "residue_scan": {"definition_paths": [], "roots": ["src"],
                         "rules": [{"id": "fixture-residue",
                                    "path_regex": "(?!)", "text_regex": "(?!)"}]},
        "evidence": {"required_files": {kind: f"{kind}.json" for kind in (
            "activation-generation", "dependency-graph", "full-validation",
            "installed", "matrix", "runtime-reachability", "symbol")}},
        "inventories": {"legacy_vm": "contracts/target-machine/legacy-vm.json",
                        "legacy_product": "contracts/target-machine/legacy-product.json",
                        "aot_plan": "contracts/target-machine/aot-plan.json"},
        "dual_owner": {"inventory": "contracts/target-machine/semantic-owner.json",
                       "mechanical_adapter": "representation adapter"},
        "installed": {"forbidden_path_regex": "(?!)", "forbidden_text_regex": "(?!)",
                      "required_deliverables": ["fixture-runtime"],
                      "required_public_headers": ["include/xray/fixture_runtime.h"]},
        "matrix": {"policy": "contracts/target-machine/validation-matrix.json",
                   "qualifying_tiers": ["supported", "ci-only"],
                   "required_artifact_routes": list(collector.ROUTES["source+native"]),
                   "required_dimensions": dimensions},
        "runtime_reachability": {"required_lanes": {}},
        "validation": {"required_lanes": []},
    }
    write_json(root / "contracts/target-machine/completion-governance.json", policy)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "governance"], root)
    return policy


def write_result(root: Path, results: Path, policy: dict[str, Any],
                 mutate: str = "", log_text: str = "fixture exact command passed\n") -> None:
    identity = collector.repository_identity(root)
    matrix_path = root / "contracts/target-machine/validation-matrix.json"
    matrix = assembler.read_object(matrix_path)
    fixture = matrix["rows"][0]
    log = results / "logs/TM-MATRIX-FIXTURE.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(log_text, encoding="utf-8")
    artifact = results / "artifacts/TM-MATRIX-FIXTURE.xtp"
    binary = results / "binaries/TM-MATRIX-FIXTURE.exe"
    artifact.parent.mkdir(parents=True, exist_ok=True)
    binary.parent.mkdir(parents=True, exist_ok=True)
    artifact.write_bytes(b"fixture target-plan artifact\n")
    binary.write_bytes(b"fixture executable image\n")
    row = {
        "schema": collector.ROW_RESULT_SCHEMA,
        "row_id": "TM-MATRIX-FIXTURE", "status": "passed",
        "source_commit": identity["source_commit"],
        "repository_sha256": identity["repository_sha256"],
        "governance_input_sha256": policy["input_identity"]["sha256"],
        "policy_sha256": assembler.sha256_file(matrix_path),
        "command": collector.exact_row_command(fixture["command"]),
        "target": fixture["target"], "provider": fixture["provider"],
        "artifact_route": fixture["artifact_route"],
        "executor_or_generation": fixture["executor_or_generation"],
        "build_or_sanitizer": fixture["build_or_sanitizer"],
        "platform": {"os": "fixture-os", "arch": "fixture-arch",
                     "toolchain": "fixture-toolchain"},
        "exit_code": 0, "positive_activation": "activated-and-passed",
        "negative_mismatch": "rejected-before-activation",
        "artifact_retention": "retained",
        "artifact": "artifacts/TM-MATRIX-FIXTURE.xtp",
        "artifact_sha256": assembler.sha256_file(artifact),
        "artifact_fingerprint": assembler.sha256_file(artifact),
        "binary": "binaries/TM-MATRIX-FIXTURE.exe",
        "binary_sha256": assembler.sha256_file(binary),
        "binary_fingerprint": assembler.sha256_file(binary),
        "source_fingerprint": identity["repository_sha256"],
        "last_verified": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "log": "logs/TM-MATRIX-FIXTURE.log",
        "log_sha256": assembler.sha256_file(log),
        "normalized_log_sha256": collector.command_log_sha256(
            collector.exact_row_command(fixture["command"]), log_text
        ),
        "identity_sha256": "",
    }
    if mutate == "command":
        row["command"] = ["forged-command"]
    elif mutate == "log-digest":
        row["log_sha256"] = "0" * 64
    elif mutate == "normalized-digest":
        row["normalized_log_sha256"] = "0" * 64
    row["identity_sha256"] = collector.row_result_identity(row)
    if mutate == "identity":
        row["identity_sha256"] = "0" * 64
    write_json(results / "TM-MATRIX-FIXTURE.json", row)


def validate_raw(path: Path, root: Path, policy: dict[str, Any],
                 status: str) -> dict[str, Any]:
    raw = assembler.read_object(path)
    if set(raw) != assembler.RAW_FIELDS or raw["status"] != status:
        raise AssertionError("raw matrix manifest shape/status is not exact")
    identity = collector.repository_identity(root)
    if raw["source_commit"] != identity["source_commit"] or \
       raw["repository_sha256"] != identity["repository_sha256"] or \
       raw["governance_input_sha256"] != policy["input_identity"]["sha256"]:
        raise AssertionError("raw matrix manifest identity is stale")
    for log in raw["logs"]:
        retained = path.parent / log["path"]
        digest = assembler.sha256_file(retained)
        expected = assembler.raw_log_identity(
            collector.KIND, raw["source_commit"], raw["repository_sha256"],
            raw["governance_input_sha256"], log["path"], digest, raw["owner"],
            raw["generated_at"], raw["command"], raw["platform"],
            raw["exit_code"], raw["status"],
        )
        if log["sha256"] != digest or log["identity_sha256"] != expected:
            raise AssertionError("raw matrix retained log identity is stale")
    return raw


def self_test() -> int:
    mutations = 0
    with tempfile.TemporaryDirectory(prefix="xray-matrix-evidence-") as directory:
        parent = Path(directory)
        root = parent / "repo"
        root.mkdir()
        policy = initialize(root)
        results = parent / "results"
        write_result(root, results, policy)
        output = parent / "clean"
        if collector.collect(root, results, output, "fixture-owner") != 0:
            raise AssertionError("complete matrix fixture did not pass")
        raw = validate_raw(output / "matrix.raw.json", root, policy, "passed")
        if raw["payload"]["axis_catalog"]["artifact_route"] != ["any", "source+native"]:
            raise AssertionError("matrix axis catalog is not source-backed")
        qualifying = [row for row in raw["payload"]["rows"]
                      if row["id"] == "TM-MATRIX-FIXTURE"][0]
        if set(qualifying["artifact_routes"]) != set(policy["matrix"]["required_artifact_routes"]):
            raise AssertionError("matrix routes are not derived exactly")

        missing = parent / "missing"
        if collector.collect(root, parent / "no-results", missing, "fixture-owner") != 1:
            raise AssertionError("missing row result did not fail honestly")
        validate_raw(missing / "matrix.raw.json", root, policy, "failed")
        mutations += 1

        for mutation in ("command", "log-digest", "normalized-digest", "identity"):
            broken_results = parent / f"results-{mutation}"
            write_result(root, broken_results, policy, mutation)
            broken = parent / f"broken-{mutation}"
            if collector.collect(root, broken_results, broken, "fixture-owner") != 1:
                raise AssertionError(f"{mutation} mutation did not fail honestly")
            validate_raw(broken / "matrix.raw.json", root, policy, "failed")
            mutations += 1

        try:
            collector.collect(root, results, output, "fixture-owner")
        except collector.CollectionError:
            mutations += 1
        else:
            raise AssertionError("collector overwrote existing raw evidence")

        local_root = parent / "local-repo"
        local_root.mkdir()
        local_policy = initialize(local_root, local_command="ctest")
        local_results = parent / "local-results"
        write_result(
            local_root, local_results, local_policy,
            log_text=(
                "Test project /independent/worker/build\n"
                "    Start 1: fixture_test\n"
                "1/1 Test #1: fixture_test ........ Passed    9.87 sec\n"
                "100% tests passed, 0 tests failed out of 1\n"
                "Label Time Summary:\n"
                "fixture = 9.87 sec*proc (1 test)\n"
                "Total Test time (real) =   9.87 sec\n"
            ),
        )
        local_output = parent / "local-output"
        local_execution = (
            f"Test project {local_root / 'build'}\n"
            "    Start 1: fixture_test\n"
            "1/1 Test #1: fixture_test ........ Passed    0.01 sec\n"
            "100% tests passed, 0 tests failed out of 1\n"
            "Label Time Summary:\n"
            "fixture = 0.01 sec*proc (1 test)\n"
            "Total Test time (real) =   0.01 sec\n"
        )
        if collector.canonical_command_log(
                ["fixture-run"], local_execution) != local_execution:
            raise AssertionError("non-CTest output was normalized")
        mutations += 1
        malformed_ctest = {
            "missing-project": (
                "    Start 1: fixture_test\n"
                "1/1 Test #1: fixture_test ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n"
                "Total Test time (real) = 0.01 sec\n"
            ),
            "missing-total": (
                "Test project C:/work/build\n"
                "    Start 1: fixture_test\n"
                "1/1 Test #1: fixture_test ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n"
            ),
            "duplicate-project": (
                "Test project C:/work/build\n"
                "Test project D:/work/build\n"
                "    Start 1: fixture_test\n"
                "1/1 Test #1: fixture_test ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n"
                "Total Test time (real) = 0.01 sec\n"
            ),
            "duplicate-total": (
                "Test project C:/work/build\n"
                "    Start 1: fixture_test\n"
                "1/1 Test #1: fixture_test ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n"
                "Total Test time (real) = 0.01 sec\n"
                "Total Test time (real) = 0.02 sec\n"
            ),
            "unknown-volatile": (
                "Test project C:/work/build\n"
                "    Start 1: fixture_test\n"
                "fixture setup took 4.2 seconds\n"
                "1/1 Test #1: fixture_test ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n"
                "Total Test time (real) = 0.01 sec\n"
            ),
            "nonstandard-result-elapsed": (
                "Test project C:/work/build\n"
                "    Start 1: fixture_test\n"
                "1/1 Test #1: fixture_test ........ Passed 10 milliseconds\n"
                "100% tests passed, 0 tests failed out of 1\n"
                "Total Test time (real) = 0.01 sec\n"
            ),
        }
        for label, transcript in malformed_ctest.items():
            try:
                collector.canonical_command_log(["ctest"], transcript)
            except collector.CollectionError:
                mutations += 1
            else:
                raise AssertionError(f"malformed CTest transcript accepted: {label}")
        original_run = collector.run_exact_command
        collector.run_exact_command = lambda argv, root, timeout: (0, local_execution)
        try:
            if collector.collect(
                    local_root, local_results, local_output, "fixture-owner") != 0:
                raise AssertionError(
                    "volatile CTest log fields rejected an equivalent result"
                )
            local_raw = validate_raw(local_output / "matrix.raw.json", local_root,
                                     local_policy, "passed")
            local_row = next(row for row in local_raw["payload"]["rows"]
                             if row["id"] == "TM-MATRIX-FIXTURE")
            retained_log = local_output / local_row["log"]
            expected_raw_digest = assembler.sha256_file(retained_log)
            expected_normalized_digest = collector.command_log_sha256(
                ["ctest", "--test-dir", "build", "--output-on-failure"],
                retained_log.read_text(encoding="utf-8"),
            )
            if (local_row["raw_log_sha256"] != expected_raw_digest
                    or local_row["normalized_log_sha256"]
                    != expected_normalized_digest):
                raise AssertionError("raw/normalized CTest identities are not both bound")
            mutations += 1

            def reject_local(name: str, independent: str, execution: str,
                             exit_code: int = 0) -> None:
                nonlocal mutations
                changed_results = parent / f"local-results-{name}"
                write_result(local_root, changed_results, local_policy,
                             log_text=independent)
                changed_output = parent / f"local-output-{name}"
                collector.run_exact_command = (
                    lambda argv, root, timeout: (exit_code, execution)
                )
                if collector.collect(
                        local_root, changed_results, changed_output,
                        "fixture-owner") != 1:
                    raise AssertionError(f"CTest {name} mutation was accepted")
                validate_raw(changed_output / "matrix.raw.json", local_root,
                             local_policy, "failed")
                mutations += 1

            changed_name = local_execution.replace("fixture_test", "different_test")
            reject_local("test-name", changed_name, local_execution)
            reject_local(
                "stdout",
                local_execution.replace(
                    "1/1 Test #1", "fixture stdout A\n1/1 Test #1"
                ),
                local_execution.replace(
                    "1/1 Test #1", "fixture stdout B\n1/1 Test #1"
                ),
            )
            reject_local(
                "summary",
                local_execution,
                local_execution.replace(
                    "100% tests passed, 0 tests failed out of 1",
                    "0% tests passed, 1 tests failed out of 1",
                ),
            )
            two_case_a = (
                "Test project C:/worker-a/build\n"
                "    Start 1: alpha\n"
                "1/2 Test #1: alpha ........ Passed 3.00 sec\n"
                "    Start 2: beta\n"
                "2/2 Test #2: beta ......... Passed 4.00 sec\n"
                "100% tests passed, 0 tests failed out of 2\n"
                "Total Test time (real) = 7.00 sec\n"
            )
            two_case_b = (
                "Test project D:/worker-b/build\n"
                "    Start 1: beta\n"
                "1/2 Test #1: beta ......... Passed 0.01 sec\n"
                "    Start 2: alpha\n"
                "2/2 Test #2: alpha ........ Passed 0.02 sec\n"
                "100% tests passed, 0 tests failed out of 2\n"
                "Total Test time (real) = 0.03 sec\n"
            )
            reject_local("case-order", two_case_a, two_case_b)
            reject_local("exit", local_execution, local_execution, exit_code=1)
        finally:
            collector.run_exact_command = original_run

        plain_root = parent / "plain-local-repo"
        plain_root.mkdir()
        plain_policy = initialize(plain_root, local_command="plain")
        plain_results = parent / "plain-local-results"
        write_result(plain_root, plain_results, plain_policy,
                     log_text="independent exact outcome\n")
        plain_output = parent / "plain-local-output"
        collector.run_exact_command = (
            lambda argv, root, timeout: (0, "different local outcome\n")
        )
        try:
            if collector.collect(
                    plain_root, plain_results, plain_output, "fixture-owner") != 1:
                raise AssertionError("non-CTest output mutation was accepted")
            validate_raw(plain_output / "matrix.raw.json", plain_root,
                         plain_policy, "failed")
            mutations += 1
        finally:
            collector.run_exact_command = original_run
    print(f"target-machine matrix evidence self-test: PASS ({mutations} mutations)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
