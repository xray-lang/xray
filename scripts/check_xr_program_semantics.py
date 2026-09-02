#!/usr/bin/env python3
"""Fail-closed Task 297 verifier/evaluator coverage and isolation gate."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


REGISTRY = Path("xisa/core/registry.json")
CORE_SPEC_HEADER = Path("src/core/xr_core_spec_gen.h")
PROGRAM_SCHEMA = Path("xisa/program/schema.json")
VERIFIER = Path("src/program/xr_program_verify.c")
EVALUATOR = Path("src/program/xr_reference_evaluator.c")
TEST = Path("tests/unit/program/test_xr_program_verify.c")
COVERAGE = Path("contracts/canonical-program/xrprogram-semantic-coverage.json")
FORBIDDEN_DEPENDENCIES = ("/aot/", "/frontend/", "/ir/", "/plan/", "/vm/")


class GateError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def read_json(path: Path) -> dict[str, Any]:
    raw = path.read_text(encoding="utf-8", errors="strict")
    value = json.loads(raw)
    require(isinstance(value, dict), f"{path} must contain an object")
    require(raw == canonical_json(value), f"{path} is not canonical JSON")
    return value


def enum_token(spelling: str) -> str:
    suffix = re.sub(r"[^A-Z0-9]+", "_", spelling.upper()).strip("_")
    return f"XR_CORE_OP_{suffix}"


def semantic_digest(header: str) -> str:
    match = re.search(r'^#define XR_CORE_SPEC_SEMANTIC_SHA256 "([0-9a-f]{64})"$',
                      header, re.MULTILINE)
    require(match is not None, f"{CORE_SPEC_HEADER} has no semantic identity")
    return match.group(1)


def expected_coverage(registry: dict[str, Any], program_schema: dict[str, Any],
                      core_spec_digest: str) -> dict[str, Any]:
    operations = [
        {
            "stable_id": row["stable_id"],
            "spelling": row["spelling"],
            "decoder": "COMPLETE",
            "verifier": "COMPLETE",
            "evaluator": "COMPLETE",
            "positive_kat": "tests/unit/program/test_xr_program_verify.c",
        }
        for row in registry["operations"]
    ]
    return {
        "schema": "xray-program-semantic-coverage/1",
        "verifier_version": 1,
        "core_spec_semantic_sha256": core_spec_digest,
        "program_schema_sha256": hashlib.sha256(
            canonical_json(program_schema).encode("utf-8")
        ).hexdigest(),
        "type_state": "XrValidatedProgram",
        "product_status": "OFF_PRODUCT_UNTIL_TASK_302",
        "execution_budgets": ["steps", "call-depth", "aggregate-value-cells"],
        "operation_count": len(operations),
        "operations": operations,
        "negative_mutations": [
            "core-spec-identity",
            "effect-mask",
            "operation-result-type",
            "missing-terminator",
            "resource-budget",
        ],
        "property_suites": {
            "typed_random_programs": 32,
            "alpha_renaming": 1,
            "resource_ladder": [16, 64, 256],
            "standalone_hostile_lengths": 513,
        },
        "inactive_contracts": [
            "sealed-invoke cleanup and nested managed storage",
            "coroutine state and cleanup",
            "imports and materialized boundaries",
        ],
    }


def validate_sources(registry: dict[str, Any], sources: dict[Path, str]) -> None:
    verifier = sources[VERIFIER]
    evaluator = sources[EVALUATOR]
    test = sources[TEST]
    for path in (VERIFIER, EVALUATOR):
        for line in sources[path].splitlines():
            if not line.lstrip().startswith("#include"):
                continue
            normalized = line.replace("..", "")
            require(
                not any(token in normalized for token in FORBIDDEN_DEPENDENCIES),
                f"{path} crosses forbidden implementation dependency: {line.strip()}",
            )
    for row in registry["operations"]:
        token = enum_token(row["spelling"])
        require(
            re.search(rf"\bcase\s+{re.escape(token)}\s*:", verifier) is not None,
            f"verifier has no explicit case for {row['spelling']}",
        )
        require(
            re.search(rf"\bcase\s+{re.escape(token)}\s*:", evaluator) is not None,
            f"evaluator has no explicit case for {row['spelling']}",
        )
        require(token in test, f"positive test has no operation token for {row['spelling']}")
    require("XrValidatedProgram" in verifier, "verifier does not construct the typed state")
    require("XrProgramDiagnosticKind" in verifier, "verifier has no stable diagnostic kind")
    require("max_work" in verifier, "verifier has no explicit work budget")
    require("max_value_cells" in evaluator and "max_value_cells" in test,
            "aggregate evaluator allocation has no tested value-cell budget")


def check(root: Path, source_override: dict[Path, str] | None = None) -> None:
    registry = read_json(root / REGISTRY)
    program_schema = read_json(root / PROGRAM_SCHEMA)
    header = (root / CORE_SPEC_HEADER).read_text(encoding="utf-8", errors="strict")
    sources = {
        path: (source_override or {}).get(path, (root / path).read_text(encoding="utf-8"))
        for path in (VERIFIER, EVALUATOR, TEST)
    }
    validate_sources(registry, sources)
    expected = canonical_json(expected_coverage(registry, program_schema, semantic_digest(header)))
    actual = (root / COVERAGE).read_text(encoding="utf-8", errors="strict")
    require(actual == expected, f"{COVERAGE} is stale")


def self_test(root: Path) -> None:
    check(root)
    registry = read_json(root / REGISTRY)
    verifier = (root / VERIFIER).read_text(encoding="utf-8")
    first = enum_token(registry["operations"][0]["spelling"])
    mutated = verifier.replace(f"case {first}:", "case XR_CORE_OP_MISSING:", 1)
    try:
        check(root, {VERIFIER: mutated})
    except GateError:
        pass
    else:
        raise GateError("missing verifier operation mutation was accepted")

    evaluator = (root / EVALUATOR).read_text(encoding="utf-8")
    mutated = evaluator.replace('#include "../core/xr_core_spec_gen.h"',
                                '#include "../vm/forbidden.h"', 1)
    try:
        check(root, {EVALUATOR: mutated})
    except GateError:
        pass
    else:
        raise GateError("forbidden evaluator dependency mutation was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.generate:
            registry = read_json(root / REGISTRY)
            program_schema = read_json(root / PROGRAM_SCHEMA)
            header = (root / CORE_SPEC_HEADER).read_text(encoding="utf-8", errors="strict")
            (root / COVERAGE).write_text(
                canonical_json(expected_coverage(registry, program_schema, semantic_digest(header))),
                encoding="utf-8",
            )
            print(f"XrProgram semantic coverage: generated {COVERAGE}")
            return 0
        if args.self_test:
            self_test(root)
            print("XrProgram semantic coverage self-test: PASS")
        else:
            check(root)
            count = len(read_json(root / REGISTRY)["operations"])
            print(f"XrProgram semantic coverage: PASS ({count} operations)")
    except (GateError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram semantic coverage: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
