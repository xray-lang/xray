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


def initialize(root: Path) -> dict[str, Any]:
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
    matrix = {"schema": 1, "rows": [{
        "id": "TM-MATRIX-FIXTURE", "support_tier": "supported",
        "target": "fixture-target", "provider": "fixture-provider",
        "artifact_route": "source+native", "executor_or_generation": "vm+aot",
        "build_or_sanitizer": "Release", "command": "fixture-run --exact",
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
                      "required_deliverables": [], "required_public_headers": []},
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
                 mutate: str = "") -> None:
    identity = collector.repository_identity(root)
    matrix_path = root / "contracts/target-machine/validation-matrix.json"
    log = results / "logs/TM-MATRIX-FIXTURE.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("fixture exact command passed\n", encoding="utf-8")
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
        "command": ["fixture-run", "--exact"],
        "target": "fixture-target", "provider": "fixture-provider",
        "artifact_route": "source+native", "executor_or_generation": "vm+aot",
        "build_or_sanitizer": "Release",
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
        "log_sha256": assembler.sha256_file(log), "identity_sha256": "",
    }
    if mutate == "command":
        row["command"] = ["forged-command"]
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

        for mutation in ("command", "identity"):
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
    print(f"target-machine matrix evidence self-test: PASS ({mutations} mutations)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
