#!/usr/bin/env python3
"""Validate the source-to-XrProgram authority boundary."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


class ContractError(ValueError):
    """Raised when the source producer contract is incomplete."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def validate(root: Path) -> None:
    header = read(root, "src/program/xr_program_from_xi.h")
    producer = read(root, "src/program/xr_program_from_xi.c")
    pipeline_header = read(root, "src/ir/xi_pipeline.h")
    pipeline = read(root, "src/ir/xi_pipeline.c")
    test = read(root, "tests/unit/ir/test_xi_pipeline.c")

    for token in ("module_roots", "entry_function", "semantic_profile_fingerprint"):
        require(token in header, f"source producer input lacks {token}")
    for token in (
        "source_semantic_module_present",
        "XI_STAGE_OPTIMIZED",
        "resolved_direct_callee",
        "close_block_arguments",
        "validate_input_value_identities",
    ):
        require(token in producer, f"source producer lacks {token}")
    for token in ("program_semantic_closure", "psc_", "semantic_function"):
        require(token not in producer, f"source producer regained legacy authority: {token}")

    require("XI_PIPE_XR_PROGRAM_INPUT" in pipeline_header,
            "pipeline lacks canonical-program input mode")
    for token in ("cfg->run_select_rep", "cfg->run_backend_lower", "cfg->run_emit"):
        require(token in pipeline, f"pipeline mode does not reject {token}")
    require("XI_STAGE_OPTIMIZED" in pipeline,
            "pipeline does not stop at target-neutral Optimized Xi")

    for token in (
        "while (index < limit)",
        "program_semantic_closure == NULL",
        "repeated_artifact.size == artifact.size",
        "XR_PROGRAM_BUILD_INVALID_INPUT",
        "xr_reference_evaluate",
        "xr_vm_code_execute",
        "xr_backend_ir_translation_validate",
    ):
        require(token in test, f"source producer evidence lacks {token}")

    registry = json.loads(read(root, "xisa/core/registry.json"))
    matrix = json.loads(
        read(root, "contracts/canonical-program/operation-capability-matrix.json")
    )
    registry_ids = {row["spelling"] for row in registry["operations"]}
    matrix_ids = {row["id"] for row in matrix["operations"]}
    require(matrix_ids == registry_ids,
            f"operation matrix differs from CoreSpec: missing={sorted(registry_ids - matrix_ids)} "
            f"extra={sorted(matrix_ids - registry_ids)}")


def self_test(root: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-source-contract-") as temporary:
        target = Path(temporary)
        for relative in (
            "src/program/xr_program_from_xi.h",
            "src/program/xr_program_from_xi.c",
            "src/ir/xi_pipeline.h",
            "src/ir/xi_pipeline.c",
            "tests/unit/ir/test_xi_pipeline.c",
            "xisa/core/registry.json",
            "contracts/canonical-program/operation-capability-matrix.json",
        ):
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, destination)
        validate(target)
        producer = target / "src/program/xr_program_from_xi.c"
        producer.write_text(producer.read_text(encoding="utf-8") + "\n/* psc_injected */\n",
                            encoding="utf-8")
        try:
            validate(target)
        except ContractError:
            return
        raise ContractError("legacy-authority mutation was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        if args.self_test:
            self_test(root)
            print("XrProgram source contract self-test: PASS")
        else:
            validate(root)
            print("XrProgram source contracts: PASS")
        return 0
    except (ContractError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram source contracts: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
