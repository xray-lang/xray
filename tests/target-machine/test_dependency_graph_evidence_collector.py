#!/usr/bin/env python3
"""Self-test the identity-bound dependency-graph raw evidence collector."""

from __future__ import annotations

import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import collect_target_machine_dependency_graph_evidence as collector  # noqa: E402


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


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
    write_json(root / "contracts/target-machine/semantic-owner.json", {
        "operation_count": 0, "operations": [],
    })
    for name, value in {
        "legacy-vm.json": {
            "opcode_count": 0, "opcodes": [], "tagged_frame_sites": [],
            "vm_public_api_symbols": [], "legacy_artifact_symbols": [], "artifact": {},
        },
        "legacy-product.json": {"total": 0, "owner_count": 0},
        "aot-plan.json": {"row_count": 0, "rows": [], "mixed_representation_types": {}},
        "validation-matrix.json": {"rows": []},
        "baseline-manifest.json": {
            "runner": "target-machine-baseline/3",
            "qualification": {"result": "passed", "evidence_sha256": "a" * 64},
        },
    }.items():
        write_json(root / "contracts/target-machine" / name, value)
    subprocess.run(["git", "init"], cwd=root, check=True, capture_output=True)
    run(["git", "config", "user.email", "fixture@example.invalid"], root)
    run(["git", "config", "user.name", "Fixture"], root)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "fixture"], root)
    kinds = [
        "activation-generation", "dependency-graph", "full-validation",
        "installed", "matrix", "runtime-reachability", "symbol",
    ]
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
            "rules": [{
                "id": "legacy-fixture", "path_regex": "(?!)",
                "text_regex": "LEGACY_EDGE",
            }],
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
            "required_deliverables": list(collector.TARGET_AUTHORITIES),
            "required_public_headers": [],
        },
        "matrix": {
            "policy": "contracts/target-machine/validation-matrix.json",
            "qualifying_tiers": [], "required_artifact_routes": [],
            "required_dimensions": [],
        },
        "runtime_reachability": {"required_lanes": {}},
        "validation": {"required_lanes": []},
    }
    write_json(root / "contracts/target-machine/completion-governance.json", policy)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "governance"], root)
    return policy


def build_fixture(build: Path, legacy: bool = False) -> None:
    (build / ".cmake/api/v1/reply").mkdir(parents=True)
    (build / "CMakeCache.txt").write_text(
        "CMAKE_GENERATOR:INTERNAL=Ninja\n", encoding="utf-8"
    )
    ninja = "build fixture: phony"
    if legacy:
        ninja += " LEGACY_EDGE"
    (build / "build.ninja").write_text(ninja + "\n", encoding="utf-8")
    write_json(build / "compile_commands.json", [{
        "directory": str(build), "command": "cc clean.c", "file": "clean.c",
    }])
    target_names = list(collector.TARGET_AUTHORITIES.values())
    write_json(build / ".cmake/api/v1/reply/codemodel.json", {
        "kind": "codemodel", "configurations": [{
            "targets": [
                {"name": name, "jsonFile": f"target-{index}.json"}
                for index, name in enumerate(target_names)
            ],
        }],
    })
    for index, name in enumerate(target_names):
        write_json(build / f".cmake/api/v1/reply/target-{index}.json", {
            "name": name, "type": "STATIC_LIBRARY" if name != "xray" else "EXECUTABLE",
        })
    write_json(build / ".cmake/api/v1/reply/index-1.json", {
        "objects": [{"kind": "codemodel", "jsonFile": "codemodel.json"}],
    })
    (build / "cmake_install.cmake").write_text(
        "# generated install graph\nfile(INSTALL DESTINATION lib TYPE FILE FILES fixture)\n",
        encoding="utf-8",
    )


def fake_tooling(root: Path) -> dict[str, Any]:
    original = collector.run_text

    def fake(arguments: list[str], cwd: Path) -> tuple[int, str]:
        command = Path(arguments[0]).name.lower()
        if command.startswith("cmake") and "--version" in arguments:
            return 0, "cmake version fixture\n"
        if command.startswith("ctest"):
            return 0, json.dumps({"tests": [{"name": "fixture"}]})
        if command.startswith("ninja"):
            return 0, "fixture.o: #deps 1\n    clean.h\n"
        if command.startswith("cmake") and "--install" in arguments:
            return 0, "-- Install configuration: Fixture\n-- Installing: fixture-target\n"
        return original(arguments, cwd)

    collector.run_text = fake
    return {"restore": original}


def validate_raw(path: Path, root: Path, policy: dict[str, Any],
                 expected: str) -> dict[str, Any]:
    row = assembler.read_object(path)
    if set(row) != assembler.RAW_FIELDS or row["status"] != expected:
        raise AssertionError("raw manifest shape/status is not exact")
    identity = collector.repository_identity(root)
    if row["source_commit"] != identity["source_commit"]:
        raise AssertionError("raw source commit is stale")
    if row["repository_sha256"] != identity["repository_sha256"]:
        raise AssertionError("raw repository tree is stale")
    if row["governance_input_sha256"] != policy["input_identity"]["sha256"]:
        raise AssertionError("raw governance identity is stale")
    if set(row["payload"]["graphs"]) != set(collector.GRAPH_KINDS):
        raise AssertionError("raw graph coverage is incomplete")
    for log in row["logs"]:
        digest = assembler.sha256_file(path.parent / log["path"])
        if digest != log["sha256"]:
            raise AssertionError("raw retained log digest is stale")
        expected_identity = assembler.raw_log_identity(
            collector.KIND, row["source_commit"], row["repository_sha256"],
            row["governance_input_sha256"], log["path"], digest, row["owner"],
            row["generated_at"], row["command"], row["platform"],
            row["exit_code"], row["status"],
        )
        if log["identity_sha256"] != expected_identity:
            raise AssertionError("raw retained log identity is stale")
    return row


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-dependency-evidence-") as directory:
        parent = Path(directory)
        root = parent / "repo"
        root.mkdir()
        policy = initialize(root)
        tooling = fake_tooling(root)
        try:
            clean_build = parent / "clean-build"
            build_fixture(clean_build)
            clean_output = parent / "clean"
            code = collector.collect(root, clean_build, clean_output, "fixture-owner")
            if code != 0:
                raise AssertionError("clean graph fixture did not pass")
            clean_manifest = clean_output / "dependency-graph.raw.json"
            clean = validate_raw(clean_manifest, root, policy, "passed")
            if any(row["legacy_edge_count"] != 0 for row in clean["payload"]["graphs"].values()):
                raise AssertionError("clean graph fixture reported legacy edges")

            legacy_build = parent / "legacy-build"
            build_fixture(legacy_build, legacy=True)
            failed_output = parent / "failed"
            code = collector.collect(root, legacy_build, failed_output, "fixture-owner")
            if code != 1:
                raise AssertionError("legacy graph fixture did not fail honestly")
            failed = validate_raw(
                failed_output / "dependency-graph.raw.json", root, policy, "failed"
            )
            if failed["exit_code"] != 1:
                raise AssertionError("failed raw evidence used a zero exit code")
            if failed["payload"]["graphs"]["build-ninja"]["legacy_edge_count"] != 1:
                raise AssertionError("legacy graph edge was not counted")
            if all(log["result"] == "passed" for log in failed["logs"]):
                raise AssertionError("failed raw evidence marked every log passed")

            stale = copy.deepcopy(failed)
            stale["repository_sha256"] = "0" * 64
            if stale == failed:
                raise AssertionError("identity mutation was not applied")
            try:
                collector.collect(root, clean_build, clean_output, "fixture-owner")
            except collector.CollectionError:
                pass
            else:
                raise AssertionError("collector overwrote existing raw evidence")
        finally:
            collector.run_text = tooling["restore"]
    print("target-machine dependency-graph evidence self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("only --self-test is supported")
    try:
        return self_test()
    except (AssertionError, OSError, ValueError) as error:
        print(f"target-machine dependency-graph evidence self-test: FAIL: {error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
