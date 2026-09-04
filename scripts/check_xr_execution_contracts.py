#!/usr/bin/env python3
"""Fail-closed target-profile and execution-binding architecture gate."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


PROFILE_HEADER = Path("src/plan/target/xr_target_profile.h")
PROFILE_SOURCE = Path("src/plan/target/xr_target_profile.c")
EXECUTION_HEADER = Path("src/execution/xr_execution.h")
EXECUTION_SOURCE = Path("src/execution/xr_execution.c")
BOUNDARY_HEADER = Path("src/execution/xr_boundary_materialization.h")
BOUNDARY_SOURCE = Path("src/execution/xr_boundary_materialization.c")
OBJECT_INSTANCE_HEADER = Path("src/runtime/class/xinstance.h")
COVERAGE = Path("contracts/canonical-program/execution-binding-coverage.json")

EXPECTED_COVERAGE = {
    "schema": "xray-execution-binding-coverage/1",
    "profile_schema_version": 5,
    "boundary_abi_schema_version": 3,
    "execution_binding_schema_version": 2,
    "profile_partitions": [
        {"name": "target-semantics", "identity": "XrTargetSemanticsId", "status": "COMPLETE"},
        {"name": "boundary-abi", "identity": "XrBoundaryAbiId", "status": "ACTIVE_COPY_VALUE"},
        {"name": "runtime-kernel", "identity": "XrRuntimeKernelId", "status": "WALKING_SKELETON"},
        {"name": "provider-contract-set", "identity": "XrProviderContractSetId", "status": "COMPLETE"},
    ],
    "boundary_value_types": [
        "void", "bool", "i64", "u32", "u16", "error", "aggregate", "variant"
    ],
    "active_boundary_contracts": [
        "call-frame-v1",
        "declaration-order-natural-aggregate",
        "u32-tag-natural-payload-variant",
        "program-profile-bound-type-layout",
        "copy-value-function-signature",
        "explicit-root-offsets",
        "explicit-cleanup-actions",
    ],
    "provider_admission": [
        "program-required-provider-subset",
        "program-required-operation-subset",
        "stable-contract-id",
        "exact-contract-fingerprint",
        "ordered-operation-id",
        "non-null-operation-entry",
        "typed-i64-trampoline",
        "exact-i64-call-abi",
        "extra-binding-rejected",
        "thread-safety",
        "reentrancy",
        "callback-safety",
    ],
    "generation_states": ["ACTIVE", "DRAINING", "RETIRED"],
    "generation_transitions": [
        "ACTIVE->DRAINING",
        "lease-ticket-active->consumed-once",
        "DRAINING+leases=0->RETIRED",
        "RETIRED(g)->ACTIVE(g+1)",
    ],
    "foreign_profile_matrix": [
        "explicit-target-machine-facts",
        "explicit-runtime-abi",
        "exact-provider-contract-fingerprint",
        "no-host-sizeof-or-preprocessor-probe",
    ],
    "inactive_boundary_contracts": [
        "borrow-move-share-drop-transfer",
        "nontrivial-root-and-cleanup-rows",
        "coroutine-continuation-state",
        "AOT-boundary-adapter",
        "FFI-and-hybrid-adapter",
    ],
    "forbidden_shared_representation": [
        "VM-slot-layout",
        "native-register-layout",
        "common-local-physical-plan",
    ],
}


class GateError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def canonical_json(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def validate(root: Path, overrides: dict[Path, str] | None = None) -> None:
    paths = (PROFILE_HEADER, PROFILE_SOURCE, EXECUTION_HEADER, EXECUTION_SOURCE,
             BOUNDARY_HEADER, BOUNDARY_SOURCE, OBJECT_INSTANCE_HEADER)
    sources = {
        path: (overrides or {}).get(path, (root / path).read_text(encoding="utf-8"))
        for path in paths
    }
    profile_header = sources[PROFILE_HEADER]
    profile_source = sources[PROFILE_SOURCE]
    execution_header = sources[EXECUTION_HEADER]
    execution_source = sources[EXECUTION_SOURCE]
    boundary_header = sources[BOUNDARY_HEADER]
    boundary_source = sources[BOUNDARY_SOURCE]

    for token in ("XrTargetSemanticsId", "XrBoundaryAbiId", "XrRuntimeKernelId",
                  "XrProviderContractSetId"):
        require(token in profile_header, f"missing profile partition identity {token}")
    for token in ("#define XR_BOUNDARY_ABI_SCHEMA_VERSION UINT32_C(3)",
                  "#define XR_BOUNDARY_ABI_VALUE_COUNT UINT8_C(6)"):
        require(token in profile_header, f"boundary ABI u16 contract omits {token}")
    for token in ("xray-boundary-abi-v3", "boundary->values[5]",
                  "XR_CORE_TYPE_U16", "layout->u16"):
        require(token in profile_source, f"boundary ABI u16 row omits {token}")
    for token in ("XrExecutionId", "XrInstance", "XrExecutionLease", "contract_fingerprint",
                  "XR_INSTANCE_ACTIVE", "XR_INSTANCE_DRAINING", "XR_INSTANCE_RETIRED"):
        require(token in execution_header, f"missing execution contract token {token}")
    require("#define XR_EXECUTION_BINDING_SCHEMA_VERSION UINT32_C(2)" in execution_header,
            "execution header schema version is not synchronized with coverage")
    for token in ("XrProviderCallStatus", "XR_PROVIDER_CALL_OK", "int64_t argument",
                  "int64_t *result_out", "XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI"):
        require(token in execution_header, f"typed provider trampoline omits {token}")
    for token in ("XrBoundaryTypeLayout", "XrBoundaryCallLayout",
                  "XrBoundaryMaterializationBudget", "XR_BOUNDARY_CALL_FRAME_V1"):
        require(token in profile_header + boundary_header,
                f"missing materialized boundary token {token}")
    require("xr_target_provider_contract_fingerprint" in execution_source,
            "provider admission does not compare the exact provider contract")
    for token in ("xr_validated_program_provider_requirement_count",
                  "xr_validated_program_provider_requirement", "find_profile_provider",
                  "find_profile_operation", "provider_operation_uses_scalar_i64_trampoline",
                  "XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER",
                  "XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE"):
        require(token in execution_source, f"program-required provider subset omits {token}")
    for token in ("xr_execution_instance_acquire", "xr_execution_lease_release",
                  "xr_execution_lease_program", "xr_execution_lease_profile",
                  "xr_execution_lease_provider_operation"):
        require(token in execution_source, f"generation lease contract omits {token}")
    for token in ("uint64_t ticket", "lease_tickets", "lease_ticket_is_active_locked",
                  "next_lease_ticket"):
        require(token in execution_header + execution_source,
                f"release-once lease ticket contract omits {token}")
    require("xr_execution_instance_program" not in execution_header and
            "xr_execution_instance_profile" not in execution_header,
            "generation-bound program/profile remain accessible without a lease")
    require("xr_execution_instance_acquire" in boundary_source and
            "xr_execution_lease_release" in boundary_source,
            "boundary materialization does not hold an exact generation lease")
    require("xr_program_validate" not in execution_source,
            "execution binding must consume XrValidatedProgram, not re-run admission")

    for token in ("xr_validated_program_id", "builder->abi->id", "boundary_kind"):
        require(token in boundary_source, f"boundary layout identity omits {token}")

    combined = (profile_header + profile_source + execution_header + execution_source +
                boundary_header + boundary_source)
    for forbidden in ("sizeof(void *)", "sizeof(void*)", "__APPLE__", "_WIN32",
                      "__linux__", "TARGET_OS_", "XrVmCode", "XrBackendIR",
                      "register allocation", "VM slot"):
        require(forbidden not in combined, f"host/local-representation leak: {forbidden}")
    for line in (execution_header + execution_source + boundary_header + boundary_source).splitlines():
        if line.lstrip().startswith("#include"):
            require("/vm/" not in line and "/aot/" not in line,
                    f"execution binding depends on executor implementation: {line.strip()}")

    require("XrObjectInstance" in sources[OBJECT_INSTANCE_HEADER],
            "runtime object instance was not renamed for the execution authority")
    require(re.search(r"\btypedef\s+struct\s+XrInstance\s+XrInstance\s*;", execution_header)
            is not None, "XrInstance is not the unique public execution authority")
    require((root / COVERAGE).read_text(encoding="utf-8") == canonical_json(EXPECTED_COVERAGE),
            f"{COVERAGE} is stale")


def self_test(root: Path) -> None:
    validate(root)
    source = (root / PROFILE_SOURCE).read_text(encoding="utf-8")
    mutated = source.replace("layout->pointer.size", "sizeof(void *)", 1)
    require(mutated != source, "host-probe mutation did not apply")
    try:
        validate(root, {PROFILE_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("host sizeof mutation was accepted")

    execution = (root / EXECUTION_SOURCE).read_text(encoding="utf-8")
    mutated = execution.replace("xr_target_provider_contract_fingerprint",
                                "missing_provider_fingerprint", 1)
    require(mutated != execution, "provider mutation did not apply")
    try:
        validate(root, {EXECUTION_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("missing exact provider fingerprint was accepted")

    mutated = execution.replace("lease_ticket_is_active_locked",
                                "lease_ticket_validation_removed")
    require(mutated != execution, "lease ticket mutation did not apply")
    try:
        validate(root, {EXECUTION_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("missing release-once lease ticket validation was accepted")

    mutated = execution.replace("provider_operation_uses_scalar_i64_trampoline",
                                "provider_operation_abi_check_removed")
    require(mutated != execution, "provider ABI mutation did not apply")
    try:
        validate(root, {EXECUTION_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("missing typed provider ABI admission was accepted")

    boundary = (root / BOUNDARY_SOURCE).read_text(encoding="utf-8")
    mutated = boundary.replace("xr_execution_instance_acquire",
                               "missing_generation_lease")
    require(mutated != boundary, "generation lease mutation did not apply")
    try:
        validate(root, {BOUNDARY_SOURCE: mutated})
    except GateError:
        pass
    else:
        raise GateError("unleased boundary materialization was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test(args.root.resolve())
            print("Xr execution contracts self-test: PASS")
        else:
            validate(args.root.resolve())
            print("Xr execution contracts: PASS (4 profile partitions, exact provider binding)")
    except (GateError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"Xr execution contracts: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
