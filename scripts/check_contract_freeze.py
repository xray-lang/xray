#!/usr/bin/env python3
"""Check semantic-contract verification responsibilities.

Migrated contracts name existing assertion tests. CTest fixtures execute those
tests once and block this gate on failure. Registration is not execution evidence.
Contracts whose replacements are still failing retain their digest checks until
the corresponding verification responsibility can be transferred safely.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ContractSpec:
    name: str
    anchors: tuple[str, ...]


CONTRACT_SPECS = (
    ContractSpec(
        "intrinsic-identity.md",
        (
            "src/frontend/analyzer/xa_intrinsic_registry.def",
            "src/ir/xi_method_sym.def",
            "src/ir/xi_semantic_intrinsic.c",
            "src/plan/semantic/xr_semantic_native_leaf_shape.h",
            "src/plan/semantic/xr_semantic_native_module_call_shape.h",
            "src/plan/semantic/xr_semantic_string_utf8_shape.h",
            "src/shared/xr_core_intrinsic.def",
            "contracts/capability-deletions.tsv",
            "scripts/check_branch_hint_surface_residue.py",
            "tests/regression/05_functions/0582_removed_builtin_names_reusable.xr",
        ),
    ),
    ContractSpec("xi-canonical-ops.md", ()),
    ContractSpec(
        "assertion-semantics.md",
        (
            "src/shared/xr_assertion_plan.h",
            "src/shared/xr_assertion_core.h",
            "src/shared/xr_deep_equality.h",
            "src/shared/xr_core_intrinsic_registry.c",
            "src/frontend/analyzer/xanalyzer_visitor_call.c",
            "src/ir/xi_emit_eh.c",
            "src/ir/xi_verify.c",
            "src/vm/xvm_dispatch_assert.inc.c",
            "src/aot/xrt_assertion.h",
            "src/aot/xrt_core_freestanding.h",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/plan/target/xr_target_capability.h",
            "src/plan/target/xr_target_profile.h",
            "src/plan/target/xr_target_builder.c",
            "src/plan/target/xr_target_verify.c",
            "src/runtime/abi/xr_runtime_target_authority.c",
            "tests/unit/aot/test_xrt_assertion.c",
            "tests/unit/aot/test_xrt_assertion_freestanding.c",
            "tests/aot/run_freestanding_assertion_provider_test.py",
            "tests/unit/ir/test_xi_emit.c",
            "tests/unit/plan/test_target_profile.c",
            "tests/unit/plan/test_target_plan.c",
            "tests/unit/fixtures/assertion/same_t_contextual.xr",
        ),
    ),
    ContractSpec("effect-semantics.md", ()),
    ContractSpec(
        "zero-cost-residue.md",
        (
            "src/aot/xi_cgen.h",
            "src/aot/xi_cgen.c",
            "src/aot/xi_cgen_ctx_impl.inc.c",
            "src/app/cli/xcmd_verify.c",
        ),
    ),
    ContractSpec("rc-contract.md", ()),
    ContractSpec("cgen-wellformedness.md", ()),
    ContractSpec("meta-ownership.md", ()),
    ContractSpec("differential-protocol.md", ()),
    ContractSpec(
        "process-byte-stream.md",
        (
            "src/os/win/proc_win.c",
            "src/aot/xrt_sys.h",
            "src/aot/xrt_os.h",
            "src/app/toolchain/xtc_process.h",
            "src/app/toolchain/xtc_process.c",
            "scripts/check_subprocess_text_boundaries.py",
            "scripts/check_process_zero_cost.py",
            "tests/probes/rc/check_execution_arena_l2.py",
            "tests/probes/rc/check_mutable_capture_cell_rss.py",
            "tests/unit/cli/test_cli_toolchain.c",
        ),
    ),
    ContractSpec(
        "target-abi.md",
        (
            "src/aot/xaot_link.c",
            "src/aot/xaot_boundary.h",
            "src/aot/xaot_boundary.c",
            "src/aot/xaot_callable.c",
            "src/aot/xaot_prepare.c",
            "src/aot/xaot_prepare.h",
            "src/aot/xaot_bundle.c",
            "src/aot/xaot_verify.c",
            "src/aot/xaot_driver.c",
            "src/aot/xaot_coro.h",
            "src/aot/refine/xr_aot_refinement.h",
            "src/aot/refine/xr_aot_representation_refinement.c",
            "src/aot/refine/xr_aot_representation_refinement.h",
            "src/aot/refine/xr_aot_scalar_ref_v1.h",
            "src/aot/refine/xr_aot_scalar_ref_v1.c",
            "src/aot/refine/xr_aot_scalar_value.c",
            "src/aot/refine/xr_aot_tail_call_conformance.h",
            "src/aot/refine/xr_aot_tail_call_conformance.c",
            "src/aot/emit_c/xr_c_emission_schema.h",
            "src/aot/emit_c/xr_c_emission_plan.h",
            "src/aot/emit_c/xr_c_emission_plan_internal.h",
            "src/aot/emit_c/xr_c_emission_plan.c",
            "src/aot/emit_c/xr_c_scalar_ref_projection.h",
            "src/aot/emit_c/xr_c_scalar_ref_projection.c",
            "src/aot/emit_c/xr_c_program_emission.h",
            "src/aot/emit_c/xr_c_program_emission.c",
            "src/aot/xi_cgen_value_helpers.inc.c",
            "src/aot/xr_leaf_value_product_program_emission.h",
            "src/aot/xr_leaf_value_product_program_emission.c",
            "src/aot/xr_target_aggregate_c_projection.h",
            "src/aot/xr_target_aggregate_c_projection.c",
            "tests/target-machine/compiler_archive_link_probe.c",
            "src/aot/xi_cgen_abi_helpers.inc.c",
            "src/aot/xi_cgen_class_helpers.inc.c",
            "src/aot/xi_cgen_class_native_helpers.inc.c",
            "src/aot/xi_cgen_array_helpers.inc.c",
            "src/aot/xi_cgen_dispatch_helpers.inc.c",
            "src/aot/xi_cgen_program_entry.inc.c",
            "src/aot/xi_cgen_struct_helpers.inc.c",
            "src/aot/xi_cgen.c",
            "src/aot/xrt_hosted_context.c",
            "src/aot/xrt_method.h",
            "src/aot/xrt_coll.h",
            "src/aot/xrt_core_freestanding.h",
            "src/aot/xrt_provider_abi.h",
            "src/aot/xrt_time.h",
            "src/base/xnumber_parse_error.h",
            "include/xray_hosted_fragment_abi.h",
            "src/app/cli/xcmd_build.c",
            "tests/aot/run_aot_incremental_cache.py",
            "src/app/toolchain/xtc_model.c",
            "src/app/toolchain/xtc_probe.c",
            "src/ir/xi.h",
            "src/ir/xi_opt.c",
            "src/stdlib/xstdlib_vm_fastpath_amalgam.c",
            "src/plan/semantic/xr_semantic_value_aggregate_shape.h",
            "src/plan/semantic/xr_semantic_shared_read_shape.h",
            "src/plan/semantic/xr_semantic_string_shape.h",
            "src/plan/semantic/xr_semantic_string_runes_shape.h",
            "src/plan/semantic/xr_semantic_string_slice_shape.h",
            "src/plan/semantic/xr_semantic_string_utf8_shape.h",
            "src/plan/target/xr_target_call_abi_shape.h",
            "src/plan/semantic/xr_semantic_iterator_rune_has_next_shape.h",
            "src/plan/semantic/xr_semantic_iterator_rune_next_shape.h",
            "src/plan/semantic/xr_semantic_rune_to_uint32_shape.h",
            "src/plan/semantic/xr_semantic_rune_is_whitespace_shape.h",
            "stdlib/simd/simd.xr",
            "tests/unit/aot/test_xr_aot_refinement.c",
            "tests/unit/aot/test_xr_aot_scalar_plan.c",
            "tests/unit/aot/test_xrt_type_identity_freestanding.c",
            "tests/unit/aot/test_xaot_driver.c",
        ),
    ),
    ContractSpec("memory-model.md", ()),
    ContractSpec("structural-object-json-map-boundary.md", ()),
    ContractSpec("sort-semantics.md", ()),
    ContractSpec("semantic-ownership.md", ()),
    ContractSpec(
        "unified-target-machine-discovery.md",
        (
            ".gitattributes",
            "contracts/target-machine/semantic-owner-inventory.json",
            "contracts/target-machine/aot-plan-destination-inventory.json",
            "contracts/target-machine/legacy-vm-inventory.json",
            "contracts/target-machine/legacy-product-residue.json",
            "contracts/target-machine/migration-source-classification.json",
            "contracts/target-machine/object-extent-inventory.json",
            "contracts/target-machine/validation-matrix.json",
            "contracts/target-machine/baseline-manifest.json",
            "contracts/target-machine/diagnostic-codes.toml",
            "contracts/target-machine/id-and-fingerprint-policy.toml",
            "scripts/target_machine_phase0.py",
            "scripts/run_target_machine_matrix_row.py",
            "scripts/check_legacy_product_residue.py",
            "scripts/check_target_machine_migration_classification.py",
            "scripts/target_machine_retired_runtime_symbols.py",
            "scripts/check_runtime_header_dependencies.py",
            "tests/target-machine/phase0/run_baseline.py",
            "tests/target-machine/test_matrix_row_runner.py",
            "tests/benchmarks/target-machine/source_run/main.xr",
            "tests/benchmarks/target-machine/source_run/helper.xr",
            "tests/benchmarks/target-machine/source_run/helper.edited.txt",
            "tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr",
            "tests/target-machine/phase0/coroutine_vertical/manifest.toml",
            "tests/target-machine/phase0/coroutine_vertical/report.json",
            "tests/target-machine/phase0/typed_slot_calibration/mailbox.xr",
            "tests/target-machine/phase0/typed_slot_calibration/manifest.toml",
            "tests/target-machine/phase0/typed_slot_calibration/report.json",
            "tests/target-machine/phase0/typed_slot_calibration/workload.xr",
            "tests/target-machine/phase0/negative/manifest.toml",
        ),
    ),
    ContractSpec("execution-error-publication.md", ()),
    ContractSpec("runtime-abi-foundation.md", ()),
    ContractSpec(
        "runtime-target-plan-load.md",
        (
            "CMakeLists.txt",
            "include/xray_target_plan_load.h",
            "include/xray_runtime_api.h",
            "src/plan/format/xr_artifact_kind.h",
            "src/plan/format/xr_artifact_kind.c",
            "src/plan/format/xr_xsm_schema.h",
            "src/plan/format/xr_xsm_decode.c",
            "src/plan/semantic/xr_semantic_plan.h",
            "src/plan/semantic/xr_semantic_plan.c",
            "src/plan/semantic/xr_semantic_verify.c",
            "src/plan/format/xr_xtp_schema.h",
            "src/plan/format/xr_xtp_internal.h",
            "src/plan/format/xr_xtp_artifact.c",
            "src/plan/format/xr_xtp_decode.c",
            "src/plan/format/xr_xtp_encode.c",
            "src/plan/format/xr_xtp_instruction_stream.h",
            "src/plan/format/xr_xtp_instruction_stream.c",
            "src/plan/format/xr_xtp_row_fields.h",
            "src/plan/format/xr_xtp_rows.c",
            "src/plan/format/xr_xtp_text.h",
            "src/plan/format/xr_xtp_text.c",
            "xisa/target/xtp_super_ops.def",
            "src/runtime/abi/xr_target_machine_facts.h",
            "src/runtime/abi/xr_runtime_target_authority.h",
            "src/runtime/abi/xr_runtime_target_authority.c",
            "src/runtime/abi/xr_runtime_target_profile.h",
            "src/runtime/abi/xr_runtime_target_profile.c",
            "src/plan/target/xr_target_profile.h",
            "src/plan/target/xr_target_profile.c",
            "src/plan/target/xr_target_entry_abi.h",
            "src/plan/target/xr_target_entry_abi.c",
            "src/plan/target/xr_target_plan.h",
            "src/plan/target/xr_target_plan.c",
            "src/plan/target/xr_target_builder.c",
            "src/plan/target/xr_target_verify.c",
            "scripts/check_coroutine_lifecycle_projection.py",
            "src/plan/target/xr_xtp_materialize.c",
            "src/runtime/xr_runtime_artifact_authority_internal.h",
            "src/runtime/xr_runtime_artifact_authority.c",
            "src/runtime/xr_runtime_artifact_verify.c",
            "src/runtime/xr_target_plan_load.c",
            "src/runtime/xr_runtime_api.c",
            "src/app/cli/xcmd_run.c",
            "contracts/target-machine/legacy-product-residue.json",
            "scripts/check_legacy_product_residue.py",
            "tests/unit/plan/test_target_plan.c",
            "tests/unit/plan/test_xtp_format.c",
            "tests/unit/plan/test_xtp_resource_stress.c",
            "tests/unit/frontend/test_xa_program_semantic_closure.c",
            "tests/unit/CMakeLists.txt",
            "tests/unit/runtime/test_runtime_target_plan_load_archive.c",
            "tests/cli/run_target_artifact_boundary_tests.py",
            "tests/cli/run_plan_command_tests.py",
            "tests/fuzz/fuzz_xtp_decode.c",
            "tests/install/run_installed_runtime_symbol_tests.py",
        ),
    ),
    ContractSpec(
        "typed-target-plan-frame.md",
        (
            "src/plan/target/xr_target_plan.h",
            "src/vm/xr_typed_frame.h",
            "src/vm/xr_typed_frame.c",
            "src/vm/xr_typed_lifecycle.h",
            "src/vm/xr_typed_lifecycle.c",
            "src/vm/audit/xr_typed_lifecycle_audit.h",
            "src/vm/audit/xr_typed_lifecycle_audit.c",
            "src/vm/xr_typed_dispatch.c",
            "src/vm/xr_vm_dynamic_entry.h",
            "scripts/check_coroutine_lifecycle_projection.py",
            "scripts/check_typed_call_staging.py",
            "tests/benchmarks/target-machine/typed_target_vm/benchmark.c",
            "tests/benchmarks/target-machine/typed_target_vm/run.py",
            "tests/unit/vm/test_typed_frame.c",
            "tests/unit/runtime/test_typed_lifecycle_audit.c",
            "tests/unit/runtime/test_typed_frame_runtime_archive.c",
            "tests/unit/runtime/test_runtime_generation.c",
            "tests/unit/runtime/test_dynamic_entry_runtime.c",
        ),
    ),
    ContractSpec(
        "typed-target-plan-opaque-boundary.md",
        (
            "src/plan/target/xr_target_plan.h",
            "src/plan/target/xr_target_builder.c",
            "src/plan/target/xr_target_verify.c",
            "src/runtime/xr_dynamic_entry_runtime.c",
            "src/vm/xr_typed_frame.h",
            "src/vm/xr_typed_frame.c",
            "tests/unit/vm/test_typed_opaque_boundary.c",
            "tests/unit/CMakeLists.txt",
        ),
    ),
    ContractSpec(
        "typed-target-plan-debug.md",
        (
            "src/vm/debug/xr_vm_debug_control.h",
            "src/vm/debug/xr_vm_debug_control_internal.h",
            "src/vm/debug/xr_vm_debug_control.c",
            "src/vm/debug/xr_vm_trace.h",
            "src/vm/debug/xr_vm_trace_internal.h",
            "src/vm/debug/xr_vm_trace.c",
            "src/vm/debug/xr_vm_profile.h",
            "src/vm/debug/xr_vm_profile.c",
            "src/vm/debug/xr_vm_materialize.h",
            "src/vm/debug/xr_vm_materialize.c",
            "src/vm/xr_typed_dispatch.h",
            "src/vm/xr_typed_dispatch.c",
            "tests/unit/vm/test_typed_dispatch.c",
            "tests/unit/runtime/test_dynamic_entry_runtime.c",
            "tests/unit/runtime/test_typed_frame_runtime_archive.c",
            "tests/install/run_installed_runtime_symbol_tests.py",
            "CMakeLists.txt",
        ),
    ),
    ContractSpec("typed-target-plan-execution.md", ()),
    ContractSpec("incremental-cache-store.md", ()),
    ContractSpec("incremental-compiler-session.md", ()),
    ContractSpec(
        "program-semantic-closure.md",
        (
            "CMakeLists.txt",
            "src/module/xmodule_identity.h",
            "src/module/xmodule_identity_view.c",
            "src/module/xmodule_graph.h",
            "src/module/xmodule_graph.c",
            "src/frontend/parser/xparse.c",
            "src/frontend/parser/xparse_decl.c",
            "src/frontend/parser/xparse_import.c",
            "src/frontend/analyzer/xanalyzer.h",
            "src/frontend/analyzer/xanalyzer.c",
            "src/frontend/analyzer/xanalyzer_visitor_call.c",
            "src/frontend/analyzer/xa_typed_program.h",
            "src/frontend/analyzer/xa_typed_program.c",
            "src/frontend/analyzer/xa_scalar_program_authority.h",
            "src/frontend/analyzer/xa_scalar_program_authority_internal.h",
            "src/frontend/analyzer/xa_scalar_program_authority.c",
            "src/frontend/analyzer/xa_scalar_program_authority_verify.c",
            "src/frontend/analyzer/xa_program_semantic_closure.h",
            "src/frontend/analyzer/xa_program_semantic_closure.c",
            "src/plan/semantic/xr_program_semantic_closure.h",
            "src/plan/semantic/xr_program_semantic_closure_internal.h",
            "src/plan/semantic/xr_program_semantic_closure.c",
            "src/plan/semantic/xr_program_semantic_closure_verify.c",
            "src/plan/semantic/xr_source_semantic_identity.h",
            "src/plan/semantic/xr_source_semantic_identity.c",
            "src/plan/semantic/xr_scalar_call_semantics.h",
            "src/plan/semantic/xr_scalar_call_semantics.c",
            "src/plan/format/xr_xsm_decode.c",
            "src/plan/format/xr_xsm_encode.c",
            "src/plan/format/xr_xsm_schema.h",
            "src/plan/semantic/xr_semantic_builder.c",
            "src/plan/semantic/xr_semantic_ids.h",
            "src/plan/semantic/xr_semantic_plan.c",
            "src/plan/semantic/xr_semantic_plan.h",
            "src/plan/semantic/xr_semantic_plan_internal.h",
            "src/plan/semantic/xr_semantic_verify.c",
            "src/plan/target/xr_scalar_call_decision.h",
            "src/plan/target/xr_scalar_call_decision.c",
            "src/plan/target/xr_scalar_call_decision_verify.c",
            "src/ir/xi.h",
            "src/ir/xi.c",
            "src/ir/xi_module.h",
            "src/ir/xi_lower.h",
            "src/ir/xi_lower.c",
            "src/ir/xi_lower_expr.c",
            "src/ir/xi_lower_stmt.c",
            "src/ir/xi_pipeline.c",
            "src/ir/xi_program_semantic.h",
            "src/ir/xi_program_semantic.c",
            "src/ir/xi_program_semantic_verify.c",
            "src/ir/xi_own.c",
            "src/ir/xi_program_semantic_plan.h",
            "src/ir/xi_program_semantic_plan.c",
            "src/aot/emit_c/xr_c_program_emission.h",
            "src/aot/emit_c/xr_c_program_emission.c",
            "src/aot/xaot_boundary.h",
            "src/aot/xaot_boundary.c",
            "src/aot/xaot_bundle.c",
            "tests/unit/plan/test_program_semantic_closure.c",
            "tests/unit/plan/test_scalar_call_decision.c",
            "tests/unit/plan/test_semantic_plan.c",
            "tests/unit/frontend/test_xa_program_semantic_closure.c",
            "tests/unit/frontend/test_parser.c",
            "tests/unit/module/test_module_identity.c",
            "tests/unit/ir/test_xi_program_semantic.c",
            "tests/unit/ir/test_xi_pipeline.c",
            "tests/unit/CMakeLists.txt",
        ),
    ),
    ContractSpec(
        "runtime-generation-lifecycle.md",
        (
            "include/xray_runtime_generation.h",
            "include/xray_runtime_api.h",
            "src/runtime/xr_module_generation_internal.h",
            "src/runtime/xr_module_generation.c",
            "src/runtime/xr_module_generation_verify.c",
            "src/runtime/xr_entry_cell.h",
            "src/runtime/xr_entry_cell.c",
            "src/runtime/xr_dynamic_entry_runtime.h",
            "src/runtime/xr_dynamic_entry_runtime.c",
            "src/runtime/xr_runtime_api.c",
            "src/vm/xr_typed_dispatch.h",
            "src/vm/xr_typed_dispatch.c",
            "src/vm/xr_vm_dynamic_entry.h",
            "src/vm/xr_vm_entry_adapter.h",
            "src/vm/xr_vm_entry_adapter.c",
            "xisa/target/vm_entry_adapters.def",
            "src/vm/xr_vm_decoded_cache.h",
            "src/vm/xr_vm_decoded_cache.c",
            "src/vm/xr_typed_frame.h",
            "src/vm/xr_typed_frame.c",
            "scripts/check_coroutine_lifecycle_projection.py",
            "contracts/target-machine/diagnostic-codes.toml",
            "tests/unit/runtime/test_runtime_generation.c",
            "tests/unit/runtime/test_dynamic_entry_runtime.c",
            "tests/unit/runtime/test_runtime_generation_archive.c",
            "tests/unit/runtime/test_runtime_api_archive.c",
            "tests/unit/runtime/test_entry_cell_runtime_archive.c",
            "tests/unit/vm/test_vm_decoded_cache.c",
            "scripts/target_machine_retired_runtime_symbols.py",
            "tests/install/run_installed_runtime_symbol_tests.py",
            "tests/install/run_install_public_surface_tests.py",
        ),
    ),
    ContractSpec("ownership-audit-foundation.md", ()),
    ContractSpec(
        "canonical-program-execution-binding.md",
        (
            "src/program/xr_program.h",
            "src/program/xr_program_internal.h",
            "src/program/xr_core_ir.c",
            "src/program/xr_program_encode.c",
            "src/program/xr_program_decode.c",
            "src/program/xr_program_schema_gen.h",
            "xisa/program/schema.json",
            "tools/programgen/programgen.py",
            "tests/unit/program/test_xr_program.c",
            "tests/unit/program/xr_program_module_fixture.h",
            "tests/unit/program/xr_program_module_slot_checks.inc.c",
            "tests/unit/program/xr_program_module_operation_checks.inc.c",
            "tests/unit/program/test_xr_program_provider_requirements.c",
            "tests/unit/program/xr_program_provider_fixture.h",
            "contracts/canonical-program/xrprogram-format-v3.md",
            "contracts/canonical-program/xrprogram-format-coverage.json",
            "contracts/canonical-program/xrprogram-semantic-coverage.json",
            "src/execution/xr_stdlib_provider_binding.h",
            "src/execution/xr_stdlib_provider_binding.c",
            "src/execution/xr_stdlib_provider_bindings_gen.inc.c",
            "src/shared/xr_time_offset.h",
            "stdlib/time/time.c",
            "src/os/unix/pipe_unix.c",
            "tests/unit/runtime/test_time_utc_offset.c",
            "tests/unit/execution/test_stdlib_provider_binding.c",
            "src/plan/target/xr_target_profile.h",
            "src/plan/target/xr_target_profile.c",
            "src/plan/target/xr_target_profile_verify.c",
            "src/plan/target/xr_target_verify.c",
            "src/execution/xr_execution.h",
            "src/execution/xr_execution.c",
            "src/execution/xr_execution_identity.h",
            "src/execution/xr_execution_identity.c",
            "src/execution/xr_boundary_materialization.h",
            "src/execution/xr_boundary_materialization.c",
            "src/program/xr_program_verify.h",
            "src/program/xr_program_verify.c",
            "src/program/xr_validated_program_internal.h",
            "src/runtime/abi/xr_runtime_contract.h",
            "src/runtime/abi/xr_runtime_contract.c",
            "src/runtime/class/xinstance.h",
            "tests/unit/plan/test_target_profile.c",
            "tests/unit/execution/test_provider_logical_admission.c",
            "scripts/canonical_program_test_profile.py",
            "tests/lib/tests/test_canonical_program_test_profile.py",
            "tests/unit/execution/test_xr_execution.c",
            "tests/unit/execution/test_xr_boundary_materialization.c",
            "tests/unit/runtime/test_runtime_abi_contract.c",
            "scripts/check_xr_execution_contracts.py",
            "contracts/canonical-program/execution-binding-coverage.json",
        ),
    ),
    ContractSpec("canonical-program-vm.md", ()),
    ContractSpec("canonical-program-aot.md", ()),
)

ANCHOR_RE = re.compile(r"^anchor-sha256:\s+(\S+)\s+([0-9a-f]{64})\s*$")
TEST_RE = re.compile(r"^verification-test: ([A-Za-z0-9_-]+)$")
ASSERTION_FIXTURE = "semantic_contract_assertions"


def assertion_tests(contracts_dir: Path, specs=CONTRACT_SPECS) -> set[str]:
    tests: set[str] = set()
    for spec in specs:
        path = contracts_dir / spec.name
        declared: set[str] = set()
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("anchor-sha256:") and not spec.anchors:
                raise ValueError(f"{path}: retired source digest record")
            if not line.startswith("verification-test:"):
                continue
            match = TEST_RE.fullmatch(line)
            if not match:
                raise ValueError(f"{path}: malformed verification-test record")
            name = match.group(1)
            if name in declared or name.startswith("contract_freeze"):
                raise ValueError(f"{path}: duplicate or recursive verification test {name}")
            declared.add(name)
        if not declared and not spec.anchors:
            raise ValueError(f"{path}: missing verification tests")
        tests.update(declared)
    return tests


def verify_assertion_registration(tests: set[str], inventory: dict) -> None:
    rows = {row["name"]: row for row in inventory["tests"]}
    for name in sorted(tests):
        if name not in rows:
            raise ValueError(f"contract assertion test is not registered: {name}")
        row = rows[name]
        properties = {p["name"]: p["value"] for p in row.get("properties", [])}
        if not row.get("command") or properties.get("DISABLED"):
            raise ValueError(f"contract assertion test cannot execute: {name}")
        if any(key in properties for key in ("SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION")):
            raise ValueError(f"contract assertion test permits a skipped result: {name}")
        if ASSERTION_FIXTURE not in properties.get("FIXTURES_SETUP", []):
            raise ValueError(f"contract assertion test does not guard freeze: {name}")
    freeze = rows.get("contract_freeze", {})
    properties = {p["name"]: p["value"] for p in freeze.get("properties", [])}
    if ASSERTION_FIXTURE not in properties.get("FIXTURES_REQUIRED", []):
        raise ValueError("contract freeze does not require its assertion tests")


def digest(path: Path) -> str:
    # Contract anchors describe repository content, whose canonical Git form
    # uses LF. Normalize checkout-only CRLF so the gate has the same result on
    # Windows and Unix hosts while preserving every other byte.
    content = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(content).hexdigest()


def parse_contract(path: Path) -> tuple[dict[str, str], list[str]]:
    anchors: dict[str, str] = {}
    errors: list[str] = []
    if not path.is_file():
        return anchors, [f"missing contract: {path}"]
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("anchor-sha256:"):
            continue
        match = ANCHOR_RE.match(line)
        if not match:
            errors.append(f"{path}:{lineno}: malformed anchor-sha256 record")
            continue
        anchor, expected = match.groups()
        if anchor in anchors:
            errors.append(f"{path}:{lineno}: duplicate anchor {anchor}")
        anchors[anchor] = expected
    return anchors, errors


def verify_digests(root: Path, contracts_dir: Path, specs=CONTRACT_SPECS) -> list[str]:
    errors: list[str] = []
    for spec in specs:
        if not spec.anchors:
            continue
        path = contracts_dir / spec.name
        if len(set(spec.anchors)) != len(spec.anchors):
            errors.append(f"{path}: duplicate registered anchors")
            continue
        recorded, parse_errors = parse_contract(path)
        errors.extend(parse_errors)
        expected_anchors = set(spec.anchors)
        if set(recorded) != expected_anchors:
            missing = sorted(expected_anchors - set(recorded))
            extra = sorted(set(recorded) - expected_anchors)
            if missing:
                errors.append(f"{path}: missing anchors: {', '.join(missing)}")
            if extra:
                errors.append(f"{path}: unregistered anchors: {', '.join(extra)}")
        for anchor in spec.anchors:
            source = root / anchor
            if not source.is_file():
                errors.append(f"{path}: anchor does not exist: {anchor}")
                continue
            actual = digest(source)
            if recorded.get(anchor) != actual:
                errors.append(
                    f"{path}: digest drift for {anchor}: "
                    f"recorded={recorded.get(anchor, '<missing>')} actual={actual}"
                )
    return errors


def refresh_digests(root: Path, contracts_dir: Path, specs=CONTRACT_SPECS) -> list[str]:
    errors: list[str] = []
    for spec in specs:
        if not spec.anchors:
            continue
        path = contracts_dir / spec.name
        if len(set(spec.anchors)) != len(spec.anchors):
            errors.append(f"{path}: duplicate registered anchors")
            continue
        recorded, parse_errors = parse_contract(path)
        errors.extend(parse_errors)
        if parse_errors:
            continue
        expected_anchors = set(spec.anchors)
        if set(recorded) != expected_anchors:
            missing = sorted(expected_anchors - set(recorded))
            extra = sorted(set(recorded) - expected_anchors)
            if missing:
                errors.append(f"{path}: missing anchors: {', '.join(missing)}")
            if extra:
                errors.append(f"{path}: unregistered anchors: {', '.join(extra)}")
            continue

        replacements: dict[str, str] = {}
        for anchor in spec.anchors:
            source = root / anchor
            if not source.is_file():
                errors.append(f"{path}: anchor does not exist: {anchor}")
                continue
            replacements[anchor] = digest(source)
        if len(replacements) != len(spec.anchors):
            continue

        lines = path.read_text(encoding="utf-8").splitlines()
        refreshed: list[str] = []
        for line in lines:
            match = ANCHOR_RE.match(line)
            if match:
                anchor, _ = match.groups()
                line = f"anchor-sha256: {anchor} {replacements[anchor]}"
            refreshed.append(line)
        path.write_text("\n".join(refreshed) + "\n", encoding="utf-8")
    return errors


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="xray-contract-freeze-") as tmp:
        root = Path(tmp)
        (root / "contracts").mkdir()
        (root / "src").mkdir()
        anchor = root / "src" / "truth.def"
        anchor.write_text("v1\n", encoding="utf-8")
        spec = ContractSpec("sample.md", ("src/truth.def",))
        contract = root / "contracts" / spec.name
        contract.write_text(
            f"# Sample\n\nanchor-sha256: src/truth.def {digest(anchor)}\n", encoding="utf-8"
        )
        assert verify_digests(root, root / "contracts", (spec,)) == []
        duplicate = ContractSpec("sample.md", spec.anchors * 2)
        before = contract.read_bytes()
        assert verify_digests(root, root / "contracts", (duplicate,))
        assert refresh_digests(root, root / "contracts", (duplicate,))
        assert contract.read_bytes() == before
        anchor.write_bytes(b"v1\r\n")
        assert verify_digests(root, root / "contracts", (spec,)) == []
        anchor.write_text("v2\n", encoding="utf-8")
        assert any("digest drift" in error for error in verify_digests(root, root / "contracts", (spec,)))
        assert refresh_digests(root, root / "contracts", (spec,)) == []
        assert verify_digests(root, root / "contracts", (spec,)) == []
        assert assertion_tests(root / "contracts", (spec,)) == set()
        contract.write_text(contract.read_text(encoding="utf-8") +
                            "verification-test: sample_behavior\n", encoding="utf-8")
        assert assertion_tests(root / "contracts", (spec,)) == {"sample_behavior"}
        assert verify_digests(root, root / "contracts", (spec,)) == []
        anchor.write_text("pending migration changed\n", encoding="utf-8")
        assert any("digest drift" in error for error in verify_digests(root, root / "contracts", (spec,)))
        contract.write_text(contract.read_text(encoding="utf-8") +
                            "verification-test: sample_behavior\n", encoding="utf-8")
        try:
            assertion_tests(root / "contracts", (spec,))
        except ValueError:
            pass
        else:
            raise AssertionError("accepted duplicate assertion in anchored contract")
        migrated = ContractSpec("sample.md", ())
        contract.write_text("verification-test: sample_behavior\n", encoding="utf-8")
        assert assertion_tests(root / "contracts", (migrated,)) == {"sample_behavior"}
        anchor.write_text("private implementation changed\n", encoding="utf-8")
        assert verify_digests(root, root / "contracts", (migrated,)) == []
        inventory = {"tests": [
            {"name": "sample_behavior", "command": ["sample"], "properties": [
                {"name": "FIXTURES_SETUP", "value": [ASSERTION_FIXTURE]}]},
            {"name": "contract_freeze", "properties": [
                {"name": "FIXTURES_REQUIRED", "value": [ASSERTION_FIXTURE]}]},
        ]}
        verify_assertion_registration({"sample_behavior"}, inventory)
        for mutation in ("missing", "disabled", "skip", "unbound", "missing-requirement"):
            bad = json.loads(json.dumps(inventory))
            if mutation == "missing":
                bad["tests"].pop(0)
            elif mutation == "disabled":
                bad["tests"][0]["properties"].append({"name": "DISABLED", "value": True})
            elif mutation == "skip":
                bad["tests"][0]["properties"].append({"name": "SKIP_RETURN_CODE", "value": 77})
            elif mutation == "unbound":
                bad["tests"][0]["properties"] = []
            else:
                bad["tests"][1]["properties"] = []
            try:
                verify_assertion_registration({"sample_behavior"}, bad)
            except ValueError:
                pass
            else:
                raise AssertionError(f"accepted {mutation} assertion registration")
        for text in ("", "verification-test: sample_behavior\n" * 2,
                     "verification-test: contract_freeze\n", "verification-test: bad name\n",
                     "anchor-sha256: retired\nverification-test: sample_behavior\n"):
            contract.write_text(text, encoding="utf-8")
            try:
                assertion_tests(root / "contracts", (migrated,))
            except ValueError:
                pass
            else:
                raise AssertionError("accepted invalid assertion contract")
    print("contract verification injection self-test: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--contracts-dir", default="contracts", help="contract directory")
    parser.add_argument("--build-dir", default="build", help="configured CTest build")
    parser.add_argument("--list-tests", action="store_true", help="emit required assertion tests")
    parser.add_argument(
        "--refresh",
        action="store_true",
        help="refresh digests after requiring the registered anchor sets to match exactly",
    )
    parser.add_argument("--self-test", action="store_true", help="run injected drift checks")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = Path(args.root).resolve()
    contracts_dir = (root / args.contracts_dir).resolve()
    try:
        tests = assertion_tests(contracts_dir)
        if args.list_tests:
            print("\n".join(sorted(tests)))
            return 0
        if not args.refresh:
            inventory = json.loads(subprocess.check_output(
                ["ctest", "--test-dir", str(root / args.build_dir), "--show-only=json-v1"],
                encoding="utf-8", errors="strict"))
            verify_assertion_registration(tests, inventory)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"contract verification failed: {exc}", file=sys.stderr)
        return 1
    if args.refresh:
        errors = refresh_digests(root, contracts_dir)
        if errors:
            print("task-220 contract freeze refresh failed:", file=sys.stderr)
            for error in errors:
                print(f"  - {error}", file=sys.stderr)
            return 1
        remaining = sum(bool(spec.anchors) for spec in CONTRACT_SPECS)
        print(f"contract freeze: REFRESHED ({remaining} remaining digest contracts)")
        return 0
    errors = verify_digests(root, contracts_dir)
    if errors:
        print("task-220 contract freeze gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    migrated = sum(not spec.anchors for spec in CONTRACT_SPECS)
    print(f"contract registration and remaining digests: PASS "
          f"({migrated} assertion contracts, {len(CONTRACT_SPECS) - migrated} digest contracts; "
          "assertion execution is enforced by CTest)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
