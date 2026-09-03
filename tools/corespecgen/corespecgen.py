#!/usr/bin/env python3
"""Validate and project the canonical Xray CoreSpec registry.

The registry is semantic metadata, not an executor. This tool validates stable
identities and language-level laws, runs a host-independent normative KAT
oracle, and mechanically generates C metadata, a readable specification table,
and consumer coverage inventory. VM and AOT never call the Python oracle.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Callable


REGISTRY_PATH = Path("xisa/core/registry.json")
SCHEMA_PATH = Path("xisa/core/schema.json")
KAT_PATH = Path("xisa/core/kats.json")
OUTPUT_PATHS = (
    Path("src/core/xr_core_spec_gen.h"),
    Path("src/core/xr_core_spec_gen.c"),
    Path("contracts/canonical-program/core-spec-operations.md"),
    Path("contracts/canonical-program/core-spec-coverage.json"),
)
REGISTRY_KEYS = {
    "schema",
    "epoch",
    "feature_policy",
    "types",
    "effects",
    "capabilities",
    "traps",
    "features",
    "retired_operation_ids",
    "operations",
}
OPERATION_KEYS = {
    "stable_id",
    "spelling",
    "class",
    "feature",
    "epoch",
    "type_rule",
    "evaluation_order",
    "successors",
    "arithmetic",
    "effects",
    "capability_requirements",
    "ownership",
    "profile_dependency",
    "materialization",
    "determinism",
    "kat_validator",
    "coverage",
}
CONSUMERS = ("spec_oracle", "decoder", "verifier", "evaluator", "vm", "aot")
CONSUMER_TASKS = {
    "spec_oracle": 295,
    "decoder": 297,
    "verifier": 297,
    "evaluator": 297,
    "vm": 299,
    "aot": 300,
}
STATUS_VALUES = {"COMPLETE", "NOT_YET_ACTIVE", "NOT_APPLICABLE"}
ARITHMETIC_KINDS = {
    "none",
    "signed-integer-constant",
    "signed-integer",
    "signed-integer-division",
    "signed-integer-compare",
}
SUCCESSOR_KEYS = {"normal", "error", "panic", "cancel", "suspend"}
GENERIC_TYPES = {
    "A", "E", "V", "T", "T...", "R?", "P...",
    "normal-edge-values...", "error-edge-values...", "panic-edge-values...",
}
VARIADIC_TYPES = {
    "T...", "P...", "normal-edge-values...", "error-edge-values...", "panic-edge-values...",
}
IMPLEMENTATION_KEYS = {
    "aot_handler",
    "c_spelling",
    "frame_offset",
    "native_register",
    "quickened_opcode",
    "slot_offset",
    "vm_handler",
}
SCENARIO_TOKENS = ("fixture", "module_count", "pair", "source_case", "tuple6")
I64_MIN = -(1 << 63)
I64_MAX = (1 << 63) - 1
I64_MODULUS = 1 << 64


class CoreSpecError(ValueError):
    """Raised for a fail-closed schema, semantics, or generation violation."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CoreSpecError(message)


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CoreSpecError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def semantic_registry_projection(registry: dict[str, Any]) -> dict[str, Any]:
    """Return only language semantics that may change XrProgram meaning."""
    projection = copy.deepcopy(registry)
    for rows in ("types", "effects", "capabilities", "traps"):
        for row in projection[rows]:
            row.pop("description", None)
    for row in projection["retired_operation_ids"]:
        row.pop("reason", None)
    for operation in projection["operations"]:
        operation.pop("kat_validator", None)
        operation.pop("coverage", None)
    return projection


def semantic_registry_digest(registry: dict[str, Any]) -> str:
    projection = semantic_registry_projection(registry)
    return hashlib.sha256(canonical_json(projection).encode("utf-8")).hexdigest()


def read_json(path: Path, require_canonical: bool = True) -> dict[str, Any]:
    try:
        raw = path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as exc:
        raise CoreSpecError(f"cannot read {path}: {exc}") from exc
    try:
        value = json.loads(raw, object_pairs_hook=reject_duplicate_keys)
    except (json.JSONDecodeError, CoreSpecError) as exc:
        raise CoreSpecError(f"invalid JSON in {path}: {exc}") from exc
    require(isinstance(value, dict), f"{path} must contain an object")
    if require_canonical:
        require(raw == canonical_json(value),
                f"{path} is not canonical two-space JSON with one final newline")
    return value


def unique_rows(rows: Any, owner: str, key: str) -> list[dict[str, Any]]:
    require(isinstance(rows, list), f"{owner} must be an array")
    require(all(isinstance(row, dict) for row in rows), f"{owner} rows must be objects")
    values = [row.get(key) for row in rows]
    require(all(value is not None and value != "" for value in values), f"{owner} has empty {key}")
    require(len(values) == len(set(values)), f"{owner} has duplicate {key}")
    return rows


def validate_named_registry(rows: Any, owner: str) -> tuple[set[int], set[str]]:
    values = unique_rows(rows, owner, "stable_id")
    ids = {row["stable_id"] for row in values}
    names = {row.get("name") for row in values}
    require(len(names) == len(values) and None not in names, f"{owner} has duplicate or empty names")
    for row in values:
        require(set(row) == {"stable_id", "name", "description"},
                f"{owner} row {row.get('name')} has unknown or missing fields")
        require(isinstance(row["stable_id"], int) and 0 <= row["stable_id"] <= 65535,
                f"{owner} row {row['name']} has invalid stable id")
        require(isinstance(row["name"], str) and row["name"], f"{owner} has empty name")
        require(isinstance(row["description"], str) and row["description"],
                f"{owner} row {row['name']} has empty description")
    require([row["stable_id"] for row in values] == sorted(ids), f"{owner} must be sorted by stable id")
    return ids, names


def walk_keys(value: Any) -> set[str]:
    keys: set[str] = set()
    if isinstance(value, dict):
        keys.update(value)
        for child in value.values():
            keys.update(walk_keys(child))
    elif isinstance(value, list):
        for child in value:
            keys.update(walk_keys(child))
    return keys


def referenced_types(type_rule: dict[str, Any]) -> set[str]:
    result: set[str] = set()
    operands = type_rule.get("operands")
    require(isinstance(operands, list), "operation type_rule.operands must be an array")
    for name in operands:
        require(isinstance(name, str), "operation operand type must be a string")
        if name not in GENERIC_TYPES:
            result.add(name)
    result_type = type_rule.get("result")
    require(isinstance(result_type, str), "operation type_rule.result must be a string")
    if result_type not in GENERIC_TYPES:
        result.add(result_type)
    require(isinstance(type_rule.get("immediates"), dict),
            "operation type_rule.immediates must be an object")
    return result


def validate_schema_document(schema: dict[str, Any]) -> None:
    require(schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
            "CoreSpec schema must use JSON Schema draft 2020-12")
    require(schema.get("$id") == "https://xray-lang.local/schema/core-spec-v1.json",
            "CoreSpec schema id drifted")
    required = schema.get("required")
    require(isinstance(required, list) and set(required) == REGISTRY_KEYS,
            "CoreSpec schema required fields differ from the parser contract")


def validate_registry(registry: dict[str, Any]) -> dict[str, dict[Any, dict[str, Any]]]:
    require(set(registry) == REGISTRY_KEYS, "CoreSpec registry top-level fields drifted")
    require(registry["schema"] == "xray-core-spec/1", "CoreSpec registry schema must be v1")
    require(registry["epoch"] == 1, "W1 CoreSpec epoch must be 1")
    require(registry["feature_policy"] == "fail-closed", "unknown features must fail closed")

    _, type_names = validate_named_registry(registry["types"], "type registry")
    _, effect_names = validate_named_registry(registry["effects"], "effect registry")
    _, capability_names = validate_named_registry(registry["capabilities"], "capability registry")
    _, trap_names = validate_named_registry(registry["traps"], "trap registry")

    features = unique_rows(registry["features"], "feature registry", "stable_id")
    require_unique_feature_names = {row.get("name") for row in features}
    require(len(require_unique_feature_names) == len(features) and None not in require_unique_feature_names,
            "feature registry has duplicate or empty names")
    feature_names: set[str] = set()
    for row in features:
        require(set(row) == {"stable_id", "name", "epoch", "status"},
                f"feature {row.get('name')} fields drifted")
        require(isinstance(row["stable_id"], int) and 1 <= row["stable_id"] <= 65535,
                f"feature {row['name']} has invalid stable id")
        require(row["epoch"] <= registry["epoch"], f"feature {row['name']} has a future epoch")
        require(row["status"] in {"ACTIVE", "RETIRED"}, f"feature {row['name']} has invalid status")
        if row["status"] == "ACTIVE":
            feature_names.add(row["name"])

    retired = unique_rows(registry["retired_operation_ids"], "retired operation registry", "stable_id")
    retired_ids: set[int] = set()
    retired_spellings: set[str] = set()
    for row in retired:
        require(set(row) == {"stable_id", "spelling", "reason"},
                f"retired operation {row.get('stable_id')} fields drifted")
        require(isinstance(row["stable_id"], int) and 1 <= row["stable_id"] <= 65535,
                "retired operation has invalid stable id")
        require(isinstance(row["spelling"], str) and row["spelling"],
                "retired operation has empty spelling")
        require(isinstance(row["reason"], str) and row["reason"],
                "retired operation has empty reason")
        retired_ids.add(row["stable_id"])
        require(row["spelling"] not in retired_spellings, "retired operation spelling is duplicated")
        retired_spellings.add(row["spelling"])

    operations = unique_rows(registry["operations"], "operation registry", "stable_id")
    spellings: set[str] = set()
    operation_ids: set[int] = set()
    for operation in operations:
        spelling = operation.get("spelling", "<missing>")
        require(set(operation) == OPERATION_KEYS,
                f"operation {spelling} has unknown or missing fields: {sorted(set(operation) ^ OPERATION_KEYS)}")
        stable_id = operation["stable_id"]
        require(isinstance(stable_id, int) and 1 <= stable_id <= 65535,
                f"operation {spelling} has invalid stable id")
        require(stable_id not in retired_ids, f"operation {spelling} reuses retired id {stable_id}")
        require(stable_id not in operation_ids, f"operation id {stable_id} is duplicated")
        operation_ids.add(stable_id)
        require(isinstance(spelling, str) and re.fullmatch(r"core\.[a-z0-9_.-]+", spelling),
                f"operation spelling is invalid: {spelling}")
        require(spelling not in retired_spellings, f"operation spelling {spelling} was retired")
        require(spelling not in spellings, f"operation spelling {spelling} is duplicated")
        spellings.add(spelling)
        require(not any(token in spelling for token in SCENARIO_TOKENS),
                f"operation {spelling} encodes a scenario family")
        require(operation["feature"] in feature_names,
                f"operation {spelling} references inactive or unknown feature {operation['feature']}")
        require(operation["epoch"] == registry["epoch"], f"operation {spelling} epoch drifted")
        require(isinstance(operation["class"], str) and operation["class"],
                f"operation {spelling} has empty class")
        require(referenced_types(operation["type_rule"]) <= type_names,
                f"operation {spelling} references an unknown type")
        require(operation["evaluation_order"] in {"none", "left-to-right"},
                f"operation {spelling} has invalid evaluation order")
        require(set(operation["successors"]) == SUCCESSOR_KEYS,
                f"operation {spelling} successor set drifted")
        require(all(isinstance(value, bool) for value in operation["successors"].values()),
                f"operation {spelling} successor values must be boolean")
        arithmetic = operation["arithmetic"]
        require(isinstance(arithmetic, dict) and arithmetic.get("kind") in ARITHMETIC_KINDS,
                f"operation {spelling} has unsupported arithmetic semantics")
        if arithmetic["kind"].startswith("signed-integer") and arithmetic["kind"] != "signed-integer-constant":
            require(arithmetic.get("width") == 64,
                    f"operation {spelling} must name exact integer width")
        require(isinstance(operation["effects"], list)
                and set(operation["effects"]) <= effect_names,
                f"operation {spelling} references unknown effects")
        require(isinstance(operation["capability_requirements"], list)
                and set(operation["capability_requirements"]) <= capability_names,
                f"operation {spelling} references unknown capabilities")
        require(isinstance(operation["ownership"], dict) and operation["ownership"],
                f"operation {spelling} lacks ownership contract")
        require(operation["profile_dependency"] in {"none", "pointer_width"},
                f"operation {spelling} has unknown profile dependency")
        require(isinstance(operation["materialization"], str) and operation["materialization"],
                f"operation {spelling} lacks materialization intent")
        require(operation["determinism"].get("kind") == "deterministic",
                f"W1 operation {spelling} must name deterministic semantics")
        require(isinstance(operation["determinism"].get("allowed_trace"), str)
                and operation["determinism"]["allowed_trace"],
                f"operation {spelling} lacks allowed trace")
        require(operation["kat_validator"] in {
            "aggregate-construct", "aggregate-project", "aggregate-update",
            "block-arguments", "branch", "conditional-branch", "error-publish",
            "owner-copy", "owner-drop", "owner-move", "panic-publish", "place-load",
            "place-local", "place-store", "return", "scalar-oracle", "sealed-call",
            "sealed-invoke", "witness-call", "witness-invoke", "variant-construct",
            "variant-project", "variant-test", "existential-pack",
            "existential-project", "existential-test",
        }, f"operation {spelling} has unknown KAT validator")
        coverage = operation["coverage"]
        require(isinstance(coverage, dict) and set(coverage) == set(CONSUMERS),
                f"operation {spelling} coverage set is incomplete")
        for consumer, task in CONSUMER_TASKS.items():
            entry = coverage[consumer]
            require(isinstance(entry, dict) and set(entry) == {"status", "task"},
                    f"operation {spelling} {consumer} coverage fields drifted")
            require(entry["status"] in STATUS_VALUES,
                    f"operation {spelling} {consumer} has invalid status")
            require(entry["task"] == task,
                    f"operation {spelling} {consumer} must be owned by task {task}")
            require(entry["status"] != "NOT_APPLICABLE",
                    f"active operation {spelling} must apply to {consumer}")
        require(coverage["spec_oracle"]["status"] == "COMPLETE",
                f"operation {spelling} has no normative oracle")
        for consumer in ("verifier", "evaluator", "vm", "aot"):
            if coverage[consumer]["status"] == "COMPLETE":
                require(coverage["decoder"]["status"] == "COMPLETE",
                        f"operation {spelling} {consumer} precedes decoder coverage")
        if coverage["evaluator"]["status"] == "COMPLETE":
            require(coverage["verifier"]["status"] == "COMPLETE",
                    f"operation {spelling} evaluator precedes verifier coverage")
        leaked = walk_keys(operation) & IMPLEMENTATION_KEYS
        require(not leaked, f"operation {spelling} leaks implementation keys {sorted(leaked)}")

    require([row["stable_id"] for row in operations] == sorted(operation_ids),
            "operations must be sorted by stable id")
    require(not (operation_ids & retired_ids), "active and retired operation ids overlap")
    require(not (spellings & retired_spellings), "active and retired operation spellings overlap")
    require("integer-overflow" in trap_names and "profile-unavailable" in trap_names,
            "walking-skeleton trap registry is incomplete")

    return {
        "types_by_name": {row["name"]: row for row in registry["types"]},
        "effects_by_name": {row["name"]: row for row in registry["effects"]},
        "capabilities_by_name": {row["name"]: row for row in registry["capabilities"]},
        "operations_by_spelling": {row["spelling"]: row for row in operations},
    }


def parse_i64(value: Any, owner: str) -> int:
    require(isinstance(value, str) and re.fullmatch(r"-?(0|[1-9][0-9]*)", value),
            f"{owner} must be a canonical decimal i64 string")
    parsed = int(value, 10)
    require(I64_MIN <= parsed <= I64_MAX, f"{owner} is outside i64 range")
    return parsed


def wrap_i64(value: int) -> int:
    unsigned = value % I64_MODULUS
    return unsigned if unsigned <= I64_MAX else unsigned - I64_MODULUS


def scalar_oracle(case: dict[str, Any]) -> dict[str, Any]:
    spelling = case["operation"]
    arguments = case.get("arguments")
    immediates = case.get("immediates")
    require(isinstance(arguments, list), f"KAT {case['id']} arguments must be an array")
    require(isinstance(immediates, dict), f"KAT {case['id']} immediates must be an object")

    if spelling == "core.constant.i64":
        require(not arguments, f"KAT {case['id']} constant must not have operands")
        return {"value": str(parse_i64(immediates.get("value"), f"KAT {case['id']} value"))}
    if spelling == "core.constant.bool":
        value = immediates.get("value")
        require(isinstance(value, bool) and not arguments,
                f"KAT {case['id']} bool constant is malformed")
        return {"value": value}
    if spelling in {"core.add.i64", "core.sub.i64", "core.mul.i64"}:
        require(len(arguments) == 2, f"KAT {case['id']} arithmetic arity is not two")
        left = parse_i64(arguments[0], f"KAT {case['id']} lhs")
        right = parse_i64(arguments[1], f"KAT {case['id']} rhs")
        mode = immediates.get("overflow_mode")
        require(mode in {"checked", "wrapping"}, f"KAT {case['id']} overflow mode is invalid")
        functions: dict[str, Callable[[int, int], int]] = {
            "core.add.i64": lambda a, b: a + b,
            "core.sub.i64": lambda a, b: a - b,
            "core.mul.i64": lambda a, b: a * b,
        }
        mathematical = functions[spelling](left, right)
        if mode == "checked" and not I64_MIN <= mathematical <= I64_MAX:
            return {"trap": "integer-overflow"}
        result = mathematical if mode == "checked" else wrap_i64(mathematical)
        return {"value": str(result)}
    if spelling == "core.div.i64":
        require(len(arguments) == 2, f"KAT {case['id']} division arity is not two")
        require(immediates.get("division_mode") == "trunc-toward-zero",
                f"KAT {case['id']} division mode is invalid")
        left = parse_i64(arguments[0], f"KAT {case['id']} lhs")
        right = parse_i64(arguments[1], f"KAT {case['id']} rhs")
        if right == 0:
            return {"trap": "integer-division-by-zero"}
        if left == I64_MIN and right == -1:
            return {"trap": "integer-division-overflow"}
        magnitude = abs(left) // abs(right)
        quotient = -magnitude if (left < 0) != (right < 0) else magnitude
        return {"value": str(quotient)}
    if spelling == "core.compare.i64":
        require(len(arguments) == 2, f"KAT {case['id']} compare arity is not two")
        left = parse_i64(arguments[0], f"KAT {case['id']} lhs")
        right = parse_i64(arguments[1], f"KAT {case['id']} rhs")
        predicate = immediates.get("predicate")
        predicates: dict[str, Callable[[int, int], bool]] = {
            "eq": lambda a, b: a == b,
            "ne": lambda a, b: a != b,
            "lt": lambda a, b: a < b,
            "le": lambda a, b: a <= b,
            "gt": lambda a, b: a > b,
            "ge": lambda a, b: a >= b,
        }
        require(predicate in predicates, f"KAT {case['id']} compare predicate is invalid")
        return {"value": predicates[predicate](left, right)}
    if spelling == "core.trap":
        require(not arguments and immediates.get("trap") == "explicit-trap",
                f"KAT {case['id']} explicit trap is malformed")
        return {"trap": "explicit-trap"}
    if spelling == "core.target.pointer_width":
        require(not arguments, f"KAT {case['id']} target query must not have operands")
        profile = case.get("profile")
        require(isinstance(profile, dict), f"KAT {case['id']} profile must be an object")
        width = profile.get("pointer_width")
        if width not in {32, 64}:
            return {"trap": "profile-unavailable"}
        return {"value": str(width)}
    raise CoreSpecError(f"KAT {case['id']} has no scalar oracle for {spelling}")


def contract_oracle(case: dict[str, Any], validator: str) -> bool:
    actual = case.get("actual")
    require(isinstance(actual, dict), f"KAT {case['id']} actual contract must be an object")
    if validator == "block-arguments":
        return actual.get("edge_types") == actual.get("parameter_types")
    if validator == "branch":
        return actual.get("argument_types") == actual.get("target_parameter_types")
    if validator == "conditional-branch":
        return (actual.get("condition_type") == "bool"
                and actual.get("true_argument_types") == actual.get("true_parameter_types")
                and actual.get("false_argument_types") == actual.get("false_parameter_types"))
    if validator == "return":
        values = actual.get("value_types")
        result = actual.get("function_result_type")
        return (isinstance(values, list)
                and ((result == "void" and values == []) or values == [result]))
    if validator == "sealed-call":
        return (actual.get("callee_sealed") is True
                and actual.get("argument_types") == actual.get("parameter_types")
                and actual.get("actual_result_type") == actual.get("declared_result_type")
                and actual.get("callee_error_type") == "void"
                and actual.get("callee_panic_type") == "void")
    if validator == "sealed-invoke":
        error_type = actual.get("callee_error_type")
        panic_type = actual.get("callee_panic_type")
        has_error = error_type not in {None, "void"}
        has_panic = panic_type not in {None, "void"}
        return (actual.get("callee_sealed") is True
                and actual.get("argument_types") == actual.get("parameter_types")
                and (has_error or has_panic)
                and actual.get("normal_result_type") == actual.get("callee_result_type")
                and actual.get("error_argument_type") == error_type
                and actual.get("panic_argument_type") == panic_type
                and (not has_error or error_type != "panic-info")
                and (not has_panic or panic_type == "panic-info"))
    if validator == "witness-call":
        ordinal = actual.get("slot_ordinal")
        count = actual.get("slot_count")
        return (actual.get("receiver_interface") == actual.get("slot_interface")
                and isinstance(ordinal, int) and isinstance(count, int)
                and 0 <= ordinal < count
                and actual.get("argument_types") == actual.get("parameter_types")
                and actual.get("actual_result_type") == actual.get("declared_result_type")
                and actual.get("callee_error_type") == "void"
                and actual.get("callee_panic_type") == "void")
    if validator == "witness-invoke":
        ordinal = actual.get("slot_ordinal")
        count = actual.get("slot_count")
        error_type = actual.get("callee_error_type")
        panic_type = actual.get("callee_panic_type")
        has_error = error_type not in {None, "void"}
        has_panic = panic_type not in {None, "void"}
        return (actual.get("receiver_interface") == actual.get("slot_interface")
                and isinstance(ordinal, int) and isinstance(count, int)
                and 0 <= ordinal < count
                and actual.get("argument_types") == actual.get("parameter_types")
                and (has_error or has_panic)
                and actual.get("normal_result_type") == actual.get("callee_result_type")
                and actual.get("error_argument_type") == error_type
                and actual.get("panic_argument_type") == panic_type
                and (not has_error or error_type != "panic-info")
                and (not has_panic or panic_type == "panic-info"))
    if validator == "aggregate-construct":
        return (actual.get("operand_types") == actual.get("field_types")
                and actual.get("result_type") == actual.get("aggregate_type"))
    if validator == "aggregate-project":
        fields = actual.get("field_types")
        ordinal = actual.get("field_ordinal")
        return (actual.get("operand_type") == actual.get("aggregate_type")
                and isinstance(fields, list) and isinstance(ordinal, int)
                and 0 <= ordinal < len(fields) and actual.get("result_type") == fields[ordinal])
    if validator == "aggregate-update":
        fields = actual.get("field_types")
        ordinal = actual.get("field_ordinal")
        return (actual.get("operand_type") == actual.get("aggregate_type")
                and actual.get("result_type") == actual.get("aggregate_type")
                and isinstance(fields, list) and isinstance(ordinal, int)
                and 0 <= ordinal < len(fields) and actual.get("value_type") == fields[ordinal])
    if validator == "variant-construct":
        payloads = actual.get("variant_payload_types")
        ordinal = actual.get("variant_ordinal")
        return (isinstance(payloads, list) and isinstance(ordinal, int)
                and 0 <= ordinal < len(payloads)
                and actual.get("operand_types") == payloads[ordinal]
                and actual.get("result_type") == actual.get("variant_type"))
    if validator == "variant-test":
        count = actual.get("variant_count")
        ordinal = actual.get("variant_ordinal")
        return (actual.get("operand_type") == actual.get("variant_type")
                and actual.get("result_type") == "bool" and isinstance(count, int)
                and isinstance(ordinal, int) and 0 <= ordinal < count)
    if validator == "variant-project":
        payloads = actual.get("variant_payload_types")
        variant = actual.get("variant_ordinal")
        field = actual.get("field_ordinal")
        return (actual.get("operand_type") == actual.get("variant_type")
                and isinstance(payloads, list) and isinstance(variant, int)
                and 0 <= variant < len(payloads) and isinstance(payloads[variant], list)
                and isinstance(field, int) and 0 <= field < len(payloads[variant])
                and actual.get("result_type") == payloads[variant][field])
    if validator == "existential-pack":
        interface_use = actual.get("interface_use")
        expected_category = "place" if interface_use == "ref" else "value"
        expected_ownership = "owner" if interface_use in {"move", "owned-storage"} else "non-owner"
        return (actual.get("concrete_nominal") is True
                and actual.get("conformance_interface") == actual.get("existential_interface")
                and interface_use in {"read", "ref", "move", "owned-storage"}
                and actual.get("operand_category") == expected_category
                and actual.get("result_category") == "value"
                and actual.get("result_ownership") == expected_ownership
                and (interface_use != "read" or actual.get("operand_ownership") == "non-owner"))
    if validator == "existential-test":
        return (actual.get("requested_nominal") is True
                and actual.get("conformance_interface") == actual.get("operand_interface")
                and actual.get("result_type") == "bool")
    if validator == "existential-project":
        interface_use = actual.get("interface_use")
        expected_category = "place" if interface_use == "ref" else "value"
        expected_ownership = "owner" if interface_use in {"move", "owned-storage"} else "non-owner"
        return (actual.get("conformance_interface") == actual.get("operand_interface")
                and actual.get("dominating_exact_test") is True
                and actual.get("result_type") == actual.get("requested_type")
                and actual.get("result_category") == expected_category
                and actual.get("result_ownership") == expected_ownership)
    if validator == "error-publish":
        return (actual.get("function_error_type") not in {None, "void", "panic-info"}
                and actual.get("operand_types") == [actual.get("function_error_type")])
    if validator == "panic-publish":
        return (actual.get("function_panic_type") == "panic-info"
                and actual.get("operand_types") == ["panic-info"])
    if validator == "owner-copy":
        ownership = actual.get("type_ownership")
        operand_ownership = actual.get("operand_ownership")
        result_ownership = actual.get("result_ownership")
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "value"
                and actual.get("result_category") == "value"
                and actual.get("copy_contract") in {"trivial", "explicit"}
                and ownership in {"trivial", "affine"}
                and operand_ownership in {"non-owner", "owner"}
                and (ownership != "trivial" or operand_ownership == "non-owner")
                and result_ownership == ("owner" if ownership == "affine" else "non-owner"))
    if validator == "owner-move":
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "value"
                and actual.get("result_category") == "value")
    if validator == "owner-drop":
        return (actual.get("operand_category") == "value"
                and actual.get("result_type") == "void")
    if validator == "place-local":
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "value"
                and actual.get("result_category") == "place")
    if validator == "place-load":
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "place"
                and actual.get("result_category") == "value")
    if validator == "place-store":
        return (actual.get("place_type") == actual.get("value_type")
                and actual.get("place_category") == "place"
                and actual.get("value_category") == "value"
                and actual.get("result_type") == "void")
    raise CoreSpecError(f"KAT {case['id']} has no contract oracle for {validator}")


def validate_kats(kats: dict[str, Any], indexes: dict[str, dict[Any, dict[str, Any]]]) -> int:
    require(set(kats) == {"schema", "cases"}, "CoreSpec KAT top-level fields drifted")
    require(kats["schema"] == "xray-core-spec-kat/1", "CoreSpec KAT schema must be v1")
    cases = unique_rows(kats["cases"], "CoreSpec KAT", "id")
    operations = indexes["operations_by_spelling"]
    covered: set[str] = set()
    for case in cases:
        require(case.get("operation") in operations,
                f"KAT {case['id']} references unknown operation {case.get('operation')}")
        operation = operations[case["operation"]]
        kind = case.get("kind")
        require(kind in {"scalar", "contract"}, f"KAT {case['id']} has invalid kind")
        expected = case.get("expect")
        require(isinstance(expected, dict) and expected,
                f"KAT {case['id']} has no expected result")
        if kind == "scalar":
            require(operation["kat_validator"] == "scalar-oracle",
                    f"KAT {case['id']} scalar kind conflicts with registry validator")
            actual = scalar_oracle(case)
        else:
            require(operation["kat_validator"] != "scalar-oracle",
                    f"KAT {case['id']} contract kind conflicts with registry validator")
            actual = {"valid": contract_oracle(case, operation["kat_validator"])}
        require(actual == expected,
                f"KAT {case['id']} failed: expected={expected!r} actual={actual!r}")
        covered.add(case["operation"])
    missing = set(operations) - covered
    require(not missing, f"operation rows without normative KATs: {sorted(missing)}")
    return len(cases)


def c_identifier(spelling: str) -> str:
    return re.sub(r"[^A-Z0-9]+", "_", spelling.upper()).strip("_")


def c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def operation_arity(operation: dict[str, Any]) -> int:
    operands = operation["type_rule"]["operands"]
    return 255 if any(name in VARIADIC_TYPES for name in operands) else len(operands)


def mask_for(names: list[str], registry_rows: list[dict[str, Any]]) -> int:
    ids = {row["name"]: row["stable_id"] for row in registry_rows}
    mask = 0
    for name in names:
        stable_id = ids[name]
        require(1 <= stable_id <= 31, f"mask registry id for {name} exceeds 31")
        mask |= 1 << (stable_id - 1)
    return mask


def generate_header(registry: dict[str, Any], digest: str) -> str:
    lines = [
        "/* AUTO-GENERATED by corespecgen - DO NOT EDIT */",
        "/* Source: xisa/core/registry.json */",
        "#ifndef XR_CORE_SPEC_GEN_H",
        "#define XR_CORE_SPEC_GEN_H",
        "",
        "#include <stdbool.h>",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"#define XR_CORE_SPEC_EPOCH {registry['epoch']}u",
        "/* clang-format off */",
        f"#define XR_CORE_SPEC_SEMANTIC_SHA256 {c_string(digest)}",
        "/* clang-format on */",
        f"#define XR_CORE_SPEC_OPERATION_COUNT {len(registry['operations'])}u",
        f"#define XR_CORE_SPEC_FEATURE_COUNT {sum(1 for row in registry['features'] if row['status'] == 'ACTIVE')}u",
        "#define XR_CORE_SPEC_VARIADIC_ARITY UINT8_MAX",
        "",
        "typedef enum XrCoreTypeId {",
    ]
    for row in registry["types"]:
        lines.append(f"    XR_CORE_TYPE_{c_identifier(row['name'])} = {row['stable_id']},")
    lines.extend([
        "} XrCoreTypeId;",
        "",
        "typedef enum XrCoreEffectMask {",
    ])
    for row in registry["effects"]:
        lines.append(
            f"    XR_CORE_EFFECT_{c_identifier(row['name'])} = UINT32_C({1 << (row['stable_id'] - 1)}),")
    lines.extend([
        "} XrCoreEffectMask;",
        "",
        "typedef enum XrCoreCapabilityMask {",
    ])
    for row in registry["capabilities"]:
        lines.append(
            f"    XR_CORE_CAPABILITY_{c_identifier(row['name'])} = UINT32_C({1 << (row['stable_id'] - 1)}),")
    lines.extend([
        "} XrCoreCapabilityMask;",
        "",
        "typedef enum XrCoreFeatureId {",
    ])
    for row in registry["features"]:
        if row["status"] == "ACTIVE":
            lines.append(f"    XR_CORE_FEATURE_{c_identifier(row['name'])} = {row['stable_id']},")
    lines.extend([
        "} XrCoreFeatureId;",
        "",
        "typedef enum XrCoreOperationId {",
    ])
    for row in registry["operations"]:
        lines.append(f"    XR_CORE_OP_{c_identifier(row['spelling'])} = {row['stable_id']},")
    lines.extend([
        "} XrCoreOperationId;",
        "",
        "typedef enum XrCoreCoverageStatus {",
        "    XR_CORE_COVERAGE_COMPLETE = 1,",
        "    XR_CORE_COVERAGE_NOT_YET_ACTIVE = 2,",
        "    XR_CORE_COVERAGE_NOT_APPLICABLE = 3,",
        "} XrCoreCoverageStatus;",
        "",
        "typedef struct XrCoreOperationSpec {",
        "    uint16_t stable_id;",
        "    uint8_t operand_arity;",
        "    uint8_t result_type;",
        "    uint8_t successor_mask;",
        "    uint32_t effect_mask;",
        "    uint32_t capability_mask;",
        "    const char *spelling;",
        "    const char *operation_class;",
        "    const char *feature;",
        "    const char *profile_dependency;",
        "    const char *materialization;",
        "    XrCoreCoverageStatus spec_oracle_status;",
        "    XrCoreCoverageStatus decoder_status;",
        "    XrCoreCoverageStatus verifier_status;",
        "    XrCoreCoverageStatus evaluator_status;",
        "    XrCoreCoverageStatus vm_status;",
        "    XrCoreCoverageStatus aot_status;",
        "} XrCoreOperationSpec;",
        "",
        "extern const XrCoreOperationSpec xr_core_operation_specs[XR_CORE_SPEC_OPERATION_COUNT];",
        "",
        "const XrCoreOperationSpec *xr_core_spec_operation_by_id(uint16_t stable_id);",
        "const XrCoreOperationSpec *xr_core_spec_operation_by_spelling(const char *spelling);",
        "bool xr_core_spec_feature_active(uint16_t stable_id);",
        "",
        "#endif /* XR_CORE_SPEC_GEN_H */",
        "",
    ])
    return "\n".join(lines)


def coverage_status_name(status: str) -> str:
    return f"XR_CORE_COVERAGE_{status}"


def generate_source(registry: dict[str, Any]) -> str:
    type_ids = {row["name"]: row["stable_id"] for row in registry["types"]}
    successor_bits = {name: 1 << index for index, name in enumerate(
        ("normal", "error", "panic", "cancel", "suspend"))}
    lines = [
        "/* AUTO-GENERATED by corespecgen - DO NOT EDIT */",
        "/* Source: xisa/core/registry.json */",
        "#include \"core/xr_core_spec_gen.h\"",
        "",
        "#include <string.h>",
        "",
        "const XrCoreOperationSpec xr_core_operation_specs[XR_CORE_SPEC_OPERATION_COUNT] = {",
    ]
    for operation in registry["operations"]:
        result = operation["type_rule"]["result"]
        result_id = type_ids.get(result, type_ids["type-variable"])
        successor_mask = sum(bit for name, bit in successor_bits.items()
                             if operation["successors"][name])
        effect_mask = mask_for(operation["effects"], registry["effects"])
        capability_mask = mask_for(operation["capability_requirements"], registry["capabilities"])
        coverage = operation["coverage"]
        lines.extend([
            "    {",
            f"        {operation['stable_id']}u,",
            f"        {operation_arity(operation)}u,",
            f"        {result_id}u,",
            f"        {successor_mask}u,",
            f"        UINT32_C({effect_mask}),",
            f"        UINT32_C({capability_mask}),",
            f"        {c_string(operation['spelling'])},",
            f"        {c_string(operation['class'])},",
            f"        {c_string(operation['feature'])},",
            f"        {c_string(operation['profile_dependency'])},",
            f"        {c_string(operation['materialization'])},",
            f"        {coverage_status_name(coverage['spec_oracle']['status'])},",
            f"        {coverage_status_name(coverage['decoder']['status'])},",
            f"        {coverage_status_name(coverage['verifier']['status'])},",
            f"        {coverage_status_name(coverage['evaluator']['status'])},",
            f"        {coverage_status_name(coverage['vm']['status'])},",
            f"        {coverage_status_name(coverage['aot']['status'])},",
            "    },",
        ])
    lines.extend([
        "};",
        "",
        "const XrCoreOperationSpec *xr_core_spec_operation_by_id(uint16_t stable_id) {",
        "    size_t index;",
        "    for (index = 0; index < XR_CORE_SPEC_OPERATION_COUNT; ++index) {",
        "        if (xr_core_operation_specs[index].stable_id == stable_id)",
        "            return &xr_core_operation_specs[index];",
        "    }",
        "    return NULL;",
        "}",
        "",
        "const XrCoreOperationSpec *xr_core_spec_operation_by_spelling(const char *spelling) {",
        "    size_t index;",
        "    if (!spelling)",
        "        return NULL;",
        "    for (index = 0; index < XR_CORE_SPEC_OPERATION_COUNT; ++index) {",
        "        if (strcmp(xr_core_operation_specs[index].spelling, spelling) == 0)",
        "            return &xr_core_operation_specs[index];",
        "    }",
        "    return NULL;",
        "}",
        "",
        "bool xr_core_spec_feature_active(uint16_t stable_id) {",
        "    switch (stable_id) {",
    ])
    for feature in registry["features"]:
        if feature["status"] == "ACTIVE":
            lines.append(f"        case {feature['stable_id']}u:")
    lines.extend([
        "            return true;",
        "        default:",
        "            return false;",
        "    }",
        "}",
        "",
    ])
    return "\n".join(lines)


def generate_markdown(registry: dict[str, Any], digest: str, kat_count: int) -> str:
    lines = [
        "# Xray CoreSpec Operation Registry",
        "",
        "> AUTO-GENERATED by `tools/corespecgen/corespecgen.py`; do not edit.",
        f"> Epoch: `{registry['epoch']}`; semantic SHA-256: `{digest}`; normative KATs: `{kat_count}`.",
        "",
        "Unknown feature IDs, operation IDs, type rules, and arithmetic kinds fail closed. The table is target-neutral; it does not contain C spellings, VM handlers, slots, frames, registers, or quickened opcodes.",
        "",
        "| Stable ID | Operation | Class | Type rule | Effects | Profile | Materialization | Consumer state |",
        "|---:|---|---|---|---|---|---|---|",
    ]
    for operation in registry["operations"]:
        type_rule = operation["type_rule"]
        operands = ", ".join(type_rule["operands"]) or "-"
        effects = ", ".join(operation["effects"]) or "-"
        consumers = ", ".join(
            f"{name}={operation['coverage'][name]['status']}"
            for name in CONSUMERS
        )
        lines.append(
            f"| {operation['stable_id']} | `{operation['spelling']}` | {operation['class']} | "
            f"`({operands}) -> {type_rule['result']}` | {effects} | "
            f"{operation['profile_dependency']} | {operation['materialization']} | {consumers} |"
        )
    lines.extend(["", "## Normative arithmetic rules", ""])
    for operation in registry["operations"]:
        if operation["arithmetic"]["kind"] != "none":
            lines.append(f"- `{operation['spelling']}`: `{json.dumps(operation['arithmetic'], sort_keys=True)}`")
    lines.append("")
    return "\n".join(lines)


def generate_coverage(registry: dict[str, Any], digest: str, kat_count: int) -> str:
    operations = []
    missing = {consumer: [] for consumer in CONSUMERS}
    for operation in registry["operations"]:
        coverage = operation["coverage"]
        operations.append({
            "stable_id": operation["stable_id"],
            "spelling": operation["spelling"],
            "coverage": coverage,
        })
        for consumer in CONSUMERS:
            if coverage[consumer]["status"] != "COMPLETE":
                missing[consumer].append(operation["spelling"])
    value = {
        "schema": "xray-core-spec-coverage/1",
        "semantic_sha256": digest,
        "epoch": registry["epoch"],
        "operation_count": len(operations),
        "normative_kat_count": kat_count,
        "operations": operations,
        "missing_rows": missing,
    }
    return canonical_json(value)


def generate_outputs(registry: dict[str, Any], kats: dict[str, Any]) -> dict[Path, str]:
    indexes = validate_registry(registry)
    kat_count = validate_kats(kats, indexes)
    digest = semantic_registry_digest(registry)
    return {
        OUTPUT_PATHS[0]: generate_header(registry, digest),
        OUTPUT_PATHS[1]: generate_source(registry),
        OUTPUT_PATHS[2]: generate_markdown(registry, digest, kat_count),
        OUTPUT_PATHS[3]: generate_coverage(registry, digest, kat_count),
    }


def load_inputs(root: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    schema = read_json(root / SCHEMA_PATH)
    validate_schema_document(schema)
    registry = read_json(root / REGISTRY_PATH)
    kats = read_json(root / KAT_PATH)
    return registry, kats


def write_outputs(root: Path, outputs: dict[Path, str]) -> None:
    for relative, content in outputs.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and path.read_text(encoding="utf-8", errors="strict") == content:
            continue
        path.write_text(content, encoding="utf-8", errors="strict")
        print(f"corespecgen: generated {relative}")


def check_outputs(root: Path, outputs: dict[Path, str]) -> None:
    for relative, expected in outputs.items():
        path = root / relative
        require(path.is_file(), f"generated output is missing: {relative}")
        actual = path.read_text(encoding="utf-8", errors="strict")
        require(actual == expected,
                f"generated output is stale: {relative}; run corespecgen.py --generate")


def expect_invalid(label: str, registry: dict[str, Any], kats: dict[str, Any]) -> None:
    try:
        generate_outputs(registry, kats)
    except CoreSpecError:
        return
    raise CoreSpecError(f"self-test mutation was accepted: {label}")


def self_test(registry: dict[str, Any], kats: dict[str, Any]) -> None:
    first = generate_outputs(registry, kats)
    second = generate_outputs(copy.deepcopy(registry), copy.deepcopy(kats))
    require(first == second, "generation is nondeterministic")

    governance = copy.deepcopy(registry)
    governance["operations"][0]["coverage"]["vm"]["status"] = "COMPLETE"
    governance["types"][0]["description"] += " Editorial clarification."
    require(semantic_registry_digest(governance) == semantic_registry_digest(registry),
            "governance or prose changed semantic identity")
    semantic = copy.deepcopy(registry)
    semantic["operations"][0]["evaluation_order"] = "left-to-right"
    require(semantic_registry_digest(semantic) != semantic_registry_digest(registry),
            "semantic mutation did not change semantic identity")

    mutation = copy.deepcopy(registry)
    mutation["operations"][1]["stable_id"] = mutation["operations"][0]["stable_id"]
    expect_invalid("duplicate stable id", mutation, kats)

    mutation = copy.deepcopy(registry)
    mutation["operations"][0]["stable_id"] = mutation["retired_operation_ids"][0]["stable_id"]
    expect_invalid("retired id reuse", mutation, kats)

    mutation = copy.deepcopy(registry)
    mutation["operations"][0]["feature"] = "core.unknown"
    expect_invalid("unknown feature", mutation, kats)

    mutation = copy.deepcopy(registry)
    del mutation["operations"][0]["coverage"]["aot"]
    expect_invalid("missing consumer coverage", mutation, kats)

    mutation = copy.deepcopy(registry)
    mutation["operations"][0]["arithmetic"] = {"kind": "host-float"}
    expect_invalid("host arithmetic placeholder", mutation, kats)

    mutation = copy.deepcopy(registry)
    mutation["operations"][0]["vm_handler"] = "execute_const"
    expect_invalid("implementation key in registry", mutation, kats)

    kat_mutation = copy.deepcopy(kats)
    kat_mutation["cases"] = [
        case for case in kat_mutation["cases"]
        if case["operation"] != registry["operations"][0]["spelling"]
    ]
    expect_invalid("dead operation without KAT", registry, kat_mutation)

    kat_mutation = copy.deepcopy(kats)
    kat_mutation["cases"][0]["operation"] = "core.unknown"
    expect_invalid("KAT with unknown operation", registry, kat_mutation)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--generate", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        registry, kats = load_inputs(root)
        outputs = generate_outputs(registry, kats)
        if args.generate:
            write_outputs(root, outputs)
        elif args.check:
            check_outputs(root, outputs)
            digest = semantic_registry_digest(registry)
            print(f"CoreSpec registry: PASS ({len(registry['operations'])} operations, "
                  f"{len(kats['cases'])} KATs, semantic-sha256={digest})")
        else:
            self_test(registry, kats)
            print("CoreSpec registry self-test: PASS")
    except (CoreSpecError, OSError, UnicodeError) as exc:
        print(f"corespecgen: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
