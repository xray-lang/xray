#!/usr/bin/env python3
"""Mutation tests for the fail-closed completion evidence assembler."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def run(arguments: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(arguments, cwd=cwd, check=False, capture_output=True,
                          text=True, encoding="utf-8")


def git(arguments: list[str], cwd: Path) -> str:
    result = run(["git", *arguments], cwd)
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return result.stdout.strip()


def framed_tree_hash(root: Path) -> str:
    paths = [item for item in git(["ls-files"], root).splitlines() if item]
    digest = hashlib.sha256()
    for relative in sorted(paths):
        name = relative.replace("\\", "/").encode("utf-8")
        content = (root / relative).read_bytes()
        digest.update(len(name).to_bytes(8, "big"))
        digest.update(name)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def init_repository(root: Path) -> dict[str, str]:
    (root / "contracts/target-machine").mkdir(parents=True)
    (root / "scripts").mkdir()
    (root / "src").mkdir()
    (root / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
    (root / "src/authority.h").write_text("#define FIXTURE_AUTHORITY 1\n",
                                           encoding="utf-8")
    (root / "scripts/check_target_machine_completion.py").write_bytes(
        (ROOT / "scripts/check_target_machine_completion.py").read_bytes()
    )
    inventories = {
        "legacy_vm": ("legacy-vm.json", {
            "opcode_count": 0, "opcodes": [], "tagged_frame_sites": [],
            "vm_public_api_symbols": [], "legacy_artifact_symbols": [],
            "artifact": {},
        }),
        "legacy_product": ("legacy-product.json", {"total": 0, "owner_count": 0}),
        "aot_plan": ("aot-plan.json", {
            "row_count": 0, "rows": [], "mixed_representation_types": {},
        }),
    }
    for _, (name, value) in inventories.items():
        write_json(root / "contracts/target-machine" / name, value)
    write_json(root / "contracts/target-machine/semantic-owner.json", {
        "operation_count": 0, "operations": [],
    })
    write_json(root / "contracts/target-machine/validation-matrix.json", {
        "rows": [],
    })
    write_json(root / "contracts/target-machine/baseline-manifest.json", {
        "runner": "target-machine-baseline/3",
        "qualification": {
            "result": "passed", "evidence_sha256": "a" * 64,
        },
    })
    git(["init"], root)
    git(["config", "user.email", "fixture@example.invalid"], root)
    git(["config", "user.name", "Fixture"], root)
    git(["add", "."], root)
    git(["commit", "-m", "fixture"], root)
    return {
        "source_commit": git(["rev-parse", "HEAD"], root),
        "repository_sha256": framed_tree_hash(root),
    }


def governance(identity_hash: str) -> dict[str, Any]:
    kinds = [
        "activation-generation", "dependency-graph", "full-validation",
        "installed", "matrix", "runtime-reachability", "symbol",
    ]
    return {
        "schema": 2,
        "checker": "target-machine-completion-governance/2",
        "policy": {
            "accepted_evidence_status": ["passed", "unsupported"],
            "compatibility_or_fallback": "forbidden",
            "missing_or_unclassified": "error",
            "residue_count": 0,
            "self_certifying_write": "forbidden",
            "skip_or_allowlist": "forbidden",
        },
        "input_identity": {
            "algorithm": "sha256", "files": ["CMakeLists.txt"],
            "sha256": identity_hash,
        },
        "authorities": [{
            "id": "fixture", "path": "src/authority.h",
            "required_regex": ["FIXTURE_AUTHORITY"],
        }],
        "residue_scan": {
            "definition_paths": ["scripts/check_target_machine_completion.py"],
            "roots": ["CMakeLists.txt", "scripts", "src"],
            "rules": [{
                "id": "fixture", "path_regex": "(?!)", "text_regex": "(?!)",
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
            "required_deliverables": [], "required_public_headers": [],
        },
        "matrix": {
            "policy": "contracts/target-machine/validation-matrix.json",
            "qualifying_tiers": ["supported", "ci-only"],
            "required_artifact_routes": [], "required_dimensions": [],
        },
        "runtime_reachability": {"required_lanes": {}},
        "validation": {"required_lanes": []},
    }


def payload(kind: str, log: str, raw: Path) -> dict[str, Any]:
    if kind == "dependency-graph":
        return {
            "graphs": {
                name: {"status": "passed", "legacy_edge_count": 0, "log": log}
                for name in assembler.completion.GRAPH_KINDS
            },
            "targets": [],
        }
    if kind == "symbol":
        return {"binaries": []}
    if kind == "installed":
        install_root = raw / "installed"
        header = install_root / "include/xray/runtime.h"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("/* fixture runtime API */\n", encoding="utf-8")
        write_json(install_root / "share/xray/install/aot-sdk-closure.json", {
            "schema": 1,
            "generator": "xray-aot-sdk-header-closure/1",
            "entries": [{
                "source_path": "include/runtime.h",
                "install_path": "include/xray/runtime.h",
                "sha256": assembler.sha256_file(header),
            }],
        })
        return {
            "empty_stage_replay": "passed", "no_work_replay": "passed",
            "deliverables": [], "public_headers": [],
            "install_root": str(install_root.resolve()), "inventory_log": log,
        }
    if kind == "runtime-reachability":
        return {
            "lanes": {}, "activation_before_verify": 0,
            "forbidden_link_symbol_count": 0,
            "external_activation_canary": "passed",
        }
    if kind == "matrix":
        return {"axis_catalog": {}, "rows": []}
    if kind == "activation-generation":
        return {
            "lifecycle": {
                name: {"result": "passed", "log": log}
                for name in ("activate", "drain", "rollback", "unload")
            },
            "negative_mismatches": {
                name: "rejected-before-activation"
                for name in ("capability", "fingerprint", "provider", "schema", "target")
            },
            "identities": {
                name: "b" * 64
                for name in ("artifact", "generation", "semantic", "target")
            },
            "activation_before_verify": 0, "runtime_only_compiler_symbols": 0,
            "artifact_routes": [],
            "route_proofs": {
                name: {"result": "passed", "log": log}
                for name in ("hosted-fragment", "target-plan-to-native")
            },
        }
    if kind == "full-validation":
        build_identity = {
            "build_root": "${BUILD_ROOT}",
            "source_root": "${SOURCE_ROOT}",
            "generator": "Ninja",
            "build_type": "Release",
            "export_compile_commands": "ON",
            "stdlib_vm_fastpaths": "OFF",
        }
        build_identity["sha256"] = hashlib.sha256(json.dumps(
            build_identity, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")).hexdigest()
        return {
            "producer": "target-machine-full-validation-evidence/3",
            "baseline_runner": "target-machine-baseline/3",
            "build_identity": build_identity,
            "baseline_manifest": log,
            "lanes": [],
        }
    raise AssertionError(f"unhandled fixture kind: {kind}")


def raw_fixture(raw: Path, identity: dict[str, str], governance_hash: str,
                kind: str) -> dict[str, Any]:
    relative = f"logs/{kind}.log"
    log = raw / relative
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(f"{kind}: passed\n", encoding="utf-8")
    digest = assembler.sha256_file(log)
    command = ["fixture-runner", "--kind", kind]
    platform = {"os": "fixture-os", "arch": "fixture-arch",
                "toolchain": "fixture-toolchain"}
    owner = "fixture-owner"
    generated_at = "2026-08-12T00:00:00Z"
    return {
        "schema": 1, "kind": kind, "status": "passed", "exit_code": 0,
        "source_commit": identity["source_commit"],
        "repository_sha256": identity["repository_sha256"],
        "governance_input_sha256": governance_hash,
        "owner": owner, "generated_at": generated_at,
        "command": command, "platform": platform,
        "payload": payload(kind, relative, raw),
        "logs": [{
            "path": relative, "sha256": digest, "result": "passed",
            "identity_sha256": assembler.raw_log_identity(
                kind, identity["source_commit"], identity["repository_sha256"],
                governance_hash, relative, digest, owner, generated_at,
                command, platform, 0, "passed",
            ),
        }],
    }


def expect_failure(label: str, action, output: Path, results: list[str]) -> None:
    try:
        action()
    except assembler.AssemblyError:
        if output.exists():
            raise AssertionError(f"{label}: failure left partial output")
        if list(output.parent.glob(f".{output.name}-staging-*")):
            raise AssertionError(f"{label}: failure left a staging directory")
        if output.with_name(f".{output.name}.publish-lock").exists():
            raise AssertionError(f"{label}: failure left a publication lock")
        results.append(label)
        return
    raise AssertionError(f"{label}: mutation unexpectedly passed")


def self_test() -> int:
    results: list[str] = []
    with tempfile.TemporaryDirectory(prefix="xray-completion-assembler-") as directory:
        root = Path(directory) / "repo"
        raw = Path(directory) / "raw"
        root.mkdir()
        raw.mkdir()
        identity = init_repository(root)
        input_hash = assembler.completion.framed_tree_hash(root, ["CMakeLists.txt"])
        policy = governance(input_hash)
        policy_path = root / "contracts/target-machine/completion-governance.json"
        write_json(policy_path, policy)
        git(["add", str(policy_path.relative_to(root))], root)
        git(["commit", "-m", "governance"], root)
        identity = {
            "source_commit": git(["rev-parse", "HEAD"], root),
            "repository_sha256": framed_tree_hash(root),
        }
        lanes: dict[str, str] = {}
        rows: dict[str, dict[str, Any]] = {}
        for kind in policy["evidence"]["required_files"]:
            row = raw_fixture(raw, identity, input_hash, kind)
            path = raw / f"{kind}.raw.json"
            write_json(path, row)
            lanes[kind] = path.name
            rows[kind] = row
        bundle_path = raw / "bundle.json"
        bundle = {
            "schema": 1, "assembler": assembler.ASSEMBLER,
            "source_commit": identity["source_commit"],
            "repository_sha256": identity["repository_sha256"],
            "governance_input_sha256": input_hash,
            "lanes": lanes,
        }
        write_json(bundle_path, bundle)

        output = Path(directory) / "out-dirty"
        (root / "dirty.txt").write_text("dirty\n", encoding="utf-8")
        expect_failure("dirty-source", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        (root / "dirty.txt").unlink()

        mutations = [
            ("stale-commit", "source_commit", "0" * 40),
            ("stale-tree", "repository_sha256", "1" * 64),
            ("stale-governance", "governance_input_sha256", "2" * 64),
            ("failed-status", "status", "failed"),
            ("nonzero-exit", "exit_code", 1),
            ("missing-owner", "owner", ""),
            ("invalid-time", "generated_at", "2026-08-12T00:00:00"),
            ("missing-command", "command", []),
            ("missing-platform", "platform", {"os": "fixture-os"}),
        ]
        kind = "symbol"
        row_path = raw / lanes[kind]
        for label, field, value in mutations:
            changed = copy.deepcopy(rows[kind])
            changed[field] = value
            write_json(row_path, changed)
            output = Path(directory) / f"out-{label}"
            expect_failure(label, lambda o=output: assembler.assemble(
                root, policy_path, bundle_path, o), output, results)
        write_json(row_path, rows[kind])

        changed = copy.deepcopy(rows[kind])
        changed["payload"] = {"binaries": "not-a-list"}
        write_json(row_path, changed)
        output = Path(directory) / "out-final-verifier"
        expect_failure("final-verifier", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        write_json(row_path, rows[kind])

        changed = copy.deepcopy(rows[kind])
        changed["logs"][0]["sha256"] = "3" * 64
        write_json(row_path, changed)
        output = Path(directory) / "out-log-digest"
        expect_failure("log-digest", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        changed = copy.deepcopy(rows[kind])
        changed["logs"][0]["identity_sha256"] = "4" * 64
        write_json(row_path, changed)
        output = Path(directory) / "out-log-identity"
        expect_failure("log-identity", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        changed = copy.deepcopy(rows[kind])
        changed["logs"][0]["result"] = "failed"
        write_json(row_path, changed)
        output = Path(directory) / "out-log-result"
        expect_failure("log-result", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        write_json(row_path, rows[kind])

        incomplete = copy.deepcopy(bundle)
        del incomplete["lanes"][kind]
        write_json(bundle_path, incomplete)
        output = Path(directory) / "out-missing-kind"
        expect_failure("missing-kind", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        extra = copy.deepcopy(bundle)
        extra["lanes"]["unclassified"] = lanes[kind]
        write_json(bundle_path, extra)
        output = Path(directory) / "out-extra-kind"
        expect_failure("extra-kind", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        write_json(bundle_path, bundle)

        inventory_path = root / "contracts/target-machine/legacy-vm.json"
        inventory = read_json(inventory_path)
        inventory["opcode_count"] = 1
        inventory["opcodes"] = [{"opcode": "forged"}]
        write_json(inventory_path, inventory)
        output = Path(directory) / "out-inventory"
        expect_failure("terminal-inventory", lambda: assembler.assemble(
            root, policy_path, bundle_path, output), output, results)
        git(["restore", str(inventory_path.relative_to(root))], root)

        output = Path(directory) / "published"
        assembler.assemble(root, policy_path, bundle_path, output)
        checker = run([
            sys.executable, str(root / "scripts/check_target_machine_completion.py"),
            "--root", str(root), "--manifest", str(policy_path),
            "--evidence-root", str(output), "--json",
        ], root)
        if checker.returncode != 0:
            raise AssertionError(
                f"published fixture failed the real checker: {checker.stdout}{checker.stderr}"
            )
        report = json.loads(checker.stdout)
        if report.get("ok") is not True:
            raise AssertionError("published fixture was not terminally valid")
        expected_files = set(policy["evidence"]["required_files"].values())
        actual_files = {
            path.relative_to(output).as_posix()
            for path in output.iterdir() if path.is_file()
        }
        if actual_files != expected_files:
            raise AssertionError("publication did not contain exactly seven evidence envelopes")
        results.append("successful-publication")

        output = Path(directory) / "existing-output"
        output.mkdir()
        marker = output / "marker"
        marker.write_text("keep\n", encoding="utf-8")
        try:
            assembler.assemble(root, policy_path, bundle_path, output)
        except assembler.AssemblyError:
            if marker.read_text(encoding="utf-8") != "keep\n":
                raise AssertionError("existing output was modified")
            results.append("existing-output")
        else:
            raise AssertionError("existing output mutation unexpectedly passed")

    expected = {
        "dirty-source", "stale-commit", "stale-tree", "stale-governance",
        "failed-status", "nonzero-exit", "missing-owner", "invalid-time",
        "missing-command", "missing-platform", "log-digest", "log-identity",
        "log-result", "final-verifier", "missing-kind", "extra-kind",
        "terminal-inventory",
        "successful-publication", "existing-output",
    }
    if set(results) != expected:
        raise AssertionError(f"mutation mismatch: {sorted(results)}")
    print(
        "target-machine completion evidence assembler self-test: PASS "
        f"({len(results) - 1} mutations + successful publication)"
    )
    return 0


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise AssertionError(f"{path} is not an object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("only --self-test is supported")
    try:
        return self_test()
    except (AssertionError, OSError, ValueError) as error:
        print(f"target-machine completion evidence assembler self-test: FAIL: {error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
