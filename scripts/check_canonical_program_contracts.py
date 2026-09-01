#!/usr/bin/env python3
"""Validate the Task 294 canonical-program contract freeze.

The seven files in contracts/canonical-program are the machine-readable handoff
from architecture design to the W1-W9 implementation train.  This checker is
deliberately strict: malformed data, duplicate ownership, incomplete migration
rows, non-canonical JSON, and an increase in frozen legacy residue all fail
closed.  It does not activate a product path.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import sys
import tempfile
import tomllib
from pathlib import Path
from typing import Any


CONTRACT_DIR = Path("contracts/canonical-program")
JSON_FILES = (
    "baseline-manifest.json",
    "forbidden-residue.json",
    "legacy-owner-migration.json",
    "operation-capability-matrix.json",
    "task-portfolio.json",
)
REQUIRED_FILES = (
    "architecture-identity.toml",
    *JSON_FILES,
    "retired-evidence.tsv",
)
REQUIRED_TASKS = {
    275,
    281,
    *range(283, 304),
}
WORK_PACKAGES = {f"W{i}" for i in range(10)} | {"DOMAIN", "HISTORICAL", "INDEPENDENT"}
REQUIRED_WALKING_OPERATIONS = {
    "core.add.i64",
    "core.block.argument",
    "core.branch",
    "core.call.sealed_direct",
    "core.compare.i64",
    "core.conditional_branch",
    "core.constant.i64",
    "core.mul.i64",
    "core.return",
    "core.sub.i64",
}
REQUIRED_CAPABILITY_GROUPS = {
    "aggregate",
    "coroutine",
    "ffi",
    "interface",
    "ownership",
    "provider",
}
MIGRATION_FIELDS = {
    "activation_wave",
    "current_owner",
    "deletion_owner",
    "disposition",
    "evidence",
    "id",
    "new_owner",
    "semantic_value_to_keep",
}
OPERATION_FIELDS = {
    "aot_lowering",
    "core_spec_contract",
    "decoder_verifier_evaluator",
    "evidence",
    "id",
    "old_owner_deletion",
    "program_encoding",
    "source_producer",
    "status",
    "vm_implementation",
}
TSV_HEADER = (
    "evidence_id",
    "source_task",
    "source_path",
    "classification",
    "semantic_value_to_keep",
    "destination",
    "activation_wave",
    "deletion_owner",
    "notes",
)


class ContractError(ValueError):
    """Raised for a fail-closed contract violation."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as exc:
        raise ContractError(f"cannot read {path}: {exc}") from exc


def load_json(path: Path) -> dict[str, Any]:
    raw = read_text(path)
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ContractError(f"invalid JSON in {path}: {exc}") from exc
    require(isinstance(value, dict), f"{path} must contain a JSON object")
    canonical = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    require(raw == canonical, f"{path} is not canonical two-space JSON with a final newline")
    return value


def require_unique(rows: list[dict[str, Any]], field: str, owner: str) -> None:
    values = [row.get(field) for row in rows]
    require(all(isinstance(value, (str, int)) and value != "" for value in values),
            f"{owner} has an empty {field}")
    require(len(values) == len(set(values)), f"{owner} has duplicate {field} values")


def validate_identity(path: Path) -> None:
    try:
        data = tomllib.loads(read_text(path))
    except tomllib.TOMLDecodeError as exc:
        raise ContractError(f"invalid TOML in {path}: {exc}") from exc
    require(data.get("schema") == "xray-canonical-program-identity/1",
            "architecture identity schema must be v1")
    public = data.get("public", {})
    require(public == {
        "program": "XrProgram",
        "profile": "XrTargetProfile",
        "instance": "XrInstance",
    }, "public architecture names are not the frozen XrProgram/profile/instance trio")
    require(data.get("grain", {}).get("program") == "whole-linked-program",
            "XrProgram grain must be whole-linked-program")
    require(data.get("grain", {}).get("source") == "source-module",
            "source grain must remain source-module")
    require(data.get("compatibility", {}).get("pre_293") == "none",
            "pre-293 compatibility must be none")
    forbidden = data.get("forbidden_aliases", {}).get("names", [])
    require(set(forbidden) == {"XrActivation", "XrExecutableModule", "XrSemanticImage"},
            "forbidden architecture alias set drifted")


def validate_portfolio(data: dict[str, Any]) -> None:
    require(data.get("schema") == "xray-canonical-program-task-portfolio/1",
            "task portfolio schema must be v1")
    predecessors = data.get("predecessor_classification")
    require(isinstance(predecessors, list), "predecessor classification must be an array")
    require_unique(predecessors, "task_id", "predecessor classification")
    predecessor_ids = {row["task_id"] for row in predecessors}
    require(predecessor_ids == set(range(269, 293)),
            "predecessor classification must cover every task from 269 through 292")
    for row in predecessors:
        require(row.get("classification") in {
            "domain_contract", "historical_tombstone", "independent_port"
        }, f"task {row['task_id']} has an invalid predecessor classification")
        require(isinstance(row.get("implementation_route"), str) and row["implementation_route"],
                f"task {row['task_id']} lacks an implementation route")
    tasks = data.get("tasks")
    require(isinstance(tasks, list), "task portfolio tasks must be an array")
    require_unique(tasks, "task_id", "task portfolio")
    ids = {row["task_id"] for row in tasks}
    require(ids == REQUIRED_TASKS,
            f"task portfolio ids differ: missing={sorted(REQUIRED_TASKS - ids)} extra={sorted(ids - REQUIRED_TASKS)}")
    required = {
        "contract_inputs", "deletion_wave", "downstream_handoff", "earliest_activation",
        "exit_evidence", "non_goals", "owned_outputs", "prerequisites", "required_gates",
        "role", "status", "task_id", "work_package",
    }
    for row in tasks:
        missing = required - row.keys()
        require(not missing, f"task {row.get('task_id')} lacks fields {sorted(missing)}")
        require(row["work_package"] in WORK_PACKAGES,
                f"task {row['task_id']} has unknown work package {row['work_package']}")
        for field in ("contract_inputs", "exit_evidence", "non_goals", "owned_outputs",
                      "prerequisites", "required_gates"):
            require(isinstance(row[field], list), f"task {row['task_id']} {field} must be an array")
        require(row["exit_evidence"], f"task {row['task_id']} has no exit evidence")


def validate_baseline(data: dict[str, Any]) -> None:
    require(data.get("schema") == "xray-canonical-program-baseline/1",
            "baseline schema must be v1")
    source = data.get("source", {})
    for field in ("branch", "commit", "tree", "worktree"):
        require(isinstance(source.get(field), str) and source[field], f"baseline source.{field} is empty")
    require(source.get("dirty") is False, "baseline source must be clean")
    build = data.get("build", {})
    require(build.get("supported_bootstrap", {}).get("status") == "PASS",
            "supported bootstrap build must pass")
    require(build.get("default_fastpaths_probe", {}).get("status") == "FAIL_RECORDED",
            "default-fastpath failure must remain an explicit baseline fact")
    tests = data.get("tests", {})
    full = tests.get("full_non_sanitizer", {})
    require(full.get("total") == (full.get("passed", 0) + full.get("failed", 0)
                                  + full.get("skipped", 0)),
            "full CTest passed+failed+skipped does not equal total")
    require(full.get("failed", 0) > 0, "W0 must record the observed red baseline, not erase it")
    clusters = data.get("known_failure_clusters")
    require(isinstance(clusters, list) and clusters, "baseline needs owned failure clusters")
    require_unique(clusters, "id", "known failure clusters")
    for cluster in clusters:
        require(cluster.get("owner_task") in REQUIRED_TASKS,
                f"failure cluster {cluster['id']} has no portfolio owner")
        require(cluster.get("reproduce"), f"failure cluster {cluster['id']} lacks reproduction")


def validate_migration(root: Path, data: dict[str, Any]) -> None:
    require(data.get("schema") == "xray-canonical-program-legacy-owner-migration/1",
            "legacy migration schema must be v1")
    rows = data.get("owners")
    require(isinstance(rows, list) and rows, "legacy migration owners must be a non-empty array")
    require_unique(rows, "id", "legacy owner migration")
    for row in rows:
        missing = MIGRATION_FIELDS - row.keys()
        require(not missing, f"migration {row.get('id')} lacks fields {sorted(missing)}")
        for field in MIGRATION_FIELDS - {"evidence"}:
            require(isinstance(row[field], str) and row[field],
                    f"migration {row.get('id')} has empty {field}")
        require(isinstance(row["evidence"], list) and row["evidence"],
                f"migration {row['id']} has no evidence paths")
    target_contracts = data.get("target_machine_contract_files")
    require(isinstance(target_contracts, list), "target-machine contract decisions must be an array")
    require_unique(target_contracts, "path", "target-machine contract decisions")
    for row in target_contracts:
        require(row.get("disposition") in {"delete", "migrate", "retain", "retire"},
                f"invalid target-machine contract disposition for {row.get('path')}")
        require(row.get("owner_task") in REQUIRED_TASKS,
                f"target-machine contract {row.get('path')} has no portfolio owner")
    actual_contracts = {
        path.relative_to(root).as_posix()
        for path in (root / "contracts/target-machine").iterdir()
        if path.is_file()
    }
    classified_contracts = {row["path"] for row in target_contracts}
    require(classified_contracts == actual_contracts,
            "target-machine contract file classification is incomplete or stale")


def validate_operations(data: dict[str, Any]) -> None:
    require(data.get("schema") == "xray-canonical-program-operation-capability/1",
            "operation matrix schema must be v1")
    operations = data.get("operations")
    require(isinstance(operations, list) and operations, "operation matrix must be non-empty")
    require_unique(operations, "id", "operation matrix")
    ids = {row["id"] for row in operations}
    require(REQUIRED_WALKING_OPERATIONS <= ids,
            f"walking skeleton lacks operations {sorted(REQUIRED_WALKING_OPERATIONS - ids)}")
    for row in operations:
        missing = OPERATION_FIELDS - row.keys()
        require(not missing, f"operation {row.get('id')} lacks fields {sorted(missing)}")
        for field in OPERATION_FIELDS - {"evidence"}:
            require(isinstance(row[field], str) and row[field],
                    f"operation {row.get('id')} has empty {field}")
        require(isinstance(row["evidence"], list), f"operation {row['id']} evidence must be an array")
    groups = data.get("capability_groups")
    require(isinstance(groups, list), "capability groups must be an array")
    require_unique(groups, "id", "capability groups")
    group_ids = {row["id"] for row in groups}
    require(group_ids == REQUIRED_CAPABILITY_GROUPS,
            f"capability groups differ: {sorted(group_ids)}")


def iter_scan_files(root: Path, roots: list[str], suffixes: list[str]) -> list[Path]:
    files: list[Path] = []
    for relative in roots:
        base = root / relative
        require(base.is_dir(), f"residue scan root does not exist: {relative}")
        files.extend(path for path in base.rglob("*") if path.is_file() and path.suffix in suffixes)
    return sorted(set(files))


def validate_residue(root: Path, data: dict[str, Any], scan: bool) -> list[tuple[str, int, int]]:
    require(data.get("schema") == "xray-canonical-program-forbidden-residue/1",
            "forbidden residue schema must be v1")
    entries = data.get("entries")
    require(isinstance(entries, list) and entries, "forbidden residue entries must be non-empty")
    require_unique(entries, "id", "forbidden residue")
    results: list[tuple[str, int, int]] = []
    for row in entries:
        for field in ("baseline_count", "ceiling", "deletion_task", "file_suffixes", "pattern",
                      "policy", "roots"):
            require(field in row, f"residue {row.get('id')} lacks {field}")
        require(row["pattern"], f"residue {row['id']} has an empty pattern")
        require(row["policy"] in {"must_remain_zero", "non_increase_until_delete"},
                f"residue {row['id']} has invalid policy")
        require(isinstance(row["baseline_count"], int) and row["baseline_count"] >= 0,
                f"residue {row['id']} has invalid baseline count")
        require(isinstance(row["ceiling"], int) and 0 <= row["ceiling"] <= row["baseline_count"],
                f"residue {row['id']} has invalid ceiling")
        require(row["deletion_task"] in REQUIRED_TASKS,
                f"residue {row['id']} has no portfolio deletion owner")
        try:
            pattern = re.compile(row["pattern"])
        except re.error as exc:
            raise ContractError(f"residue {row['id']} has invalid regex: {exc}") from exc
        if not scan:
            continue
        count = 0
        for path in iter_scan_files(root, row["roots"], row["file_suffixes"]):
            count += len(pattern.findall(read_text(path)))
        require(count <= row["ceiling"],
                f"residue {row['id']} grew: current={count} ceiling={row['ceiling']}")
        results.append((row["id"], count, row["ceiling"]))
    return results


def validate_retired(path: Path) -> None:
    try:
        with path.open("r", encoding="utf-8", errors="strict", newline="") as stream:
            rows = list(csv.reader(stream, delimiter="\t"))
    except (OSError, UnicodeError, csv.Error) as exc:
        raise ContractError(f"cannot parse {path}: {exc}") from exc
    require(rows and tuple(rows[0]) == TSV_HEADER, "retired evidence TSV header drifted")
    require(len(rows) > 1, "retired evidence TSV must contain evidence rows")
    require(all(len(row) == len(TSV_HEADER) for row in rows[1:]),
            "retired evidence TSV has a ragged row")
    ids = [row[0] for row in rows[1:]]
    require(all(ids) and len(ids) == len(set(ids)), "retired evidence ids must be non-empty and unique")


def validate_all(root: Path, scan_residue: bool = True) -> list[tuple[str, int, int]]:
    directory = root / CONTRACT_DIR
    require(directory.is_dir(), f"missing contract directory {directory}")
    for name in REQUIRED_FILES:
        require((directory / name).is_file(), f"missing canonical-program contract {name}")
    data = {name: load_json(directory / name) for name in JSON_FILES}
    validate_identity(directory / "architecture-identity.toml")
    validate_portfolio(data["task-portfolio.json"])
    validate_baseline(data["baseline-manifest.json"])
    validate_migration(root, data["legacy-owner-migration.json"])
    validate_operations(data["operation-capability-matrix.json"])
    residue = validate_residue(root, data["forbidden-residue.json"], scan_residue)
    validate_retired(directory / "retired-evidence.tsv")
    return residue


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def expect_rejected(root: Path, label: str) -> None:
    try:
        validate_all(root, scan_residue=False)
    except ContractError:
        return
    raise ContractError(f"self-test mutation was accepted: {label}")


def self_test(root: Path) -> None:
    validate_all(root)
    with tempfile.TemporaryDirectory(prefix="xray-canonical-contracts-") as raw:
        scratch = Path(raw)
        target = scratch / CONTRACT_DIR
        target.parent.mkdir(parents=True)
        shutil.copytree(root / CONTRACT_DIR, target)
        shutil.copytree(root / "contracts/target-machine",
                        scratch / "contracts/target-machine")

        portfolio_path = target / "task-portfolio.json"
        portfolio = load_json(portfolio_path)
        portfolio["tasks"] = portfolio["tasks"][1:]
        write_json(portfolio_path, portfolio)
        expect_rejected(scratch, "missing portfolio task")
        shutil.copy2(root / CONTRACT_DIR / portfolio_path.name, portfolio_path)

        operation_path = target / "operation-capability-matrix.json"
        operations = load_json(operation_path)
        operations["operations"].append(operations["operations"][0])
        write_json(operation_path, operations)
        expect_rejected(scratch, "duplicate operation")
        shutil.copy2(root / CONTRACT_DIR / operation_path.name, operation_path)

        residue_path = target / "forbidden-residue.json"
        residue = load_json(residue_path)
        residue["entries"][0]["pattern"] = ""
        write_json(residue_path, residue)
        expect_rejected(scratch, "empty residue pattern")
        shutil.copy2(root / CONTRACT_DIR / residue_path.name, residue_path)

        baseline_path = target / "baseline-manifest.json"
        baseline_path.write_text(read_text(baseline_path).rstrip() + "\n\n", encoding="utf-8")
        expect_rejected(scratch, "non-canonical JSON")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.self_test:
            self_test(root)
            print("canonical-program contract self-test: PASS")
        else:
            residue = validate_all(root)
            print("canonical-program contracts: PASS")
            for identifier, count, ceiling in residue:
                print(f"  {identifier}: {count}/{ceiling}")
    except ContractError as exc:
        print(f"canonical-program contracts: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
