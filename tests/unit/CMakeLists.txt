# Unit tests for xray
# All unit tests are managed here (no glob in root CMakeLists.txt)

# ============================================================================
# Macro for adding unit tests that link to xray_core
# ============================================================================

macro(add_xray_unit_test name source)
    add_executable(${name} ${source} ${ARGN})
    target_compile_definitions(${name} PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
    target_include_directories(${name} PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${name} PRIVATE xray_core)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endmacro()

# ============================================================================
# value/ — Value system
# ============================================================================

add_xray_unit_test(test_xvalue value/test_xvalue.c)


# ============================================================================
# util/ — Hash, dynamic array, hashmap, arena
# ============================================================================

add_xray_unit_test(test_xhash util/test_xhash.c)
add_xray_unit_test(test_dynarray util/test_dynarray.c)
add_xray_unit_test(test_hashmap util/test_hashmap.c)
add_xray_unit_test(test_arena util/test_arena.c)

# ============================================================================
# mem/ — RC memory model
# ============================================================================

add_xray_unit_test(test_region mem/test_region.c)
add_xray_unit_test(test_obj_header mem/test_obj_header.c)
add_xray_unit_test(test_fixed_heap_teardown mem/test_fixed_heap_teardown.c)
# The runtime half of `weak`, tested before any language
# surface exists for it (W1 read-promotes, W5 definite clearing point).
add_xray_unit_test(test_weak_handle mem/test_weak_handle.c)
# The whole-heap traversal is validated against
# heap->object_count before any cycle logic runs on top of it. The body is
# compiled out unless XR_ENABLE_CYCLE_DETECTOR is on, so the test is a no-op
# in a default build rather than a missing target.
add_xray_unit_test(test_cycle_detector_traversal mem/test_cycle_detector_traversal.c)
if(XR_ENABLE_CYCLE_DETECTOR)
    target_compile_definitions(test_cycle_detector_traversal PRIVATE XR_ENABLE_CYCLE_DETECTOR=1)
endif()
# The shared-domain scan (system-heap registry, dual-band trial
# deletion, channel/task edges) plus per-domain metering. Same no-op-by-default
# shape as the traversal test above.
add_xray_unit_test(test_cycle_detector_shared mem/test_cycle_detector_shared.c)
if(XR_ENABLE_CYCLE_DETECTOR)
    target_compile_definitions(test_cycle_detector_shared PRIVATE XR_ENABLE_CYCLE_DETECTOR=1)
endif()

# These contract tests build only the low-level runtime modules. The focused
# validation loop stays independent of the full compiler archive. Ownership
# audit instrumentation is intentionally absent from release product archives.
add_executable(test_runtime_descriptor
    runtime/test_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_descriptor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_descriptor COMMAND $<TARGET_FILE:test_runtime_descriptor>)
set_tests_properties(test_runtime_descriptor PROPERTIES LABELS "runtime;abi;target-machine")

add_executable(test_runtime_abi_contract
    runtime/test_runtime_abi_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_abi_contract PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_abi_contract COMMAND $<TARGET_FILE:test_runtime_abi_contract>)
set_tests_properties(test_runtime_abi_contract PROPERTIES LABELS "runtime;abi;target-machine")

add_executable(test_runtime_object_header
    runtime/test_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_object_header PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_object_header COMMAND $<TARGET_FILE:test_runtime_object_header>)
set_tests_properties(test_runtime_object_header PROPERTIES LABELS "runtime;abi;target-machine")

add_executable(test_runtime_string_object
    runtime/test_runtime_string_object.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_string_object.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/plan/semantic/xr_semantic_ids.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_string_object PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_string_object COMMAND $<TARGET_FILE:test_runtime_string_object>)
set_tests_properties(test_runtime_string_object PROPERTIES
    LABELS "runtime;abi;object;string;target-machine")

add_test(
    NAME check_runtime_string_object_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_runtime_string_object_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_runtime_string_object_boundary PROPERTIES
    LABELS "runtime;abi;object;string;static-analysis;target-machine")

add_test(
    NAME check_runtime_object_header_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_runtime_object_header_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_runtime_object_header_boundary PROPERTIES
    LABELS "runtime;abi;static-analysis;target-machine")

add_executable(test_ownership_audit
    runtime/test_ownership_audit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/ownership/xr_ownership_audit.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_ownership_audit PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_ownership_audit PRIVATE
    XR_OWNERSHIP_AUDIT_TESTING=1
)
if(NOT WIN32)
    target_link_libraries(test_ownership_audit PRIVATE pthread)
endif()
add_test(NAME test_ownership_audit COMMAND $<TARGET_FILE:test_ownership_audit>)
set_tests_properties(test_ownership_audit PROPERTIES
    LABELS "runtime;ownership;concurrency;target-machine")

add_test(
    NAME check_ownership_audit_record_no_alloc
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_ownership_audit_record_no_alloc.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_ownership_audit_record_no_alloc PROPERTIES
    LABELS "runtime;ownership;static-analysis;target-machine"
)

add_test(
    NAME check_ownership_audit_release_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_ownership_audit_release_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_ownership_audit_release_boundary PROPERTIES
    LABELS "runtime;ownership;static-analysis;target-machine"
)

# ============================================================================
# object/ — Runtime objects (string, array, utf8)
# ============================================================================

add_xray_unit_test(test_utf8_diagnostic base/test_utf8_diagnostic.c)
add_xray_unit_test(test_utf8 object/test_utf8.c)

add_xray_unit_test(test_xarray object/test_xarray.c)
add_xray_unit_test(test_dynamic_class object/test_dynamic_class.c)
add_xray_unit_test(test_xstring object/test_xstring.c)
add_xray_unit_test(test_xmap object/test_xmap.c)
add_xray_unit_test(test_xset object/test_xset.c)
add_xray_unit_test(test_xtuple object/test_xtuple.c)
add_xray_unit_test(test_rc_container_release object/test_rc_container_release.c)

# ============================================================================
# frontend/ — Lexer, parser, diagnostics
# ============================================================================

add_xray_unit_test(test_lexer frontend/test_lexer.c)
add_xray_unit_test(test_lexer_keywords frontend/test_lexer_keywords.c)
add_xray_unit_test(test_lexer_positions frontend/test_lexer_positions.c)
add_xray_unit_test(test_lexer_trivia frontend/test_lexer_trivia.c)
add_xray_unit_test(test_parser frontend/test_parser.c)
add_xray_unit_test(test_parser_type_scope frontend/test_parser_type_scope.c)
add_xray_unit_test(test_parser_recoverable frontend/test_parser_recoverable.c)
add_xray_unit_test(test_parser_asi frontend/test_parser_asi.c)
add_xray_unit_test(test_diag_fmt frontend/test_diag_fmt.c)
add_xray_unit_test(test_xfmt_literal frontend/test_xfmt_literal.c)
add_xray_unit_test(test_formatter_comments frontend/test_formatter_comments.c)
add_xray_unit_test(test_formatter_strings frontend/test_formatter_strings.c)
add_xray_unit_test(test_fmt_roundtrip frontend/test_fmt_roundtrip.c)
add_xray_unit_test(test_analyzer_remove_file frontend/test_analyzer_remove_file.c)
add_xray_unit_test(test_analyzer_incremental frontend/test_analyzer_incremental.c)
add_xray_unit_test(test_xa_typed_program frontend/test_xa_typed_program.c)
# Phase 3.5: X-01 AstNode -> XrType side table acceptance tests.
add_xray_unit_test(test_ast_side_table frontend/test_ast_side_table.c)
add_xray_unit_test(test_symbol_binding frontend/test_symbol_binding.c)
add_xray_unit_test(test_scope_tree frontend/test_scope_tree.c)
add_xray_unit_test(test_xa_selection frontend/test_xa_selection.c)
add_xray_unit_test(test_ast_visit_coverage frontend/test_ast_visit_coverage.c)
add_xray_unit_test(test_selection_facts frontend/test_selection_facts.c)

# Phase 3: in-tree frontend architecture lint. Walks the project src/
# tree at runtime, so it needs the absolute path baked in at build
# time. The test does NOT link xray_core (it is a pure path/text
# walker), so we add it directly without the helper macro.
add_executable(test_frontend_architecture
    frontend/test_frontend_architecture.c
)
target_include_directories(test_frontend_architecture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(test_frontend_architecture PRIVATE
    XR_TEST_SRC_DIR="${CMAKE_SOURCE_DIR}/src"
)
add_test(NAME test_frontend_architecture
         COMMAND test_frontend_architecture)

# ============================================================================
# analyzer/ — Type inference, type checking
# ============================================================================

add_xray_unit_test(test_analyzer analyzer/test_analyzer.c)
add_xray_unit_test(test_effect_db analyzer/test_effect_db.c)
add_xray_unit_test(test_xrd_effect_metadata analyzer/test_xrd_effect_metadata.c)
add_xray_unit_test(test_mono analyzer/test_mono.c)
add_xray_unit_test(test_interface_conformance analyzer/test_interface_conformance.c)
add_xray_unit_test(test_xglobal_summary analysis/test_xglobal_summary.c)
add_xray_unit_test(test_xglobal_cache_payload analysis/test_xglobal_cache_payload.c)

# ============================================================================
# vm/ — Instruction encoding, opcode
# ============================================================================

add_xray_unit_test(test_instruction vm/test_instruction.c)
add_xray_unit_test(test_opcode vm/test_opcode.c)
add_xray_unit_test(test_bytecode_io vm/test_bytecode_io.c)
add_xray_unit_test(test_ffi_callback vm/test_ffi_callback.c)
add_xray_unit_test(test_ffi_scalar vm/test_ffi_scalar.c)
add_xray_unit_test(test_vm_api vm/test_vm_api.c)
add_xray_unit_test(test_run_closure_blocking vm/test_run_closure_blocking.c)
add_xray_unit_test(test_ic vm/test_ic.c)
# xray_core already PUBLIC-links pthread (coroutines need it), so the
# concurrent IC test inherits the dependency through xray_core.
add_xray_unit_test(test_ic_concurrent vm/test_ic_concurrent.c)
add_xray_unit_test(test_vm_exception vm/test_vm_exception.c)
add_xray_unit_test(test_vm_parallel_isolate vm/test_vm_parallel_isolate.c)
add_xray_unit_test(test_vm_grapheme_iterator vm/test_vm_grapheme_iterator.c)
add_test(
    NAME test_grapheme_internal_boundary
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/vm/test_grapheme_internal_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_grapheme_internal_boundary PROPERTIES
    LABELS "unicode;vm;task-199"
)
add_test(
    NAME test_release_evidence_contract
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/release/test_release_evidence.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_release_evidence_contract PROPERTIES
    LABELS "install;release;task-240"
)

# ============================================================================
# symbol/ — Symbol table
# ============================================================================

add_xray_unit_test(test_symbol symbol/test_symbol.c)

# ============================================================================
# ir/ — New typed SSA IR (Xi)
# ============================================================================

add_xray_unit_test(test_xi ir/test_xi.c)
add_xray_unit_test(test_xi_lower ir/test_xi_lower.c)
add_xray_unit_test(test_quoted_literal_scaling ir/test_quoted_literal_scaling.c)
add_xray_unit_test(test_xi_lowering_gen ir/test_xi_lowering_gen.c)
add_xray_unit_test(test_xi_opt ir/test_xi_opt.c)
add_xray_unit_test(test_xi_analysis ir/test_xi_analysis.c)
add_xray_unit_test(test_xi_cfg_edit ir/test_xi_cfg_edit.c)
add_xray_unit_test(test_xi_emit ir/test_xi_emit.c)
add_xray_unit_test(test_xi_pipeline ir/test_xi_pipeline.c)
add_xray_unit_test(test_xi_compare ir/test_xi_compare.c)
add_xray_unit_test(test_xi_cgen ir/test_xi_cgen.c)
target_sources(test_xi_cgen PRIVATE plan/target_profile_test_fixture.c)
target_compile_definitions(test_xi_cgen PRIVATE
    XRAY_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(MSVC)
    # This exhaustive generator test recursively lowers hundreds of fixtures.
    # MSVC ASan expands the call frames beyond PE/COFF's 1 MiB default stack.
    target_link_options(test_xi_cgen PRIVATE "/STACK:8388608")
endif()
add_xray_unit_test(test_xi_lower_hardfail ir/test_xi_lower_hardfail.c)
add_xray_unit_test(test_xi_stage ir/test_xi_stage.c)
add_xray_unit_test(test_xi_evidence ir/test_xi_evidence.c)
add_xray_unit_test(test_xi_intrinsic ir/test_xi_intrinsic.c)
add_xray_unit_test(test_xi_effect ir/test_xi_effect.c)
add_xray_unit_test(test_xi_escape ir/test_xi_escape.c)
add_xray_unit_test(test_xi_own ir/test_xi_own.c)
add_xray_unit_test(test_xi_arc_verify ir/test_xi_arc_verify.c)
add_xray_unit_test(test_xi_source_move_verify ir/test_xi_source_move_verify.c)
add_xray_unit_test(test_semantic_plan plan/test_semantic_plan.c)
set(XR_TARGET_PROFILE_TEST_FIXTURE plan/target_profile_test_fixture.c)
add_xray_unit_test(test_target_profile plan/test_target_profile.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_target_plan plan/test_target_plan.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_xtp_format plan/test_xtp_format.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_executable(test_runtime_target_plan_load_archive
    runtime/test_runtime_target_plan_load_archive.c)
set(XR_RUNTIME_SCALAR_ARTIFACT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(XR_RUNTIME_SCALAR_ARTIFACT_HEADER
    ${XR_RUNTIME_SCALAR_ARTIFACT_DIR}/runtime_scalar_artifacts.h)
add_custom_command(
    OUTPUT ${XR_RUNTIME_SCALAR_ARTIFACT_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${XR_RUNTIME_SCALAR_ARTIFACT_DIR}
    COMMAND $<TARGET_FILE:test_xtp_format>
            --write-runtime-header ${XR_RUNTIME_SCALAR_ARTIFACT_HEADER}
    DEPENDS test_xtp_format
    COMMENT "Generating native exact XSM/XTP scalar runtime fixture"
    VERBATIM)
add_custom_target(runtime_scalar_artifact_fixture
    DEPENDS ${XR_RUNTIME_SCALAR_ARTIFACT_HEADER})
add_dependencies(test_runtime_target_plan_load_archive
    runtime_scalar_artifact_fixture)
target_include_directories(test_runtime_target_plan_load_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${XR_RUNTIME_SCALAR_ARTIFACT_DIR})
target_link_libraries(test_runtime_target_plan_load_archive PRIVATE
    xray_vm)
add_test(NAME test_runtime_target_plan_load_archive
    COMMAND $<TARGET_FILE:test_runtime_target_plan_load_archive>)
add_xray_unit_test(test_runtime_generation
    runtime/test_runtime_generation.c)
add_executable(test_runtime_generation_archive
    runtime/test_runtime_generation_archive.c)
target_include_directories(test_runtime_generation_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_runtime_generation_archive PRIVATE
    xray_vm)
add_test(NAME test_runtime_generation_archive
    COMMAND $<TARGET_FILE:test_runtime_generation_archive>)
add_xray_unit_test(test_typed_frame vm/test_typed_frame.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_typed_dispatch vm/test_typed_dispatch.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_executable(test_typed_frame_runtime_archive
    runtime/test_typed_frame_runtime_archive.c)
target_include_directories(test_typed_frame_runtime_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_typed_frame_runtime_archive PRIVATE
    xray_vm)
add_test(NAME test_typed_frame_runtime_archive
    COMMAND $<TARGET_FILE:test_typed_frame_runtime_archive>)
add_executable(test_xtp_fuzz_entry ../fuzz/fuzz_xtp_decode.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_compile_definitions(test_xtp_fuzz_entry PRIVATE FUZZ_STANDALONE)
target_include_directories(test_xtp_fuzz_entry PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xtp_fuzz_entry PRIVATE xray_core)
add_test(NAME test_xtp_fuzz_entry COMMAND $<TARGET_FILE:test_xtp_fuzz_entry>)
add_xray_unit_test(test_xr_aot_scalar_plan aot/test_xr_aot_scalar_plan.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_xaot_scalar_target_cutover aot/test_xaot_scalar_target_cutover.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_xr_aot_refinement aot/test_xr_aot_refinement.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
set_tests_properties(test_xr_aot_refinement PROPERTIES
    LABELS "aot;refinement;backend;task-279"
)

# Keep the dumb-emitter contract self-contained even though the behavioral
# test later includes compiler-only fixtures. This compile probe has only the
# emission directory on its include path and fails if a forbidden layer leaks
# through the public emission headers.
include(CheckCSourceCompiles)
set(_XR_C_EMISSION_SAVED_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES "${PROJECT_SOURCE_DIR}/src/aot/emit_c")
unset(XR_C_EMISSION_HEADERS_ARE_ISOLATED CACHE)
check_c_source_compiles([=[
#include "xr_c_emission_schema.h"
#include "xr_c_emission_plan.h"
#if defined(XR_SEMANTIC_PLAN_H) || defined(XI_H) || defined(XAOT_REP_H) || defined(XTYPE_H)
#error "C emission headers exposed a forbidden compiler layer"
#endif
_Static_assert(XR_C_VALUE_REP_COUNT == 17, "value spelling schema changed");
int main(void) {
    XrCValueEmissionView view = {0};
    return (int) view.semantic_value;
}
]=] XR_C_EMISSION_HEADERS_ARE_ISOLATED)
set(CMAKE_REQUIRED_INCLUDES "${_XR_C_EMISSION_SAVED_REQUIRED_INCLUDES}")
unset(_XR_C_EMISSION_SAVED_REQUIRED_INCLUDES)
if(NOT XR_C_EMISSION_HEADERS_ARE_ISOLATED)
    message(FATAL_ERROR
        "C emission headers must compile without SemanticPlan, IR, Xaot, or xtype")
endif()
add_xray_unit_test(test_cache_key incremental/test_cache_key.c)
add_xray_unit_test(test_cache_store incremental/test_cache_store.c)
add_xray_unit_test(test_cache_artifact_verify
                   incremental/test_cache_artifact_verify.c
                   ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_target_plan_tasks
                   incremental/test_target_plan_tasks.c
                   ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_dependency_graph incremental/test_dependency_graph.c)
add_xray_unit_test(test_compiler_session_generation
                   toolchain/test_compiler_session_generation.c)
add_xray_unit_test(test_fs_atomic os/test_fs_atomic.c)
add_xray_unit_test(test_xi_tbaa ir/test_xi_tbaa.c)
add_xray_unit_test(test_xi_range ir/test_xi_range.c)
add_xray_unit_test(test_xi_bce ir/test_xi_bce.c)
add_xray_unit_test(test_xi_block_simplify ir/test_xi_block_simplify.c)
add_xray_unit_test(test_xi_jump_thread ir/test_xi_jump_thread.c)
add_xray_unit_test(test_xi_loop_iv ir/test_xi_loop_iv.c)
add_xray_unit_test(test_xi_loop_rotate ir/test_xi_loop_rotate.c)
add_xray_unit_test(test_xi_loop_peel ir/test_xi_loop_peel.c)
add_xray_unit_test(test_xi_loop_unroll ir/test_xi_loop_unroll.c)
add_xray_unit_test(test_xi_loop_split ir/test_xi_loop_split.c)
add_xray_unit_test(test_xi_loop_inv_branch ir/test_xi_loop_inv_branch.c)
add_xray_unit_test(test_xi_devirt ir/test_xi_devirt.c)
add_xray_unit_test(test_xi_cha ir/test_xi_cha.c)
add_xray_unit_test(test_xi_block_layout ir/test_xi_block_layout.c)
add_xray_unit_test(test_xi_slp ir/test_xi_slp.c)
add_xray_unit_test(test_xi_vec_scalar ir/test_xi_vec_scalar.c)
add_xray_unit_test(test_xi_vec_cost ir/test_xi_vec_cost.c)
add_xray_unit_test(test_xi_loop_vec ir/test_xi_loop_vec.c)
add_xray_unit_test(test_xi_reduction ir/test_xi_reduction.c)
add_xray_unit_test(test_xi_call_specialize ir/test_xi_call_specialize.c)
add_xray_unit_test(test_xi_comptime ir/test_xi_comptime.c)
add_xray_unit_test(test_xi_lto aot/test_xi_lto.c)
add_xray_unit_test(test_xaot_driver
    aot/test_xaot_driver.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_model.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_target_profile.c
)
add_xray_unit_test(test_xaot_transfer_plan aot/test_xaot_transfer_plan.c)
add_xray_unit_test(test_cgen_verify_output aot/test_cgen_verify_output.c)
set_tests_properties(test_cgen_verify_output PROPERTIES LABELS "aot;cgen;task-218")
add_xray_unit_test(test_xi_backend_plan_contract aot/test_xi_backend_plan_contract.c)
add_xray_unit_test(test_xrt_array aot/test_xrt_array.c)
add_xray_unit_test(test_xrt_execution_arena aot/test_xrt_execution_arena.c)
set_tests_properties(test_xrt_execution_arena PROPERTIES LABELS "aot;rc;task-252")
add_xray_unit_test(test_xrt_boolmap aot/test_xrt_boolmap.c)
add_xray_unit_test(test_xrt_dense_enum aot/test_xrt_dense_enum.c)
add_xray_unit_test(test_xrt_json_decode aot/test_xrt_json_decode.c)
add_xray_unit_test(test_xrt_static_map_set aot/test_xrt_static_map_set.c)
add_xray_unit_test(test_xrt_exception_tls aot/test_xrt_exception_tls.c)
add_xray_unit_test(test_type_names_parity aot/test_type_names_parity.c)
set_tests_properties(test_type_names_parity PROPERTIES LABELS "aot;vm;shared-core")
add_xray_unit_test(test_xrt_method_truthy aot/test_xrt_method_truthy.c)
add_executable(test_xrt_type_identity_freestanding aot/test_xrt_type_identity_freestanding.c)
target_compile_definitions(test_xrt_type_identity_freestanding PRIVATE
    XRAY_PROFILE_FREESTANDING=1)
target_include_directories(test_xrt_type_identity_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_xrt_type_identity_freestanding
    COMMAND $<TARGET_FILE:test_xrt_type_identity_freestanding>)
# This test deliberately compiles the header-only AOT runtime with XRT_IMPL and
# custom allocators.  Linking xray_core would give it a second runtime-global
# owner and fails deterministically under sanitizer linkers.
add_executable(test_xrt_user_hash_eq aot/test_xrt_user_hash_eq.c)
target_compile_definitions(test_xrt_user_hash_eq PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xrt_user_hash_eq PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_xrt_user_hash_eq COMMAND $<TARGET_FILE:test_xrt_user_hash_eq>)

# Generated AOT cores are also compiled as C++ by governed compatibility
# workflows.  Keep this target at the minimum supported C++ dialect and avoid
# linking xray_core so the header-only ABI/atomic contract is tested directly.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_executable(test_xr_atomic_cxx aot/test_xr_atomic_cxx.cpp)
    target_include_directories(test_xr_atomic_cxx PRIVATE ${XRAY_COMMON_INCLUDES})
    set_target_properties(test_xr_atomic_cxx PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME test_xr_atomic_cxx COMMAND $<TARGET_FILE:test_xr_atomic_cxx>)
    set_tests_properties(test_xr_atomic_cxx PROPERTIES LABELS "aot;cgen;cxx;task-243")
endif()
add_xray_unit_test(test_xi_licm ir/test_xi_licm.c)
add_xray_unit_test(test_xi_ifconv ir/test_xi_ifconv.c)
add_xray_unit_test(test_xi_sccp ir/test_xi_sccp.c)
add_xray_unit_test(test_xi_gvn_pre ir/test_xi_gvn_pre.c)
add_xray_unit_test(test_xi_strength ir/test_xi_strength.c)
add_xray_unit_test(test_xi_dom_cache ir/test_xi_dom_cache.c)
add_xray_unit_test(test_xi_ivsr ir/test_xi_ivsr.c)
add_xray_unit_test(test_xi_inline ir/test_xi_inline.c)
add_xray_unit_test(test_xi_tail_call ir/test_xi_tail_call.c)
add_xray_unit_test(test_xi_verify_ext ir/test_xi_verify_ext.c)
add_xray_unit_test(test_xi_shuffle ir/test_xi_shuffle.c)

# ============================================================================
# base/ — Config, logging, SIMD, SWAR, mutex, intmap, mem_profiler
# ============================================================================

add_xray_unit_test(test_config base/test_config.c)
add_xray_unit_test(test_log base/test_log.c)
add_xray_unit_test(test_swar base/test_swar.c)
add_xray_unit_test(test_hash_core base/test_hash_core.c)
add_xray_unit_test(test_cstr_core base/test_cstr_core.c)
add_xray_unit_test(test_error_core base/test_error_core.c)
add_xray_unit_test(test_builtin_schema base/test_builtin_schema.c)
add_xray_unit_test(test_map_set_abi base/test_map_set_abi.c)
add_xray_unit_test(test_native_type_core base/test_native_type_core.c)
add_xray_unit_test(test_value_format_core base/test_value_format_core.c)
add_xray_unit_test(test_swiss_index base/test_swiss_index.c)
add_xray_unit_test(test_typed_ops base/test_typed_ops.c)
add_xray_unit_test(test_simd base/test_simd.c)
add_xray_unit_test(test_mutex base/test_mutex.c)
add_xray_unit_test(test_intmap base/test_intmap.c)
add_xray_unit_test(test_mem_profiler base/test_mem_profiler.c)
add_xray_unit_test(test_xmalloc base/test_xmalloc.c)
add_xray_unit_test(test_process_shutdown base/test_process_shutdown.c)
set_tests_properties(test_process_shutdown PROPERTIES LABELS "runtime;memory;task-218")

# ============================================================================
# coro/ — MPSC queue, work-stealing queue
# ============================================================================

add_xray_unit_test(test_mpsc_queue coro/test_mpsc_queue.c)
add_xray_unit_test(test_steal_queue coro/test_steal_queue.c)
add_xray_unit_test(test_async_pool coro/test_async_pool.c)
add_xray_unit_test(test_scheduler_runq coro/test_scheduler_runq.c)
add_xray_unit_test(test_timer_wheel coro/test_timer_wheel.c)
add_xray_unit_test(test_channel_close coro/test_channel_close.c)
add_xray_unit_test(test_scope_wait coro/test_scope_wait.c)
add_xray_unit_test(test_work_queue_wait coro/test_work_queue_wait.c)
add_xray_unit_test(test_result_group coro/test_result_group.c)
add_xray_unit_test(test_native_backend coro/test_native_backend.c)
if(MSVC)
    # These white-box scheduler fixtures embed the complete isolate/runtime
    # state and exceed the PE default 1 MiB stack before the first assertion.
    foreach(target IN ITEMS
            test_scheduler_runq
            test_channel_close
            test_scope_wait
            test_work_queue_wait
            test_result_group
            test_native_backend)
        target_link_options(${target} PRIVATE "/STACK:16777216")
    endforeach()
endif()

# ============================================================================
# module/ — Resolver, semver, lockfile, source cache
# ============================================================================

add_xray_unit_test(test_resolver module/test_resolver.c)
if(NOT WIN32)
  add_xray_unit_test(test_module_resolver module/test_module_resolver.c)
  add_xray_unit_test(test_module_graph module/test_module_graph.c)
endif()
add_xray_unit_test(test_module_publication module/test_module_publication.c)
add_xray_unit_test(test_semver module/test_semver.c)
add_xray_unit_test(test_lockfile module/test_lockfile.c)
add_xray_unit_test(test_source_cache module/test_source_cache.c)
add_xray_unit_test(test_project module/test_project.c)

# ============================================================================
# encoding/ — Base64, hex, URL encoding, Unicode
# ============================================================================

add_xray_unit_test(test_unicode encoding/test_unicode.c)
add_xray_unit_test(test_unicode_grapheme_table encoding/test_unicode_grapheme_table.c)
add_xray_unit_test(test_unicode_grapheme_cursor encoding/test_unicode_grapheme_cursor.c)
target_compile_definitions(test_unicode_grapheme_cursor PRIVATE
    XR_GRAPHEME_BREAK_TEST_FILE="${CMAKE_SOURCE_DIR}/third_party/unicode/17.0.0/GraphemeBreakTest.txt"
)
add_test(
    NAME test_unicode_grapheme_generator
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/encoding/test_unicode_grapheme_generator.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_unicode_grapheme_generator PROPERTIES
    LABELS "unicode;generator;task-199"
)

# ============================================================================
# http/ — HTTP/2 helpers
# ============================================================================

add_xray_unit_test(test_http2_huffman http/test_http2_huffman.c)

# ============================================================================
# stdlib/ — DateTime, crypto, compress, XML, buffer pool
# ============================================================================

add_xray_unit_test(test_array_core stdlib/test_array_core.c)
add_executable(test_assert_condition_core stdlib/test_assert_condition_core.c)
target_include_directories(test_assert_condition_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_assert_condition_core COMMAND $<TARGET_FILE:test_assert_condition_core>)
add_xray_unit_test(test_xrt_assert_condition_owner_freestanding
    aot/test_xrt_assert_condition_owner_freestanding.c)
if(NOT WIN32)
    add_executable(test_xrt_assert_condition_owner_c90
        aot/test_xrt_assert_condition_owner_c90.c)
    target_include_directories(test_xrt_assert_condition_owner_c90 PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(test_xrt_assert_condition_owner_c90 PROPERTIES
        C_STANDARD 90
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    add_test(NAME test_xrt_assert_condition_owner_c90
        COMMAND $<TARGET_FILE:test_xrt_assert_condition_owner_c90>)
endif()
add_xray_unit_test(test_byte_array_copy_core stdlib/test_byte_array_copy_core.c)
add_xray_unit_test(test_byte_array_repeat_core stdlib/test_byte_array_repeat_core.c)
add_executable(test_byte_array_append_core stdlib/test_byte_array_append_core.c)
target_include_directories(test_byte_array_append_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_byte_array_append_core COMMAND $<TARGET_FILE:test_byte_array_append_core>)
add_xray_unit_test(test_math_core stdlib/test_math_core.c)
add_xray_unit_test(test_numeric_core stdlib/test_numeric_core.c)
add_xray_unit_test(test_null_test_core stdlib/test_null_test_core.c)
add_xray_unit_test(test_compare_core stdlib/test_compare_core.c)
add_xray_unit_test(test_target_simd_core stdlib/test_target_simd_core.c)
add_executable(test_xrt_compare_owner_freestanding
    aot/test_xrt_compare_owner_freestanding.c)
target_include_directories(test_xrt_compare_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_compare_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_compare_owner_freestanding>)
add_xray_unit_test(test_range_core stdlib/test_range_core.c)
add_executable(test_data_pointer_core stdlib/test_data_pointer_core.c)
target_include_directories(test_data_pointer_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_data_pointer_core COMMAND $<TARGET_FILE:test_data_pointer_core>)
add_executable(test_xrt_data_pointer_owner_freestanding
    aot/test_xrt_data_pointer_owner_freestanding.c)
target_include_directories(test_xrt_data_pointer_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_data_pointer_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_data_pointer_owner_freestanding>)
add_executable(test_cell_access_core stdlib/test_cell_access_core.c)
target_include_directories(test_cell_access_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_cell_access_core COMMAND $<TARGET_FILE:test_cell_access_core>)
add_executable(test_xrt_cell_access_owner_freestanding
    aot/test_xrt_cell_access_owner_freestanding.c)
target_include_directories(test_xrt_cell_access_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_cell_access_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_cell_access_owner_freestanding>)
add_xray_unit_test(test_xrt_range_owner_freestanding aot/test_xrt_range_owner_freestanding.c)
add_executable(test_enum_metadata_core stdlib/test_enum_metadata_core.c)
target_include_directories(test_enum_metadata_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_enum_metadata_core COMMAND $<TARGET_FILE:test_enum_metadata_core>)
add_executable(test_xrt_enum_metadata_owner_freestanding
    aot/test_xrt_enum_metadata_owner_freestanding.c)
target_include_directories(test_xrt_enum_metadata_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_enum_metadata_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_enum_metadata_owner_freestanding>)
add_xray_unit_test(test_xrt_numeric_neg_owner_freestanding aot/test_xrt_numeric_neg_owner_freestanding.c)
add_xray_unit_test(test_xrt_null_test_owner_freestanding aot/test_xrt_null_test_owner_freestanding.c)
if(NOT WIN32)
    add_executable(test_xrt_null_test_owner_c90 aot/test_xrt_null_test_owner_c90.c)
    target_include_directories(test_xrt_null_test_owner_c90 PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(test_xrt_null_test_owner_c90 PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
    add_test(NAME test_xrt_null_test_owner_c90 COMMAND $<TARGET_FILE:test_xrt_null_test_owner_c90>)
endif()
add_xray_unit_test(test_xrt_byte_slice_compare_owner_freestanding aot/test_xrt_byte_slice_compare_owner_freestanding.c)
add_xray_unit_test(test_xrt_byte_array_copy_owner_freestanding aot/test_xrt_byte_array_copy_owner_freestanding.c)
add_xray_unit_test(test_xrt_raw_scalar_owner_freestanding aot/test_xrt_raw_scalar_owner_freestanding.c)
if(NOT WIN32)
    add_executable(test_xrt_byte_array_copy_owner_c90 aot/test_xrt_byte_array_copy_owner_c90.c)
    target_include_directories(test_xrt_byte_array_copy_owner_c90 PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(test_xrt_byte_array_copy_owner_c90 PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
    add_test(NAME test_xrt_byte_array_copy_owner_c90
        COMMAND $<TARGET_FILE:test_xrt_byte_array_copy_owner_c90>)
endif()
add_xray_unit_test(test_string_core stdlib/test_string_core.c)
add_xray_unit_test(test_strbuf_core stdlib/test_strbuf_core.c)
add_xray_unit_test(test_truthy_core stdlib/test_truthy_core.c)
add_xray_unit_test(test_type_identity_core stdlib/test_type_identity_core.c)
add_xray_unit_test(test_io_core stdlib/test_io_core.c)
add_xray_unit_test(test_os_core stdlib/test_os_core.c)
add_xray_unit_test(test_crypto stdlib/test_crypto.c)
if(XR_STDLIB_COMPRESS)
    add_xray_unit_test(test_compress stdlib/test_compress.c)
endif()
add_xray_unit_test(test_xml_node stdlib/test_xml_node.c)
add_xray_unit_test(test_cluster_proto stdlib/test_cluster_proto.c)
set_tests_properties(test_cluster_proto PROPERTIES LABELS "cluster;protocol;stdlib")
add_xray_unit_test(test_prelude_init stdlib/test_prelude_init.c)
add_xray_unit_test(test_native_type_surface stdlib/test_native_type_surface.c)
set_tests_properties(test_native_type_surface PROPERTIES LABELS "stdlib;metadata;native-types")
if(XRAY_STDLIB_VM_FASTPATHS)
    add_xray_unit_test(test_stdlib_vm_fastpath_abi stdlib/test_stdlib_vm_fastpath_abi.c)
    set_tests_properties(test_stdlib_vm_fastpath_abi PROPERTIES
        LABELS "stdlib;performance;abi;task-256"
        RUN_SERIAL TRUE
        TIMEOUT 120
    )
    add_test(
        NAME test_generate_vm_fastpaths
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/test_generate_vm_fastpaths.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_generate_vm_fastpaths PROPERTIES
        LABELS "stdlib;metadata;tooling;task-256"
    )
endif()
if(NOT WIN32)
    add_xray_unit_test(test_io stdlib/test_io.c)
endif()

# ============================================================================
# regex/ — Regex engine
# ============================================================================

add_xray_unit_test(test_regex regex/test_regex.c)

# ============================================================================
# api/ — Public API defense tests
# ============================================================================

add_xray_unit_test(test_api_defense api/test_api_defense.c)
add_test(
    NAME test_api_inventory_param_modes
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_param_modes.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_api_inventory_param_modes PROPERTIES
    LABELS "api;metadata;tooling"
)
add_test(
    NAME test_api_inventory_diff
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_diff.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_api_inventory_diff PROPERTIES
    LABELS "api;metadata;tooling"
)
add_test(
    NAME test_api_inventory_enum_metadata
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_enum_metadata.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_api_inventory_enum_metadata PROPERTIES
    LABELS "api;metadata;tooling;enum"
)
add_test(
    NAME test_param_mode_tooling_display
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_mode_tooling_display.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_mode_tooling_display PROPERTIES
    LABELS "api;metadata;tooling;lsp;mcp"
)
add_test(
    NAME test_param_mode_diagnostics
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_mode_diagnostics.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_mode_diagnostics PROPERTIES
    LABELS "api;syntax;diagnostics;tooling"
)
add_test(
    NAME test_param_contract_frontend
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_contract_frontend.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_contract_frontend PROPERTIES
    LABELS "frontend;syntax;analyzer;task-206"
    TIMEOUT 60
)
add_test(
    NAME test_param_contract_aot
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_contract_aot.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_contract_aot PROPERTIES
    LABELS "aot;frontend;task-206"
    TIMEOUT 60
)
add_test(
    NAME test_string_native_error_abi
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_string_native_error_abi.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_string_native_error_abi PROPERTIES
    LABELS "vm;aot;frontend;task-198"
    TIMEOUT 120
)
add_test(
    NAME test_compress_native_error_abi
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_compress_native_error_abi.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_compress_native_error_abi PROPERTIES
    LABELS "vm;aot;frontend;stdlib;compress;task-198"
    TIMEOUT 120
)
add_test(
    NAME test_crypto_native_error_abi
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_crypto_native_error_abi.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_crypto_native_error_abi PROPERTIES
    LABELS "vm;aot;frontend;stdlib;crypto;task-198"
    TIMEOUT 120
)

# test_repl needs the isolate-profile factory (xr_isolate_profile_*)
# in addition to xray_core, so build it like test_cli_spec with an
# explicit source list.
add_executable(test_repl
    api/test_repl.c
    ${CMAKE_SOURCE_DIR}/src/api/xisolate_profile.c
)
target_include_directories(test_repl PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/api
)
target_link_libraries(test_repl PRIVATE xray_core)
add_test(NAME test_repl COMMAND test_repl)

# ============================================================================
# lsp/ — Language Server Protocol (conditional)
# ============================================================================

if(XR_BUILD_LSP)
    add_executable(test_lsp_json
        lsp/test_lsp_json.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_lsp_json PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    add_test(NAME test_lsp_json COMMAND test_lsp_json)

    add_executable(test_lsp_stdlib
        lsp/test_lsp_stdlib.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_stdlib.c
    )
    target_include_directories(test_lsp_stdlib PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
    )
    add_test(NAME test_lsp_stdlib COMMAND test_lsp_stdlib)

    file(GLOB LSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/lsp/*.c)
    list(REMOVE_ITEM LSP_SOURCES ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_main.c)

    # The full-source LSP tests exercise the same Windows CRT-pipe adapters as
    # the production server; keep them enabled on every host.
    add_executable(test_lsp_document
        lsp/test_lsp_document.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_document PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_document PRIVATE xray_core)
    add_test(NAME test_lsp_document COMMAND test_lsp_document)
    add_test(NAME test_lsp_protocol_transcript
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/lsp/test_protocol_transcript.py
            --xray $<TARGET_FILE:xray>
    )
    set_tests_properties(test_lsp_protocol_transcript PROPERTIES TIMEOUT 20)

    add_executable(test_lsp_config
        lsp/test_lsp_config.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_config PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_config PRIVATE xray_core)
    add_test(NAME test_lsp_config COMMAND test_lsp_config)

    add_executable(test_lsp_navigation
        lsp/test_lsp_navigation.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_navigation PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_navigation PRIVATE xray_core)
    add_test(NAME test_lsp_navigation COMMAND test_lsp_navigation)

    add_executable(test_lsp_diagnostics
        lsp/test_lsp_diagnostics.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_diagnostics PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_diagnostics PRIVATE xray_core)
    add_test(NAME test_lsp_diagnostics COMMAND test_lsp_diagnostics)
endif()

# ============================================================================
# dap/ — Debug Adapter Protocol (conditional)
# ============================================================================

if(XR_BUILD_DAP)
    add_executable(test_dap_server
        dap/test_dap_server.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_dap_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    add_test(NAME test_dap_server COMMAND test_dap_server)

    add_executable(test_dap_breakpoints
        dap/test_dap_breakpoints.c
    )
    target_include_directories(test_dap_breakpoints PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/dap
    )
    add_test(NAME test_dap_breakpoints COMMAND test_dap_breakpoints)

    if(WIN32)
        set(_dap_fd_impl ${CMAKE_SOURCE_DIR}/src/os/win/fd_win.c)
    else()
        set(_dap_fd_impl ${CMAKE_SOURCE_DIR}/src/os/unix/fd_unix.c)
    endif()
    add_executable(test_dap_transport
        dap/test_dap_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/dap/xdap_transport.c
        ${CMAKE_SOURCE_DIR}/src/base/xframing.c
        ${_dap_fd_impl}
    )
    target_include_directories(test_dap_transport PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/src/base
    )
    add_test(NAME test_dap_transport COMMAND test_dap_transport)
endif()

# ============================================================================
# mcp/ — MCP protocol, tools, resources, prompts
# ============================================================================

if(XR_BUILD_MCP)
    add_executable(test_mcp_transport
        mcp/test_mcp_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_transport_stdio.c
    )
    target_include_directories(test_mcp_transport PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
    )
    target_link_libraries(test_mcp_transport PRIVATE xray_core)
    add_test(NAME test_mcp_transport COMMAND test_mcp_transport)

    add_executable(test_mcp_protocol
        mcp/test_mcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_jsonrpc.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_registry.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_schema.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_lang.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_run.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_kb.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_resources.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_prompts.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge_generated.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_mcp_protocol PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    target_link_libraries(test_mcp_protocol PRIVATE xray_core)
    add_test(NAME test_mcp_protocol COMMAND test_mcp_protocol)
    add_test(NAME test_mcp_knowledge_generation
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/mcp/test_knowledge_generation.py
            --root ${CMAKE_SOURCE_DIR}
            --xray $<TARGET_FILE:xray>
    )
    add_test(NAME test_mcp_protocol_transcript
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/mcp/test_protocol_transcript.py
            --xray $<TARGET_FILE:xray>
    )
endif()

# ============================================================================
# cli/ — CLI spec, parser, dispatch, help
# ============================================================================

add_executable(test_cli_fs
    cli/test_cli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_fs PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
target_link_libraries(test_cli_fs PRIVATE xray_core)
add_test(NAME test_cli_fs COMMAND test_cli_fs)

add_test(NAME test_conversion_inventory
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/tests/unit/api/test_conversion_inventory.py
        $<TARGET_FILE:xray>
        ${CMAKE_SOURCE_DIR}/tests/diff/cases/semantics/int/contextual_numeric_conversion.xr
)
set_tests_properties(test_conversion_inventory PROPERTIES
    LABELS "api;cli;analyzer;numeric;task-242"
    PROCESSORS 1
    TIMEOUT 30
)

add_executable(test_cli_spec
    cli/test_cli_spec.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_spec.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_parser.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_help.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_diag.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_spec PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
target_link_libraries(test_cli_spec PRIVATE xray_core)
add_test(NAME test_cli_spec COMMAND test_cli_spec)

add_executable(test_cli_toolchain
    cli/test_cli_toolchain.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_command.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_model.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_config.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_discovery.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_json.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_probe_cache.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_process.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_runtime_manifest.c
)
target_include_directories(test_cli_toolchain PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/toolchain
)
target_link_libraries(test_cli_toolchain PRIVATE xray_core)
add_test(NAME test_cli_toolchain COMMAND test_cli_toolchain)

add_xray_unit_test(test_target_profile_authority
    cli/test_target_profile_authority.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_model.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_target_profile.c
)
set_tests_properties(test_target_profile_authority PROPERTIES
    LABELS "aot;runtime;target-machine")

add_executable(process_spawn_benchmark EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/benchmarks/process/process_spawn_benchmark.c
)
target_include_directories(process_spawn_benchmark PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(process_spawn_benchmark PRIVATE xray_core)

# ============================================================================
# test_harness/ — xray test runner self-tests
# Verify that @test discover, execution, pass/fail/skip, and exit codes
# all behave correctly.  These run the xray binary, not C unit tests.
# ============================================================================

set(_HARNESS_DIR ${CMAKE_SOURCE_DIR}/tests/test_harness)

add_test(NAME harness_single_pass
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/single_pass.xr)
add_test(NAME harness_multi_pass
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/multi_pass.xr)
add_test(NAME harness_with_skip
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/with_skip.xr)

add_test(NAME harness_single_fail
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/single_fail.xr)
set_tests_properties(harness_single_fail PROPERTIES WILL_FAIL TRUE)

add_test(NAME harness_no_tests
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/no_tests.xr)
set_tests_properties(harness_no_tests PROPERTIES WILL_FAIL TRUE)

add_test(NAME harness_mixed_pass_fail
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/mixed_pass_fail.xr)
set_tests_properties(harness_mixed_pass_fail PROPERTIES WILL_FAIL TRUE)

# ============================================================================
# Reverse-invariant checks for the Xi IR layer (static grep-based gates).
# Runs as part of ctest so PRs that introduce raw malloc, phase coordinates
# in source, or fallback mode flags are blocked locally before CI even runs.
# ============================================================================
add_test(
    NAME check_xi_invariants
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_xi_invariants.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME check_rc_naming
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_rc_naming.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_rc_naming PROPERTIES
    LABELS "memory;rc;naming"
)

add_test(
    NAME check_xi_template_lowering
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xi_template_lowering.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
