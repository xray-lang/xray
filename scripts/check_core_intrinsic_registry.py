#!/usr/bin/env python3
"""Validate the typed core-intrinsic source registry without compiling Xray."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROW = re.compile(r"^XR_CORE_INTRINSIC\((.*?)\)$", re.MULTILINE | re.DOTALL)
EXPECTED_IDS = {
    "assert": 1,
    "assertEqual": 2,
    "assertThrows": 3,
    "assertPanics": 4,
    "print": 5,
}
EXPECTED_CONTRACTS = {
    "assert": (
        "ASSERTION",
        "BOOL_OPTIONAL_MESSAGE",
        1,
        2,
        "MAY_PANIC",
        "ASSERT_TRUE",
        "NONE",
        "ASSERT_CONDITION",
        "ASSERTION_ALL",
    ),
    "assertEqual": (
        "ASSERTION",
        "SAME_TYPE_PAIR_OPTIONAL_MESSAGE",
        2,
        3,
        "MAY_PANIC",
        "NONE",
        "NONE",
        "ASSERT_EQUAL",
        "ASSERTION_ALL",
    ),
    "assertThrows": (
        "ASSERTION",
        "ACTION_OPTIONAL_MESSAGE",
        1,
        2,
        "INVOKES_ACTION_MAY_PANIC",
        "NONE",
        "TYPED_ERROR",
        "ASSERT_THROWS",
        "ASSERTION_ALL",
    ),
    "assertPanics": (
        "ASSERTION",
        "ACTION_OPTIONAL_MESSAGE",
        1,
        2,
        "INVOKES_ACTION_MAY_PANIC",
        "NONE",
        "PANIC",
        "ASSERT_PANICS",
        "ASSERTION_ALL",
    ),
    "print": (
        "OUTPUT",
        "VARIADIC_VALUES",
        0,
        65535,
        "OUTPUT_MAY_PANIC",
        "NONE",
        "NONE",
        "PRINT_GROUP",
        "OUTPUT_ALL",
    ),
}
REMOVED_NAMES = {
    "likely",
    "unlikely",
    "assert_true",
    "assert_false",
    "assert_eq",
    "assert_ne",
    "assert_throws",
}
VALID_CATEGORIES = {"ASSERTION", "OUTPUT"}
VALID_CALL_FORMS = {"DIRECT_ONLY", "ORDINARY_CALLABLE"}
VALID_PARAMETER_SHAPES = {
    "BOOL_OPTIONAL_MESSAGE",
    "SAME_TYPE_PAIR_OPTIONAL_MESSAGE",
    "ACTION_OPTIONAL_MESSAGE",
    "VARIADIC_VALUES",
}
VALID_EFFECTS = {"MAY_PANIC", "INVOKES_ACTION_MAY_PANIC", "OUTPUT_MAY_PANIC"}
VALID_FLOW_RULES = {"NONE", "ASSERT_TRUE"}
VALID_FAILURE_CHANNELS = {"NONE", "TYPED_ERROR", "PANIC"}
VALID_SEMANTIC_OPS = {
    "ASSERT_CONDITION",
    "ASSERT_EQUAL",
    "ASSERT_THROWS",
    "ASSERT_PANICS",
    "PRINT_GROUP",
}
VALID_TARGETS = {"ASSERTION_ALL", "OUTPUT_ALL"}


def _split_fields(body: str) -> list[str]:
    compact = " ".join(line.strip() for line in body.splitlines())
    return [field.strip() for field in compact.split(",")]


def _parse_rows(path: Path) -> list[list[str]]:
    text = path.read_text(encoding="utf-8")
    return [_split_fields(match.group(1)) for match in ROW.finditer(text)]


def _unquote(value: str, field: str) -> str:
    if len(value) < 2 or value[0] != '"' or value[-1] != '"':
        raise ValueError(f"{field} must be a quoted string")
    return value[1:-1]


def validate(root: Path) -> list[str]:
    definition = root / "src" / "shared" / "xr_core_intrinsic.def"
    header = root / "src" / "shared" / "xr_core_intrinsic.h"
    errors: list[str] = []
    rows = _parse_rows(definition)

    if not re.search(
        r"^#define XR_CORE_BUILTIN_SCHEMA_VERSION UINT32_C\(1\)$",
        header.read_text(encoding="utf-8"),
        re.MULTILINE,
    ):
        errors.append("schema version must be the stable integer 1")

    if len(rows) != len(EXPECTED_IDS):
        errors.append(f"expected {len(EXPECTED_IDS)} live rows, found {len(rows)}")

    ids: set[int] = set()
    names: set[str] = set()
    semantic_ops: set[str] = set()
    for index, fields in enumerate(rows, start=1):
        if len(fields) != 15:
            errors.append(f"row {index}: expected 15 fields, found {len(fields)}")
            continue
        (
            symbolic_id,
            stable_id_text,
            source_name_text,
            category,
            call_form,
            parameter_shape,
            min_arity_text,
            max_arity_text,
            result_shape,
            effect,
            flow_rule,
            expected_failure_channel,
            semantic_op,
            targets,
            diagnostic_text,
        ) = fields
        try:
            stable_id = int(stable_id_text)
            min_arity = int(min_arity_text)
            max_arity = 65535 if max_arity_text == "UINT16_MAX" else int(max_arity_text)
            source_name = _unquote(source_name_text, "source_name")
            diagnostic = _unquote(diagnostic_text, "diagnostic_name")
        except ValueError as exc:
            errors.append(f"row {index}: {exc}")
            continue

        expected_id = EXPECTED_IDS.get(source_name)
        if expected_id != stable_id:
            errors.append(
                f"row {index}: {source_name!r} must keep stable ID {expected_id}, got {stable_id}"
            )
        expected_symbol = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", source_name).upper()
        if symbolic_id != expected_symbol:
            errors.append(
                f"row {index}: symbolic ID {symbolic_id!r} does not match {source_name!r}"
            )
        if source_name in REMOVED_NAMES:
            errors.append(f"row {index}: removed source name {source_name!r} is registered")
        if stable_id in ids:
            errors.append(f"row {index}: duplicate stable ID {stable_id}")
        if source_name in names:
            errors.append(f"row {index}: duplicate source name {source_name!r}")
        if semantic_op in semantic_ops:
            errors.append(f"row {index}: duplicate semantic operation {semantic_op}")
        ids.add(stable_id)
        names.add(source_name)
        semantic_ops.add(semantic_op)

        if category not in VALID_CATEGORIES:
            errors.append(f"row {index}: invalid category {category}")
        if call_form not in VALID_CALL_FORMS or call_form != "DIRECT_ONLY":
            errors.append(f"row {index}: core intrinsic must be direct-only")
        if parameter_shape not in VALID_PARAMETER_SHAPES:
            errors.append(f"row {index}: invalid parameter shape {parameter_shape}")
        if min_arity < 0 or min_arity > max_arity or max_arity > 65535:
            errors.append(f"row {index}: invalid arity {min_arity_text}..{max_arity_text}")
        if result_shape != "UNIT":
            errors.append(f"row {index}: result shape must be UNIT")
        if effect not in VALID_EFFECTS:
            errors.append(f"row {index}: invalid effect {effect}")
        if flow_rule not in VALID_FLOW_RULES:
            errors.append(f"row {index}: invalid flow rule {flow_rule}")
        if expected_failure_channel not in VALID_FAILURE_CHANNELS:
            errors.append(
                f"row {index}: invalid expected failure channel {expected_failure_channel}"
            )
        if semantic_op not in VALID_SEMANTIC_OPS:
            errors.append(f"row {index}: invalid semantic operation {semantic_op}")
        if targets not in VALID_TARGETS:
            errors.append(f"row {index}: invalid target applicability {targets}")
        if category == "ASSERTION" and targets != "ASSERTION_ALL":
            errors.append(f"row {index}: assertion row has dead target applicability")
        if category == "OUTPUT" and targets != "OUTPUT_ALL":
            errors.append(f"row {index}: output row has dead target applicability")
        if not diagnostic:
            errors.append(f"row {index}: diagnostic name must not be empty")
        actual_contract = (
            category,
            parameter_shape,
            min_arity,
            max_arity,
            effect,
            flow_rule,
            expected_failure_channel,
            semantic_op,
            targets,
        )
        if EXPECTED_CONTRACTS.get(source_name) != actual_contract:
            errors.append(f"row {index}: typed contract drift for {source_name!r}")

    if names != set(EXPECTED_IDS):
        missing = sorted(set(EXPECTED_IDS) - names)
        extra = sorted(names - set(EXPECTED_IDS))
        errors.append(f"live source-name set is not exhaustive: missing={missing}, extra={extra}")
    if ids != set(EXPECTED_IDS.values()):
        errors.append(f"stable ID domain is not exhaustive: {sorted(ids)}")
    if semantic_ops != VALID_SEMANTIC_OPS:
        errors.append(
            "semantic operation domain is not exhaustive: "
            f"missing={sorted(VALID_SEMANTIC_OPS - semantic_ops)}"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = validate(args.root.resolve())
    if errors:
        for error in errors:
            print(f"core-intrinsic registry: {error}", file=sys.stderr)
        return 1
    print("core-intrinsic registry: PASS (5 live rows, 7 removed names rejected)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
