#!/usr/bin/env python3
"""Validate the W7 Wave 3 ownership/error/panic closure.

This gate proves that the canonical pipeline consumes only the frozen Wave 3
facts.  It deliberately does not scan the still-live pre-cut product route;
that route is frozen for atomic deletion by Task 302.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path


class ContractError(ValueError):
    """Raised when the Wave 3 closure is incomplete."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def canonical_pipeline_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for relative in ("src/program", "src/aot/program", "src/execution"):
        files.extend(sorted((root / relative).glob("*.c")))
        files.extend(sorted((root / relative).glob("*.h")))
    files.extend((root / relative) for relative in (
        "src/vm/xr_program_vm.c",
        "src/vm/xr_program_vm.h",
    ))
    return files


def validate(root: Path) -> None:
    freeze = json.loads(read(root, "contracts/canonical-program/w7-wave3-contract-freeze.json"))
    require(freeze.get("wave_status") == "COMPLETE_W7_WAVE3_CORE_ALGEBRA",
            "Wave 3 closure status is not complete")
    closure = freeze.get("closure")
    require(isinstance(closure, dict), "Wave 3 closure record is absent")
    require(closure.get("canonical_dependency_status") == "ZERO",
            "Wave 3 canonical legacy dependency status is not zero")
    require(closure.get("precut_product_route") ==
            "single frozen route; physical deletion is atomic Task 302",
            "Wave 3 pre-cut route boundary drifted")
    require(closure.get("compatibility_bridge") == "forbidden",
            "Wave 3 regained a compatibility bridge")
    require(closure.get("next_wave") == 4, "Wave 3 next wave is not Wave 4")

    deferred = closure.get("deferred_domain_contracts")
    require(deferred == {
        "borrowed_result_origin": "Wave 4 callable/interface signature activation",
        "external_alias_and_cross_execution": "Wave 5 concurrency activation",
        "domain_specific_container_place": "Wave 6 specialized capability activation",
        "precut_route_physical_deletion": "Task 302 atomic product cutover",
    }, "Wave 3 deferred domain ownership drifted")

    forbidden = tuple(closure.get("forbidden_canonical_tokens", []))
    require(forbidden == (
        "pending_error",
        "pending_panic",
        "cleanup_stack",
        "XrSemanticPlan",
        "XrTargetPlan",
        "XrProto",
        "XChunk",
        "xr_semantic_plan",
        "xr_target_plan",
        "program_semantic_closure",
        "CallDecision",
    ), "Wave 3 forbidden canonical token set drifted")
    for path in canonical_pipeline_files(root):
        text = path.read_text(encoding="utf-8", errors="strict")
        for token in forbidden:
            require(token not in text,
                    f"canonical Wave 3 pipeline regained old owner token: {path}: {token}")

    registry = json.loads(read(root, "xisa/core/registry.json"))
    spellings = {row.get("spelling") for row in registry.get("operations", [])}
    for spelling in (
        "core.call.sealed_invoke",
        "core.error.publish",
        "core.panic.publish",
        "core.owner.copy",
        "core.owner.move",
        "core.owner.drop",
        "core.place.local",
        "core.place.load",
        "core.place.store",
    ):
        require(spelling in spellings, f"Wave 3 CoreSpec atom is absent: {spelling}")
    for forbidden_operation in (
        "core.borrow.begin",
        "core.borrow.end",
        "core.cleanup.enter",
        "core.cleanup.leave",
        "core.error.check",
        "core.error.pending",
        "core.panic.pending",
        "core.retain",
        "core.release",
        "core.place.index",
        "core.place.field",
    ):
        require(forbidden_operation not in spellings,
                f"Wave 3 non-operation became canonical: {forbidden_operation}")

    kats = json.loads(read(root, "xisa/core/kats.json"))
    kat_ids = {row.get("id") for row in kats.get("cases", [])}
    required_kats = {
        "sealed-invoke-valid",
        "sealed-invoke-rejects-infallible-callee",
        "sealed-invoke-panic-only-valid",
        "sealed-invoke-rejects-panic-type-mismatch",
        "sealed-invoke-rejects-panic-as-business-error",
        "error-publish-valid",
        "error-publish-type-mismatch",
        "error-publish-rejects-panic",
        "panic-publish-valid",
        "panic-publish-rejects-business-error",
    }
    require(required_kats <= kat_ids,
            f"Wave 3 normative KATs are incomplete: {sorted(required_kats - kat_ids)}")

    projection = json.loads(read(root, "xisa/program/xi-source-projection.json"))
    value_rows = projection.get("value_mappings", [])
    require(not any(row.get("xi_operation") in {"xi.retain", "xi.release", "xi.err.set"}
                    for row in value_rows if isinstance(row, dict)),
            "physical ARC or pending-error Xi regained a canonical projection")

    producer = read(root, "src/program/xr_program_from_xi.c")
    for token in (
        "still uses implicit panic-handler state",
        "lacks an explicit panic continuation",
        "XR_CORE_OP_CORE_ERROR_PUBLISH",
        "XR_CORE_OP_CORE_PANIC_PUBLISH",
    ):
        require(token in producer, f"Wave 3 producer boundary lacks {token}")

    verifier_test = read(root, "tests/unit/program/test_xr_program_verify.c")
    vm_test = read(root, "tests/unit/vm/test_xr_program_vm.c")
    aot_test = read(root, "tests/unit/aot/test_xr_program_aot.c")
    for token in (
        "XR_INVOKE_FIXTURE_MISSING_ERROR_OWNER",
        "XR_INVOKE_FIXTURE_DUPLICATE_ERROR_OWNER",
        "XR_PANIC_FIXTURE_MISSING_PANIC_OWNER",
        "XR_PANIC_FIXTURE_DUPLICATE_PANIC_OWNER",
        "XR_PANIC_FIXTURE_PANIC_AS_ERROR",
    ):
        require(token in verifier_test, f"Wave 3 verifier mutation is absent: {token}")
    for token in (
        "XR_VM_OUTCOME_ERROR",
        "XR_VM_OUTCOME_PANIC",
        "test_sealed_invoke_and_cleanup_cfg",
        "test_typed_panic_invoke_and_cleanup_cfg",
    ):
        require(token in vm_test, f"Wave 3 VM differential evidence is absent: {token}")
    for token in (
        "out_error",
        "out_panic",
        ".kind == 2",
        ".kind == 3",
        "xr_backend_ir_translation_validate",
    ):
        require(token in aot_test, f"Wave 3 AOT evidence is absent: {token}")


def self_test(root: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-wave3-closure-") as temporary:
        target = Path(temporary)
        for relative in (
            "contracts/canonical-program/w7-wave3-contract-freeze.json",
            "xisa/core/registry.json",
            "xisa/core/kats.json",
            "xisa/program/xi-source-projection.json",
            "src/program",
            "src/aot/program",
            "src/execution",
            "src/vm/xr_program_vm.c",
            "src/vm/xr_program_vm.h",
            "tests/unit/program/test_xr_program_verify.c",
            "tests/unit/vm/test_xr_program_vm.c",
            "tests/unit/aot/test_xr_program_aot.c",
        ):
            source = root / relative
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, destination)
            else:
                shutil.copy2(source, destination)
        validate(target)
        victim = target / "src/vm/xr_program_vm.c"
        victim.write_text(victim.read_text(encoding="utf-8") +
                          "\n/* injected pending_error authority */\n", encoding="utf-8")
        try:
            validate(target)
        except ContractError:
            return
        raise ContractError("injected pending-error authority was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        if args.self_test:
            self_test(root)
            print("XrProgram Wave 3 closure self-test: PASS")
        else:
            validate(root)
            print("XrProgram Wave 3 closure: PASS")
        return 0
    except (ContractError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"XrProgram Wave 3 closure: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
