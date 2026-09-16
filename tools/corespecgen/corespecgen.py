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
SUCCESSOR_KEYS = {"normal", "error", "panic", "trap", "cancel", "suspend"}
GENERIC_TYPES = {
    "A", "C", "Capture?", "E", "V", "T", "T...", "R", "R?", "P...",
    "TargetEnum",
    "normal-edge-values...", "error-edge-values...", "panic-edge-values...",
    "trap-edge-values...", "cancel-edge-values...", "suspend-edge-values...", "request-values...",
}
VARIADIC_TYPES = {
    "T...", "P...", "normal-edge-values...", "error-edge-values...", "panic-edge-values...",
    "trap-edge-values...", "cancel-edge-values...", "suspend-edge-values...", "request-values...",
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
    domain = type_rule.get("operand_domain")
    if domain is not None:
        require(isinstance(domain, list) and domain
                and all(isinstance(name, str) and name not in GENERIC_TYPES for name in domain)
                and len(domain) == len(set(domain)),
                "operation type_rule.operand_domain must list unique concrete types")
        require(any(name in VARIADIC_TYPES for name in operands),
                "operation type_rule.operand_domain requires a variadic operand rule")
        result.update(domain)
    require(set(type_rule) <= {"operands", "result", "immediates", "operand_domain"},
            "operation type_rule has unknown fields")
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
        require(operation["profile_dependency"] in {
            "none", "pointer_width", "operating_system", "architecture", "native_abi",
            "endianness", "provider_contract", "scheduler-suspension",
        },
                f"operation {spelling} has unknown profile dependency")
        require(isinstance(operation["materialization"], str) and operation["materialization"],
                f"operation {spelling} lacks materialization intent")
        require(operation["determinism"].get("kind") == "deterministic",
                f"W1 operation {spelling} must name deterministic semantics")
        require(isinstance(operation["determinism"].get("allowed_trace"), str)
                and operation["determinism"]["allowed_trace"],
                f"operation {spelling} lacks allowed trace")
        require(operation["kat_validator"] in {
            "aggregate-construct", "aggregate-project", "aggregate-update", "assert-condition",
            "class-construct", "class-share", "class-field-load", "class-field-place",
            "block-arguments", "branch", "cancel-publish", "conditional-branch", "error-publish",
            "owner-copy", "owner-drop", "owner-move", "panic-publish", "place-load",
            "place-exchange", "place-local", "place-module", "place-initialize",
            "place-project", "place-store", "place-take", "return",
            "scalar-oracle", "sealed-call",
            "sealed-invoke", "indirect-call", "indirect-invoke", "witness-call",
            "witness-invoke", "callable-pack", "variant-construct",
            "variant-project", "variant-test", "existential-pack",
            "existential-project", "existential-reborrow-read", "existential-test",
            "provider-call", "output-group",
            "coroutine-yield", "coroutine-suspend", "coroutine-call",
            "coroutine-indirect-call",
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
            require(consumer == "evaluator" or entry["status"] != "NOT_APPLICABLE",
                    f"active operation {spelling} must apply to {consumer}")
        require(coverage["spec_oracle"]["status"] == "COMPLETE",
                f"operation {spelling} has no normative oracle")
        for consumer in ("verifier", "evaluator", "vm", "aot"):
            if coverage[consumer]["status"] == "COMPLETE":
                require(coverage["decoder"]["status"] == "COMPLETE",
                        f"operation {spelling} {consumer} precedes decoder coverage")
        for consumer in ("evaluator", "vm", "aot"):
            if coverage[consumer]["status"] == "COMPLETE":
                require(coverage["verifier"]["status"] == "COMPLETE",
                        f"operation {spelling} {consumer} precedes verifier coverage")
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


RUNE_MAX = 0x10FFFF
SURROGATE_MIN = 0xD800
SURROGATE_MAX = 0xDFFF
COMPARE_PREDICATES: dict[str, Callable[[Any, Any], bool]] = {
    "eq": lambda a, b: a == b,
    "ne": lambda a, b: a != b,
    "lt": lambda a, b: a < b,
    "le": lambda a, b: a <= b,
    "gt": lambda a, b: a > b,
    "ge": lambda a, b: a >= b,
}


def rune_is_scalar_value(value: Any) -> bool:
    return (isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= RUNE_MAX
            and not SURROGATE_MIN <= value <= SURROGATE_MAX)


def parse_rune(value: Any, owner: str) -> int:
    require(rune_is_scalar_value(value), f"{owner} must be a Unicode scalar value")
    return value


def string_bytes(value: Any, owner: str) -> bytes | None:
    """Decode one KAT string operand.

    A JSON string names its UTF-8 encoding. An object with a `bytes_hex` field
    names raw bytes so hostile sequences can be written down; the oracle
    answers None for a sequence that is not valid UTF-8."""
    if isinstance(value, str):
        return value.encode("utf-8")
    require(isinstance(value, dict) and set(value) == {"bytes_hex"}
            and isinstance(value["bytes_hex"], str)
            and re.fullmatch(r"(?:[0-9a-f]{2})*", value["bytes_hex"]),
            f"{owner} must be a string or a bytes_hex object")
    raw = bytes.fromhex(value["bytes_hex"])
    try:
        decoded = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    if any(SURROGATE_MIN <= ord(char) <= SURROGATE_MAX for char in decoded):
        return None
    return raw


def string_result(raw: bytes) -> dict[str, Any]:
    return {"value": raw.decode("utf-8", errors="strict"), "byte_length": len(raw)}


def render_display_operand(operand: Any, owner: str) -> bytes | None:
    """Render one typed output operand in its canonical display form."""
    require(isinstance(operand, dict) and set(operand) == {"type", "value"},
            f"{owner} must name a type and a value")
    kind = operand["type"]
    value = operand["value"]
    if kind == "i64":
        return str(parse_i64(value, owner)).encode("ascii")
    if kind == "bool":
        require(isinstance(value, bool), f"{owner} bool operand must be a JSON bool")
        return b"true" if value else b"false"
    if kind == "string":
        return string_bytes(value, owner)
    if kind == "rune":
        return chr(parse_rune(value, owner)).encode("utf-8")
    return None


def render_output_group(operands: list[Any], owner: str) -> bytes | None:
    pieces = []
    for index, operand in enumerate(operands):
        rendered = render_display_operand(operand, f"{owner} operand {index}")
        if rendered is None:
            return None
        pieces.append(rendered)
    return b" ".join(pieces) + b"\n"


def scalar_oracle(case: dict[str, Any]) -> dict[str, Any]:
    spelling = case["operation"]
    arguments = case.get("arguments")
    immediates = case.get("immediates")
    require(isinstance(arguments, list), f"KAT {case['id']} arguments must be an array")
    require(isinstance(immediates, dict), f"KAT {case['id']} immediates must be an object")

    if spelling == "core.constant.string":
        require(not arguments, f"KAT {case['id']} string constant must not have operands")
        raw = string_bytes(immediates.get("value"), f"KAT {case['id']} value")
        if raw is None:
            return {"rejected": "invalid-utf8"}
        return string_result(raw)
    if spelling == "core.constant.rune":
        require(not arguments, f"KAT {case['id']} rune constant must not have operands")
        value = immediates.get("value")
        require(isinstance(value, int) and not isinstance(value, bool),
                f"KAT {case['id']} rune constant must be an integer")
        if not rune_is_scalar_value(value):
            return {"rejected": "not-a-unicode-scalar-value"}
        return {"value": value}
    if spelling == "core.string.from_i64":
        require(len(arguments) == 1 and not immediates,
                f"KAT {case['id']} string.from_i64 contract is malformed")
        return string_result(str(parse_i64(arguments[0], f"KAT {case['id']} operand")).encode("ascii"))
    if spelling == "core.string.concat":
        require(len(arguments) == 2 and not immediates,
                f"KAT {case['id']} string.concat arity is not two")
        left = string_bytes(arguments[0], f"KAT {case['id']} lhs")
        right = string_bytes(arguments[1], f"KAT {case['id']} rhs")
        require(left is not None and right is not None,
                f"KAT {case['id']} string.concat operands must be valid UTF-8")
        return string_result(left + right)
    if spelling == "core.compare.string":
        require(len(arguments) == 2, f"KAT {case['id']} string compare arity is not two")
        left = string_bytes(arguments[0], f"KAT {case['id']} lhs")
        right = string_bytes(arguments[1], f"KAT {case['id']} rhs")
        require(left is not None and right is not None,
                f"KAT {case['id']} string compare operands must be valid UTF-8")
        predicate = immediates.get("predicate")
        require(predicate in COMPARE_PREDICATES, f"KAT {case['id']} compare predicate is invalid")
        return {"value": COMPARE_PREDICATES[predicate](left, right)}
    if spelling == "core.compare.rune":
        require(len(arguments) == 2, f"KAT {case['id']} rune compare arity is not two")
        left = parse_rune(arguments[0], f"KAT {case['id']} lhs")
        right = parse_rune(arguments[1], f"KAT {case['id']} rhs")
        predicate = immediates.get("predicate")
        require(predicate in COMPARE_PREDICATES, f"KAT {case['id']} compare predicate is invalid")
        return {"value": COMPARE_PREDICATES[predicate](left, right)}

    if spelling == "core.constant.i64":
        require(not arguments, f"KAT {case['id']} constant must not have operands")
        return {"value": str(parse_i64(immediates.get("value"), f"KAT {case['id']} value"))}
    if spelling == "core.constant.bool":
        value = immediates.get("value")
        require(isinstance(value, bool) and not arguments,
                f"KAT {case['id']} bool constant is malformed")
        return {"value": value}
    if spelling == "core.logical.not":
        require(len(arguments) == 1 and isinstance(arguments[0], bool) and not immediates,
                f"KAT {case['id']} logical not contract is malformed")
        return {"value": not arguments[0]}
    if spelling in {"core.logical.and", "core.logical.or"}:
        require(len(arguments) == 2 and all(isinstance(value, bool) for value in arguments)
                and not immediates,
                f"KAT {case['id']} logical binary contract is malformed")
        return {"value": (arguments[0] and arguments[1])
                if spelling == "core.logical.and" else (arguments[0] or arguments[1])}
    if spelling == "core.constant.target_enum":
        enum_type = immediates.get("type")
        value = immediates.get("value")
        domains = {"TargetOs": 5, "TargetArch": 5, "TargetAbi": 9, "TargetEndian": 2}
        require(enum_type in domains and isinstance(value, int) and 1 <= value <= domains[enum_type],
                f"KAT {case['id']} target enum constant is malformed")
        require(not arguments, f"KAT {case['id']} target enum constant has operands")
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
    if spelling == "core.compare.target_enum":
        require(len(arguments) == 2, f"KAT {case['id']} target enum compare arity is not two")
        enum_type = immediates.get("type")
        predicate = immediates.get("predicate")
        domains = {"TargetOs": 5, "TargetArch": 5, "TargetAbi": 9, "TargetEndian": 2}
        require(enum_type in domains and predicate in {"eq", "ne"},
                f"KAT {case['id']} target enum compare contract is malformed")
        require(all(isinstance(value, int) and 1 <= value <= domains[enum_type]
                    for value in arguments),
                f"KAT {case['id']} target enum compare value is outside its domain")
        equal = arguments[0] == arguments[1]
        return {"value": equal if predicate == "eq" else not equal}
    if spelling == "core.trap":
        trap = immediates.get("trap")
        require(not arguments and trap in {"explicit-trap", "provider-call-failed"},
                f"KAT {case['id']} named trap is malformed")
        return {"trap": trap}
    target_fields = {
        "core.target.pointer_width": ("pointer_width", {32, 64}),
        "core.target.operating_system": ("operating_system", set(range(1, 6))),
        "core.target.architecture": ("architecture", set(range(1, 6))),
        "core.target.native_abi": ("native_abi", set(range(1, 10))),
        "core.target.endianness": ("endianness", {1, 2}),
    }
    if spelling in target_fields:
        require(not arguments, f"KAT {case['id']} target query must not have operands")
        profile = case.get("profile")
        require(isinstance(profile, dict), f"KAT {case['id']} profile must be an object")
        field, valid_values = target_fields[spelling]
        value = profile.get(field)
        if value not in valid_values:
            return {"trap": "profile-unavailable"}
        return {"value": str(value)}
    raise CoreSpecError(f"KAT {case['id']} has no scalar oracle for {spelling}")


def construct_ownership_valid(actual: dict[str, Any], field_types: Any,
                              field_ownerships: Any) -> bool:
    """Check a construct's explicit owner-state transition, never an implicit copy."""
    values = actual.get("operand_values")
    dispositions = actual.get("operand_ownerships")
    before = actual.get("consumed_before")
    after = actual.get("consumed_after")
    type_ownership = actual.get("type_ownership")
    if (not isinstance(field_types, list) or not isinstance(field_ownerships, list)
            or not isinstance(values, list) or not isinstance(dispositions, list)
            or not isinstance(before, list) or not isinstance(after, list)
            or len({len(field_types), len(field_ownerships), len(values), len(dispositions)}) != 1
            or any(not isinstance(name, str) or not name for name in field_types)
            or any(ownership not in ("trivial", "affine") for ownership in field_ownerships)
            or type_ownership not in ("trivial", "affine")
            or ("affine" in field_ownerships and type_ownership != "affine")
            or actual.get("result_ownership") !=
                ("owner" if type_ownership == "affine" else "non-owner")):
        return False
    if any(not isinstance(value, str) or not value or value.strip() != value
           for value in values + before + after):
        return False
    if len(set(before)) != len(before) or len(set(after)) != len(after):
        return False
    consumed = set(before)
    identities: dict[str, tuple[str, str]] = {}
    for value, field_type, ownership, disposition in zip(
            values, field_types, field_ownerships, dispositions):
        if value in consumed or disposition != ("owner" if ownership == "affine" else "non-owner"):
            return False
        identity = (field_type, ownership)
        if value in identities and identities[value] != identity:
            return False
        identities[value] = identity
        if ownership == "affine":
            consumed.add(value)
    return consumed == set(after)


def continuation_graph_valid(actual: dict[str, Any], *, cancel: bool, trap: bool) -> bool:
    """Check explicit KAT edge facts, never a claimed cleanup-valid boolean.

    These are finite control-flow test inputs, not another executable format.
    Refusal edges change the pending reason; ordinary edges preserve it. Cycles
    are legal: this proves exit closure, not termination of arbitrary bodies.
    """
    if "cancel_terminal" in actual or "trap_terminal" in actual:
        return False
    graph = actual.get("continuation_graph")
    if not isinstance(graph, dict) or set(graph) != {"entries", "blocks"}:
        return False
    entries, blocks = graph["entries"], graph["blocks"]
    reasons = {"normal"} | ({"cancel"} if cancel else set()) | ({"trap7"} if trap else set())
    if (not isinstance(entries, dict) or set(entries) != reasons
            or not isinstance(blocks, list) or not blocks):
        return False

    def valid_target(value: Any) -> bool:
        return type(value) is int and 0 <= value < len(blocks)

    exits = {"branch", "return", "error", "panic", "cancel", "trap7", "trap-other", "suspend"}
    for block in blocks:
        if (not isinstance(block, dict) or set(block) != {"exit", "flow", "refusal"}
                or not isinstance(block["exit"], str) or block["exit"] not in exits
                or not isinstance(block["flow"], list)
                or not isinstance(block["refusal"], list)
                or any(not valid_target(target) for target in block["flow"] + block["refusal"])
                or bool(block["flow"]) != (block["exit"] == "branch")):
            return False
    pending: list[int] = []
    incoming: dict[int, str] = {}

    def admit(target: int, reason: str) -> bool:
        if not valid_target(target):
            return False
        if target in incoming:
            return incoming[target] == reason
        incoming[target] = reason
        pending.append(target)
        return True

    for reason, target in entries.items():
        if not admit(target, reason):
            return False
    while pending:
        target = pending.pop()
        reason, block = incoming[target], blocks[target]
        exit_kind = block["exit"]
        if exit_kind != "branch":
            if ((reason == "cancel" and exit_kind != "cancel")
                    or (reason == "trap7" and exit_kind != "trap7")
                    or (reason == "normal" and exit_kind == "cancel")):
                return False
        for successor in block["flow"]:
            if not admit(successor, reason):
                return False
        for successor in block["refusal"]:
            if not admit(successor, "trap7"):
                return False
    return len(incoming) == len(blocks)


def contract_oracle(case: dict[str, Any], validator: str) -> bool:
    actual = case.get("actual")
    require(isinstance(actual, dict), f"KAT {case['id']} actual contract must be an object")
    if validator in {"provider-call", "sealed-call", "sealed-invoke", "indirect-call",
                     "indirect-invoke", "witness-call", "witness-invoke"}:
        typed_successors = 0
        if validator.endswith("-invoke"):
            prefix = "callable" if validator == "indirect-invoke" else "callee"
            typed_successors = (1 + int(actual.get(f"{prefix}_error_type") not in {None, "void"})
                                + int(actual.get(f"{prefix}_panic_type") not in {None, "void"}))
        if (actual.get("successor_count", typed_successors) != typed_successors
                and not continuation_graph_valid(actual, cancel=False, trap=True)):
            return False
    if validator == "provider-call":
        operand_count = actual.get("provider_operand_count", actual.get("operand_count"))
        operand_types = actual.get("provider_operand_types", actual.get("operand_types"))
        operand_categories = actual.get("provider_operand_categories",
                                        actual.get("operand_categories"))
        operand_ownerships = actual.get("provider_operand_ownerships",
                                        actual.get("operand_ownerships"))
        if operand_count is None and isinstance(operand_types, list):
            operand_count = len(operand_types)
        structural_shape = (
            operand_count == 1
            and operand_types == ["i64"]
            and operand_categories == ["value"]
            and operand_ownerships == ["non-owner"]
            and actual.get("result_type") == "i64"
        ) or (
            operand_count == 0
            and actual.get("result_type") in {"i64", "optional-i64-pair"}
        )
        has_trap_edge = actual.get("successor_count", 0) != 0
        live_values = actual.get("live_values")
        edge_values = actual.get("trap_edge_values")
        trap_edge_valid = (
            actual.get("successor_count", 0) == 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(live_values, list)
            and live_values == edge_values
        )
        return (structural_shape
                and operand_categories in (None, [], ["value"])
                and operand_ownerships in (None, [], ["non-owner"])
                and actual.get("result_category") == "value"
                and actual.get("result_ownership") == "non-owner"
                and actual.get("provider_requirement") is True
                and (not has_trap_edge or trap_edge_valid))
    if validator == "output-group":
        operands = actual.get("operands")
        if not isinstance(operands, list):
            return False
        rendered = render_output_group(operands, f"KAT {case['id']}")
        expected_hex = actual.get("rendered_hex")
        if rendered is None:
            return False
        if expected_hex is not None and (not isinstance(expected_hex, str)
                                         or bytes.fromhex(expected_hex) != rendered):
            return False
        categories = actual.get("operand_categories")
        ownerships = actual.get("operand_ownerships")
        return (actual.get("result_type") == "void"
                and categories == ["value"] * len(operands)
                and ownerships == ["non-owner"] * len(operands)
                and actual.get("provider_requirement") is True
                and actual.get("atomic_group") is True
                and actual.get("separator") == "space"
                and actual.get("terminator") == "line-feed")
    if validator == "block-arguments":
        return actual.get("edge_types") == actual.get("parameter_types")
    if validator == "branch":
        return actual.get("argument_types") == actual.get("target_parameter_types")
    if validator == "conditional-branch":
        return (actual.get("condition_type") == "bool"
                and actual.get("true_argument_types") == actual.get("true_parameter_types")
                and actual.get("false_argument_types") == actual.get("false_parameter_types"))
    if validator == "assert-condition":
        has_panic_edge = actual.get("successor_count", 0) != 0
        panic_edge_valid = (
            actual.get("successor_count", 0) == 1
            and actual.get("panic_type") == "panic-info"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("panic_edge_values")
        )
        return (actual.get("condition_type") == "bool"
                and actual.get("failure_kind") == "condition-false"
                and (not has_panic_edge or panic_edge_valid))
    if validator == "return":
        values = actual.get("value_types")
        result = actual.get("function_result_type")
        return (isinstance(values, list)
                and ((result == "void" and values == []) or values == [result]))
    if validator == "sealed-call":
        has_trap_edge = actual.get("successor_count", 0) != 0
        trap_edge_valid = (
            actual.get("successor_count", 0) == 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("trap_edge_values")
        )
        return (actual.get("callee_sealed") is True
                and actual.get("argument_types") == actual.get("parameter_types")
                and actual.get("actual_result_type") == actual.get("declared_result_type")
                and actual.get("callee_error_type") == "void"
                and actual.get("callee_panic_type") in {"void", "panic-info"}
                and (not has_trap_edge or trap_edge_valid))
    if validator == "sealed-invoke":
        error_type = actual.get("callee_error_type")
        panic_type = actual.get("callee_panic_type")
        has_error = error_type not in {None, "void"}
        has_panic = panic_type not in {None, "void"}
        typed_successors = 1 + int(has_error) + int(has_panic)
        successor_count = actual.get("successor_count", typed_successors)
        has_trap_edge = successor_count != typed_successors
        trap_edge_valid = (
            successor_count == typed_successors + 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("trap_edge_values")
        )
        return (actual.get("callee_sealed") is True
                and actual.get("argument_types") == actual.get("parameter_types")
                and (has_error or has_panic)
                and actual.get("normal_result_type") == actual.get("callee_result_type")
                and actual.get("error_argument_type") == error_type
                and actual.get("panic_argument_type") == panic_type
                and (not has_error or error_type != "panic-info")
                and (not has_panic or panic_type == "panic-info")
                and (not has_trap_edge or trap_edge_valid))
    if validator == "indirect-call":
        has_trap_edge = actual.get("successor_count", 0) != 0
        trap_edge_valid = (
            actual.get("successor_count", 0) == 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("trap_edge_values")
        )
        return (actual.get("operand_kind") == "callable"
                and actual.get("argument_types") == actual.get("parameter_types")
                and actual.get("actual_result_type") == actual.get("declared_result_type")
                and actual.get("callable_error_type") == "void"
                and actual.get("callable_panic_type") == "void"
                and actual.get("callable_may_suspend") is False
                and actual.get("callable_may_cancel") is False
                and (not has_trap_edge or trap_edge_valid))
    if validator == "indirect-invoke":
        error_type = actual.get("callable_error_type")
        panic_type = actual.get("callable_panic_type")
        has_error = error_type not in {None, "void"}
        has_panic = panic_type not in {None, "void"}
        typed_successors = 1 + int(has_error) + int(has_panic)
        successor_count = actual.get("successor_count", typed_successors)
        has_trap_edge = successor_count != typed_successors
        trap_edge_valid = (
            successor_count == typed_successors + 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("trap_edge_values")
        )
        return (actual.get("operand_kind") == "callable"
                and actual.get("argument_types") == actual.get("parameter_types")
                and (has_error or has_panic)
                and actual.get("normal_result_type") == actual.get("callable_result_type")
                and actual.get("error_argument_type") == error_type
                and actual.get("panic_argument_type") == panic_type
                and (not has_error or error_type != "panic-info")
                and (not has_panic or panic_type == "panic-info")
                and actual.get("callable_may_suspend") is False
                and actual.get("callable_may_cancel") is False
                and (not has_trap_edge or trap_edge_valid))
    if validator == "witness-call":
        ordinal = actual.get("slot_ordinal")
        count = actual.get("slot_count")
        has_trap_edge = actual.get("successor_count", 0) != 0
        trap_edge_valid = (
            actual.get("successor_count", 0) == 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("trap_edge_values")
        )
        return (actual.get("receiver_interface") == actual.get("slot_interface")
                and isinstance(ordinal, int) and isinstance(count, int)
                and 0 <= ordinal < count
                and actual.get("argument_types") == actual.get("parameter_types")
                and actual.get("actual_result_type") == actual.get("declared_result_type")
                and actual.get("callee_error_type") == "void"
                and actual.get("callee_panic_type") == "void"
                and (not has_trap_edge or trap_edge_valid))
    if validator == "witness-invoke":
        ordinal = actual.get("slot_ordinal")
        count = actual.get("slot_count")
        error_type = actual.get("callee_error_type")
        panic_type = actual.get("callee_panic_type")
        has_error = error_type not in {None, "void"}
        has_panic = panic_type not in {None, "void"}
        typed_successors = 1 + int(has_error) + int(has_panic)
        successor_count = actual.get("successor_count", typed_successors)
        has_trap_edge = successor_count != typed_successors
        trap_edge_valid = (
            successor_count == typed_successors + 1
            and actual.get("trap") == "provider-call-failed"
            and isinstance(actual.get("live_values"), list)
            and actual.get("live_values") == actual.get("trap_edge_values")
        )
        return (actual.get("receiver_interface") == actual.get("slot_interface")
                and isinstance(ordinal, int) and isinstance(count, int)
                and 0 <= ordinal < count
                and actual.get("argument_types") == actual.get("parameter_types")
                and (has_error or has_panic)
                and actual.get("normal_result_type") == actual.get("callee_result_type")
                and actual.get("error_argument_type") == error_type
                and actual.get("panic_argument_type") == panic_type
                and (not has_error or error_type != "panic-info")
                and (not has_panic or panic_type == "panic-info")
                and (not has_trap_edge or trap_edge_valid))
    if validator == "aggregate-construct":
        return (actual.get("operand_types") == actual.get("field_types")
                and isinstance(actual.get("aggregate_type"), str)
                and bool(actual["aggregate_type"])
                and actual.get("result_type") == actual["aggregate_type"]
                and construct_ownership_valid(actual, actual.get("field_types"),
                                              actual.get("field_type_ownerships")))
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
    if validator == "class-construct":
        return (actual.get("type_kind") == "class-reference"
                and actual.get("operand_types") == actual.get("field_types")
                and actual.get("type_ownership") == "affine"
                and actual.get("copy_contract") in {"explicit", "forbidden"}
                and actual.get("result_ownership") == "owner"
                and construct_ownership_valid(actual, actual.get("field_types"),
                                              actual.get("field_type_ownerships")))
    if validator == "class-share":
        return (actual.get("type_kind") == "class-reference"
                and actual.get("operand_type") == actual.get("result_type")
                and actual.get("type_ownership") == "affine"
                and actual.get("operand_category") == "value"
                and actual.get("operand_ownership") in {"owner", "non-owner"}
                and (actual.get("operand_ownership") == "owner"
                     or actual.get("source_rooted") is True)
                and actual.get("result_category") == "value"
                and actual.get("result_ownership") == "owner"
                and actual.get("source_consumed") is False
                and actual.get("referent_identity_preserved") is True)
    if validator in {"class-field-load", "class-field-place"}:
        fields = actual.get("field_types")
        ordinal = actual.get("field_ordinal")
        shared = (actual.get("type_kind") == "class-reference"
                  and actual.get("operand_type") == actual.get("class_type")
                  and actual.get("operand_category") == "value"
                  and isinstance(fields, list) and isinstance(ordinal, int)
                  and 0 <= ordinal < len(fields)
                  and actual.get("result_type") == fields[ordinal]
                  and actual.get("result_ownership") == "non-owner"
                  and actual.get("receiver_rooted") is True)
        if validator == "class-field-load":
            ownerships = actual.get("field_type_ownerships")
            if not (shared and isinstance(ownerships, list)
                    and len(ownerships) == len(fields)):
                return False
            semantics = "rooted-borrow" if ownerships[ordinal] == "affine" else "value-snapshot"
            return actual.get("result_category") == "value" and actual.get("semantics") == semantics
        return shared and actual.get("result_category") == "place"
    if validator == "place-exchange":
        ownership = actual.get("type_ownership")
        affine = ownership == "affine"
        return (ownership in {"trivial", "affine"}
                and actual.get("place_type") == actual.get("replacement_type")
                and actual.get("place_type") == actual.get("result_type")
                and actual.get("place_category") == "place"
                and actual.get("replacement_category") == "value"
                and actual.get("result_category") == "value"
                and actual.get("replacement_ownership") == ("owner" if affine else "non-owner")
                and actual.get("result_ownership") == ("owner" if affine else "non-owner")
                and actual.get("replacement_consumed") is affine
                and actual.get("atomic") is True
                and actual.get("may_transfer_control") is False)
    if validator == "variant-construct":
        payloads = actual.get("variant_payload_types")
        ownerships = actual.get("variant_payload_type_ownerships")
        ordinal = actual.get("variant_ordinal")
        return (isinstance(payloads, list) and isinstance(ownerships, list)
                and len(payloads) == len(ownerships) and type(ordinal) is int
                and all(isinstance(types, list) and isinstance(owners, list)
                        and len(types) == len(owners)
                        and all(isinstance(name, str) and bool(name) for name in types)
                        and all(owner in ("trivial", "affine") for owner in owners)
                        for types, owners in zip(payloads, ownerships))
                and 0 <= ordinal < len(payloads)
                and actual.get("operand_types") == payloads[ordinal]
                and isinstance(actual.get("variant_type"), str) and bool(actual["variant_type"])
                and actual.get("result_type") == actual["variant_type"]
                and actual.get("type_ownership") ==
                    ("affine" if any("affine" in owners for owners in ownerships) else "trivial")
                and construct_ownership_valid(actual, payloads[ordinal], ownerships[ordinal]))
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
        operand_ownership = actual.get("operand_ownership")
        return (actual.get("concrete_nominal") is True
                and actual.get("conformance_interface") == actual.get("existential_interface")
                and interface_use in {"read", "ref", "move", "owned-storage"}
                and actual.get("operand_category") == expected_category
                and actual.get("result_category") == "value"
                and actual.get("result_ownership") == expected_ownership
                and (interface_use != "read"
                     or operand_ownership in {"non-owner", "owner"}))
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
    if validator == "existential-reborrow-read":
        return (actual.get("operand_interface") == actual.get("result_interface")
                and actual.get("operand_use") in {"move", "owned-storage"}
                and actual.get("result_use") == "read"
                and actual.get("operand_category") == "value"
                and actual.get("result_category") == "value"
                and actual.get("result_ownership") == "non-owner"
                and actual.get("payload_identity_preserved") is True
                and actual.get("operand_consumed") is False)
    if validator == "callable-pack":
        capture_type = actual.get("capture_type")
        shared = (actual.get("target_identity_closed") is True
                  and actual.get("callable_signature") == actual.get("target_visible_signature"))
        if capture_type == "none":
            return (shared and actual.get("target_has_receiver") is False
                    and actual.get("result_ownership") == "non-owner")
        return (shared and actual.get("result_ownership") == "owner"
                and actual.get("capture_kind") == "aggregate"
                and actual.get("capture_type_ownership") == "affine"
                and actual.get("capture_copy_contract") == "explicit"
                and actual.get("capture_value_ownership") == "owner"
                and actual.get("target_has_receiver") is True
                and actual.get("target_receiver_mode") == "read"
                and actual.get("target_receiver_type") == capture_type
                and actual.get("result_borrows_receiver") is False)
    if validator == "error-publish":
        return (actual.get("function_error_type") not in {None, "void", "panic-info"}
                and actual.get("operand_types") == [actual.get("function_error_type")])
    if validator == "panic-publish":
        return (actual.get("function_panic_type") == "panic-info"
                and actual.get("operand_types") == ["panic-info"])
    if validator == "cancel-publish":
        return (actual.get("operand_types") == []
                and actual.get("successor_count") == 0
                and actual.get("result_type") == "void")
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
        ownership = actual.get("type_ownership")
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "value"
                and actual.get("result_category") == "place"
                and ownership in {"trivial", "affine"}
                and actual.get("operand_ownership") ==
                    ("owner" if ownership == "affine" else "non-owner")
                and actual.get("result_ownership") == "non-owner"
                and actual.get("storage_relation") == "aliases-operand-storage")
    if validator == "place-module":
        return (actual.get("slot_type") == actual.get("result_type")
                and actual.get("slot_type") not in {None, "void"}
                and actual.get("operand_count") == 0
                and actual.get("module_exists") is True
                and actual.get("slot_exists") is True
                and actual.get("result_category") == "place"
                and actual.get("result_ownership") == "non-owner"
                and actual.get("storage_owner") == "execution-instance")
    if validator == "place-initialize":
        ownership = actual.get("type_ownership")
        return (actual.get("place_type") == actual.get("value_type")
                and actual.get("place_category") == "place"
                and actual.get("place_origin") == "module-slot"
                and actual.get("value_category") == "value"
                and actual.get("result_type") == "void"
                and actual.get("owning_initializer") is True
                and ownership in {"trivial", "affine"}
                and actual.get("value_ownership") ==
                    ("owner" if ownership == "affine" else "non-owner"))
    if validator == "place-load":
        ownership = actual.get("type_ownership")
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "place"
                and actual.get("result_category") == "value"
                and ownership in {"trivial", "affine"}
                and actual.get("result_ownership") == "non-owner"
                and actual.get("value_semantics") ==
                    ("borrow-owner-storage" if ownership == "affine" else "value-snapshot"))
    if validator == "place-store":
        return (actual.get("place_type") == actual.get("value_type")
                and actual.get("place_category") == "place"
                and actual.get("value_category") == "value"
                and actual.get("result_type") == "void")
    if validator == "place-project":
        fields = actual.get("field_types")
        ordinal = actual.get("field_ordinal")
        return (actual.get("operand_type") == actual.get("aggregate_type")
                and actual.get("operand_category") == "place"
                and actual.get("result_category") == "place"
                and isinstance(fields, list) and isinstance(ordinal, int)
                and 0 <= ordinal < len(fields) and actual.get("result_type") == fields[ordinal])
    if validator == "place-take":
        return (actual.get("operand_type") == actual.get("result_type")
                and actual.get("operand_category") == "place"
                and actual.get("result_category") == "value"
                and actual.get("place_origin") == "local-storage-alias"
                and actual.get("type_ownership") == "affine"
                and actual.get("result_ownership") == "owner"
                and actual.get("consumes_storage_owner") is True)
    if validator == "coroutine-yield":
        return (actual.get("result_type") == "void"
                and actual.get("successor_count") == 2
                and actual.get("resume_state") == actual.get("continuation_state")
                and continuation_graph_valid(actual, cancel=True, trap=False)
                and isinstance(actual.get("live_values"), list)
                and actual.get("live_values") == actual.get("resume_edge_values"))
    if validator == "coroutine-suspend":
        requested = actual.get("requested_ms")
        normalized = actual.get("normalized_ms")
        return (actual.get("result_type") == "void"
                and actual.get("successor_count") == 2
                and actual.get("resume_state") == actual.get("continuation_state")
                and continuation_graph_valid(actual, cancel=True, trap=False)
                and actual.get("request_kind") == "timer-after-ms"
                and actual.get("request_operand_count") == 1
                and actual.get("request_operand_types") == ["i64"]
                and isinstance(requested, int) and not isinstance(requested, bool)
                and normalized == max(0, min(requested, 86400000))
                and isinstance(actual.get("live_values"), list)
                and actual.get("live_values") == actual.get("resume_edge_values"))
    if validator in {"coroutine-call", "coroutine-indirect-call"}:
        live = actual.get("live_values")
        owners = actual.get("owner_values", [])
        cancel_values = actual.get("cancel_edge_values", [])
        trap_values = actual.get("trap_edge_values", [])
        successor_count = actual.get("successor_count")
        trap_valid = (
            successor_count == 2 and trap_values == []
        ) or (
            successor_count == 3
            and actual.get("trap") == "provider-call-failed"
            and isinstance(live, list) and isinstance(owners, list)
            and isinstance(trap_values, list)
            and isinstance(cancel_values, list)
            and all(live.count(owner) == 1 and owners.count(owner) == 1 for owner in owners)
            and all(value in owners for value in cancel_values)
            and all(cancel_values.count(owner) == 1 for owner in owners)
            and all(value in live for value in trap_values)
            and all(trap_values.count(owner) == 1 for owner in owners)
        )
        target_valid = (
            actual.get("callee_coroutine") is True
            and actual.get("callee_suspend_kind") in {
                "cooperative-yield", "timer-after-ms"
            }
        ) if validator == "coroutine-call" else (
            actual.get("callable_signature") is True
            and actual.get("exact_target_set") is True
            and actual.get("callable_carrier_frame_stable") is True
            and actual.get("callable_effects") == ["cancel", "suspend"]
            and actual.get("callable_capabilities") == ["runtime.coroutine-suspension"]
        )
        return (actual.get("result_type") == "void"
                and trap_valid
                and actual.get("resume_state") == actual.get("continuation_state")
                and continuation_graph_valid(actual, cancel=True, trap=successor_count == 3)
                and target_valid
                and actual.get("callee_error_type") == "void"
                and actual.get("callee_panic_type") == "void"
                and actual.get("scalar_non_owner_boundary") is True
                and isinstance(actual.get("live_values"), list)
                and actual.get("live_values") == actual.get("resume_edge_values"))
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
    camel_split = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", spelling)
    return re.sub(r"[^A-Z0-9]+", "_", camel_split.upper()).strip("_")


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
    ])
    type_ids = {row["name"]: row["stable_id"] for row in registry["types"]}
    for operation in registry["operations"]:
        domain = operation["type_rule"].get("operand_domain")
        if domain is None:
            continue
        mask = sum(1 << type_ids[name] for name in domain)
        name = c_identifier(operation["spelling"])
        lines.extend([
            f"/* Operand types admitted by {operation['spelling']}: {', '.join(domain)}. */",
            f"#define XR_CORE_OPERAND_DOMAIN_{name} UINT32_C({mask})",
            "",
        ])
    lines.extend([
        "typedef enum XrCoreEffectMask {",
    ])
    for row in registry["effects"]:
        lines.append(
            f"    XR_CORE_EFFECT_{c_identifier(row['name'])} = UINT32_C({1 << (row['stable_id'] - 1)}),")
    effect_mask = sum(1 << (row["stable_id"] - 1) for row in registry["effects"])
    lines.append(f"    XR_CORE_EFFECT_ALL = UINT32_C({effect_mask}),")
    lines.extend([
        "} XrCoreEffectMask;",
        f"#define XR_CORE_EFFECT_ALL UINT32_C({sum(1 << (row['stable_id'] - 1) for row in registry['effects'])})",
        "",
        "typedef enum XrCoreCapabilityMask {",
    ])
    for row in registry["capabilities"]:
        lines.append(
            f"    XR_CORE_CAPABILITY_{c_identifier(row['name'])} = UINT32_C({1 << (row['stable_id'] - 1)}),")
    capability_mask = sum(1 << (row["stable_id"] - 1) for row in registry["capabilities"])
    lines.append(f"    XR_CORE_CAPABILITY_ALL = UINT32_C({capability_mask}),")
    lines.extend([
        "} XrCoreCapabilityMask;",
        f"#define XR_CORE_CAPABILITY_ALL UINT32_C({sum(1 << (row['stable_id'] - 1) for row in registry['capabilities'])})",
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
        "typedef enum XrCoreSuccessorMask {",
        "    XR_CORE_SUCCESSOR_NORMAL = UINT8_C(1),",
        "    XR_CORE_SUCCESSOR_ERROR = UINT8_C(2),",
        "    XR_CORE_SUCCESSOR_PANIC = UINT8_C(4),",
        "    XR_CORE_SUCCESSOR_TRAP = UINT8_C(8),",
        "    XR_CORE_SUCCESSOR_CANCEL = UINT8_C(16),",
        "    XR_CORE_SUCCESSOR_SUSPEND = UINT8_C(32),",
        "} XrCoreSuccessorMask;",
        "#define XR_CORE_SUCCESSOR_ALL UINT8_C(63)",
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
        ("normal", "error", "panic", "trap", "cancel", "suspend"))}
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
        if type_rule.get("operand_domain"):
            operands += " in {" + ", ".join(type_rule["operand_domain"]) + "}"
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
    require(c_identifier("TargetOs") == "TARGET_OS",
            "CamelCase Core type names must project to stable separated C identifiers")
    first = generate_outputs(registry, kats)
    second = generate_outputs(copy.deepcopy(registry), copy.deepcopy(kats))
    require(first == second, "generation is nondeterministic")

    unmodeled = copy.deepcopy(registry)
    unmodeled["operations"][0]["coverage"]["evaluator"]["status"] = "NOT_APPLICABLE"
    generate_outputs(unmodeled, kats)
    require(semantic_registry_digest(unmodeled) == semantic_registry_digest(registry),
            "reference modeling changed executable semantic identity")
    for consumer in ("vm", "aot"):
        unverified = copy.deepcopy(unmodeled)
        unverified["operations"][0]["coverage"][consumer]["status"] = "COMPLETE"
        unverified["operations"][0]["coverage"]["verifier"]["status"] = "NOT_YET_ACTIVE"
        expect_invalid(f"{consumer} without Program admission", unverified, kats)

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

    for validator in ("aggregate-construct", "variant-construct"):
        positive = next(case for case in kats["cases"]
                        if case["id"] == f"{validator}-affine-transfer")
        ownership_field = ("field_type_ownerships" if validator == "aggregate-construct"
                           else "variant_payload_type_ownerships")
        for field in (ownership_field, "operand_values", "operand_ownerships", "type_ownership",
                      "result_ownership", "consumed_before", "consumed_after"):
            missing = copy.deepcopy(positive)
            del missing["actual"][field]
            require(not contract_oracle(missing, validator),
                    f"{validator} accepted missing ownership fact: {field}")
        for field, value in (("operand_values", [True, "scalar"]),
                             ("operand_values", ["owner", "owner"]),
                             ("operand_ownerships", ["owner"]),
                             ("type_ownership", "unknown"),
                             ("consumed_before", ["earlier-owner", "earlier-owner"]),
                             ("consumed_after", ["earlier-owner", "owner", "owner"])):
            malformed = copy.deepcopy(positive)
            malformed["actual"][field] = value
            require(not contract_oracle(malformed, validator),
                    f"{validator} accepted malformed ownership fact: {field}")

    for case_id, validator in (
            ("coroutine-yield-valid", "coroutine-yield"),
            ("coroutine-suspend-timer-valid", "coroutine-suspend"),
            ("coroutine-call-provider-trap-continuation-valid", "coroutine-call"),
            ("provider-call-trap-continuation-valid", "provider-call"),
            ("sealed-call-trap-continuation-valid", "sealed-call"),
            ("sealed-invoke-provider-trap-continuation", "sealed-invoke"),
            ("indirect-call-trap-continuation-valid", "indirect-call"),
            ("indirect-invoke-provider-trap-continuation", "indirect-invoke"),
            ("witness-direct-trap-continuation-valid", "witness-call"),
            ("witness-invoke-provider-trap-continuation", "witness-invoke")):
        positive = next(case for case in kats["cases"] if case["id"] == case_id)
        for field in ("entries", "blocks"):
            missing = copy.deepcopy(positive)
            del missing["actual"]["continuation_graph"][field]
            require(not contract_oracle(missing, validator),
                    f"{validator} accepted missing cleanup graph fact: {field}")
        for field in ("cancel_terminal", "trap_terminal"):
            legacy = copy.deepcopy(positive)
            del legacy["actual"]["continuation_graph"]
            legacy["actual"][field] = True
            require(not contract_oracle(legacy, validator),
                    f"{validator} accepted a terminal claim instead of edge facts")
        malformed = copy.deepcopy(positive)
        malformed["actual"]["continuation_graph"]["entries"]["normal"] = True
        require(not contract_oracle(malformed, validator),
                f"{validator} accepted a boolean block identity")

    positive = next(case for case in kats["cases"]
                    if case["id"] == "cleanup-graph-multiblock-reason-exits")
    for field, value in (("exit", ["trap7"]), ("exit", "unknown"),
                         ("flow", [True]), ("flow", [-1]), ("flow", [999]),
                         ("flow", None), ("refusal", [999]), ("refusal", {})):
        malformed = copy.deepcopy(positive)
        malformed["actual"]["continuation_graph"]["blocks"][2][field] = value
        require(not contract_oracle(malformed, "coroutine-call"),
                f"cleanup oracle accepted malformed {field}: {value!r}")


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
