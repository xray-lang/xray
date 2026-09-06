#!/usr/bin/env python3
"""Validate the pre-activation W7 Wave 5 contract and source inventory.

Ordinary mode validates the frozen algebra, authority partition, current source
inventory, and truthful OPEN state. ``--require-ready`` is deliberately red
until implementation updates every governed row and closes every exit
condition. The checker never treats the pre-cut product route as a fallback;
its physical deletion remains owned by the later atomic product cutover.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


class ContractError(ValueError):
    """Raised when the Wave 5 contract or inventory is inconsistent."""


EXPECTED_ATOMS = [
    ("coroutine", 112, "core.task.spawn"),
    ("coroutine", 113, "core.task.await"),
    ("coroutine", 114, "core.task.cancel"),
    ("coroutine", 115, "core.task.is_cancelled"),
    ("coroutine", 116, "core.coroutine.yield"),
    ("coroutine", 117, "core.generator.create"),
    ("coroutine", 118, "core.generator.yield"),
    ("coroutine", 119, "core.generator.resume"),
    ("concurrency", 120, "core.thread.spawn"),
    ("concurrency", 121, "core.scope.enter"),
    ("concurrency", 122, "core.scope.exit"),
    ("concurrency", 123, "core.channel.create"),
    ("concurrency", 124, "core.channel.send"),
    ("concurrency", 125, "core.channel.receive"),
    ("concurrency", 126, "core.channel.try_send"),
    ("concurrency", 127, "core.channel.try_receive"),
    ("concurrency", 128, "core.channel.is_closed"),
    ("concurrency", 129, "core.channel.select"),
    ("concurrency", 130, "core.atomic.load"),
    ("concurrency", 131, "core.atomic.store"),
    ("concurrency", 132, "core.atomic.rmw"),
    ("provider-call", 136, "core.provider.call"),
    ("target-profile-query", 65, "core.target.operating_system"),
    ("target-profile-query", 66, "core.target.architecture"),
    ("target-profile-query", 67, "core.target.native_abi"),
    ("target-profile-query", 68, "core.target.endianness"),
    ("target-profile-query", 64, "core.target.pointer_width"),
]

EXPECTED_LAYERS = [
    "source",
    "xglobal",
    "xi",
    "corespec",
    "program",
    "verifier",
    "reference",
    "vm",
    "aot",
    "legacy_owner",
]

EXPECTED_INVENTORY_STATUS = {
    "coroutine": [
        "PRESENT_SOURCE_SURFACE",
        "PRESENT_COMPILER_PRIVATE_SUMMARY",
        "PRESENT_PRECUT_LOGICAL_PLAN",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "PRESENT_FROZEN_PRECUT",
    ],
    "concurrency": [
        "PRESENT_SOURCE_SURFACE",
        "PRESENT_COMPILER_PRIVATE_SUMMARY",
        "PRESENT_PRECUT_OPERATIONS",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "ABSENT_CANONICAL",
        "PRESENT_FROZEN_PRECUT",
    ],
    "generation": [
        "NOT_APPLICABLE_INSTANCE_ONLY",
        "NOT_APPLICABLE_INSTANCE_ONLY",
        "NOT_APPLICABLE_INSTANCE_ONLY",
        "NOT_APPLICABLE_INSTANCE_ONLY",
        "ABSENT_GENERATION_OBLIGATIONS",
        "ABSENT_GENERATION_OBLIGATIONS",
        "ABSENT_GENERATION_PINNING",
        "FOUNDATION_EXACT_CACHE_KEY",
        "ABSENT_GENERATION_PINNING",
        "FOUNDATION_INSTANCE_AUTHORITY_AND_PRECUT_DUPLICATES",
    ],
    "provider": [
        "PARTIAL_IMPORTED_NATIVE_SURFACE",
        "PARTIAL_CAPABILITY_SUMMARY_ONLY",
        "ABSENT_STABLE_PROVIDER_REQUIREMENT",
        "ABSENT_CANONICAL",
        "ABSENT_PROVIDER_REQUIREMENT_TABLE",
        "ABSENT_PROVIDER_CALL_VERIFICATION",
        "ABSENT_PROVIDER_CALL_EXECUTION",
        "ABSENT_PROVIDER_CALL_EXECUTION",
        "ABSENT_PROVIDER_CALL_EXECUTION",
        "FOUNDATION_PROFILE_AND_INSTANCE_AUTHORITY",
    ],
    "target-profile-query": [
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "POINTER_WIDTH_CANONICAL_ONLY",
        "PRESENT_HOST_AND_PRECUT_TARGET_PROBES",
    ],
}

EXPECTED_EXIT_CONDITIONS = [
    "wave4-ready",
    "all-operation-atoms-have-stable-registry-rows-and-kats",
    "source-xglobal-xi-program-producer-closure",
    "logical-coroutine-and-concurrency-program-schema-closed",
    "provider-requirement-and-target-query-program-schema-closed",
    "independent-verifier-and-negative-mutation-closed",
    "reference-vm-aot-same-profile-differential-closed",
    "concurrency-allowed-trace-refinement-closed",
    "generation-provider-lease-drain-rebind-hostile-mutation-closed",
    "foreign-profile-never-reads-host-facts",
    "covered-old-owner-new-canonical-dependency-zero",
    "source-corpus-sanitizer-provider-and-code-shape-gates-closed",
]

CANONICAL_COMPLETE = "CANONICAL_COMPLETE"
LEGACY_CANONICAL_DEPENDENCY_ZERO = "NEW_CANONICAL_DEPENDENCY_ZERO"
COROUTINE_YIELD_ONLY = "COROUTINE_YIELD_CANONICAL_ONLY"

EXPECTED_TARGET_QUERY_RESULTS = {
    "core.target.operating_system": "TargetOs",
    "core.target.architecture": "TargetArch",
    "core.target.native_abi": "TargetAbi",
    "core.target.endianness": "TargetEndian",
    "core.target.pointer_width": "u16",
}

EXPECTED_READY_COVERAGE = {
    "spec_oracle",
    "decoder",
    "verifier",
    "evaluator",
    "vm",
    "aot",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def load(root: Path, relative: str) -> dict[str, object]:
    path = root / relative
    raw = path.read_text(encoding="utf-8", errors="strict")
    value = json.loads(raw)
    require(isinstance(value, dict), f"{relative} must contain an object")
    require(
        raw == json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        f"{relative} is not canonical JSON",
    )
    return value


def validate_evidence(root: Path, evidence: object, label: str) -> None:
    require(isinstance(evidence, list), f"{label} evidence is malformed")
    for row in evidence:
        require(
            isinstance(row, dict) and set(row) == {"path", "tokens"},
            f"{label} evidence row is malformed",
        )
        relative = row["path"]
        tokens = row["tokens"]
        require(isinstance(relative, str) and relative, f"{label} evidence path is malformed")
        require(
            isinstance(tokens, list)
            and tokens
            and all(isinstance(token, str) and token for token in tokens),
            f"{label} evidence tokens are malformed: {relative}",
        )
        path = root / relative
        require(path.is_file(), f"{label} evidence path is absent: {relative}")
        source = path.read_text(encoding="utf-8", errors="strict")
        for token in tokens:
            require(token in source, f"{label} evidence drifted: {relative}: {token}")


def validate_atoms(root: Path, data: dict[str, object], ready: bool) -> None:
    atoms = data.get("operation_atoms")
    require(isinstance(atoms, list), "Wave 5 operation atoms are absent")
    actual = [
        (row.get("family"), row.get("stable_id"), row.get("id"))
        for row in atoms
        if isinstance(row, dict)
    ]
    require(actual == EXPECTED_ATOMS, "Wave 5 operation atom set or order drifted")
    require(len(actual) == len({row[2] for row in actual}), "Wave 5 operation atoms are duplicated")
    require(len(actual) == len({row[1] for row in actual}), "Wave 5 stable IDs are duplicated")
    require(
        all(isinstance(row, dict) and set(row) == {"family", "stable_id", "id", "law"}
            and isinstance(row.get("law"), str) and row.get("law")
            for row in atoms),
        "Wave 5 operation atom row is malformed",
    )

    registry = load(root, "xisa/core/registry.json")
    registry_rows = {
        row.get("spelling"): row
        for row in registry.get("operations", [])
        if isinstance(row, dict)
    }
    registry_ids = {
        row.get("stable_id"): row.get("spelling")
        for row in registry.get("operations", [])
        if isinstance(row, dict)
    }
    retired_ids = {
        row.get("stable_id"): row.get("spelling")
        for row in registry.get("retired_operation_ids", [])
        if isinstance(row, dict)
    }
    kat_operations: set[object] = set()
    if ready:
        kats = load(root, "xisa/core/kats.json")
        cases = kats.get("cases")
        require(isinstance(cases, list), "CoreSpec KAT cases are absent")
        kat_operations = {
            case.get("operation") for case in cases if isinstance(case, dict)
        }
    for family, stable_id, spelling in EXPECTED_ATOMS:
        row = registry_rows.get(spelling)
        if row is None:
            require(not ready, f"Wave 5 registered atom is absent: {spelling}")
            require(stable_id not in registry_ids,
                    f"Wave 5 reserved stable ID is occupied: {stable_id}: "
                    f"{registry_ids.get(stable_id)}")
            require(stable_id not in retired_ids,
                    f"Wave 5 atom reuses retired stable ID: {stable_id}: "
                    f"{retired_ids.get(stable_id)}")
            continue
        require(row.get("stable_id") == stable_id, f"Wave 5 stable ID drifted: {spelling}")
        coverage = row.get("coverage")
        require(isinstance(coverage, dict), f"Wave 5 registry coverage is absent: {spelling}")
        if ready:
            type_rule = row.get("type_rule")
            if spelling in EXPECTED_TARGET_QUERY_RESULTS:
                require(isinstance(type_rule, dict) and
                        type_rule.get("result") == EXPECTED_TARGET_QUERY_RESULTS[spelling],
                        f"Wave 5 target-query result type drifted: {spelling}")
            require(spelling in kat_operations, f"Wave 5 CoreSpec KAT is absent: {spelling}")
            require(
                set(coverage) == EXPECTED_READY_COVERAGE and
                all(isinstance(claim, dict) and claim.get("status") == "COMPLETE"
                    for claim in coverage.values()),
                f"Wave 5 registry coverage is incomplete: {family}: {spelling}",
            )


def validate_inventory(root: Path, data: dict[str, object], ready: bool) -> None:
    inventory = data.get("layer_inventory")
    require(isinstance(inventory, list), "Wave 5 layer inventory is absent")
    require(
        [row.get("id") for row in inventory if isinstance(row, dict)]
        == list(EXPECTED_INVENTORY_STATUS),
        "Wave 5 inventory family set or order drifted",
    )
    for row in inventory:
        require(isinstance(row, dict) and set(row) == {"id", "layers"},
                "Wave 5 inventory row is malformed")
        family = row["id"]
        layers = row["layers"]
        require(isinstance(layers, dict) and list(layers) == EXPECTED_LAYERS,
                f"Wave 5 layer set or order drifted: {family}")
        statuses: list[object] = []
        for layer in EXPECTED_LAYERS:
            entry = layers[layer]
            require(isinstance(entry, dict) and set(entry) == {"status", "evidence"},
                    f"Wave 5 layer row is malformed: {family}/{layer}")
            status = entry["status"]
            require(isinstance(status, str) and status,
                    f"Wave 5 layer status is malformed: {family}/{layer}")
            statuses.append(status)
            evidence = entry["evidence"]
            validate_evidence(root, evidence, f"{family}/{layer}")
            if status.startswith("ABSENT_") or status.startswith("NOT_APPLICABLE_"):
                require(evidence == [], f"absent layer contains positive evidence: {family}/{layer}")
            elif ready and layer != "legacy_owner":
                require(evidence, f"Wave 5 READY layer lacks evidence: {family}/{layer}")
        baseline_statuses = EXPECTED_INVENTORY_STATUS[family]
        for layer, status, baseline in zip(EXPECTED_LAYERS, statuses, baseline_statuses):
            if baseline.startswith("NOT_APPLICABLE_"):
                require(status == baseline,
                        f"Wave 5 not-applicable layer drifted: {family}/{layer}: {status}")
                continue
            completed = (LEGACY_CANONICAL_DEPENDENCY_ZERO
                         if layer == "legacy_owner" else CANONICAL_COMPLETE)
            if ready:
                require(status == completed,
                        f"Wave 5 READY inventory is incomplete: {family}/{layer}: {status}")
            else:
                allowed = {baseline, completed}
                if family == "coroutine" and layer != "legacy_owner":
                    allowed.add(COROUTINE_YIELD_ONLY)
                require(status in allowed,
                        f"Wave 5 OPEN inventory status is not a monotonic transition: "
                        f"{family}/{layer}: {status}")
            if status in {completed, COROUTINE_YIELD_ONLY}:
                require(bool(row["layers"][layer]["evidence"]),
                        f"Wave 5 completed layer lacks evidence: {family}/{layer}")


def validate_contract_shapes(root: Path, data: dict[str, object]) -> None:
    require(data.get("schema") == "xray-w7-wave5-contract-freeze/1",
            "Wave 5 schema drifted")
    require(data.get("owner_tasks") == [275, 284, 292, 293, 298, 301],
            "Wave 5 owner tasks drifted")
    require(data.get("scope") ==
            "coroutine-concurrency-generation-provider-target-profile-query",
            "Wave 5 scope drifted")
    require(data.get("compatibility") == "none", "Wave 5 regained compatibility")
    require(data.get("precut_product_route") ==
            "frozen during W7; physical deletion belongs to atomic Task 302",
            "Wave 5 pre-cut product boundary drifted")

    activation = data.get("activation_precondition")
    require(isinstance(activation, dict) and set(activation) == {
        "gate", "required_status", "observed_status_at_freeze", "rule"
    }, "Wave 5 activation precondition is malformed")
    require(activation.get("gate") ==
            "contracts/canonical-program/w7-wave4-exit-gate.json" and
            activation.get("required_status") == "READY_W7_WAVE4" and
            activation.get("observed_status_at_freeze") == "OPEN_W7_WAVE4",
            "Wave 5 activation precondition drifted")

    authority = data.get("authority_partition")
    require(isinstance(authority, dict) and set(authority) == {
        "source_and_xglobal", "xi", "corespec", "program", "target_profile",
        "instance", "reference_vm_aot"
    }, "Wave 5 authority partition drifted")
    non_operations = data.get("explicit_non_operations")
    require(isinstance(non_operations, dict) and set(non_operations) == {
        "physical_suspend_or_resume", "coroutine_complete", "provider_requirement",
        "provider_refusal", "generation_create_or_rebind", "generation_lease_or_drain",
        "build_feature_query", "explicit_compile_time_target_namespace_read", "host_target_probe"
    }, "Wave 5 explicit non-operation partition drifted")
    require("forbidden" in str(non_operations.get("host_target_probe", "")),
            "Wave 5 host target probe is no longer forbidden")

    coroutine = data.get("logical_coroutine_program_contract")
    require(isinstance(coroutine, dict) and set(coroutine) == {
        "function_contract", "state_rows", "safepoints", "live_across", "cleanup",
        "open_targets", "private_realization"
    }, "Wave 5 coroutine program contract drifted")
    concurrency = data.get("concurrency_contract")
    require(isinstance(concurrency, dict) and set(concurrency) == {
        "transfer", "scheduler", "atomic", "channel", "structured_scope"
    }, "Wave 5 concurrency contract drifted")
    generation = data.get("generation_provider_contract")
    require(isinstance(generation, dict) and set(generation) == {
        "program_requirements", "profile_contracts", "instance_binding",
        "continuation_lifetime", "reload", "failure"
    }, "Wave 5 generation/provider contract drifted")

    catalog = data.get("target_query_catalog")
    require(isinstance(catalog, list), "Wave 5 target-query catalog is absent")
    require(
        [(row.get("source"), row.get("operation"), row.get("profile_field"), row.get("result"))
         for row in catalog if isinstance(row, dict)] == [
            ("target.os", "core.target.operating_system",
             "XrTargetMachineFacts.operating_system", "TargetOs"),
            ("target.arch", "core.target.architecture",
             "XrTargetMachineFacts.architecture", "TargetArch"),
            ("target.abi", "core.target.native_abi",
             "XrTargetMachineFacts.native_abi", "TargetAbi"),
            ("target.endian", "core.target.endianness",
             "XrTargetDataLayout.endian", "TargetEndian"),
            ("target.pointerBits", "core.target.pointer_width",
             "XrTargetDataLayout.pointer.size", "u16"),
        ],
        "Wave 5 target-query catalog drifted",
    )
    require(all(isinstance(row, dict) and set(row) == {
        "source", "operation", "profile_field", "result"
    } for row in catalog), "Wave 5 target-query row is malformed")

    owners = data.get("old_owner_inventory")
    require(isinstance(owners, list) and
            [row.get("id") for row in owners if isinstance(row, dict)] == [
                "old-coro-rows-and-semantic-plan",
                "hidden-retained-stack-and-typed-target-plan-vm",
                "aot-xi-coro-and-backend-suspend-inference",
                "generation-and-provider-duplicate-owner",
                "host-target-probe",
            ], "Wave 5 old-owner inventory drifted")
    for row in owners:
        require(isinstance(row, dict) and set(row) == {
            "id", "state", "paths", "canonical_replacement"
        }, "Wave 5 old-owner row is malformed")
        paths = row["paths"]
        require(isinstance(paths, list) and paths and
                all(isinstance(path, str) and (Path(path).suffix in {".c", ".h"})
                    for path in paths),
                f"Wave 5 old-owner paths are malformed: {row['id']}")
        require(all((root / path).is_file() for path in paths),
                f"Wave 5 old-owner inventory path is absent: {row['id']}")

    qualification = data.get("qualification_entries")
    require(isinstance(qualification, dict) and set(qualification) == {
        "normal", "self_test", "exit", "result_claims"
    }, "Wave 5 qualification entries drifted")
    require(qualification.get("result_claims") == [],
            "Wave 5 contract contains an unverified qualification claim")


def validate(root: Path) -> list[str]:
    data = load(root, "contracts/canonical-program/w7-wave5-contract-freeze.json")
    validate_contract_shapes(root, data)
    status = data.get("status")
    require(status in {"OPEN_PRE_W7_WAVE5", "READY_W7_WAVE5"},
            "Wave 5 status is invalid")
    ready = status == "READY_W7_WAVE5"
    validate_atoms(root, data, ready)
    validate_inventory(root, data, ready)

    activation = data["activation_precondition"]
    wave4 = load(root, activation["gate"])
    wave4_ready = wave4.get("status") == activation["required_status"]

    exits = data.get("exit_conditions")
    require(isinstance(exits, list) and
            [row.get("id") for row in exits if isinstance(row, dict)] ==
            EXPECTED_EXIT_CONDITIONS,
            "Wave 5 exit-condition set or order drifted")
    require(all(isinstance(row, dict) and set(row) == {"id", "status"}
                and row.get("status") in {"PENDING", "COMPLETE"}
                for row in exits), "Wave 5 exit-condition row is malformed")
    if not wave4_ready:
        require(exits[0].get("status") == "PENDING",
                "Wave 5 claims the Wave 4 precondition is complete")
    elif ready:
        require(exits[0].get("status") == "COMPLETE",
                "Wave 5 READY state did not close the Wave 4 precondition")

    blockers = [row["id"] for row in exits if row["status"] != "COMPLETE"]
    if ready:
        require(wave4_ready, "Wave 5 READY state bypassed the Wave 4 exit gate")
        require(not blockers, "Wave 5 READY state has unresolved exit conditions: " +
                ", ".join(blockers))
    else:
        require(blockers, "Wave 5 has no blocker but its state is still OPEN")
    return blockers


def copy_input(root: Path, target: Path, relative: str) -> None:
    source = root / relative
    require(source.is_file(), f"self-test input is absent: {relative}")
    destination = target / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def expect_failure(target: Path, label: str) -> None:
    try:
        validate(target)
    except ContractError:
        return
    raise ContractError(f"injected {label} mutation was accepted")


def self_test(root: Path) -> None:
    relative = "contracts/canonical-program/w7-wave5-contract-freeze.json"
    data = load(root, relative)
    evidence_paths = {
        evidence["path"]
        for family in data["layer_inventory"]
        for layer in family["layers"].values()
        for evidence in layer["evidence"]
    }
    old_owner_paths = {
        path for owner in data["old_owner_inventory"] for path in owner["paths"]
    }
    support = sorted(evidence_paths | old_owner_paths | {
        "contracts/canonical-program/w7-wave4-exit-gate.json",
        "xisa/core/registry.json",
        "xisa/core/kats.json",
    })
    with tempfile.TemporaryDirectory(prefix="xray-wave5-contract-") as temporary:
        target = Path(temporary)
        copy_input(root, target, relative)
        for path in support:
            copy_input(root, target, path)
        baseline_blockers = validate(target)
        require(bool(baseline_blockers), "Wave 5 self-test expected an OPEN baseline")

        contract_path = target / relative
        original_contract = contract_path.read_text(encoding="utf-8")
        mutated = json.loads(original_contract)
        mutated["operation_atoms"][0]["id"] = mutated["operation_atoms"][1]["id"]
        contract_path.write_text(json.dumps(mutated, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
        expect_failure(target, "duplicate operation")
        contract_path.write_text(original_contract, encoding="utf-8")

        victim_relative = "src/frontend/analyzer/xanalyzer_suspend.c"
        victim = target / victim_relative
        original_victim = victim.read_text(encoding="utf-8")
        victim.write_text(original_victim.replace("sus_call_is_coro_yield",
                                                  "injected_call_is_coro_yield"),
                          encoding="utf-8")
        expect_failure(target, "source evidence removal")
        victim.write_text(original_victim, encoding="utf-8")

        mutated = json.loads(original_contract)
        generation = next(row for row in mutated["layer_inventory"]
                          if row["id"] == "generation")
        generation["layers"]["vm"]["status"] = CANONICAL_COMPLETE
        contract_path.write_text(json.dumps(mutated, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
        require(validate(target) == baseline_blockers,
                "a monotonic OPEN inventory transition was rejected")
        generation["layers"]["vm"]["status"] = "UNREVIEWED_PROGRESS"
        contract_path.write_text(json.dumps(mutated, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
        expect_failure(target, "non-canonical inventory transition")
        contract_path.write_text(original_contract, encoding="utf-8")

        mutated = json.loads(original_contract)
        mutated["status"] = "READY_W7_WAVE5"
        contract_path.write_text(json.dumps(mutated, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
        expect_failure(target, "forged ready state")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--require-ready", action="store_true")
    args = parser.parse_args()
    try:
        require(not (args.self_test and args.require_ready),
                "--self-test and --require-ready are mutually exclusive")
        root = args.root.resolve()
        if args.self_test:
            self_test(root)
            print("XrProgram Wave 5 contract self-test: PASS")
            return 0
        blockers = validate(root)
        if args.require_ready and blockers:
            raise ContractError("Wave 5 activation is blocked: " + " | ".join(blockers))
        if blockers:
            print(f"XrProgram Wave 5 contract inventory: OPEN ({len(blockers)} blockers)")
            for blocker in blockers:
                print(f"- {blocker}")
        else:
            print("XrProgram Wave 5 contract: READY")
        return 0
    except (ContractError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram Wave 5 contract: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
