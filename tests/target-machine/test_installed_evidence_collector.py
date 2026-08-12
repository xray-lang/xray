#!/usr/bin/env python3
"""Self-test the identity-bound installed-tree evidence collector."""

from __future__ import annotations

import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import collect_target_machine_installed_evidence as collector  # noqa: E402


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
        "schema": 2,
        "checker": "target-machine-completion-governance/2",
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
            "forbidden_path_regex": r"(?i)(?:^|/)[^/]*\.xrc$",
            "forbidden_text_regex": r"LEGACY_INSTALL_TEXT",
            "required_deliverables": list(collector.DELIVERABLE_PATTERNS),
            "required_public_headers": ["include/xray/runtime.h"],
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


def build_fixture(build: Path) -> None:
    build.mkdir()
    (build / "CMakeCache.txt").write_text(
        "CMAKE_GENERATOR:INTERNAL=Ninja\n"
        "CMAKE_C_COMPILER:FILEPATH=C:/fixture/cl.exe\n",
        encoding="utf-8",
    )
    (build / "build.ninja").write_text("# fixture\n", encoding="utf-8")


def populate(prefix: Path) -> None:
    files = {
        "bin/xray.exe": "binary",
        "lib/xray/aot/x86_64-windows-msvc/xray_rt_coro.lib": "exec",
        "lib/xray/compiler/x86_64-windows-msvc/xray_compiler.lib": "compiler",
        "lib/xray/vm/x86_64-windows-msvc/xray_vm.lib": "vm",
        "include/xray/runtime.h": "#define RUNTIME 1\n",
        "share/xray/install/aot-sdk-closure.json": "",
        "share/xray/install/payload-manifest.json": "",
    }
    for relative, text in files.items():
        path = prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if relative.endswith("payload-manifest.json"):
            continue
        path.write_text(text, encoding="utf-8")
    runtime = prefix / "include/xray/runtime.h"
    write_json(prefix / "share/xray/install/aot-sdk-closure.json", {
        "schema": 1,
        "generator": "xray-aot-sdk-header-closure/1",
        "entries": [{
            "source_path": "include/runtime.h",
            "install_path": "include/xray/runtime.h",
            "sha256": assembler.sha256_file(runtime),
        }],
    })


def fake_install(mutation: str = "") -> Callable[[Path, Path], tuple[int, list[str], str]]:
    calls = 0

    def install(build: Path, prefix: Path) -> tuple[int, list[str], str]:
        nonlocal calls
        calls += 1
        command = ["cmake", "--install", str(build), "--prefix", str(prefix)]
        populate(prefix)
        identity = collector.repository_identity(build.parent / "repo")
        write_json(prefix / "share/xray/install/payload-manifest.json", {
            "commit": identity["source_commit"], "dirty": False,
        })
        if mutation == "missing":
            (prefix / "include/xray/runtime.h").unlink()
        if mutation == "residue":
            path = prefix / "lib/xray/legacy.xrc"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("LEGACY_INSTALL_TEXT\n", encoding="utf-8")
        if mutation == "sdk-extra":
            path = prefix / "lib/xray/sdk/src/unexpected.h"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#define UNEXPECTED 1\n", encoding="utf-8")
        if mutation == "replay" and calls == 2:
            (prefix / "share/replayed.txt").parent.mkdir(parents=True, exist_ok=True)
            (prefix / "share/replayed.txt").write_text("changed\n", encoding="utf-8")
        return 0, command, f"fixture install {calls}\n"

    return install


def validate_raw(path: Path, root: Path, policy: dict[str, Any],
                 status: str) -> dict[str, Any]:
    row = assembler.read_object(path)
    if set(row) != assembler.RAW_FIELDS or row["status"] != status:
        raise AssertionError("raw installed manifest shape/status is not exact")
    identity = collector.repository_identity(root)
    for field in ("source_commit", "repository_sha256"):
        if row[field] != identity[field]:
            raise AssertionError(f"raw installed {field} is stale")
    if row["governance_input_sha256"] != policy["input_identity"]["sha256"]:
        raise AssertionError("raw installed governance identity is stale")
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
            raise AssertionError("raw installed retained-log identity is stale")
    return row


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-installed-evidence-") as directory:
        parent = Path(directory)
        root = parent / "repo"
        root.mkdir()
        policy = initialize(root)
        build = parent / "build"
        build_fixture(build)
        original = collector.run_install
        try:
            collector.run_install = fake_install()
            clean_output = parent / "clean"
            if collector.collect(root, build, clean_output, "fixture-owner") != 0:
                raise AssertionError("clean installed fixture did not pass")
            clean = validate_raw(clean_output / "installed.raw.json", root, policy, "passed")
            payload = clean["payload"]
            if payload["empty_tree_sha256"] != payload["installed_tree_sha256"]:
                raise AssertionError("clean no-work replay changed the installed tree")
            if Path(payload["install_root"]).resolve() != (clean_output / "install").resolve():
                raise AssertionError("published install root identity is stale")
            stage = parent / "assembly-stage"
            stage.mkdir()
            envelope = assembler.validate_raw_manifest(
                clean_output / "installed.raw.json", clean_output, "installed",
                collector.repository_identity(root), policy["input_identity"]["sha256"], stage,
            )
            verified = {row["path"] for row in envelope["logs"]}
            findings = assembler.completion.installed_findings(
                envelope, verified, stage, policy
            )
            if findings:
                raise AssertionError(f"clean installed envelope was rejected: {findings}")

            for name, mutation, field in (
                ("missing", "missing", "absent_public_headers"),
                ("residue", "residue", "residue_count"),
                ("replay", "replay", "no_work_replay"),
                ("sdk-extra", "sdk-extra", "aot_sdk_closure"),
            ):
                collector.run_install = fake_install(mutation)
                output = parent / name
                if collector.collect(root, build, output, "fixture-owner") != 1:
                    raise AssertionError(f"{name} mutation did not fail honestly")
                failed = validate_raw(output / "installed.raw.json", root, policy, "failed")
                if field == "residue_count" and failed["payload"][field] == 0:
                    raise AssertionError("residue mutation was not counted")
                if field == "absent_public_headers" and not failed["payload"][field]:
                    raise AssertionError("missing header mutation was not reported")
                if field == "no_work_replay" and failed["payload"][field] != "failed":
                    raise AssertionError("no-work mutation was not reported")
                if field == "aot_sdk_closure" and failed["payload"][field] != "failed":
                    raise AssertionError("unexpected SDK file mutation was not reported")

            try:
                collector.run_install = fake_install()
                collector.collect(root, build, clean_output, "fixture-owner")
            except collector.CollectionError:
                pass
            else:
                raise AssertionError("collector overwrote an existing evidence package")

            stale = copy.deepcopy(clean)
            stale["repository_sha256"] = "0" * 64
            if stale == clean:
                raise AssertionError("identity mutation was not applied")
        finally:
            collector.run_install = original
    print("target-machine installed evidence self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else 2


if __name__ == "__main__":
    raise SystemExit(main())
