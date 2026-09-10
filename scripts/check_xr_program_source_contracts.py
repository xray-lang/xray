#!/usr/bin/env python3
"""Validate the source-to-XrProgram authority boundary."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

import program_source_fixtures as source_fixtures


class ContractError(ValueError):
    """Raised when the source producer contract is incomplete."""


CANONICAL_LEGACY_AUTHORITY_TOKENS = {
    "frozen legacy route": (
        "XrSemanticPlan",
        "XrTargetPlan",
        "XrProto",
        "XChunk",
        "xr_semantic_plan",
        "xr_target_plan",
        "program_semantic_closure",
    ),
    "CallDecision": (
        "XrScalarCallDecision",
        "xr_scalar_call_decision_",
        "scalar_call_decision",
    ),
    "selector/name fallback": (
        "method_name_id",
        "method_signature_key",
        "xr_class_lookup_method",
        "xr_symbol_lookup_in_table",
        "cg_lookup_method",
        "cg_resolve_static_function_call",
        "cg_resolve_import_function_call",
    ),
    "class-only itable": (
        "XAOT_DISPATCH_ITABLE",
        "XAOT_INTERFACE_USE_NEEDS_ITABLE",
        "XAOT_INTERFACE_ABI_NEEDS_ITABLE",
        "XrtInterfaceMethodTable",
        "xrt_itable_method",
        "xrt_type_set_itable",
        "itable_source",
    ),
    "erased closure ABI": (
        "XrAotCallableDesc",
        "XrClosure",
        "xrt_closure_t",
        "xrt_closure_new",
        "xr_vm_call_closure",
    ),
    "backend signature recovery": (
        "xaot_type_fingerprint",
        "xaot_callable_plans_build",
        "xaot_callable_plans_verify",
        "xaot_boundary_resolve_direct_call_target",
        "callable_body_effects",
        "callable_rederive_matches",
    ),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def validate_native_fixtures(root: Path, unit_cmake: str, owner_test: str) -> None:
    try:
        registry = source_fixtures.load_registry(
            root / "tests/unit/program/xr_program_source_cases.json",
            root / "tests/unit/program/test_xr_program_source_build.c")
    except source_fixtures.FixtureError as exc:
        raise ContractError(f"source native fixture registry: {exc}") from exc
    header, registration = source_fixtures.project_registry(registry)
    native_targets = source_fixtures.native_target_names(registry)
    registered_targets = re.findall(
        rb"^add_xr_program_source_native_fixture\(\w+ (\w+) ", registration, re.MULTILINE)
    require(tuple(name.decode("ascii") for name in registered_targets) == native_targets,
            "source native projection does not cover the complete manifest")
    require(header.count(b"    X(source_owner_") == len(registry["cases"]),
            "source case projection does not cover the complete manifest")
    for token in (
        "scripts/program_source_fixtures.py",
        "program/xr_program_source_cases.json",
        "COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} project",
        "--header ${XR_PROGRAM_SOURCE_FIXTURE_HEADER}",
        "--cmake ${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION}",
        "COMMAND_ERROR_IS_FATAL ANY",
        "add_test(NAME xr_program_source_fixtures_self_test",
        "add_custom_command(TARGET test_xr_program_source_build PRE_LINK",
        "COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} check",
    ):
        require(token in unit_cmake, f"source fixture registration lacks {token}")
    include = "include(${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION})"
    require(unit_cmake.count(include) == 1,
            "source native registration must consume exactly one manifest projection")
    functions = re.findall(
        r"function\(add_xr_program_source_native_fixture fixture_id native_target "
        r"expected_exit fixture_labels\)(.*?)endfunction\(\)", unit_cmake, re.DOTALL)
    require(len(functions) == 1, "source natives require one fixture registration owner")
    for token in (
        'set(generated_c "${XR_PROGRAM_SOURCE_FIXTURE_DIR}/${fixture_id}_native.c")',
        'OUTPUT "${generated_c}"',
        "COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} generate",
        '--fixture "${fixture_id}"',
        "--producer $<TARGET_FILE:test_xr_program_source_build>",
        '--output "${generated_c}"',
        "DEPENDS test_xr_program_source_build",
        '${XR_PROGRAM_SOURCE_FIXTURE_MANIFEST}',
        'add_executable(${native_target} "${generated_c}")',
        "xr_enable_pure_aot_symbol_map(${native_target})",
        "-std=c11 -pedantic-errors -Wall -Wextra -Werror",
        "target_compile_options(${native_target} PRIVATE /W4 /WX)",
        "add_test(NAME ${native_target}",
        "scripts/check_xr_program_aot_native.py",
        "--executable $<TARGET_FILE:${native_target}>",
        "--expected-exit ${expected_exit}",
        '${fixture_labels};generated-c;native;task-293',
    ):
        require(token in functions[0], f"source native fixture binding lacks {token}")
    for token in (
        '#include "xr_program_source_cases.gen.h"',
        "XR_SOURCE_FIXTURES(SELECT_SOURCE_FIXTURE)",
        "XR_SOURCE_CASES(RUN_SOURCE_CASE)",
        "strcmp(argv[4], XR_SOURCE_REGISTRY_ID) != 0",
        "selected_source_fixture == XR_SOURCE_FIXTURE_NONE ? XR_SOURCE_CASE_COUNT : 1u",
    ):
        require(token in owner_test, f"source fixture dispatch lacks {token}")


def validate(root: Path) -> None:
    header = read(root, "src/program/xr_program_from_xi.h")
    producer = read(root, "src/program/xr_program_from_xi.c")
    owner_header = read(root, "src/program/xr_program_source_build.h")
    owner = read(root, "src/program/xr_program_source_build.c")
    owner_test = read(root, "tests/unit/program/test_xr_program_source_build.c")
    unit_cmake = read(root, "tests/unit/CMakeLists.txt")
    cli_adapter = read(root, "src/app/cli/xcli_canonical_source.c")
    run_route = read(root, "src/app/cli/xcmd_run.c")
    check_route = read(root, "src/app/cli/xcmd_check.c")
    cli_spec = read(root, "src/app/cli/xcli_spec.c")
    run_route_test = read(root, "tests/cli/run_canonical_source_route_tests.py")
    pipeline_header = read(root, "src/ir/xi_pipeline.h")
    pipeline = read(root, "src/ir/xi_pipeline.c")
    lower_class = read(root, "src/ir/xi_lower_class.inc.c")
    test = read(root, "tests/unit/ir/test_xi_pipeline.c")
    global_producer = read(root, "src/analysis/xglobal_producer.c")
    imported_callable_test = read(root, "tests/unit/ir/test_xr_program_imported_callable.c")
    projection = json.loads(read(root, "xisa/program/xi-source-projection.json"))
    projection_header = read(root, "src/program/xr_program_xi_projection_gen.h")
    projection_source = read(root, "src/program/xr_program_xi_projection_gen.c")
    wave_three = json.loads(
        read(root, "contracts/canonical-program/w7-wave3-contract-freeze.json")
    )

    for token in ("module_roots", "entry_function", "semantic_profile_fingerprint"):
        require(token in header, f"source producer input lacks {token}")
    for token in (
        "XrProgramSourceEntryIdentity",
        "module_identity",
        "function_name",
        "source_content_fingerprint",
        "XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER",
        "XrProgramSourceProduct",
        "XrValidatedProgram *program",
        "XrProgramSourceDiagnostic",
        "source_path",
        "source_column",
        "XrProgramSourceBuildBudget",
        "max_monomorphization_depth",
        "max_monomorphization_instances",
        "max_program_bytes",
        "xr_program_source_build_default_budget",
    ):
        require(token in owner_header, f"source owner API lacks {token}")
    for token in (
        "xr_module_graph_build",
        "xa_analyzer_analyze",
        "pre_monomorphization_evidence",
        "xa_mono_graph_pass",
        "xr_canon_program",
        "xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer",
        "xg_global_evidence_merge_generic_inst_roots",
        "xi_pipeline_program_input_config",
        "xi_resolve_imports",
        ".entry_function = entry",
        "xr_program_write_from_xi",
        "xr_program_validate",
        "xr_program_source_product_free",
        "XR_ERR_ANALYZE_MONO_BUDGET",
        "XR_ERR_ANALYZE_MONO_DEPTH",
        "XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT",
        "XaMonoUsage mono_usage",
        "product->artifact.size > context->input->budget.max_program_bytes",
    ):
        require(token in owner, f"source owner implementation lacks {token}")
    for forbidden in ("XrProto", "XrTargetPlan", "XrInstance"):
        require(forbidden not in owner_header and forbidden not in owner,
                f"source owner regained forbidden product dependency {forbidden}")
    for token in (
        "xr_cli_graph_authority_open",
        "xr_module_identity_from_source",
        "xr_module_source_fingerprint",
        "xr_program_source_build",
    ):
        require(token in cli_adapter, f"CLI source adapter lacks {token}")
    for token in (
        "xr_cli_canonical_source_build",
        "xr_execution_instance_create",
        "xr_vm_code_build",
        "xr_vm_code_execute",
        "xr_vm_execution_create",
        ".entry_kind = XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER",
        'strcmp(path + length - 3u, ".xr")',
        "XR_RUN_6013",
    ):
        require(token in run_route, f"canonical run route lacks {token}")
    for forbidden in (
        "xr_isolate_dofile",
        "xr_isolate_dostring",
        "xr_execute(",
        "XrProto",
        "XrTargetPlan",
        "xr_xtp_",
        "semantic-plan",
    ):
        require(forbidden not in run_route,
                f"canonical run route regained legacy execution: {forbidden}")
    for token in (
        "xa_mono_default_budget",
        "xa_mono_graph_pass",
        "monomorphization failed without a diagnostic",
        'fprintf(stderr, "E%04d: ", diagnostic->code)',
    ):
        require(token in check_route, f"check route lacks canonical monomorphization evidence: {token}")
    require('"run", "Run one exact .xr source entry"' in cli_spec,
            "run command schema does not expose the canonical source-only contract")
    run_options = cli_spec.split("static const XrCliOptionSpec repl_options[]", 1)[0]
    for retired_option in (
        '"trace"',
        '"dump-bytecode"',
        '"semantic-plan"',
        '"timings"',
        '"workers"',
        '"coro-watch"',
        '"coro-http"',
        '"dump-ic"',
    ):
        require(retired_option not in run_options,
                f"run command schema regained retired option {retired_option}")
    for token in (
        "main_zero.xr",
        "declaration_only.xr",
        "declaration-only module initializer failed",
        "unknown option '--semantic-plan'",
        "source fingerprint drifted across CRLF ingestion",
        "run accepted unbounded generic specialization",
        "check accepted unbounded generic specialization",
        ":1033:0: error[E0389]: E0389:",
    ):
        require(token in run_route_test, f"canonical run evidence lacks {token}")
    for token in (
        "source_owner_single_module_is_deterministic_and_detached",
        "source_owner_two_module_graph_is_deterministic",
        "source_owner_cross_module_coroutine_call_has_one_program_and_private_executors",
        "source_owner_cross_module_static_method_coroutine_has_one_program_and_private_executors",
        "source_owner_generic_specializations_are_exact_program_functions",
        "source_owner_generic_constraint_methods_have_exact_concrete_targets",
        "source_owner_generic_value_struct_specializations_are_exact_nominal_aggregates",
        "source_owner_module_initializer_is_a_canonical_entry",
        "source_owner_rejects_non_authoritative_entry_identity",
        "source_owner_rejects_module_budget_before_analysis",
        "source_owner_rejects_invalid_or_expanded_budget_request",
        "source_owner_reports_exact_monomorphization_depth_budget",
        "source_owner_reports_exact_monomorphization_instance_budget",
        "source_owner_applies_instance_budget_across_module_graph",
        "source_owner_rejects_program_bytes_over_request_budget",
        "source_owner_reports_structured_analysis_failure",
        "xr_validated_program_bytes",
        "XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION",
        "XR_CORE_OP_CORE_COROUTINE_CALL_SEALED",
        "xr_reference_execution_step",
        "xr_vm_execution_step",
        "xr_backend_ir_emit_c",
        "child_active_0",
    ):
        require(token in owner_test, f"source owner evidence lacks {token}")
    validate_native_fixtures(root, unit_cmake, owner_test)
    for token in (
        "source_semantic_module_present",
        "XI_STAGE_OPTIMIZED",
        "resolved_sealed_callee",
        "close_block_arguments",
        "validate_input_value_identities",
        "xr_program_xi_projection",
        "xr_program_xi_value_is_materialized",
        "map_type_recursive",
        "logical_value_identity",
        "read_value_call_place_is_exact",
        "read_value_receiver_load_is_exact",
        "resolved_value_aggregate_construction",
        "static_nominal_publication_is_exact",
        "block_typed_invoke_call",
        "map_function_error_type",
        "XR_CORE_OP_CORE_CALL_SEALED_INVOKE",
        "XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE",
        "XR_CORE_OP_CORE_ERROR_PUBLISH",
        "XR_CORE_OP_CORE_PANIC_PUBLISH",
    ):
        require(token in producer, f"source producer lacks {token}")
    for token in (
        "class_field_evidence_source_node_id",
        "instance_field_source_node_ids",
        "class_xglobal_row_for_info",
    ):
        require(token in lower_class,
                f"Xi class lowering lacks exact specialized field evidence: {token}")
    for token in ("program_semantic_closure", "psc_", "semantic_function"):
        require(token not in producer, f"source producer regained legacy authority: {token}")
    for token in (
        "XgModuleImportRow",
        "producer_register_module_import",
        "producer_find_method_for_symbol_in_hierarchy",
    ):
        require(token in global_producer,
                f"global evidence lacks exact cross-module method ownership: {token}")
    for forbidden in ("XgStdlibImportRow", "producer_lookup_stdlib_import"):
        require(forbidden not in global_producer,
                f"global evidence regained stdlib-only import ownership: {forbidden}")
    for token in (
        "imported_static_method_uses_exact_cross_module_evidence",
        "XG_CALL_METHOD",
        "callsite->receiver_static_class_id",
        "method_call->xg_method_id",
        "XG_METHOD_STATIC",
    ):
        require(token in imported_callable_test,
                f"cross-module static method evidence lacks {token}")
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
        for owner, tokens in CANONICAL_LEGACY_AUTHORITY_TOKENS.items():
            for token in tokens:
                require(token not in text,
                        f"new canonical pipeline depends on {owner}: {path}: {token}")

    require("XI_PIPE_XR_PROGRAM_INPUT" in pipeline_header,
            "pipeline lacks canonical-program input mode")
    for token in ("cfg->run_select_rep", "cfg->run_backend_lower", "cfg->run_emit"):
        require(token in pipeline, f"pipeline mode does not reject {token}")
    require("XI_STAGE_OPTIMIZED" in pipeline,
            "pipeline does not stop at target-neutral Optimized Xi")

    for token in (
        "while (index < limit)",
        "XI_AGG_UPDATE",
        "unreachable_update_artifact",
        "xr_program_id_equal(unreachable_update_artifact.id, artifact.id)",
        "XR_CORE_OP_CORE_AGGREGATE_UPDATE",
        "program_semantic_closure == NULL",
        "repeated_artifact.size == artifact.size",
        "XR_PROGRAM_BUILD_INVALID_INPUT",
        "XR_CORE_OP_CORE_OWNER_COPY",
        "XR_CORE_OP_CORE_CALL_SEALED_INVOKE",
        "XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE",
        "callable_error_value(-2)",
        "missing_fallible_target_artifact",
        "rebound_fallible_target_artifact",
        "mismatched_fallible_error_artifact",
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
    require(("xi.call.method", "sealed-direct-call", "core.call.sealed_direct") in
            projection_rows, "static method call projection is not exact")
    require(("xi.call.method.direct", "sealed-direct-call", "core.call.sealed_direct") in
            projection_rows, "direct static method call projection is not exact")
    require(("xi.target.pointer.bits", "target-query", "core.target.pointer_width") in
            projection_rows, "target.pointerBits projection is not exact")
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
    require(("resolved-nominal-ref-field-access",
             "core.place.project") in structural_rows,
            "nominal REF field place projection is absent")
    require(("affine-xi.place.load-writeback",
             "core.place.take") in structural_rows,
            "affine REF writeback take projection is absent")
    require(("exact-value-struct-xi.agg.new-plus-xi.agg.set-sequence",
             "core.aggregate.construct") in structural_rows,
            "exact value-struct construction projection is absent")
    require(("exact-value-struct-read-call-local-address",
             "core.call.sealed_direct") in structural_rows,
            "exact value-struct READ call-place normalization is absent")
    require(("exact-value-struct-read-receiver-place-load",
             "core.block.argument") in structural_rows,
            "exact value-struct READ receiver normalization is absent")
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
        "core.place.project",
        "core.place.load",
        "core.place.store",
        "core.place.take",
    ], "Wave 3 operation atom order or set drifted")
    stable_ids = [row.get("stable_id") for row in atoms if isinstance(row, dict)]
    require(stable_ids == [37, 48, 49, 50, 96, 97, 98, 104, 107, 105, 106, 108] and
            len(stable_ids) == len(set(stable_ids)),
            "Wave 3 stable operation IDs drifted")
    non_operations = wave_three.get("explicit_non_operations")
    require(isinstance(non_operations, dict) and set(non_operations) == {
        "borrow_begin_end",
        "cleanup_enter_leave",
        "error_check_pending_slot",
        "generic_place_index",
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
        "state_during_w7": "each migrated product route is canonical-only and fail-closed; uncovered capabilities remain explicitly incomplete",
        "canonical_dependency_on_precut_route": "forbidden",
        "physical_deletion_owner": 302,
        "cutover": "each development route removes its legacy reachability atomically; terminal closure removes every remaining old product node together",
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
    }
    wave_five_pointer = {"core.target.pointer_width"}
    wave_five_target_profile = {
        "core.constant.target_enum",
        "core.compare.target_enum",
        "core.target.operating_system",
        "core.target.architecture",
        "core.target.native_abi",
        "core.target.endianness",
    }
    wave_five_coroutine_yield = {"core.coroutine.yield"}
    canonical_coroutine_call = {"core.coroutine.call.sealed"}
    canonical_coroutine_indirect_call = {"core.coroutine.call.indirect"}
    canonical_coroutine_suspend = {"core.coroutine.suspend"}
    canonical_coroutine_cancel = {"core.cancel.publish"}
    wave_five_provider = {"core.provider.call"}
    wave_five_reborrow = {"core.existential.reborrow_read"}
    canonical_source_output = {"core.output.group.i64"}
    canonical_boolean = {
        "core.logical.not",
        "core.logical.and",
        "core.logical.or",
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
    wave_three_aggregate_place = {"core.place.project", "core.place.take"}
    wave_three_slice_four = {"core.error.publish"}
    wave_three_slice_five = {"core.call.sealed_invoke", "core.panic.publish"}
    canonical_assertion = {"core.assert.condition"}
    wave_four_executor = {
        "core.call.indirect_direct",
        "core.call.indirect_invoke",
        "core.call.witness_direct",
        "core.call.witness_invoke",
        "core.callable.pack",
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
        **{operation: "COMPLETE_W7_WAVE3_AGGREGATE_PLACE"
           for operation in wave_three_aggregate_place},
        **{operation: "COMPLETE_W7_WAVE3_SLICE4" for operation in wave_three_slice_four},
        **{operation: "COMPLETE_W7_WAVE3_SLICE5" for operation in wave_three_slice_five},
        **{operation: "COMPLETE_CANONICAL_ASSERTION_PANIC_CLEANUP"
           for operation in canonical_assertion},
        **{operation: "COMPLETE_W7_WAVE4" for operation in wave_four_executor},
        **{operation: "COMPLETE_W7_WAVE5_POINTER" for operation in wave_five_pointer},
        **{operation: "COMPLETE_W7_WAVE5_TARGET_PROFILE"
           for operation in wave_five_target_profile},
        **{operation: "COROUTINE_YIELD_CANONICAL_ONLY"
           for operation in wave_five_coroutine_yield},
        **{operation: "COROUTINE_CALL_CANONICAL_ONLY"
           for operation in canonical_coroutine_call},
        **{operation: "COROUTINE_INDIRECT_CALL_CANONICAL_ONLY"
           for operation in canonical_coroutine_indirect_call},
        **{operation: "COROUTINE_SUSPENSION_CANONICAL_ONLY"
           for operation in canonical_coroutine_suspend},
        **{operation: "COROUTINE_CANCEL_CANONICAL_ONLY"
           for operation in canonical_coroutine_cancel},
        **{operation: "IN_PROGRESS_W7_WAVE5_PROVIDER_SOURCE"
           for operation in wave_five_provider},
        **{operation: "COMPLETE_W7_WAVE5_REBORROW"
           for operation in wave_five_reborrow},
        **{operation: "COMPLETE_CANONICAL_SOURCE_RUN_OUTPUT_SLICE"
           for operation in canonical_source_output},
        **{operation: "COMPLETE_W7_WAVE5_BOOLEAN" for operation in canonical_boolean},
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
        support = {
            "src/program/xr_program_from_xi.h",
            "src/program/xr_program_from_xi.c",
            "src/program/xr_program_source_build.h",
            "src/program/xr_program_source_build.c",
            "src/app/cli/xcli_canonical_source.c",
            "src/app/cli/xcli_spec.c",
            "src/app/cli/xcmd_check.c",
            "src/app/cli/xcmd_run.c",
            "src/program/xr_program_verify.c",
            "src/program/xr_reference_evaluator.c",
            "src/ir/xi_pipeline.h",
            "src/ir/xi_pipeline.c",
            "src/ir/xi_lower_class.inc.c",
            "src/analysis/xglobal_producer.c",
            "tests/unit/ir/test_xi_pipeline.c",
            "tests/unit/ir/test_xr_program_imported_callable.c",
            "tests/unit/CMakeLists.txt",
            "tests/unit/program/test_xr_program_source_build.c",
            "tests/unit/program/xr_program_source_cases.json",
            "tests/cli/run_canonical_source_route_tests.py",
            "tests/unit/program/test_xr_program_verify.c",
            "tests/unit/program/xr_program_invoke_fixture.h",
            "tests/unit/program/xr_program_panic_fixture.h",
            "tests/unit/program/xr_program_callable_fixture.h",
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
        }
        matrix_source = json.loads(
            (root / "contracts/canonical-program/operation-capability-matrix.json")
            .read_text(encoding="utf-8")
        )
        support.update(
            relative
            for operation in matrix_source["operations"]
            for relative in operation.get("evidence", [])
        )
        for relative in sorted(support):
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, destination)
        validate(target)
        cmake = target / "tests/unit/CMakeLists.txt"
        original_cmake = cmake.read_text(encoding="utf-8")
        source_test = read(target, "tests/unit/program/test_xr_program_source_build.c")
        for before, after in (
            ("include(${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION})", ""),
            ('--fixture "${fixture_id}"', '--fixture "${native_target}"'),
            ("--expected-exit ${expected_exit}", "--expected-exit 0"),
            ('add_executable(${native_target} "${generated_c}")',
             'add_executable(${native_target} "other.c")'),
            ("TARGET test_xr_program_source_build PRE_LINK", "TARGET other PRE_LINK"),
        ):
            try:
                validate_native_fixtures(target, original_cmake.replace(before, after), source_test)
            except ContractError:
                pass
            else:
                raise ContractError(f"incorrect native fixture registration was accepted: {before}")
        manifest = target / "tests/unit/program/xr_program_source_cases.json"
        original_manifest = manifest.read_text(encoding="utf-8")
        value = json.loads(original_manifest)
        native_cases = [case for case in value["cases"] if case["fixture"] is not None]
        native_cases[0]["fixture"], native_cases[1]["fixture"] = (
            native_cases[1]["fixture"], native_cases[0]["fixture"])
        manifest.write_text(json.dumps(value), encoding="utf-8")
        try:
            validate_native_fixtures(target, original_cmake, source_test)
        except ContractError:
            pass
        else:
            raise ContractError("swapped source case native fixture ownership was accepted")
        manifest.write_text(original_manifest, encoding="utf-8")
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
        original_producer = producer.read_text(encoding="utf-8")
        allowed_tokens = (
            "XI_CLOSURE_NEW",
            "signature_id",
            "slot_signature_ids",
            "conformance_id",
        )
        producer.write_text(
            original_producer + "\n/* Allowed canonical tokens: " +
            ", ".join(allowed_tokens) + ". */\n",
            encoding="utf-8",
        )
        validate(target)
        mutations = (
            ("frozen legacy route", "XrTargetPlan"),
            ("CallDecision", "XrScalarCallDecision"),
            ("selector/name fallback", "method_name_id"),
            ("class-only itable", "XrtInterfaceMethodTable"),
            ("erased closure ABI", "xrt_closure_t"),
            ("backend signature recovery", "xaot_type_fingerprint"),
        )
        for owner, token in mutations:
            producer.write_text(original_producer + f"\n/* {token} injected */\n",
                                encoding="utf-8")
            try:
                validate(target)
            except ContractError as exc:
                require(owner in str(exc),
                        f"{owner} mutation reported the wrong authority: {exc}")
            else:
                raise ContractError(f"{owner} mutation was accepted")
        producer.write_text(original_producer, encoding="utf-8")
        owner = target / "src/program/xr_program_source_build.c"
        original_owner = owner.read_text(encoding="utf-8")
        owner.write_text(
            original_owner.replace("xr_program_write_from_xi", "removed_program_writer", 1),
            encoding="utf-8",
        )
        try:
            validate(target)
        except ContractError as exc:
            require("xr_program_write_from_xi" in str(exc),
                    f"source owner mutation reported the wrong invariant: {exc}")
        else:
            raise ContractError("source owner without canonical writer was accepted")
        owner.write_text(original_owner, encoding="utf-8")


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
