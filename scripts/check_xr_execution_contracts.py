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
OBJECT_INSTANCE_HEADER = Path("src/runtime/class/xinstance.h")
COVERAGE = Path("contracts/canonical-program/execution-binding-coverage.json")

EXPECTED_COVERAGE = {
    "schema": "xray-execution-binding-coverage/1",
    "profile_schema_version": 4,
    "execution_binding_schema_version": 1,
    "profile_partitions": [
        {"name": "target-semantics", "identity": "XrTargetSemanticsId", "status": "COMPLETE"},
        {"name": "boundary-abi", "identity": "XrBoundaryAbiId", "status": "WALKING_SKELETON"},
        {"name": "runtime-kernel", "identity": "XrRuntimeKernelId", "status": "WALKING_SKELETON"},
        {"name": "provider-contract-set", "identity": "XrProviderContractSetId", "status": "COMPLETE"},
    ],
    "boundary_value_types": ["void", "bool", "i64", "u32", "error"],
    "provider_admission": [
        "stable-contract-id",
        "exact-contract-fingerprint",
        "ordered-operation-id",
        "non-null-operation-entry",
        "thread-safety",
        "reentrancy",
        "callback-safety",
    ],
    "generation_states": ["ACTIVE", "DRAINING", "RETIRED"],
    "generation_transitions": [
        "ACTIVE->DRAINING",
        "DRAINING+pins=0->RETIRED",
        "RETIRED(g)->ACTIVE(g+1)",
    ],
    "foreign_profile_matrix": [
        "explicit-target-machine-facts",
        "explicit-runtime-abi",
        "exact-provider-contract-fingerprint",
        "no-host-sizeof-or-preprocessor-probe",
    ],
    "inactive_boundary_contracts": [
        "aggregate-carriers",
        "borrow-move-share-drop-transfer",
        "root-and-cleanup-tables",
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
             OBJECT_INSTANCE_HEADER)
    sources = {
        path: (overrides or {}).get(path, (root / path).read_text(encoding="utf-8"))
        for path in paths
    }
    profile_header = sources[PROFILE_HEADER]
    profile_source = sources[PROFILE_SOURCE]
    execution_header = sources[EXECUTION_HEADER]
    execution_source = sources[EXECUTION_SOURCE]

    for token in ("XrTargetSemanticsId", "XrBoundaryAbiId", "XrRuntimeKernelId",
                  "XrProviderContractSetId"):
        require(token in profile_header, f"missing profile partition identity {token}")
    for token in ("XrExecutionId", "XrInstance", "contract_fingerprint",
                  "XR_INSTANCE_ACTIVE", "XR_INSTANCE_DRAINING", "XR_INSTANCE_RETIRED"):
        require(token in execution_header, f"missing execution contract token {token}")
    require("xr_target_provider_contract_fingerprint" in execution_source,
            "provider admission does not compare the exact provider contract")
    require("xr_program_validate" not in execution_source,
            "execution binding must consume XrValidatedProgram, not re-run admission")

    combined = profile_header + profile_source + execution_header + execution_source
    for forbidden in ("sizeof(void *)", "sizeof(void*)", "__APPLE__", "_WIN32",
                      "__linux__", "TARGET_OS_", "XrVmCode", "XrBackendIR",
                      "register allocation", "VM slot"):
        require(forbidden not in combined, f"host/local-representation leak: {forbidden}")
    for line in (execution_header + execution_source).splitlines():
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
