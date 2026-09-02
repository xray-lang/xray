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
    with tempfile.TemporaryDirectory(prefix="xray-wave4-contract-") as temporary:
        target = Path(temporary)
        (target / relative.parent).mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, target / relative)
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
