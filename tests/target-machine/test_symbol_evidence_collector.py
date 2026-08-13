#!/usr/bin/env python3
"""Self-test the identity-bound target-machine symbol evidence collector."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import collect_target_machine_symbol_evidence as collector  # noqa: E402


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
    for name, value in {
        "semantic-owner.json": {"operation_count": 0, "operations": []},
        "legacy-vm.json": {"opcode_count": 0, "opcodes": [],
                           "tagged_frame_sites": [], "vm_public_api_symbols": [],
                           "legacy_artifact_symbols": [], "artifact": {}},
        "legacy-product.json": {"total": 0, "owner_count": 0},
        "aot-plan.json": {"row_count": 0, "rows": [],
                          "mixed_representation_types": {}},
        "validation-matrix.json": {"rows": []},
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
        "residue_scan": {"definition_paths": [], "roots": ["src"], "rules": []},
        "evidence": {"required_files": {kind: f"{kind}.json" for kind in (
            "activation-generation", "dependency-graph", "full-validation",
            "installed", "matrix", "runtime-reachability", "symbol")}},
        "inventories": {"legacy_vm": "contracts/target-machine/legacy-vm.json",
                        "legacy_product": "contracts/target-machine/legacy-product.json",
                        "aot_plan": "contracts/target-machine/aot-plan.json"},
        "dual_owner": {"inventory": "contracts/target-machine/semantic-owner.json",
                       "mechanical_adapter": "representation adapter"},
        "installed": {"forbidden_path_regex": "(?!)", "forbidden_text_regex": "(?!)",
                      "required_deliverables": list(collector.TARGET_FILES),
                      "required_public_headers": []},
        "matrix": {"policy": "contracts/target-machine/validation-matrix.json",
                   "qualifying_tiers": [], "required_artifact_routes": [],
                   "required_dimensions": []},
        "runtime_reachability": {"required_lanes": {}},
        "validation": {"required_lanes": []},
    }
    policy["residue_scan"]["rules"] = [{
        "id": "fixture-residue", "path_regex": "(?!)", "text_regex": "(?!)",
    }]
    write_json(root / "contracts/target-machine/completion-governance.json", policy)
    run(["git", "add", "."], root)
    run(["git", "commit", "-m", "governance"], root)
    return policy


def build_fixture(build: Path, legacy: bool = False,
                  missing_authority: bool = False) -> None:
    build.mkdir()
    for name, filenames in collector.TARGET_FILES.items():
        path = build / filenames[0 if collector.os.name == "nt" else 1]
        content = "CLEAN"
        if legacy and name == "libxray-vm":
            content = "LEGACY"
        elif missing_authority and name == "libxray-compiler":
            content = "MISSING_AUTHORITY"
        path.write_text(content, encoding="utf-8")


def validate(path: Path, root: Path, policy: dict[str, Any], status: str) -> dict[str, Any]:
    row = assembler.read_object(path)
    if set(row) != assembler.RAW_FIELDS or row["status"] != status:
        raise AssertionError("raw symbol manifest shape/status is not exact")
    identity = collector.repository_identity(root)
    if row["source_commit"] != identity["source_commit"] or \
       row["repository_sha256"] != identity["repository_sha256"] or \
       row["governance_input_sha256"] != policy["input_identity"]["sha256"]:
        raise AssertionError("raw symbol manifest identity is stale")
    for log in row["logs"]:
        path_on_disk = path.parent / log["path"]
        digest = assembler.sha256_file(path_on_disk)
        if log["sha256"] != digest:
            raise AssertionError("raw symbol log digest is stale")
        expected = assembler.raw_log_identity(
            collector.KIND, row["source_commit"], row["repository_sha256"],
            row["governance_input_sha256"], log["path"], digest, row["owner"],
            row["generated_at"], row["command"], row["platform"],
            row["exit_code"], row["status"],
        )
        if log["identity_sha256"] != expected:
            raise AssertionError("raw symbol log identity is stale")
    return row


def self_test() -> int:
    if not collector.RETIRED_RUNTIME_EXACT or not all(
        collector.retired_runtime.matches(symbol)
        and collector.retired_runtime.compiled_pattern().match(symbol)
        for symbol in collector.RETIRED_RUNTIME_EXACT
    ):
        raise AssertionError("retired exact runtime symbols are not detected")
    if collector.retired_runtime.matches("xr_runtime_target_plan_load"):
        raise AssertionError("current runtime authority was classified as retired")
    with tempfile.TemporaryDirectory(prefix="xray-symbol-evidence-") as directory:
        parent = Path(directory)
        root = parent / "repo"
        root.mkdir()
        policy = initialize(root)
        original_symbols = collector.binlib.defined_symbol_names
        original_nm = collector.binlib.find_nm
        original_dumpbin = collector.binlib.find_dumpbin
        collector.binlib.find_nm = lambda: "fixture-nm"
        collector.binlib.find_dumpbin = lambda: None
        def fixture_symbols(path: Path) -> list[str]:
            content = path.read_text(encoding="utf-8")
            if content == "LEGACY":
                return [sorted(collector.RETIRED_RUNTIME_EXACT)[0], "clean_symbol"]
            symbols = ["clean_symbol"]
            authorities = sorted(collector.REQUIRED_AUTHORITY_SYMBOLS.get(
                "libxray-compiler", set()
            ))
            symbols.extend(authorities[1:] if content == "MISSING_AUTHORITY"
                           else authorities)
            return symbols

        collector.binlib.defined_symbol_names = fixture_symbols
        try:
            clean_build = parent / "clean-build"
            build_fixture(clean_build)
            clean_output = parent / "clean"
            if collector.collect(root, clean_build, clean_output, "fixture-owner") != 0:
                raise AssertionError("clean symbol fixture did not pass")
            clean = validate(clean_output / "symbol.raw.json", root, policy, "passed")
            if any(row["forbidden_symbol_count"] for row in clean["payload"]["binaries"]):
                raise AssertionError("clean symbol fixture reported residue")

            legacy_build = parent / "legacy-build"
            build_fixture(legacy_build, legacy=True)
            failed_output = parent / "failed"
            if collector.collect(root, legacy_build, failed_output, "fixture-owner") != 1:
                raise AssertionError("legacy symbol fixture did not fail honestly")
            failed = validate(failed_output / "symbol.raw.json", root, policy, "failed")
            if sum(row["forbidden_symbol_count"] for row in failed["payload"]["binaries"]) != 1:
                raise AssertionError("legacy symbol count is not exact")

            missing_build = parent / "missing-authority-build"
            build_fixture(missing_build, missing_authority=True)
            missing_output = parent / "missing-authority"
            if collector.collect(root, missing_build, missing_output,
                                 "fixture-owner") != 1:
                raise AssertionError("missing compiler authority did not fail honestly")
            missing = validate(
                missing_output / "symbol.raw.json", root, policy, "failed"
            )
            compiler = next(
                row for row in missing["payload"]["binaries"]
                if row["name"] == "libxray-compiler"
            )
            log = (missing_output / compiler["symbol_log"]).read_text(
                encoding="utf-8"
            )
            if "missing_authority_symbols=1" not in log:
                raise AssertionError("missing compiler authority was not retained")
            try:
                collector.collect(root, clean_build, clean_output, "fixture-owner")
            except collector.CollectionError:
                pass
            else:
                raise AssertionError("collector overwrote existing raw evidence")
        finally:
            collector.binlib.defined_symbol_names = original_symbols
            collector.binlib.find_nm = original_nm
            collector.binlib.find_dumpbin = original_dumpbin
    print("target-machine symbol evidence self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
