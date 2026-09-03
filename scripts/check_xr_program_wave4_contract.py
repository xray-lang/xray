#!/usr/bin/env python3
"""Validate the W7 Wave 4 callable/interface contract freeze."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


class ContractError(ValueError):
    """Raised when the Wave 4 contract is incomplete."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def load(path: Path) -> dict[str, object]:
    raw = path.read_text(encoding="utf-8", errors="strict")
    value = json.loads(raw)
    require(isinstance(value, dict), f"{path} must contain an object")
    require(raw == json.dumps(value, ensure_ascii=False, indent=2) + "\n",
            f"{path} is not canonical JSON")
    return value


def validate(root: Path) -> None:
    path = root / "contracts/canonical-program/w7-wave4-contract-freeze.json"
    data = load(path)
    require(data.get("schema") == "xray-w7-wave4-contract-freeze/1",
            "Wave 4 schema drifted")
    require(data.get("status") == "FROZEN_W7_WAVE4_P0", "Wave 4 P0 is not frozen")
    require(data.get("owner_tasks") == [283, 284, 287, 291, 301],
            "Wave 4 owner tasks drifted")
    require(data.get("compatibility") == "none", "Wave 4 regained compatibility")
    require(data.get("implementation_state") == {
        "declaration_receiver_contract": "COMPLETE",
        "borrow_origin_set": "COMPLETE",
        "borrow_root_tables": "COMPLETE",
        "program_tables": "COMPLETE",
        "existential_operations": "PENDING",
        "witness_dispatch": "PENDING",
        "callable_dispatch": "PENDING",
        "source_integration": "PENDING",
        "wave_closure": "PENDING",
    }, "Wave 4 implementation state drifted")

    phase = data.get("phase_erasure")
    require(isinstance(phase, dict) and set(phase) == {
        "generic_constraint_call",
        "source_import_call",
        "interface_conformance_check",
        "devirtualized_existential_call",
    }, "Wave 4 phase-erasure partition drifted")

    atoms = data.get("operation_atoms")
    require(isinstance(atoms, list), "Wave 4 operation atoms are absent")
    actual = [(row.get("stable_id"), row.get("id")) for row in atoms if isinstance(row, dict)]
    require(actual == [
        (38, "core.call.indirect_direct"),
        (39, "core.call.indirect_invoke"),
        (40, "core.call.witness_direct"),
        (41, "core.call.witness_invoke"),
        (86, "core.existential.pack"),
        (87, "core.existential.test"),
        (88, "core.existential.project"),
        (89, "core.callable.pack"),
    ], "Wave 4 operation atom IDs or order drifted")
    require(len(actual) == len(set(actual)), "Wave 4 operation atoms are duplicated")

    non_operations = data.get("explicit_non_operations")
    require(isinstance(non_operations, dict) and set(non_operations) == {
        "generic_constraint",
        "source_import",
        "interface_upcast",
        "devirtualized_call",
        "selector_or_name_lookup",
        "class_only_itable",
        "borrow_lifetime",
        "dynamic_unknown_call",
    }, "Wave 4 explicit non-operation set drifted")
    require(data.get("nominal_implementor_kinds") == ["CLASS", "STRUCT", "ENUM"],
            "Wave 4 nominal implementor kinds drifted")
    require(data.get("interface_use_kinds") == [
        "CONSTRAINT_BOUND",
        "EXISTENTIAL_READ",
        "EXISTENTIAL_REF",
        "EXISTENTIAL_MOVE",
        "EXISTENTIAL_OWNED_STORAGE",
    ], "Wave 4 interface-use kinds drifted")
    require(data.get("borrow_origin_kinds") == ["PARAM", "RECEIVER", "STATIC"],
            "Wave 4 borrow-origin kinds drifted")
    borrow = data.get("borrow_origin_contract")
    require(isinstance(borrow, dict) and set(borrow) == {
        "source_syntax",
        "type_identity",
        "body_role",
        "call_mapping",
        "xi_evidence",
        "program_rows",
        "semantic_plan_boundary",
        "compatibility",
        "positive_fixtures",
        "negative_fixtures",
    }, "Wave 4 BorrowOriginSet contract drifted")
    require(borrow.get("compatibility") ==
            "no scalar origin fields, body-derived fallback, old reader or dual path",
            "Wave 4 BorrowOriginSet regained a legacy path")
    positive = borrow.get("positive_fixtures")
    negative = borrow.get("negative_fixtures")
    require(positive == [
        "tests/fixtures/task284_ownership_positive/borrow_origin_set.xr",
        "tests/fixtures/task284_ownership_positive/borrow_origin_static.xr",
    ], "Wave 4 BorrowOriginSet positive fixtures drifted")
    require(negative == [
        "tests/fixtures/task284_ownership_negative/borrow_origin_ambiguous.xr",
        "tests/fixtures/task284_ownership_negative/borrow_origin_invalid.xr",
        "tests/fixtures/task284_ownership_negative/borrow_origin_outside_set.xr",
        "tests/fixtures/task284_ownership_negative/borrow_origin_ref.xr",
    ], "Wave 4 BorrowOriginSet negative fixtures drifted")
    require(all((root / fixture).is_file() for fixture in positive + negative),
            "Wave 4 BorrowOriginSet fixture is absent")

    tables = data.get("program_table_contract")
    require(isinstance(tables, dict) and set(tables) == {
        "signature_owner",
        "type_rows",
        "interface_rows",
        "conformance_rows",
        "receiver_substitution",
        "canonicality",
        "independent_verification",
        "compatibility",
    }, "Wave 4 program-table contract drifted")
    require(tables.get("compatibility") ==
            "no duplicate function-owned wire signatures, class-only table, selector/name lookup, old reader or dual schema",
            "Wave 4 program tables regained a legacy path")

    required_anchors = {
        "src/shared/xr_view_origin.h": ["typedef struct XrViewOrigin"],
        "src/runtime/value/xtype.h": ["XrViewOrigin *view_origin_set;"],
        "src/frontend/analyzer/xanalyzer_visitor_decl.c": [
            "void xa_bind_declared_view_origins(",
            "void xa_validate_declared_view_origin_returns(",
        ],
        "src/ir/xi.h": ["typedef struct XiViewSourceEvidence"],
        "src/ir/xi.c": [
            "bool xi_value_materialize_view_origins(",
            "static bool xi_view_materialize_value(",
        ],
        "src/ir/xi_lower_expr.c": ["static void lower_instantiate_call_view_evidence("],
        "src/ir/xi_verify.c": [
            "has an invalid ViewEvidence source recipe",
        ],
        "src/program/xr_program.h": [
            "typedef struct XrCoreIrRootInput",
            "typedef struct XrCoreIrValueRootSetInput",
            "struct XrCoreIrCallableSignatureInput {",
            "typedef struct XrCoreIrInterfaceInput",
            "typedef struct XrCoreIrConformanceInput",
        ],
        "src/program/xr_program_encode.c": ["static SignatureRef *collect_signatures("],
        "src/program/xr_program_verify.c": [
            "static bool parse_semantic_metadata(",
            "static bool mapped_call_result_roots_match(",
            "static bool validated_signature_satisfies_interface(",
        ],
    }
    for relative, anchors in required_anchors.items():
        source = (root / relative).read_text(encoding="utf-8", errors="strict")
        require(all(source.count(anchor) == 1 for anchor in anchors),
                f"Wave 4 BorrowOriginSet owner drifted: {relative}")
    retired = {
        "src/frontend/analyzer/xanalyzer_visitor_decl.c": ["view_return_source"],
        "src/runtime/value/xtype.h": ["view_return_source", "view_return_param"],
        "src/runtime/value/xtype.c": ["view_return_source", "view_return_param"],
        "src/runtime/value/xtype_generic.c": ["view_return_source", "view_return_param"],
        "src/ir/xi.h": ["view_return_source", "view_return_param"],
        "src/ir/xi_lower_expr.c": ["view_return_source", "view_return_param"],
    }
    for relative, anchors in retired.items():
        source = (root / relative).read_text(encoding="utf-8", errors="strict")
        require(all(anchor not in source for anchor in anchors),
                f"Wave 4 retired scalar origin owner returned: {relative}")
    owners = data.get("old_owner_inventory")
    require(isinstance(owners, list) and len(owners) == 5,
            "Wave 4 old-owner inventory is incomplete")
    require(all(isinstance(row, dict) and set(row) == {"owner", "canonical_replacement"}
                and all(isinstance(value, str) and value for value in row.values())
                for row in owners), "Wave 4 old-owner row is malformed")
    order = data.get("slice_order")
    require(isinstance(order, list) and len(order) == 7 and
            order[0] == "declaration-owned receiver modes and borrowed-result origins" and
            order[-1] ==
                "old-owner dependency deletion, differential, sanitizer and residue closure",
            "Wave 4 slice order drifted")


def self_test(root: Path) -> None:
    relative = Path("contracts/canonical-program/w7-wave4-contract-freeze.json")
    support = [
        Path("src/shared/xr_view_origin.h"),
        Path("src/runtime/value/xtype.h"),
        Path("src/runtime/value/xtype.c"),
        Path("src/runtime/value/xtype_generic.c"),
        Path("src/frontend/analyzer/xanalyzer_visitor_decl.c"),
        Path("src/ir/xi.h"),
        Path("src/ir/xi.c"),
        Path("src/ir/xi_lower_expr.c"),
        Path("src/ir/xi_verify.c"),
        Path("src/program/xr_program.h"),
        Path("src/program/xr_program_encode.c"),
        Path("src/program/xr_program_verify.c"),
        Path("tests/fixtures/task284_ownership_positive/borrow_origin_set.xr"),
        Path("tests/fixtures/task284_ownership_positive/borrow_origin_static.xr"),
        Path("tests/fixtures/task284_ownership_negative/borrow_origin_ambiguous.xr"),
        Path("tests/fixtures/task284_ownership_negative/borrow_origin_invalid.xr"),
        Path("tests/fixtures/task284_ownership_negative/borrow_origin_outside_set.xr"),
        Path("tests/fixtures/task284_ownership_negative/borrow_origin_ref.xr"),
    ]
    with tempfile.TemporaryDirectory(prefix="xray-wave4-contract-") as temporary:
        target = Path(temporary)
        (target / relative.parent).mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, target / relative)
        for path in support:
            (target / path.parent).mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / path, target / path)
        validate(target)
        data = load(target / relative)
        atoms = data["operation_atoms"]
        assert isinstance(atoms, list) and isinstance(atoms[0], dict)
        atoms[0]["stable_id"] = 36
        (target / relative).write_text(
            json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        try:
            validate(target)
        except ContractError:
            return
        raise ContractError("injected Wave 4 stable-ID collision was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        if args.self_test:
            self_test(root)
            print("XrProgram Wave 4 contract self-test: PASS")
        else:
            validate(root)
            print("XrProgram Wave 4 contract: PASS")
        return 0
    except (ContractError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram Wave 4 contract: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
