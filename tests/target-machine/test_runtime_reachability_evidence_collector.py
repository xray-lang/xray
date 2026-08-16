#!/usr/bin/env python3
"""Self-test the identity-bound runtime reachability evidence collector."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import collect_target_machine_runtime_reachability_evidence as collector  # noqa: E402


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run(arguments: list[str], cwd: Path) -> str:
    result = subprocess.run(
        arguments, cwd=cwd, check=False, capture_output=True,
        text=True, encoding="utf-8",
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout.strip()


def initialize(root: Path) -> dict[str, Any]:
    (root / "scripts").mkdir(parents=True)
    (root / "src").mkdir()
    (root / "contracts/target-machine").mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
    (root / "src/authority.h").write_text("#define FIXTURE 1\n", encoding="utf-8")
    for name, value in {
        "semantic-owner.json": {"operation_count": 0, "operations": []},
        "legacy-vm.json": {
            "opcode_count": 0, "opcodes": [], "tagged_frame_sites": [],
            "vm_public_api_symbols": [], "legacy_artifact_symbols": [], "artifact": {},
        },
        "legacy-product.json": {"total": 0, "owner_count": 0},
        "aot-plan.json": {"row_count": 0, "rows": [], "mixed_representation_types": {}},
        "validation-matrix.json": {"rows": []},
    }.items():
        write_json(root / "contracts/target-machine" / name, value)
    subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
    run(["git", "config", "user.email", "fixture@example.invalid"], root)
    run(["git", "config", "user.name", "Fixture"], root)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "fixture"], root)
    kinds = (
        "activation-generation", "dependency-graph", "full-validation",
        "installed", "matrix", "runtime-reachability", "symbol",
    )
    policy = {
        "schema": 2, "checker": "target-machine-completion-governance/2",
        "policy": {
            "accepted_evidence_status": ["passed", "unsupported"],
            "compatibility_or_fallback": "forbidden",
            "missing_or_unclassified": "error", "residue_count": 0,
            "self_certifying_write": "forbidden", "skip_or_allowlist": "forbidden",
        },
        "input_identity": {
            "algorithm": "sha256", "files": ["CMakeLists.txt"],
            "sha256": assembler.completion.framed_tree_hash(root, ["CMakeLists.txt"]),
        },
        "authorities": [{
            "id": "fixture", "path": "src/authority.h", "required_regex": ["FIXTURE"],
        }],
        "residue_scan": {
            "definition_paths": [], "roots": ["src"],
            "rules": [{"id": "fixture", "path_regex": "(?!)", "text_regex": "(?!)"}],
        },
        "evidence": {"required_files": {kind: f"{kind}.json" for kind in kinds}},
        "inventories": {
            "legacy_vm": "contracts/target-machine/legacy-vm.json",
            "legacy_product": "contracts/target-machine/legacy-product.json",
            "aot_plan": "contracts/target-machine/aot-plan.json",
        },
        "dual_owner": {
            "inventory": "contracts/target-machine/semantic-owner.json",
            "mechanical_adapter": "representation adapter",
        },
        "installed": {
            "forbidden_path_regex": "(?!)", "forbidden_text_regex": "(?!)",
            "required_deliverables": ["fixture-runtime"],
            "required_public_headers": ["include/xray/fixture_runtime.h"],
        },
        "matrix": {
            "policy": "contracts/target-machine/validation-matrix.json",
            "qualifying_tiers": [], "required_artifact_routes": [],
            "required_dimensions": [],
        },
        "runtime_reachability": {"required_lanes": collector.EXPECTED_RESULTS},
        "validation": {"required_lanes": []},
    }
    write_json(root / "contracts/target-machine/completion-governance.json", policy)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "governance"], root)
    return policy


def build_fixture(build: Path) -> None:
    suffix = ".exe" if os.name == "nt" else ""
    files = (
        f"xray{suffix}", f"tests/unit/test_xtp_format{suffix}",
        f"tests/unit/test_typed_frame_runtime_archive{suffix}",
        f"tests/unit/test_runtime_target_plan_load_archive{suffix}",
        "xray_vm.lib" if os.name == "nt" else "libxray_vm.a",
    )
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        "CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_C_COMPILER:FILEPATH=fixture-cc\n",
        encoding="utf-8",
    )
    (build / "build.ninja").write_text("# fixture\n", encoding="utf-8")
    for relative in files:
        path = build / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture\n", encoding="utf-8")


def fake_runner(mutation: str = "") -> Callable[[list[str], Path], tuple[int, str]]:
    retired_config_init = "".join(("xray_", "vm_", "config_", "init"))

    def execute(arguments: list[str], cwd: Path) -> tuple[int, str]:
        command = Path(arguments[0]).name
        if "test_typed_frame_runtime_archive" in command:
            return 0, "runtime-only typed frame boundary passed\n"
        if "test_runtime_target_plan_load_archive" in command:
            if mutation == "canary":
                return 1, "external activation failed\n"
            return 0, "runtime scalar artifact load and execution passed\n"
        if "test_xtp_format" in command:
            Path(arguments[-2]).write_bytes(b"xsm")
            Path(arguments[-1]).write_bytes(b"xtp")
            return 0, "runtime artifacts written\n"
        if any(str(value).endswith("legacy-api-probe.c") for value in arguments):
            return 1, f"undefined reference to {retired_config_init}\n"
        if "--bytecode" in arguments:
            return 2, "unknown option '--bytecode'\n"
        if len(arguments) > 1 and arguments[1] == "eval":
            if mutation == "eval":
                return 0, "runtime-eval-residue\n"
            return 2, "eval removed: not supported\n"
        if len(arguments) > 1 and arguments[1] == "compile":
            Path(arguments[-1]).write_bytes(b"legacy-xrc")
            return 0, "legacy fixture compiled\n"
        if len(arguments) > 1 and arguments[1] == "run" and str(arguments[2]).endswith(".xrc"):
            return 2, "XR_ARTIFACT_2000 legacy XRC removed\n"
        if len(arguments) > 1 and arguments[1] == "run" and str(arguments[2]).endswith(".xtp"):
            return 0, (
                "42\nxray-run-timing artifact_read_ns=1 semantic_verify_ns=2 "
                "target_verify_ns=3 activation_ns=4 entry_output_ns=5 total_ns=15\n"
            )
        return 127, f"unexpected fixture command: {arguments}\n"

    return execute


def validate_raw(path: Path, root: Path, policy: dict[str, Any],
                 status: str) -> dict[str, Any]:
    row = assembler.read_object(path)
    if set(row) != assembler.RAW_FIELDS or row["status"] != status:
        raise AssertionError("raw runtime manifest shape/status is not exact")
    identity = collector.repository_identity(root)
    if row["source_commit"] != identity["source_commit"] or \
       row["repository_sha256"] != identity["repository_sha256"] or \
       row["governance_input_sha256"] != policy["input_identity"]["sha256"]:
        raise AssertionError("raw runtime evidence identity is stale")
    for log in row["logs"]:
        retained = path.parent / log["path"]
        digest = assembler.sha256_file(retained)
        expected = assembler.raw_log_identity(
            collector.KIND, row["source_commit"], row["repository_sha256"],
            row["governance_input_sha256"], log["path"], digest, row["owner"],
            row["generated_at"], row["command"], row["platform"],
            row["exit_code"], row["status"],
        )
        if log["sha256"] != digest or log["identity_sha256"] != expected:
            raise AssertionError("raw runtime log identity is stale")
    return row


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-runtime-evidence-") as directory:
        parent = Path(directory)
        root = parent / "repo"
        root.mkdir()
        policy = initialize(root)
        build = parent / "build"
        build_fixture(build)
        original_run = collector.run_command
        original_symbols = collector.binlib.defined_symbol_names
        original_nm = collector.binlib.find_nm
        original_dumpbin = collector.binlib.find_dumpbin
        collector.binlib.find_nm = lambda: "fixture-nm"
        collector.binlib.find_dumpbin = lambda: None
        try:
            collector.run_command = fake_runner()
            collector.binlib.defined_symbol_names = lambda path: [
                "xr_runtime_target_plan_load", "xr_module_generation_activate",
            ]
            clean_output = parent / "clean"
            if collector.collect(root, build, clean_output, "fixture-owner") != 0:
                raise AssertionError("clean runtime fixture did not pass")
            clean = validate_raw(
                clean_output / "runtime-reachability.raw.json", root, policy, "passed"
            )
            if set(clean["payload"]["lanes"]) != set(collector.EXPECTED_RESULTS):
                raise AssertionError("clean runtime fixture omitted lanes")
            stage = parent / "stage"
            stage.mkdir()
            envelope = assembler.validate_raw_manifest(
                clean_output / "runtime-reachability.raw.json", clean_output,
                collector.KIND, collector.repository_identity(root),
                policy["input_identity"]["sha256"], stage,
            )
            verified = {row["path"] for row in envelope["logs"]}
            findings = assembler.completion.runtime_findings(envelope, verified, policy)
            if findings:
                raise AssertionError(f"clean runtime envelope rejected: {findings}")

            retired_constructor = "".join(("xray_", "vm_", "new"))
            for name, runner, symbols in (
                ("eval", fake_runner("eval"), ["xr_runtime_target_plan_load"]),
                ("canary", fake_runner("canary"), ["xr_runtime_target_plan_load"]),
                ("compiler", fake_runner(), ["xr_runtime_target_plan_load", "xi_compile"]),
                ("legacy", fake_runner(),
                 ["xr_runtime_target_plan_load", retired_constructor]),
            ):
                collector.run_command = runner
                collector.binlib.defined_symbol_names = lambda path, value=symbols: value
                failed_output = parent / name
                if collector.collect(root, build, failed_output, "fixture-owner") != 1:
                    raise AssertionError(f"{name} mutation did not fail honestly")
                failed = validate_raw(
                    failed_output / "runtime-reachability.raw.json", root, policy, "failed"
                )
                if all(row["result"] == "passed" for row in failed["logs"]):
                    raise AssertionError(f"{name} mutation marked every log passed")

            collector.run_command = fake_runner()
            collector.binlib.defined_symbol_names = lambda path: ["clean"]
            try:
                collector.collect(root, build, clean_output, "fixture-owner")
            except collector.CollectionError:
                pass
            else:
                raise AssertionError("runtime collector overwrote existing evidence")
        finally:
            collector.run_command = original_run
            collector.binlib.defined_symbol_names = original_symbols
            collector.binlib.find_nm = original_nm
            collector.binlib.find_dumpbin = original_dumpbin
    print("target-machine runtime reachability evidence self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
