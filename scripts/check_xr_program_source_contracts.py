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
    projection = json.loads(read(root, "xisa/program/xi-source-projection.json"))
    projection_header = read(root, "src/program/xr_program_xi_projection_gen.h")
    projection_source = read(root, "src/program/xr_program_xi_projection_gen.c")
    wave_three = json.loads(
        read(root, "contracts/canonical-program/w7-wave3-contract-freeze.json")
    )

    for token in ("module_roots", "entry_function", "semantic_profile_fingerprint"):
        require(token in header, f"source producer input lacks {token}")
    for token in (
        "source_semantic_module_present",
        "XI_STAGE_OPTIMIZED",
        "resolved_direct_callee",
        "close_block_arguments",
        "validate_input_value_identities",
        "xr_program_xi_projection",
        "xr_program_xi_value_is_materialized",
        "map_type_recursive",
        "logical_value_identity",
        "block_sealed_invoke_call",
        "map_function_error_type",
        "XR_CORE_OP_CORE_CALL_SEALED_INVOKE",
        "XR_CORE_OP_CORE_ERROR_PUBLISH",
        "XR_CORE_OP_CORE_PANIC_PUBLISH",
    ):
        require(token in producer, f"source producer lacks {token}")
    for token in ("program_semantic_closure", "psc_", "semantic_function"):
        require(token not in producer, f"source producer regained legacy authority: {token}")
    for token in (
        "case XI_ADD", "case XI_SUB", "case XI_MUL", "case XI_DIV",
        "case XI_EQ", "case XI_NE", "case XI_LT", "case XI_LE", "case XI_GT", "case XI_GE",
        "XR_CORE_OP_CORE_ADD_I64", "XR_CORE_OP_CORE_SUB_I64",
        "XR_CORE_OP_CORE_MUL_I64", "XR_CORE_OP_CORE_DIV_I64",
        "XR_CORE_OP_CORE_COMPARE_I64", "XR_CORE_OP_CORE_CALL_SEALED_DIRECT",
    ):
        require(token not in producer, f"source producer regained handwritten operation mapping: {token}")

    require(projection.get("schema") == "xray-program-xi-source-projection/1",
            "source projection schema drifted")
    require(projection.get("source_stage") == "XI_STAGE_OPTIMIZED",
            "source projection stage drifted")
    require(projection.get("migration_policy") == {
        "semantic_authority": "CoreSpec",
        "unlisted_xi_operation": "reject",
        "new_pipeline_legacy_dependencies": [],
        "old_product_route": "frozen-not-consumed",
        "physical_route_deletion": "atomic-task-302",
        "compatibility_bridge": "forbidden",
    }, "source projection migration policy drifted")
    for token in (
        "XrProgramXiProjection", "xr_program_xi_projection",
        "XR_PROGRAM_XI_ANY_RESULT_TYPE",
    ):
        require(token in projection_header or token in projection_source,
                f"generated source projection lacks {token}")

    new_pipeline_files = [
        *(root / "src/program").glob("*.c"),
        *(root / "src/program").glob("*.h"),
        root / "src/vm/xr_program_vm.c",
        *(root / "src/aot/program").glob("*.c"),
        *(root / "src/aot/program").glob("*.h"),
        *(root / "src/execution").glob("*.c"),
        *(root / "src/execution").glob("*.h"),
    ]
    for path in new_pipeline_files:
        text = path.read_text(encoding="utf-8", errors="strict")
        for token in (
            "XrSemanticPlan", "XrTargetPlan", "XrProto", "XChunk",
            "xr_semantic_plan", "xr_target_plan", "program_semantic_closure",
        ):
            require(token not in text,
                    f"new canonical pipeline depends on frozen legacy route: {path}: {token}")

    require("XI_PIPE_XR_PROGRAM_INPUT" in pipeline_header,
            "pipeline lacks canonical-program input mode")
    for token in ("cfg->run_select_rep", "cfg->run_backend_lower", "cfg->run_emit"):
        require(token in pipeline, f"pipeline mode does not reject {token}")
    require("XI_STAGE_OPTIMIZED" in pipeline,
            "pipeline does not stop at target-neutral Optimized Xi")

    for token in (
        "while (index < limit)",
        "XI_AGG_UPDATE",
        "invalid_update_artifact",
        "XR_CORE_OP_CORE_AGGREGATE_UPDATE",
        "program_semantic_closure == NULL",
        "repeated_artifact.size == artifact.size",
        "XR_PROGRAM_BUILD_INVALID_INPUT",
        "XR_CORE_OP_CORE_OWNER_COPY",
        "XR_CORE_OP_CORE_CALL_SEALED_INVOKE",
        "XR_CORE_OP_CORE_ERROR_PUBLISH",
        "xr_reference_evaluate",
        "xr_vm_code_execute",
        "xr_backend_ir_translation_validate",
    ):
        require(token in test, f"source producer evidence lacks {token}")

    value_mappings = projection.get("value_mappings")
    require(isinstance(value_mappings, list), "source projection value mappings are absent")
    projection_rows = {
        (row.get("xi_operation"), row.get("projection_kind"), row.get("core_operation"))
        for row in value_mappings if isinstance(row, dict)
    }
    require(("xi.agg.get", "aggregate-project", "core.aggregate.project") in projection_rows,
            "named aggregate projection is not exact")
    require(("xi.agg.update", "aggregate-update", "core.aggregate.update") in projection_rows,
            "pure aggregate update is not exact")
    require(("xi.call.builtin", "owner-copy", "core.owner.copy") in projection_rows,
            "explicit source copy is not exact")
    structural_rows = {
        (row.get("source"), row.get("core_operation"))
        for row in projection.get("structural_mappings", []) if isinstance(row, dict)
    }
    require(("resolved-fallible-call-plus-xi.err.check-cfg",
             "core.call.sealed_invoke") in structural_rows,
            "fallible call/check CFG does not project to sealed invoke")
    require(("xi.err.return-after-explicit-cleanup-cfg",
             "core.error.publish") in structural_rows,
            "typed error publication source projection is absent")
    require(("xi.throw-after-explicit-cleanup-cfg",
             "core.panic.publish") in structural_rows,
            "typed panic publication source projection is absent")
    require(not any(row.get("xi_operation") == "xi.agg.set" for row in value_mappings
                    if isinstance(row, dict)),
            "mutating aggregate storage regained a CoreSpec projection")
    for token in ("case XR_PROGRAM_XI_PROJECTION_AGGREGATE_UPDATE",
                  "case XR_PROGRAM_XI_PROJECTION_OWNER_COPY",
                  'strcmp((const char *) value->aux, "copy")',
                  "xi_type_has_logical_value_identity"):
        require(token in producer, f"aggregate update producer closure lacks {token}")

    require(wave_three.get("schema") == "xray-w7-wave3-contract-freeze/1",
            "Wave 3 contract schema drifted")
    require(wave_three.get("compatibility") == "none",
            "Wave 3 contract regained compatibility")
    signature = wave_three.get("signature_contract")
    require(isinstance(signature, dict) and
            signature.get("parameter_modes") == ["READ", "REF", "MOVE"] and
            signature.get("receiver_modes") == ["READ", "REF", "MOVE"],
            "Wave 3 signature mode set drifted")
    require(signature.get("out_mode") ==
            "forbidden; Xray has no fourth OUT parameter mode",
            "Wave 3 regained an OUT parameter mode")
    atoms = wave_three.get("operation_atoms")
    require(isinstance(atoms, list), "Wave 3 operation atoms are absent")
    atom_ids = [row.get("id") for row in atoms if isinstance(row, dict)]
    require(atom_ids == [
        "core.call.sealed_invoke",
        "core.trap",
        "core.error.publish",
        "core.panic.publish",
        "core.owner.copy",
        "core.owner.move",
        "core.owner.drop",
        "core.place.local",
        "core.place.load",
        "core.place.store",
    ], "Wave 3 operation atom order or set drifted")
    stable_ids = [row.get("stable_id") for row in atoms if isinstance(row, dict)]
    require(stable_ids == [37, 48, 49, 50, 96, 97, 98, 104, 105, 106] and
            len(stable_ids) == len(set(stable_ids)),
            "Wave 3 stable operation IDs drifted")
    non_operations = wave_three.get("explicit_non_operations")
    require(isinstance(non_operations, dict) and set(non_operations) == {
        "borrow_begin_end",
        "cleanup_enter_leave",
        "error_check_pending_slot",
        "generic_place_field_or_index",
        "retain_release",
        "out_parameter",
    }, "Wave 3 explicit non-operation set drifted")
    xi_projection = wave_three.get("xi_projection")
    require(isinstance(xi_projection, dict) and
            xi_projection.get("xi.retain") == "no canonical projection" and
            xi_projection.get("xi.err.check") ==
                "core.call.sealed_invoke plus explicit CFG" and
            xi_projection.get("xi.cleanup.enter") == "no canonical projection" and
            xi_projection.get("xi.cleanup.leave") == "no canonical projection",
            "Wave 3 Xi projection policy drifted")

    registry = json.loads(read(root, "xisa/core/registry.json"))
    matrix = json.loads(
        read(root, "contracts/canonical-program/operation-capability-matrix.json")
    )
    registry_ids = {row["spelling"] for row in registry["operations"]}
    matrix_ids = {row["id"] for row in matrix["operations"]}
    require(matrix_ids == registry_ids,
            f"operation matrix differs from CoreSpec: missing={sorted(registry_ids - matrix_ids)} "
            f"extra={sorted(matrix_ids - registry_ids)}")
    require(matrix.get("precut_route_policy") == {
        "state_during_w7": "single frozen current product; canonical implementation remains off-product",
        "canonical_dependency_on_precut_route": "forbidden",
        "physical_deletion_owner": 302,
        "cutover": "one atomic product-reachability change with no fallback or hidden executor",
    }, "operation matrix pre-cut route policy drifted")
    wave_one = {
        "core.constant.i64",
        "core.constant.bool",
        "core.add.i64",
        "core.sub.i64",
        "core.mul.i64",
        "core.div.i64",
        "core.compare.i64",
        "core.block.argument",
        "core.branch",
        "core.conditional_branch",
        "core.return",
        "core.call.sealed_direct",
    }
    wave_two_complete = {
        "core.aggregate.construct",
        "core.aggregate.project",
        "core.aggregate.update",
        "core.variant.construct",
        "core.variant.test",
        "core.variant.project",
    }
    frozen = {
        "core.trap",
        "core.target.pointer_width",
    }
    wave_three_slice_two = {
        "core.owner.move",
        "core.owner.drop",
        "core.place.local",
        "core.place.load",
        "core.place.store",
    }
    wave_three_slice_three = {
        "core.owner.copy",
    }
    wave_three_slice_four = {"core.error.publish"}
    wave_three_slice_five = {"core.call.sealed_invoke", "core.panic.publish"}
    wave_four_executor = {
        "core.call.witness_direct",
        "core.call.witness_invoke",
        "core.existential.pack",
        "core.existential.test",
        "core.existential.project",
    }
    rows = {row["id"]: row for row in matrix["operations"]}
    expected_status = {
        **{operation: "COMPLETE_W7_WAVE1" for operation in wave_one},
        **{operation: "COMPLETE_W7_WAVE2" for operation in wave_two_complete},
        **{operation: "COMPLETE_W7_WAVE3_SLICE2" for operation in wave_three_slice_two},
        **{operation: "COMPLETE_W7_WAVE3_SLICE3" for operation in wave_three_slice_three},
        **{operation: "COMPLETE_W7_WAVE3_SLICE4" for operation in wave_three_slice_four},
        **{operation: "COMPLETE_W7_WAVE3_SLICE5" for operation in wave_three_slice_five},
        **{operation: "IN_PROGRESS_W7_WAVE4_EXECUTOR" for operation in wave_four_executor},
        **{operation: "FROZEN_WALKING_SKELETON" for operation in frozen},
    }
    require(set(expected_status) == registry_ids,
            "source gate status partition does not cover the CoreSpec registry")
    for operation, status in expected_status.items():
        require(rows[operation]["status"] == status,
                f"operation has wrong source status: {operation}: "
                f"expected {status}, got {rows[operation]['status']}")
        evidence = rows[operation].get("evidence")
        if status == "FROZEN_WALKING_SKELETON":
            require(evidence == [], f"frozen operation gained evidence: {operation}")
            continue
        require(isinstance(evidence, list) and evidence,
                f"active operation has no evidence: {operation}")
        for relative in evidence:
            require(isinstance(relative, str) and (root / relative).is_file(),
                    f"active operation has missing evidence: {operation}: {relative}")


def self_test(root: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-source-contract-") as temporary:
        target = Path(temporary)
        for relative in (
            "src/program/xr_program_from_xi.h",
            "src/program/xr_program_from_xi.c",
            "src/program/xr_program_verify.c",
            "src/program/xr_reference_evaluator.c",
            "src/ir/xi_pipeline.h",
            "src/ir/xi_pipeline.c",
            "tests/unit/ir/test_xi_pipeline.c",
            "tests/unit/program/test_xr_program_verify.c",
            "tests/unit/program/xr_program_invoke_fixture.h",
            "tests/unit/program/xr_program_panic_fixture.h",
            "tests/unit/program/xr_program_existential_fixture.h",
            "tests/unit/vm/test_xr_program_vm.c",
            "tests/unit/aot/test_xr_program_aot.c",
            "xisa/core/registry.json",
            "xisa/program/schema.json",
            "xisa/program/xi-source-projection.json",
            "src/program/xr_program_xi_projection_gen.h",
            "src/program/xr_program_xi_projection_gen.c",
            "src/frontend/analyzer/xa_enum_record_plan.c",
            "src/ir/xi_lower_misc.c",
            "src/vm/xr_program_vm.c",
            "src/aot/program/xr_backend_ir_emit_c.c",
            "contracts/canonical-program/operation-capability-matrix.json",
            "contracts/canonical-program/w7-wave3-contract-freeze.json",
        ):
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, destination)
        validate(target)
        matrix = target / "contracts/canonical-program/operation-capability-matrix.json"
        original_matrix = matrix.read_text(encoding="utf-8")
        matrix_value = json.loads(original_matrix)
        matrix_value["operations"][0]["status"] = "FROZEN_WALKING_SKELETON"
        matrix.write_text(json.dumps(matrix_value, ensure_ascii=False, indent=2) + "\n",
                          encoding="utf-8")
        try:
            validate(target)
        except ContractError:
            pass
        else:
            raise ContractError("incomplete Wave 1 operation was accepted")
        matrix.write_text(original_matrix, encoding="utf-8")
        producer = target / "src/program/xr_program_from_xi.c"
        producer.write_text(producer.read_text(encoding="utf-8") + "\n/* XrTargetPlan injected */\n",
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
