#!/usr/bin/env python3
"""Inspect legacy consumer obligations without running or qualifying Xray."""
from __future__ import annotations

import argparse
import collections
import dataclasses
import functools
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import subprocess

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


# Explicit reviewed transitions prevent old negative oracles becoming target truth.
FROZEN_TARGETS = {}
FROZEN_TARGETS['tests/diff/cases/semantics/optimizer/known_empty_call_result_is_null.xr'] = ('POSITIVE_COMPLETE_EMPTY_CALL_UNIT_IDENTITY_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/stdlib/ws_pure_protocol_direct.xr'] = ('POSITIVE_COMPLETE_WEBSOCKET_EXPLICIT_UTF8_PORT', 'P13')
FROZEN_TARGETS['tests/diff/cases/semantics/collections/map_bool_key.xr'] = ('POSITIVE_CLASS_IDENTITY_METHOD_AFTER_FROZEN_READ_RECEIVER_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr'] = ('POSITIVE_COMPLETE_EXPLICIT_SCALAR_TEXT_AFTER_HETEROGENEOUS_ARRAY_RETIREMENT', 'P13')
FROZEN_TARGETS['tests/diff/cases/semantics/slice/array_negative_both.xr'] = ('POSITIVE_BORROWED_VIEW_PERMISSION_AND_EXPLICIT_OWNED_DISPLAY', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/slice/array_negative_end.xr'] = ('POSITIVE_BORROWED_VIEW_PERMISSION_AND_EXPLICIT_OWNED_DISPLAY', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/slice/array_negative_start.xr'] = ('POSITIVE_BORROWED_VIEW_PERMISSION_AND_EXPLICIT_OWNED_DISPLAY', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/collections/slice_window_shared_core.xr'] = ('POSITIVE_BORROWED_VIEW_PERMISSION_AND_EXPLICIT_OWNED_DISPLAY', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/collections/method_self_return_ownership.xr'] = ('POSITIVE_OWNED_STRING_ACCUMULATION_AFTER_STRINGBUILDER_RETIREMENT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/collections/stringbuilder_append_shared_core.xr'] = ('POSITIVE_OWNED_STRING_ACCUMULATION_AFTER_STRINGBUILDER_RETIREMENT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/collections/stringbuilder_scalar_known_append_shared_core.xr'] = ('POSITIVE_OWNED_STRING_ACCUMULATION_AFTER_STRINGBUILDER_RETIREMENT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/int/enum_ordinal_conversion.xr'] = ('POSITIVE_EXPLICIT_ENUM_ORDINAL_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/string/multiconcat.xr'] = ('POSITIVE_EXACT_NULLABLE_CONTEXT_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/extern_layout_bytecode_roundtrip.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/mem_view_extern_layout.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/mem_view_extern_layout_native_size.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/mem_view_imported_layout_native_size.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/mem_view_packed_nested_native_size.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/mem_with_slice_mut_exception_loan_end.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/ffi/mem_with_slice_mut_extern_layout.xr'] = ('POSITIVE_FOREIGN_MUTABLE_VIEW_PERMISSION_PORT', 'P09')
FROZEN_TARGETS['tests/diff/cases/semantics/modules/_classlib.xr'] = ('POSITIVE_ORDINARY_CLASS_RECEIVER_MODULE_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/modules/xmod_class.xr'] = ('POSITIVE_ORDINARY_CLASS_RECEIVER_MODULE_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/modules/_exported_struct_methods_lib.xr'] = ('POSITIVE_NAMED_STRUCT_FACTORY_MODULE_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/modules/exported_struct_methods.xr'] = ('POSITIVE_NAMED_STRUCT_FACTORY_MODULE_PORT', 'P10')
FROZEN_TARGETS['tests/diff/cases/semantics/cleanup/defer_late_binding.xr'] = ('POSITIVE_VALUE_SNAPSHOT_BINDING_PORT', 'P07')
FROZEN_TARGETS['tests/diff/cases/semantics/functions/bare_lambda_expression_positions.xr'] = ('POSITIVE_EXPLICIT_CALLABLE_ARRAY_FILL_PORT', 'P10')
FROZEN_TARGETS.update({'tests/diff/cases/semantics/ownership/auto_borrow_class_alias_return_downgrade.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/auto_borrow_class_readonly_param.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/converged_capability_positive.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/copy_deep_independent.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/copy_preserves_type.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_alias_reassigned_copy_escape_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_closure_copy_capture_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_forward_function_param_copy_escape_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_function_param_copy_escape_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_function_param_method_copy_escape_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_go_copy_argument_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_method_wrapper_param_copy_escape_allowed.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_readonly_user_method_names.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/in_ref_heap_borrows.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/move_into_go.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/move_local_heap.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/move_local_in_fn.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/shared_class_copy_go.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/diff/cases/semantics/ownership/shared_freeze_go.xr': ('POSITIVE_VALUE_IDENTITY_OWNERSHIP_AFTER_FROZEN_TASK313_PORT', 'P10')})
FROZEN_TARGETS.update({'tests/diff/cases/semantics/int_wrap/stringbuilder_numeric_shared_core.xr': ('POSITIVE_STRING_APPEND_AFTER_FROZEN_TASK317_PORT', 'P13'), 'tests/diff/cases/semantics/string/string_indexof_start_shared_core.xr': ('POSITIVE_INDEXOF_BOUNDS_AFTER_FROZEN_TASK323_PORT', 'P09'), 'tests/diff/cases/basic/arrays.xr': ('POSITIVE_EXPLICIT_MUTSLICE_AFTER_FROZEN_TASK313_PORT', 'P10')})
FROZEN_TARGETS.update({'tests/compile_errors/ownership/002_shared_bare_local_heap.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/012_ref_rejects_duplicate_in_arg.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/013_ref_rejects_in_field_projection.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/036_ref_return_escape.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/037_ref_field_store_escape.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/038_ref_array_push_escape.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/149_in_rejects_ref_alias_arg.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/181_defer_cleanup_blocks_return.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/188_ref_rejects_alias_root_arg.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/189_ref_rejects_receiver_overlap.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/ownership/closure_capture_blocks_rebind.xr': ('POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT', 'P10'), 'tests/compile_errors/type/array_get_unchecked_requires_unsafe.xr': ('POSITIVE_CHECKED_ARRAY_METHOD_AFTER_FROZEN_TASK317_PORT', 'P09'), 'tests/compile_errors/type/array_set_unchecked_requires_unsafe.xr': ('POSITIVE_CHECKED_ARRAY_METHOD_AFTER_FROZEN_TASK317_PORT', 'P09')})
for name in ("001_comptime_non_const_call.xr", "012_comptime_fixed_array_element_call.xr",
             "014_comptime_struct_field_call.xr", "016_comptime_member_access_call.xr",
             "028_comptime_block_return_call.xr"):
    FROZEN_TARGETS["tests/compile_errors/binding/" + name] = ("POSITIVE_REQUIRED_BY_FROZEN_TASK320", "P11")
for name in ("007_in_instance_mutating_method.xr", "008_in_instance_indirect_mutating_method.xr",
             "009_in_instance_inherited_mutating_method.xr"):
    FROZEN_TARGETS["tests/compile_errors/ownership/" + name] = ("POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT", "P10")
for name in ("generator_await_rejected.xr", "generator_coro_yield_rejected.xr",
             "generator_defer_rejected.xr", "generator_dynamic_call_rejected.xr",
             "generator_scope_rejected.xr", "generator_transitive_suspend_rejected.xr"):
    FROZEN_TARGETS["tests/compile_errors/concurrency/" + name] = ("POSITIVE_REQUIRED_BY_FROZEN_TASK316", "P08")
FROZEN_TARGETS["tests/compile_errors/oop/020_enum_implements_rejected.xr"] = ("REJECTION_FOR_MISSING_INTERFACE_REQUIREMENT", "P10")


for name in ("024_struct_dynamic_container_field.xr", "084_const_object_field_store_rejected.xr",
             "085_const_array_alias_mutating_method.xr", "086_const_map_alias_index_store_rejected.xr",
             "087_const_object_alias_field_store_rejected.xr", "088_const_array_mutable_alias_init_rejected.xr",
             "089_const_map_mutable_alias_init_rejected.xr", "090_const_object_mutable_alias_init_rejected.xr",
             "099_shared_object_field_store_rejected.xr", "100_shared_object_compound_field_store_rejected.xr",
             "105_shared_member_array_alias_index_store_rejected.xr", "106_shared_member_array_alias_mutating_method_rejected.xr",
             "109_shared_member_array_alias_mutating_param_rejected.xr", "118_shared_derived_closure_capture_mutation_rejected.xr"):
    FROZEN_TARGETS["tests/compile_errors/type/" + name] = ("POSITIVE_VALUE_OR_IDENTITY_AFTER_FROZEN_TASK313_PORT", "P10")
for name in ("union_indiscriminable_int_members.xr", "union_indiscriminable_float_members.xr", "union_indiscriminable_via_alias.xr"):
    FROZEN_TARGETS["tests/compile_errors/type/" + name] = ("POSITIVE_EXACT_UNION_MEMBER_AFTER_FROZEN_TASK319_PORT", "P10")
FROZEN_TARGETS["tests/compile_errors/type/span_tuple_literal_escape.xr"] = ("POSITIVE_LOCAL_VIEW_TUPLE_AFTER_FROZEN_TASK313_PORT", "P10")
FROZEN_TARGETS["tests/compile_errors/ownership/080_transfer_task_second_await_rejected.xr"] = (
    "POSITIVE_STICKY_TASK_RESULT_REQUIRED_BY_FROZEN_TASK316", "P08")
FROZEN_TARGETS["tests/diff/cases/semantics/coro/task_transfer_already_taken.xr"] = (
    "POSITIVE_STICKY_TASK_RESULT_AFTER_FROZEN_TASK316_PORT", "P08")
FROZEN_TARGETS["tests/diff/cases/semantics/coro/task_transfer_return_native_class.xr"] = (
    "REJECT_NONSENDABLE_TASK_RESULT_AFTER_FROZEN_TASK313_PORT", "P10")
for name in ("array_resize_reserve_shared_core.xr", "array_typed_fill_storage_shared_core.xr"):
    FROZEN_TARGETS["tests/diff/cases/semantics/collections/" + name] = (
        "POSITIVE_STRICT_ARRAY_RANGE_AFTER_FROZEN_TASK313_PORT", "P09")
for name in ("array_expression_view.xr", "ref_range_place.xr", "slice_borrow_return_contracts.xr",
             "slice_borrow_return_cross_module.xr", "_borrow_return_lib.xr"):
    FROZEN_TARGETS["tests/diff/cases/semantics/slice/" + name] = (
        "POSITIVE_EXPLICIT_VIEW_PERMISSION_AFTER_FROZEN_TASK313_PORT", "P10")
FROZEN_TARGETS["tests/diff/cases/semantics/evaluation_order/e5_e6_assignment_order.xr"] = (
    "POSITIVE_WRITABLE_ARRAY_PLACE_AFTER_FROZEN_TASK313_PORT", "P10")
for path in (
    "tests/regression/12_type_checking/1206_generic_functions.xr",
    "tests/regression/13_types/1437_generic_method_param_contract.xr",
):
    FROZEN_TARGETS[path] = ("POSITIVE_DEFINED_EQUAL_PROOF_AFTER_FROZEN_TASK313_PORT", "P10")
for path in (
    "tests/regression/12_type_checking/1223_generic_channel.xr",
    "tests/regression/13_types/1433_param_mode_move_source_action.xr",
    "tests/regression/13_types/1453_closure_contextual_param_mode_writeback.xr",
    "tests/regression/13_types/1314_json_instance_methods.xr",
):
    FROZEN_TARGETS[path] = ("POSITIVE_LOGICAL_VALUE_TRANSFER_AFTER_FROZEN_TASK313_PORT", "P10")
FROZEN_TARGETS["tests/regression/13_types/1407_fixed_array_repeat_value_lanes_and_borrow.xr"] = (
    "POSITIVE_EXCLUSIVE_FIXED_ARRAY_VIEW_AFTER_FROZEN_TASK313_PORT", "P10")


FROZEN_TARGETS["tests/diff/cases/semantics/slice/slice_single_source_return.xr"] = (
    "POSITIVE_EXPLICIT_VIEW_PERMISSION_AFTER_FROZEN_TASK313_PORT", "P10")


for name in ("base64_isvalid_direct", "base64_module", "encoding_core_direct", "encoding_module", "compress_roundtrip_direct"):
    FROZEN_TARGETS["tests/diff/cases/semantics/stdlib/" + name + ".xr"] = (
        "POSITIVE_NONOPTIONAL_UTF8_DECLARATION_AFTER_FROZEN_PORT", "P10")
FROZEN_TARGETS["tests/diff/cases/semantics/stdlib/csv_pure_module_direct.xr"] = (
    "POSITIVE_EXACT_DECLARED_CSV_PAYLOAD_AFTER_FROZEN_PORT", "P10")
FROZEN_TARGETS["tests/diff/cases/semantics/stdlib/http_error_nested_enum_payload_direct.xr"] = (
    "POSITIVE_EXACT_TYPED_ERROR_PAYLOAD_AFTER_FROZEN_PORT", "P10")

# The demanded imported diagnostics depend on these complete companion inputs.
IMPORTED_ORACLE_INPUTS = {}
for name in ("156_imported_ref_call_requires_marker.xr", "158_imported_const_function_value_ref_call_requires_marker.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/ownership/" + name] = ("tests/fixtures/param_mode_import_lib.xr",)
for name in ("sys_thread_spawn_imported_function_value_suspendable_rejected.xr",
             "sys_thread_spawn_imported_suspendable_call_rejected.xr",
             "sys_thread_spawn_namespace_suspendable_call_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/concurrency/" + name] = ("tests/fixtures/thread_spawn_suspendable_lib.xr",)
for name in ("parallel_callback_imported_function_value_suspendable_rejected.xr",
             "parallel_callback_imported_suspendable_call_rejected.xr",
             "parallel_callback_namespace_suspendable_call_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/stdlib/" + name] = ("tests/fixtures/thread_spawn_suspendable_lib.xr",)
for name in ("parallel_callback_selective_xrd_unknown_effect_rejected.xr",
             "parallel_callback_xrd_handle_method_unknown_effect_rejected.xr",
             "parallel_callback_xrd_unknown_effect_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/stdlib/" + name] = ("tests/compile_errors/stdlib/parallel_unknown_effect_native.xrd",)


for name in ("113_shared_derived_imported_function_value_mutating_param_rejected.xr",
             "114_shared_derived_namespace_function_value_mutating_param_rejected.xr"):
    IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/" + name] = ("tests/fixtures/shared_mutation_lib.xr",)
IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/115_shared_derived_reexported_function_value_mutating_param_rejected.xr"] = (
    "tests/fixtures/shared_mutation_reexport.xr", "tests/fixtures/shared_mutation_lib.xr")
IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/116_shared_derived_star_reexported_function_value_mutating_param_rejected.xr"] = (
    "tests/fixtures/shared_mutation_reexport_all.xr", "tests/fixtures/shared_mutation_lib.xr")
IMPORTED_ORACLE_INPUTS["tests/compile_errors/type/slice_active_view_cross_module_relocation.xr"] = (
    "tests/fixtures/slice/_slice_memory_effect_lib.xr",)
for name, companion in (
    ("1149_module_call_stack_grow.xr", "modules/big_stack.xr"),
    ("1170_value_tag_is_the_type_answer.xr", "_value_tag_lib.xr"),
    ("1172_module_readonly_capture.xr", "modules/module_readonly_capture.xr"),
):
    IMPORTED_ORACLE_INPUTS["tests/regression/11_coroutine/" + name] = (
        "tests/regression/11_coroutine/" + companion,)
IMPORTED_ORACLE_INPUTS["tests/diff/cases/semantics/collections/array_fill_ref_cross_module.xr"] = (
    "tests/diff/cases/semantics/collections/_array_fill_ref_lib.xr",)
IMPORTED_ORACLE_INPUTS["tests/diff/cases/semantics/slice/slice_borrow_return_cross_module.xr"] = (
    "tests/diff/cases/semantics/slice/_borrow_return_lib.xr",)
IMPORTED_ORACLE_INPUTS["tests/regression/13_types/1436_param_mode_imported_contracts.xr"] = (
    "tests/fixtures/param_mode_import_lib.xr",)
IMPORTED_ORACLE_INPUTS["tests/diff/cases/semantics/stdlib/simd_intrinsic_identity_alias.xr"] = (
    "tests/diff/cases/semantics/stdlib/_simd_intrinsic_reexport.xr",)

IMPORTED_ORACLE_INPUTS["tests/diff/cases/semantics/stdlib/probe_module_shapes.xr"] = (
    "stdlib/_probe/_probe.xr",)

TARGET_PORT_ARGUMENTS = {
    "tests/diff/cases/semantics/stdlib/process_schema_shared_core.xr": ["alpha", "beta"],
}

TARGET_PORT_IMPORTS = {
    'tests/diff/cases/semantics/collections/map_bool_key.xr': (),
    'tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr': (),
    'tests/diff/cases/semantics/slice/array_negative_both.xr': (),
    'tests/diff/cases/semantics/slice/array_negative_end.xr': (),
    'tests/diff/cases/semantics/slice/array_negative_start.xr': (),
    'tests/diff/cases/semantics/collections/slice_window_shared_core.xr': (),
    'tests/diff/cases/semantics/collections/method_self_return_ownership.xr': (),
    'tests/diff/cases/semantics/collections/stringbuilder_append_shared_core.xr': (),
    'tests/diff/cases/semantics/collections/stringbuilder_scalar_known_append_shared_core.xr': (),
    'tests/diff/cases/semantics/int/enum_ordinal_conversion.xr': (),
    'tests/diff/cases/semantics/modules/_classlib.xr': (),
    'tests/diff/cases/semantics/modules/xmod_class.xr': ('tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/_classlib.xr',),
    'tests/diff/cases/semantics/modules/_exported_struct_methods_lib.xr': (),
    'tests/diff/cases/semantics/modules/exported_struct_methods.xr': ('tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/_exported_struct_methods_lib.xr',),
    "tests/diff/cases/semantics/slice/slice_borrow_return_cross_module.xr": (
        "tests/unit/xir/legacy_consumers/fixtures/slice_borrow_cross_module/_borrow_return_lib.xr",)
}


TARGET_PORT_IMPORTS.update({
    'tests/diff/cases/semantics/ffi/extern_layout_bytecode_roundtrip.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/extern_layout_bytecode_roundtrip/xray.toml',
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/extern_layout_bytecode_roundtrip/_extern_layout_bytecode_lib.xr'),
    'tests/diff/cases/semantics/ffi/mem_view_extern_layout.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_extern_layout/xray.toml',),
    'tests/diff/cases/semantics/ffi/mem_view_extern_layout_native_size.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_extern_layout_native_size/xray.toml',),
    'tests/diff/cases/semantics/ffi/mem_view_imported_layout_native_size.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_imported_layout_native_size/xray.toml',
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_imported_layout_native_size/_native_size_layout_lib.xr'),
    'tests/diff/cases/semantics/ffi/mem_view_packed_nested_native_size.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_packed_nested_native_size/xray.toml',),
    'tests/diff/cases/semantics/ffi/mem_with_slice_mut_exception_loan_end.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_with_slice_mut_exception_loan_end/xray.toml',),
    'tests/diff/cases/semantics/ffi/mem_with_slice_mut_extern_layout.xr': (
        'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_with_slice_mut_extern_layout/xray.toml',),
})


# Complete reviewed regression module closures and frozen full source drafts.
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1031_module_scope.xr'] = ('tests/regression/10_stdlib/modules/mod_a.xr', 'tests/regression/10_stdlib/modules/mod_b.xr')
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1032_reexport.xr'] = ('tests/regression/10_stdlib/reexport_test/index.xr', 'tests/regression/10_stdlib/reexport_test/user.xr', 'tests/regression/10_stdlib/reexport_test/product.xr')
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1033_selective_import_nested.xr'] = ('tests/regression/10_stdlib/modules/sel_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1034_cross_module_typed.xr'] = ('tests/regression/10_stdlib/modules/typed_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1035_cross_module_shared_method.xr'] = ('tests/regression/10_stdlib/modules/shared_class_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1035_namespace_import_typed.xr'] = ('tests/regression/10_stdlib/modules/typed_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1036_cross_module_generic_enum.xr'] = ('tests/regression/10_stdlib/modules/generic_result_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1036_module_regex_init.xr'] = ('tests/regression/10_stdlib/modules/regex_compile_mod.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1037_cross_module_owned_transfer_summary.xr'] = ('tests/fixtures/owned_transfer_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1038_cross_module_return_ownership_summary.xr'] = ('tests/fixtures/return_ownership_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1039_cross_module_generic_annotation.xr'] = ('tests/regression/10_stdlib/modules/generic_box_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1040_selective_import_generic_return.xr'] = ('tests/regression/10_stdlib/modules/generic_box_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/10_stdlib/1041_imported_instance_field_substitution.xr'] = ('tests/regression/10_stdlib/modules/generic_box_lib.xr',)
TARGET_PORT_IMPORTS['tests/regression/10_stdlib/1033_selective_import_nested.xr'] = ('tests/unit/xir/legacy_consumers/fixtures/regression_library_ports/1033_selective_import_nested/modules/sel_lib.xr',)
TARGET_PORT_IMPORTS['tests/regression/10_stdlib/1034_cross_module_typed.xr'] = ('tests/unit/xir/legacy_consumers/fixtures/regression_library_ports/1034_cross_module_typed/modules/typed_lib.xr',)
TARGET_PORT_IMPORTS['tests/regression/10_stdlib/1035_namespace_import_typed.xr'] = ('tests/unit/xir/legacy_consumers/fixtures/regression_library_ports/1035_namespace_import_typed/modules/typed_lib.xr',)
TARGET_PORT_IMPORTS['tests/regression/10_stdlib/1035_cross_module_shared_method.xr'] = ('tests/unit/xir/legacy_consumers/fixtures/regression_library_ports/1035_cross_module_shared_method/modules/shared_class_lib.xr',)
TARGET_PORT_IMPORTS['tests/regression/10_stdlib/1037_cross_module_owned_transfer_summary.xr'] = ('tests/unit/xir/legacy_consumers/fixtures/regression_library_ports/1037_cross_module_owned_transfer_summary/owned_transfer_lib.xr',)
TARGET_PORT_IMPORTS['tests/regression/10_stdlib/1038_cross_module_return_ownership_summary.xr'] = ('tests/unit/xir/legacy_consumers/fixtures/regression_library_ports/1038_cross_module_return_ownership_summary/return_ownership_lib.xr',)
FROZEN_TARGETS['tests/regression/10_stdlib/1033_selective_import_nested.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1034_cross_module_typed.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1035_namespace_import_typed.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1035_cross_module_shared_method.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1037_cross_module_owned_transfer_summary.xr'] = ('POSITIVE_LOGICAL_VALUE_TRANSFER_AFTER_FROZEN_TASK313_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1038_cross_module_return_ownership_summary.xr'] = ('POSITIVE_LOGICAL_VALUE_TRANSFER_AFTER_FROZEN_TASK313_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1150_string_byte_ops.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1180_base64_basic.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1300_encoding.xr'] = ('POSITIVE_DECLARATION_OWNED_LIBRARY_AFTER_FROZEN_PORT', 'P10')


FROZEN_TARGETS['tests/regression/10_stdlib/1010_time.xr'] = ('POSITIVE_REAL_TIMER_ELAPSED_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1011_time_edge.xr'] = ('POSITIVE_REAL_TIMER_ELAPSED_AFTER_DECLARED_PORT', 'P13')

REGRESSION_LIBRARY_REVIEW = ('tests/regression/10_stdlib/1010_json_serialize_types.xr', 'tests/regression/10_stdlib/1010_time.xr', 'tests/regression/10_stdlib/1011_time_edge.xr', 'tests/regression/10_stdlib/1020_math.xr', 'tests/regression/10_stdlib/1021_math_advanced.xr', 'tests/regression/10_stdlib/1022_math_type_preserve.xr', 'tests/regression/10_stdlib/1023_math_new_functions.xr', 'tests/regression/10_stdlib/1030_module_import.xr', 'tests/regression/10_stdlib/1031_module_scope.xr', 'tests/regression/10_stdlib/1032_reexport.xr', 'tests/regression/10_stdlib/1033_selective_import_nested.xr', 'tests/regression/10_stdlib/1034_cross_module_typed.xr', 'tests/regression/10_stdlib/1035_cross_module_shared_method.xr', 'tests/regression/10_stdlib/1035_import_scope_reject.xr', 'tests/regression/10_stdlib/1035_namespace_import_typed.xr', 'tests/regression/10_stdlib/1036_cross_module_generic_enum.xr', 'tests/regression/10_stdlib/1036_module_regex_init.xr', 'tests/regression/10_stdlib/1037_cross_module_owned_transfer_summary.xr', 'tests/regression/10_stdlib/1038_cross_module_return_ownership_summary.xr', 'tests/regression/10_stdlib/1039_cross_module_generic_annotation.xr', 'tests/regression/10_stdlib/1040_selective_import_generic_return.xr', 'tests/regression/10_stdlib/1041_imported_instance_field_substitution.xr', 'tests/regression/10_stdlib/1060_simd_intrinsic_same_name.xr', 'tests/regression/10_stdlib/1061_simd_runtime_selected_vm.xr', 'tests/regression/10_stdlib/1100_regex.xr', 'tests/regression/10_stdlib/1101_regex_engine.xr', 'tests/regression/10_stdlib/1110_regex_unicode.xr', 'tests/regression/10_stdlib/1115_regex_advanced.xr', 'tests/regression/10_stdlib/1120_regex_re2_compat.xr', 'tests/regression/10_stdlib/1121_regex_jit_inline_call_args.xr', 'tests/regression/10_stdlib/1122_regex_jit_dynamic_null_index.xr', 'tests/regression/10_stdlib/1130_regex_literal.xr', 'tests/regression/10_stdlib/1131_regex_source_field_ownership.xr', 'tests/regression/10_stdlib/1150_string_byte_ops.xr', 'tests/regression/10_stdlib/1151_string_translate.xr', 'tests/regression/10_stdlib/1152_string_slice.xr', 'tests/regression/10_stdlib/1170_path_basic.xr', 'tests/regression/10_stdlib/1180_base64_basic.xr', 'tests/regression/10_stdlib/1181_binary_codec_properties.xr', 'tests/regression/10_stdlib/1300_encoding.xr')

REGRESSION_LIBRARY_REVIEW += ('tests/regression/10_stdlib/1301_compress.xr', 'tests/regression/10_stdlib/1302_yaml_basic.xr', 'tests/regression/10_stdlib/1303_yaml_nested_flow.xr', 'tests/regression/10_stdlib/1304_yaml_advanced.xr', 'tests/regression/10_stdlib/1305_csv_basic.xr', 'tests/regression/10_stdlib/1306_toml_basic.xr', 'tests/regression/10_stdlib/1307_toml_advanced.xr', 'tests/regression/10_stdlib/1308_xml_basic.xr', 'tests/regression/10_stdlib/1309_xml_advanced.xr', 'tests/regression/10_stdlib/1400_crypto_hash.xr', 'tests/regression/10_stdlib/1401_crypto_hmac.xr', 'tests/regression/10_stdlib/1402_crypto_random.xr', 'tests/regression/10_stdlib/1403_crypto_sha512.xr', 'tests/regression/10_stdlib/1404_crypto_aes.xr', 'tests/regression/10_stdlib/1405_crypto_utils.xr', 'tests/regression/10_stdlib/1406_crypto_aead.xr', 'tests/regression/10_stdlib/1410_url_encode.xr', 'tests/regression/10_stdlib/1411_url_parse.xr', 'tests/regression/10_stdlib/1420_datetime_basic.xr', 'tests/regression/10_stdlib/1421_datetime_format.xr', 'tests/regression/10_stdlib/1422_datetime_calc.xr')
FROZEN_TARGETS['tests/regression/10_stdlib/1301_compress.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1302_yaml_basic.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1304_yaml_advanced.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1305_csv_basic.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1307_toml_advanced.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1401_crypto_hmac.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1403_crypto_sha512.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1402_crypto_random.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1404_crypto_aes.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1411_url_parse.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1421_datetime_format.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1422_datetime_calc.xr'] = ('POSITIVE_FULL_LIBRARY_CONTENT_AFTER_DECLARED_PORT', 'P13')

REGRESSION_LIBRARY_REVIEW += ('tests/regression/10_stdlib/1000_json.xr', 'tests/regression/10_stdlib/1001_json_cjson_compat.xr', 'tests/regression/10_stdlib/1002_json_string.xr', 'tests/regression/10_stdlib/1003_json_error.xr', 'tests/regression/10_stdlib/1004_json_nested.xr', 'tests/regression/10_stdlib/1005_json_special.xr', 'tests/regression/10_stdlib/1006_json_roundtrip.xr', 'tests/regression/10_stdlib/1007_json_fuzz.xr', 'tests/regression/10_stdlib/1008_json_yaml_fuzz.xr', 'tests/regression/10_stdlib/1009_json_number.xr', 'tests/regression/10_stdlib/1160_os_basic.xr', 'tests/regression/10_stdlib/1161_os_extended.xr', 'tests/regression/10_stdlib/1162_os_home.xr', 'tests/regression/10_stdlib/1190_io_basic.xr', 'tests/regression/10_stdlib/1191_io_edge.xr', 'tests/regression/10_stdlib/1192_io_extended.xr', 'tests/regression/10_stdlib/1193_io_invalid_utf8.xr', 'tests/regression/10_stdlib/1193_io_read_dir_recursive.xr', 'tests/regression/10_stdlib/1194_io_mkdirp.xr', 'tests/regression/10_stdlib/1195_io_remove_all.xr', 'tests/regression/10_stdlib/1196_io_touch.xr', 'tests/regression/10_stdlib/1197_io_temp.xr', 'tests/regression/10_stdlib/1198_io_invalid_utf8.xr', 'tests/regression/10_stdlib/1199_time_units.xr', 'tests/regression/10_stdlib/1200_log_basic.xr', 'tests/regression/10_stdlib/1200_runtime_info.xr', 'tests/regression/10_stdlib/1201_log_child.xr', 'tests/regression/10_stdlib/1202_log_source.xr', 'tests/regression/10_stdlib/1203_log_async.xr', 'tests/regression/10_stdlib/1204_runtime.xr', 'tests/regression/10_stdlib/1205_runtime_alloc_pressure.xr', 'tests/regression/10_stdlib/1206_runtime_enhanced.xr', 'tests/regression/10_stdlib/1207_runtime_stress.xr')
FROZEN_TARGETS['tests/regression/10_stdlib/1162_os_home.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1197_io_temp.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1161_os_extended.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1190_io_basic.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1191_io_edge.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1192_io_extended.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1193_io_invalid_utf8.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1198_io_invalid_utf8.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1193_io_read_dir_recursive.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1195_io_remove_all.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1194_io_mkdirp.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1196_io_touch.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1205_runtime_alloc_pressure.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1207_runtime_stress.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1203_log_async.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P13')

FROZEN_TARGETS['tests/regression/10_stdlib/1206_runtime_enhanced.xr'] = ('POSITIVE_COMPLETE_CONSUMER_CLEANUP_OR_CONTENT_PORT', 'P07')

REGRESSION_LIBRARY_REVIEW += ('tests/regression/10_stdlib/1430_net_basic.xr', 'tests/regression/10_stdlib/1431_net_extended.xr', 'tests/regression/10_stdlib/1432_net_typed_handle.xr', 'tests/regression/10_stdlib/1433_net_loopback.xr', 'tests/regression/10_stdlib/1434_net_byte_array_loopback.xr', 'tests/regression/10_stdlib/1435_net_deadline_error.xr', 'tests/regression/10_stdlib/1436_net_halfclose_bidi.xr', 'tests/regression/10_stdlib/1437_net_tls_timeout.xr', 'tests/regression/10_stdlib/1438_ws_self_connect.xr', 'tests/regression/10_stdlib/1439_net_alpn.xr', 'tests/regression/10_stdlib/1500_http_url.xr', 'tests/regression/10_stdlib/1501_http_client_basic.xr', 'tests/regression/10_stdlib/1502_http_request_options.xr', 'tests/regression/10_stdlib/1503_http_status_codes.xr', 'tests/regression/10_stdlib/1504_http_https.xr', 'tests/regression/10_stdlib/1505_http_response_body.xr', 'tests/regression/10_stdlib/1506_http_pure_helpers.xr', 'tests/regression/10_stdlib/1506_http_redirect.xr', 'tests/regression/10_stdlib/1507_http_formdata_pure.xr', 'tests/regression/10_stdlib/1508_http_headers.xr', 'tests/regression/10_stdlib/1509_ws_pure_protocol.xr', 'tests/regression/10_stdlib/1510_cluster_typed_control.xr', 'tests/regression/10_stdlib/1510_http2_client.xr', 'tests/regression/10_stdlib/1511_cluster_opaque_transport.xr', 'tests/regression/10_stdlib/1512_http2_protocol.xr', 'tests/regression/10_stdlib/1520_websocket.xr', 'tests/regression/10_stdlib/1540_http_url_edge.xr', 'tests/regression/10_stdlib/crash_min.xr', 'tests/regression/15_project/1500_project_mode_detect.xr')
FROZEN_TARGETS['tests/regression/10_stdlib/1432_net_typed_handle.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1433_net_loopback.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1434_net_byte_array_loopback.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1435_net_deadline_error.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1436_net_halfclose_bidi.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1437_net_tls_timeout.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1438_ws_self_connect.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/10_stdlib/1502_http_request_options.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1507_http_formdata_pure.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1509_ws_pure_protocol.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/1512_http2_protocol.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/10_stdlib/crash_min.xr'] = ('POSITIVE_COMPLETE_NETWORK_OR_PROTOCOL_AFTER_FROZEN_PORT', 'P10')

CORE_NAMED_REVIEW = ('tests/regression/01_basic/0125_trailing_comma.xr', 'tests/regression/01_basic/0126_implicit_language_core.xr', 'tests/regression/01_literals/0100_int_basic.xr', 'tests/regression/01_literals/0101_int_boundary.xr', 'tests/regression/01_literals/0110_float_basic.xr', 'tests/regression/01_literals/0120_string_basic.xr', 'tests/regression/01_literals/0130_bool_null.xr', 'tests/regression/01_literals/0140_special_values.xr', 'tests/regression/01_literals/0150_bigint_basic.xr', 'tests/regression/02_variables/0011_zero_value_init.xr', 'tests/regression/02_variables/0200_var_const.xr', 'tests/regression/02_variables/0210_scope.xr', 'tests/regression/02_variables/0210_unknown_identifier.xr', 'tests/regression/02_variables/0211_public_identifier.xr', 'tests/regression/02_variables/0220_destructure.xr', 'tests/regression/02_variables/0221_map_destructure.xr', 'tests/regression/02_variables/0230_type_annotation.xr', 'tests/regression/03_operators/0006_as_numeric_cast.xr', 'tests/regression/03_operators/0300_arithmetic.xr', 'tests/regression/03_operators/0301_int64_overflow.xr', 'tests/regression/03_operators/0302_int64_native.xr', 'tests/regression/03_operators/0302_uint64_print_compare.xr', 'tests/regression/03_operators/0302_unsigned_to_string.xr', 'tests/regression/03_operators/0304_fixed_width_wrapping.xr', 'tests/regression/03_operators/0310_comparison.xr', 'tests/regression/03_operators/0320_logical.xr', 'tests/regression/03_operators/0330_bitwise.xr', 'tests/regression/03_operators/0331_shift_mod64.xr', 'tests/regression/03_operators/0332_int_bits_methods.xr', 'tests/regression/03_operators/0340_compound_assign.xr', 'tests/regression/03_operators/0350_ternary_nullish.xr', 'tests/regression/03_operators/0360_bigint_arithmetic.xr', 'tests/regression/03_operators/0361_bigint_methods.xr', 'tests/regression/03_operators/0362_bigint_comparison.xr', 'tests/regression/03_operators/0365_precedence.xr', 'tests/regression/03_operators/0370_range.xr', 'tests/regression/03_operators/0371_range_methods.xr', 'tests/regression/03_operators/0372_range_inclusive.xr', 'tests/regression/03_operators/0373_range_value_for_in.xr', 'tests/regression/03_operators/0380_optional_chain.xr', 'tests/regression/03_operators/0381_optional_call.xr', 'tests/regression/03_operators/0395_inc_dec_bnot.xr', 'tests/regression/03_operators/0396_bnot_owner_edges.xr', 'tests/regression/03_operators/0397_numeric_neg_owner_edges.xr')
FROZEN_TARGETS['tests/regression/01_basic/0125_trailing_comma.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/02_variables/0011_zero_value_init.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/02_variables/0230_type_annotation.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/03_operators/0373_range_value_for_in.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/03_operators/0340_compound_assign.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/01_literals/0100_int_basic.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/01_literals/0110_float_basic.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/01_literals/0150_bigint_basic.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/01_literals/0130_bool_null.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/01_literals/0140_special_values.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/02_variables/0200_var_const.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/03_operators/0006_as_numeric_cast.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P09')
FROZEN_TARGETS['tests/regression/03_operators/0310_comparison.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P09')

FROZEN_TARGETS['tests/regression/03_operators/0302_uint64_print_compare.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P09')

FROZEN_TARGETS['tests/regression/03_operators/0362_bigint_comparison.xr'] = ('POSITIVE_COMPLETE_CORE_AFTER_FROZEN_PORT', 'P09')

CORE_NAMED_REVIEW += ('tests/regression/04_control_flow/0400_if.xr', 'tests/regression/04_control_flow/0410_while.xr', 'tests/regression/04_control_flow/0413_null_unwrap.xr', 'tests/regression/04_control_flow/0420_for.xr', 'tests/regression/04_control_flow/0430_for_in.xr', 'tests/regression/04_control_flow/0431_for_in_try_scope.xr', 'tests/regression/04_control_flow/0440_match.xr', 'tests/regression/04_control_flow/0441_match_tuple.xr', 'tests/regression/04_control_flow/0442_match_struct_destructure.xr', 'tests/regression/04_control_flow/0445_is_type_pattern.xr', 'tests/regression/04_control_flow/0450_break_continue.xr', 'tests/regression/04_control_flow/0451_labeled_break_continue.xr', 'tests/regression/04_control_flow/0460_early_return.xr', 'tests/regression/05_functions/0500_basic.xr', 'tests/regression/05_functions/0510_recursion.xr', 'tests/regression/05_functions/0511_self_recursive_iterator.xr', 'tests/regression/05_functions/0511_tail_recursion.xr', 'tests/regression/05_functions/0512_defer_around_return_call.xr', 'tests/regression/05_functions/0520_closure.xr', 'tests/regression/05_functions/0521_cell_upval.xr', 'tests/regression/05_functions/0530_arrow.xr', 'tests/regression/05_functions/0531_bare_lambda.xr', 'tests/regression/05_functions/0540_higher_order.xr', 'tests/regression/05_functions/0560_method_chaining.xr', 'tests/regression/05_functions/0570_hoisting.xr', 'tests/regression/05_functions/0580_default_params.xr', 'tests/regression/05_functions/0581_function_item_value_binding.xr')
FROZEN_TARGETS['tests/regression/04_control_flow/0413_null_unwrap.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/04_control_flow/0445_is_type_pattern.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0500_basic.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0511_tail_recursion.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0530_arrow.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0540_higher_order.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0560_method_chaining.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0570_hoisting.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0580_default_params.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/05_functions/0512_defer_around_return_call.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')

FROZEN_TARGETS['tests/regression/04_control_flow/0410_while.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')

FROZEN_TARGETS['tests/regression/04_control_flow/0420_for.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')

FROZEN_TARGETS['tests/regression/05_functions/0511_self_recursive_iterator.xr'] = ('POSITIVE_COMPLETE_CONTROL_FUNCTION_AFTER_FROZEN_PORT', 'P10')

CORE_NAMED_REVIEW += ('tests/regression/06_collections/0600_array.xr', 'tests/regression/06_collections/0610_array_functional.xr', 'tests/regression/06_collections/0615_array_callback_signature.xr', 'tests/regression/06_collections/0620_map.xr', 'tests/regression/06_collections/0630_set.xr', 'tests/regression/06_collections/0640_array_methods.xr', 'tests/regression/06_collections/0650_array_index_set_strict.xr', 'tests/regression/06_collections/0650_nested_higher_order.xr', 'tests/regression/06_collections/0651_nested_higher_order_combo.xr', 'tests/regression/06_collections/0655_object_literal.xr', 'tests/regression/06_collections/0660_jit_store_field_types.xr', 'tests/regression/06_collections/0661_json_object_field_boundary.xr', 'tests/regression/06_collections/0670_slice_syntax.xr', 'tests/regression/06_collections/0680_deep_eq_cyclic.xr', 'tests/regression/06_collections/0681_ref_container_write.xr', 'tests/regression/06_collections/0690_user_hashable_key.xr', 'tests/regression/07_strings/0700_string_basic.xr', 'tests/regression/07_strings/0710_template_string.xr', 'tests/regression/07_strings/0720_stringbuilder.xr', 'tests/regression/07_strings/0730_unicode.xr', 'tests/regression/07_strings/0732_utf8_conversion.xr', 'tests/regression/07_strings/0733_string_bytes_receiver_nll.xr', 'tests/regression/07_strings/0734_unicode_escape.xr', 'tests/regression/07_strings/0740_context_auto_select.xr', 'tests/regression/07_strings/0741_context_buffer_isolation.xr', 'tests/regression/07_strings/0750_short_string_concat.xr', 'tests/regression/07_strings/0751_threshold_branch.xr', 'tests/regression/07_strings/0760_concurrent_concat_property.xr', 'tests/regression/07_strings/0770_string_pool_concurrent_property.xr', 'tests/regression/07_strings/0780_chr_function.xr', 'tests/regression/07_strings/0790_string_repeat.xr')
FROZEN_TARGETS['tests/regression/06_collections/0600_array.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0610_array_functional.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0620_map.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0630_set.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0640_array_methods.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0650_nested_higher_order.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0651_nested_higher_order_combo.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0655_object_literal.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0670_slice_syntax.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/06_collections/0680_deep_eq_cyclic.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/07_strings/0710_template_string.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/07_strings/0720_stringbuilder.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/07_strings/0751_threshold_branch.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/07_strings/0770_string_pool_concurrent_property.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')
FROZEN_TARGETS['tests/regression/07_strings/0780_chr_function.xr'] = ('POSITIVE_COMPLETE_COLLECTION_STRING_AFTER_FROZEN_PORT', 'P13')

CORE_NAMED_REVIEW += ('tests/regression/08_error_handling/0830_throw_catch_value_return.xr', 'tests/regression/08_error_handling/0831_cross_module_error.xr', 'tests/regression/08_error_handling/0840_enum_throw.xr', 'tests/regression/14_typed_array/1400_byte_array_basic.xr', 'tests/regression/14_typed_array/1401_byte_array_slice.xr', 'tests/regression/14_typed_array/1402_byte_array_intrinsics.xr', 'tests/regression/14_typed_array/1403_slice_grow_lifetime.xr', 'tests/regression/14_typed_array/1405_span_static_surface.xr', 'tests/regression/14_typed_array/1408_span_closure_owner_capture.xr', 'tests/regression/14_typed_array/1409_byte_u8_canonical_identity.xr', 'tests/regression/14_typed_array/1410_shared_provenance_rebind_reset.xr', 'tests/regression/14_typed_array/1411_shared_provenance_readonly_param.xr', 'tests/regression/14_typed_array/1412_shared_provenance_readonly_function_value_param.xr', 'tests/regression/14_typed_array/1413_shared_provenance_imported_readonly_function_value_param.xr', 'tests/regression/14_typed_array/1414_shared_provenance_reexported_readonly_function_value_param.xr', 'tests/regression/14_typed_array/1415_shared_provenance_returned_readonly_function_value_param.xr', 'tests/regression/14_typed_array/1416_byte_receiver_effect_matrix.xr', 'tests/regression/14_typed_array/1417_for_in_slice_borrow_nll.xr', 'tests/regression/14_typed_array/1418_shared_provenance_readonly_closure_capture.xr', 'tests/regression/14_typed_array/1419_inline_range_slice_arg.xr', 'tests/regression/14_typed_array/1420_view_return_through_user_copy.xr', 'tests/regression/14_typed_array/1421_view_return_through_renamed_callee.xr', 'tests/regression/16_narrowing/1601_narrowing_facts.xr', 'tests/regression/16_narrowing/1602_optional_chain_shortcircuit.xr', 'tests/regression/16_narrowing/1603_narrowing_joins.xr', 'tests/regression/16_narrowing/1604_for_in_loop_header.xr', 'tests/regression/16_narrowing/1604_loop_control_early_exit.xr')
IMPORTED_ORACLE_INPUTS['tests/regression/08_error_handling/0831_cross_module_error.xr'] = ('tests/regression/08_error_handling/modules/error_source.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/14_typed_array/1413_shared_provenance_imported_readonly_function_value_param.xr'] = ('tests/fixtures/shared_mutation_lib.xr',)
IMPORTED_ORACLE_INPUTS['tests/regression/14_typed_array/1414_shared_provenance_reexported_readonly_function_value_param.xr'] = ('tests/fixtures/shared_mutation_reexport.xr', 'tests/fixtures/shared_mutation_reexport_all.xr', 'tests/fixtures/shared_mutation_lib.xr')
FROZEN_TARGETS['tests/regression/14_typed_array/1400_byte_array_basic.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1401_byte_array_slice.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1402_byte_array_intrinsics.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1403_slice_grow_lifetime.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1405_span_static_surface.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1408_span_closure_owner_capture.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1410_shared_provenance_rebind_reset.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1416_byte_receiver_effect_matrix.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1420_view_return_through_user_copy.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/14_typed_array/1421_view_return_through_renamed_callee.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/16_narrowing/1601_narrowing_facts.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')
FROZEN_TARGETS['tests/regression/16_narrowing/1602_optional_chain_shortcircuit.xr'] = ('POSITIVE_COMPLETE_ERROR_BYTE_NARROWING_AFTER_FROZEN_PORT', 'P10')

CORE_NAMED_REVIEW += ('tests/regression/08_mem/0804_self_returning_method_rc.xr',)
FROZEN_TARGETS['tests/regression/08_mem/0804_self_returning_method_rc.xr'] = ('POSITIVE_COMPLETE_RETURN_LIFETIME_AFTER_FROZEN_PORT', 'P10')

FROZEN_TARGETS['tests/diff/cases/semantics/exception/force_unwrap_error_shape.xr'] = ('POSITIVE_NULL_UNWRAP_PANIC_AFTER_FROZEN_PORT', 'P10')
TARGET_PORT_ARGUMENTS['tests/diff/cases/semantics/exception/force_unwrap_error_shape.xr'] = []

FROZEN_TARGETS['tests/diff/cases/semantics/collections/array_member_reference_element_refused.xr'] = ('POSITIVE_COMPLETE_STRING_ARRAY_MEMBERS_AFTER_SINGLE_XIR_PORT', 'P09')
TARGET_PORT_ARGUMENTS['tests/diff/cases/semantics/collections/array_member_reference_element_refused.xr'] = []
FROZEN_TARGETS['tests/regression/08_oop/0836_closure_cycle_no_false_positives.xr'] = ('POSITIVE_COMPLETE_ACYCLIC_CLOSURES_AFTER_CLASS_RECEIVER_PORT', 'P10')
TARGET_PORT_ARGUMENTS['tests/regression/08_oop/0836_closure_cycle_no_false_positives.xr'] = []

def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@functools.lru_cache(maxsize=16384)
def _relative_path(path: Path, root: Path) -> str:
    # Only cache immutable lexical path conversion. Never cache file contents,
    # existence or hashes; every audit must see current source and sidecar bytes.
    return path.relative_to(root).as_posix()


def relative(path: Path) -> str:
    return _relative_path(path, ROOT)


def file_record(path: Path) -> dict:
    if not path.is_file():
        return {"path": relative(path), "state": "MISSING"}
    raw = path.read_bytes()
    digest = sha(raw)
    normalized = raw.replace(b"\r\n", b"\n") if b"\r" in raw else raw
    return {"path": relative(path), "state": "PRESENT", "bytes": len(raw),
            "sha256": digest, "lf_sha256": digest if normalized is raw else sha(normalized)}


def _audit_file_records():
    """Read each path once in this audit; never retain records across calls."""
    records = {}

    def record(path: Path) -> dict:
        key = str(path)
        if key not in records:
            records[key] = file_record(path)
        return records[key]

    return record


def read_json(path: Path) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def validate_responsibilities(data: dict) -> None:
    expected = {"capabilities": 27, "task308_obligations": 8,
                "task318_families": 14, "task322_requirements": 18}
    for key, count in expected.items():
        rows = data[key]
        ids = [r["id"] for r in rows]
        if len(rows) != count or len(set(ids)) != count:
            raise ValueError(f"missing or duplicate responsibility in {key}")
        for row in rows:
            if row["status"] != "OPEN" or not row["packages"]:
                raise ValueError(f"preparation cannot qualify {row['id']}")
    if {r["id"] for r in data["task322_requirements"]} != {f"R{i:02}" for i in range(1, 19)}:
        raise ValueError("root-effect requirement set changed")


def validate_routes(data: dict, legacy: dict, responsibilities: dict) -> dict:
    """Preserve the full original responsibility set while assigning producers."""
    families = {row["id"] for row in responsibilities["capabilities"]}
    packages = {"P02"} | {f"P{i:02}" for i in range(6, 19)}
    if data["status"] != "ROUTED_NOT_QUALIFIED":
        raise ValueError("routing cannot publish product qualification")
    originals = {row["name"]: row for row in legacy["source"]}
    rows = data["source_cases"]
    identities = [row["case_id"] for row in rows]
    if len(set(identities)) != len(rows) or set(identities) != set(originals):
        raise ValueError("source routing changed the original responsibility set")
    for row in rows:
        original = originals[row["case_id"]]
        if row["primary_family"] not in families or not row["producer_packages"] or not set(row["producer_packages"]) <= packages:
            raise ValueError("source routing has no known family or producer")
        if row["body_lf_sha256"] != original["body_lf_sha256"] or row["legacy_path"] != original["legacy_path"]:
            raise ValueError("source routing detached its original assertion body")
        if row["original_native_harness_expectation"] != original["original_fixture"]:
            raise ValueError("native harness expectation changed during routing")
        if row["existing_projection"] != original.get("initial_projection"):
            raise ValueError("source projection changed during routing")
        if row["replacement_qualification"] != "OPEN" or row["new_entry_status"] != "PENDING_FIXED_PRODUCT_DELIVERY":
            raise ValueError("routing cannot grant replacement qualification")
    paths = [row["path"] for row in data["other_files"]]
    originals_by_path = legacy["other_consumer_files"]
    if len(set(paths)) != len(paths) or set(paths) != set(originals_by_path):
        raise ValueError("other-file routing omitted a function or helper owner")
    for row in data["other_files"]:
        original = originals_by_path[row["path"]]
        functions = [{"name": f["name"], "body_lf_sha256": f["body_lf_sha256"]} for f in original["functions"]]
        if row["lf_sha256"] != original["lf_sha256"] or row["functions"] != functions:
            raise ValueError("other-file routing changed original function obligations")
        if (not row["producer_packages"] or not set(row["producer_packages"]) <= packages
                or row["replacement_qualification"] != "OPEN" or row["assertion_mapping"] != "OPEN"):
            raise ValueError("other-file routing is unowned or incorrectly qualified")
    return {"source_routes": len(rows), "other_file_routes": len(paths),
            "other_function_routes": sum(len(r["functions"]) for r in data["other_files"])}


def textual_test_declarations(text: str) -> list[dict]:
    """Preserve reviewed marker segments; this is not parser/runtime discovery."""
    import re
    hits = list(re.finditer(r"(?m)^[ \t]*@test[ \t\r\n]+fn[ \t]+([A-Za-z_]\w*)[ \t]*\(", text))
    return [{"name": m.group(1), "marker_line": text.count("\n", 0, m.start()) + 1,
             "source_segment_lf_sha256": sha(text[m.start():hits[i + 1].start() if i + 1 < len(hits) else len(text)]
                                             .replace("\r\n", "\n").encode("utf-8"))}
            for i, m in enumerate(hits)]


def differential_input_obligation(source: Path, runner) -> dict:
    """Preserve parsed arguments and exact stdin bytes, including absent sidecars."""
    return {"arguments": runner.read_args(source), "stdin_hex": runner.read_stdin(source).hex(),
            "arguments_sidecar": file_record(source.with_suffix(".args")),
            "stdin_sidecar": file_record(source.with_suffix(".stdin")), "execution": "NOT_RUN"}


def validate_runner_routes(data: dict, responsibilities: dict, actual_entries: dict | None = None) -> dict:
    """Bind explicit reviewed routes to original source, oracle and runner entry."""
    record = _audit_file_records()
    if data["status"] != "REVIEWED_ROUTING_NOT_QUALIFICATION":
        raise ValueError("runner routing cannot qualify execution")
    families = ({r["id"] for r in responsibilities["capabilities"]}
                | {"task318:" + r["id"] for r in responsibilities["task318_families"]})
    packages = {"P02"} | {f"P{i:02}" for i in range(6, 19)}
    rows = data["cases"]
    ids = [r["case_id"] for r in rows]
    if not rows or len(ids) != len(set(ids)):
        raise ValueError("empty or duplicate reviewed runner inputs")
    # Reject invalid declarations before compiling unrelated runner modules.
    # Every valid row still reaches all source, sidecar and parser checks below.
    for row in rows:
        identity = row["case_id"]
        port = row.get("target_source_port")
        disposition = row.get("target_disposition")
        target = FROZEN_TARGETS.get(identity)
        if target is not None or disposition is not None:
            if (not disposition or target != (disposition["kind"], disposition["owner_package"])
                    or disposition["owner_package"] not in row["producer_packages"]
                    or disposition["qualification"] != "NOT_RUN" or not disposition.get("reason")):
                raise ValueError("planned frozen migration cannot publish qualification or change its required disposition")
            if (target[0] == "POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT" or target[0].endswith("_PORT")) and not port:
                raise ValueError("planned frozen migration requires its frozen source port")
    errors = load_module("runner_route_expectations", "tests/compile_errors/run_compile_error_tests.py")
    diff = load_module("runner_route_differential", "tests/diff/run_backend_diff.py")
    if actual_entries is None:
        reg = load_module("runner_route_regression", "scripts/run_regression_tests.py")
        actual_entries = collections.defaultdict(list)
        for path in reg.collect_cases():
            actual_entries[relative(path)].append({"suite": "regression", "entry": "test" if reg.is_test_module(path) else "run"})
        for path in diff.collect_cases("", str(diff.SCRIPT_DIR / "coro_regression_cases.txt")):
            actual_entries[relative(path)].append({"suite": "differential", "entry": "run"})
        for path in errors.collect_cases():
            actual_entries[relative(path)].append({"suite": "compile_errors", "entry": "implicit_compile"})
    for row in rows:
        identity = row["case_id"]
        source = ROOT / identity
        if identity not in actual_entries or row["original_runner_entries"] != actual_entries[identity]:
            raise ValueError("reviewed input changed its original runner entry")
        original = record(source)
        if original["state"] != "PRESENT" or original["lf_sha256"] != row["source_lf_sha256"]:
            raise ValueError("reviewed runner source changed")
        if (row["primary_family"] not in families or not set(row["secondary_families"]) <= families
                or not row["producer_packages"] or not set(row["producer_packages"]) <= packages):
            raise ValueError("runner routing has an unknown family or producer")
        if row["replacement_qualification"] != "OPEN" or row["new_entry_status"] != "PENDING_FIXED_PRODUCT_DELIVERY":
            raise ValueError("runner routing cannot grant product qualification")
        if not row["semantic_basis"]:
            raise ValueError("runner routing lacks reviewed semantic evidence")
        differential = row.get("original_differential_obligation")
        if any(e["suite"] == "differential" for e in actual_entries[identity]):
            if differential != differential_input_obligation(source, diff):
                raise ValueError("differential input lost arguments, stdin bytes or sidecar identity")
        elif differential is not None:
            raise ValueError("differential input obligation has no original runner entry")
        imported = row.get("original_imported_inputs", [])
        required_imports = IMPORTED_ORACLE_INPUTS.get(identity, ())
        if required_imports and tuple(i["path"] for i in imported) != required_imports:
            raise ValueError("reviewed imported oracle lost its required companion inputs")
        for item in imported:
            actual = record(ROOT / item["path"])
            if item != {"path": actual["path"], "state": actual["state"], "lf_sha256": actual.get("lf_sha256")}:
                raise ValueError("reviewed imported oracle companion changed")
        regression = row.get("original_regression_obligation")
        if regression:
            entries = [e["entry"] for e in actual_entries[identity] if e["suite"] == "regression"]
            if (entries != [regression["entry"]] or type(regression["expected_exit"]) is not int
                    or regression["expected_exit"] != 0 or regression["test_discovery"] != "NOT_RUN"):
                raise ValueError("original regression positive/test discovery obligation changed")
        named = row.get("original_named_test_obligation")
        reviewed_named_scope = identity in CORE_NAMED_REVIEW or identity.startswith(("tests/regression/08_mem/", "tests/regression/08_oop/",
                                                    "tests/regression/09_advanced/", "tests/regression/10_stdlib/",
                                                    "tests/regression/11_coroutine/",
                                                    "tests/regression/12_type_checking/", "tests/regression/13_types/",
                                                    "tests/regression/15_project/"))
        if named is not None or (reviewed_named_scope and regression and regression["entry"] == "test"):
            if (not named or named["state"] != "REVIEWED_TEXTUAL_DECLARATIONS_NOT_RUNTIME_DISCOVERY"
                    or named["execution"] != "NOT_RUN"
                    or named["declarations"] != textual_test_declarations(source.read_text(encoding="utf-8"))):
                raise ValueError("named source obligations changed or falsely qualified discovery")
        port = row.get("target_source_port")
        if port:
            path = ROOT / port["fixture"]
            actual = record(path)
            if (port["qualification"] != "NOT_RUN" or port["owner_package"] not in row["producer_packages"]
                    or actual["state"] != "PRESENT" or actual["lf_sha256"] != port["lf_sha256"]):
                raise ValueError("planned source port changed or incorrectly qualified")
            companions = port.get("companion_inputs", [])
            if tuple(i["path"] for i in companions) != TARGET_PORT_IMPORTS.get(identity, ()):
                raise ValueError("target source port lost its required companion inputs")
            for item in companions:
                if record(ROOT / item["path"]).get("lf_sha256") != item["lf_sha256"]:
                    raise ValueError("target source port companion changed")
        target_arguments = TARGET_PORT_ARGUMENTS.get(identity)
        if target_arguments is not None:
            if not port or port.get("differential_input_obligation") != differential_input_obligation(path, diff):
                raise ValueError("target program lost its required arguments/stdin context")
            if port["differential_input_obligation"]["arguments"] != target_arguments:
                raise ValueError("target program lost its required explicit argument values")
        migration = row.get("named_test_migration")
        if migration:
            original_names = [d["name"] for d in named["declarations"]] if named else []
            targets = list(migration["mapping"].values())
            if (not port or not original_names or set(migration["mapping"]) != set(original_names)
                    or len(targets) != len(set(targets)) or migration["state"] != "PREPARED_NOT_EXECUTED"
                    or migration["original_tests_removed"] is not False
                    or targets != [d["name"] for d in textual_test_declarations((ROOT / port["fixture"]).read_text(encoding="utf-8"))]):
                raise ValueError("named target migration omitted or requalified an original test")
        partial = row.get("partial_target_program")
        if partial:
            actual = record(ROOT / partial["fixture"])
            names = [d["name"] for d in named["declarations"]] if named else []
            covered, remaining = partial["covered_original_tests"], partial["remaining_original_tests"]
            if (not names or not covered or len(covered) != len(set(covered))
                    or len(remaining) != len(set(remaining)) or set(covered) & set(remaining)
                    or set(covered) | set(remaining) != set(names)
                    or partial["complete_file_replacement"] is not False or partial["qualification"] != "NOT_RUN"
                    or partial["owner_package"] not in row["producer_packages"]
                    or actual.get("lf_sha256") != partial["lf_sha256"]):
                raise ValueError("partial target program changed or hid remaining original tests")
        if "original_rejection_oracle" in row:
            oracle = row["original_rejection_oracle"]
            path, kind = errors.expected_path_for(source)
            if oracle["path"] != relative(path) or oracle["kind"] != kind:
                raise ValueError("rejection oracle changed expectation kind")
            if path.is_file():
                assertions = [dataclasses.asdict(a) for a in errors.parse_expected(path)]
                if (oracle["state"] != "PRESENT" or oracle["lf_sha256"] != record(path)["lf_sha256"]
                        or oracle["assertions"] != assertions):
                    raise ValueError("rejection oracle lost original position, code or text")
            elif oracle["state"] != "MISSING" or oracle["assertions"] is not None or oracle["lf_sha256"] is not None:
                raise ValueError("missing rejection oracle cannot be fabricated")
        else:
            oracle = row["original_stdout_oracle"]
            path = Path(str(source) + ".expected")
            actual = record(path)
            state = actual["state"]
            digest = actual.get("lf_sha256")
            if oracle != {"path": relative(path), "state": state, "lf_sha256": digest}:
                raise ValueError("runner stdout oracle changed")
    missing = set(actual_entries) - set(ids)
    return {"unreviewed_runner_inputs": len(missing),
            "unreviewed_runner_entry_links": sum(len(actual_entries[p]) for p in missing),
            "unreviewed_regression_inputs": sum(any(e["suite"] == "regression" for e in actual_entries[p]) for p in missing),
            "unreviewed_differential_inputs": sum(any(e["suite"] == "differential" for e in actual_entries[p]) for p in missing),
            "reviewed_runner_inputs": len(rows),
            "reviewed_runner_entry_links": sum(len(r["original_runner_entries"]) for r in rows),
            "reviewed_dual_runner_inputs": sum(len(r["original_runner_entries"]) > 1 for r in rows),
            "reviewed_differential_input_obligations": sum("original_differential_obligation" in r for r in rows),
            "reviewed_regression_inputs": sum(any(e["suite"] == "regression" for e in r["original_runner_entries"]) for r in rows),
            "prepared_target_source_ports": sum("target_source_port" in r for r in rows),
            "reviewed_named_test_files": sum("original_named_test_obligation" in r for r in rows),
            "unreviewed_named_regression_test_files": sum(r.get("original_regression_obligation", {}).get("entry") == "test"
                and "original_named_test_obligation" not in r for r in rows),
            "reviewed_textual_test_declarations": sum(len(r.get("original_named_test_obligation", {}).get("declarations", [])) for r in rows),
            "prepared_partial_target_programs": sum("partial_target_program" in r for r in rows),
            "reviewed_frozen_target_transitions": sum("target_disposition" in r for r in rows),
            "reviewed_imported_companion_links": sum(len(r.get("original_imported_inputs", [])) for r in rows),
            "reviewed_rejection_cases": sum("original_rejection_oracle" in r for r in rows),
            "unreviewed_rejection_inputs": len({p for p, e in actual_entries.items()
                if any(i["suite"] == "compile_errors" for i in e)} - set(ids)),
            "reviewed_rejection_oracles": sum(r.get("original_rejection_oracle", {}).get("state") == "PRESENT" for r in rows),
            "reviewed_missing_rejection_oracles": sum(r.get("original_rejection_oracle", {}).get("state") == "MISSING" for r in rows)}


def compare_required(expected: dict, observed: dict) -> None:
    """Compare required fields recursively, keeping bool distinct from numeric data."""
    def compare(value, actual, path):
        if type(value) is not type(actual):
            raise ValueError(f"independent observation mismatch: {path}")
        if isinstance(value, dict):
            for key, field in value.items():
                if key not in actual:
                    raise ValueError(f"independent observation mismatch: {path}.{key}")
                compare(field, actual[key], f"{path}.{key}")
        elif isinstance(value, list):
            if len(value) != len(actual):
                raise ValueError(f"independent observation mismatch: {path}")
            for index, field in enumerate(value):
                compare(field, actual[index], f"{path}[{index}]")
        elif actual != value:
            raise ValueError(f"independent observation mismatch: {path}")

    for key, value in expected.items():
        if key not in observed:
            raise ValueError(f"independent observation mismatch: {key}")
        compare(value, observed[key], key)


def require_full_explicit_runner_coverage(counts: dict) -> None:
    if counts["unreviewed_runner_inputs"] or counts["unreviewed_runner_entry_links"]:
        raise ValueError("complete explicit routing lost original discovered inputs or entries")


def validate_oracles(data: dict) -> dict:
    record = _audit_file_records()
    ids = [row["id"] for row in data["cases"]]
    if len(ids) != len(set(ids)) or not ids:
        raise ValueError("empty or duplicate oracle cases")
    for row in data["cases"]:
        if row["status"] != "PREPARED_NOT_EXECUTED" or not row["rationale"] or not row["expected"]:
            raise ValueError("an oracle needs independent reasoning and must remain unqualified")
        execution_source = row.get("execution_source")
        if execution_source and execution_source not in {i["path"] for i in row["inputs"]}:
            raise ValueError("oracle execution source is not an owned input")
        if row.get("target_source_status") == "PENDING_PUBLISHED_DECLARATION_OWNED_LIBRARY_REPLACEMENT":
            if execution_source or row.get("obligation_scope") != "SEMANTIC_DUTY_PRESERVED_TARGET_SOURCE_NOT_PREPARED":
                raise ValueError("pending library replacement cannot pose as an executable target source")
        for backend in ("vm", "native"):
            if row["execution"][backend] != "NOT_RUN":
                raise ValueError("fixture preparation is not execution evidence")
        if not row["remaining_qualification"]:
            raise ValueError("preparation must retain the full qualification gap")
    reviewed = {r["case_id"]: r for r in read_json(HERE / "runner_routes.json")["cases"]
                if r["case_id"] in REGRESSION_LIBRARY_REVIEW or r["case_id"] in CORE_NAMED_REVIEW}
    by_id = {r["id"]: r for r in data["cases"]}
    obligations = [(identity, "regression_library_review_") for identity in REGRESSION_LIBRARY_REVIEW]
    obligations += [(identity, "regression_core_review_") for identity in CORE_NAMED_REVIEW]
    for identity, prefix in obligations:
        oracle_id = prefix + Path(identity).stem
        route = reviewed.get(identity)
        row = by_id.get(oracle_id)
        if not route or not row:
            raise ValueError("reviewed library oracle lost a complete original input")
        names = list(route.get("named_test_migration", {}).get("mapping", {}).values()) or [
            d["name"] for d in route["original_named_test_obligation"]["declarations"]]
        if (row["expected"].get("required_test_names") != names
                or row["expected"].get("all_required_tests_passed") is not True):
            raise ValueError("reviewed library oracle lost full named test execution obligations")
        if identity in CORE_NAMED_REVIEW:
            observations = row["expected"].get("per_test_observations", {})
            if (set(observations) != set(names)
                    or any(type(v) is not dict or set(v) != {"values"}
                           or type(v["values"]) is not list or not v["values"] for v in observations.values())):
                raise ValueError("reviewed core oracle lost a per-test independent obligation")
        mandatory_inputs = {identity} | {i["path"] for i in route.get("original_imported_inputs", [])}
        mandatory_inputs |= {i["path"] for i in route.get("target_source_port", {}).get("companion_inputs", [])}
        if not mandatory_inputs <= {i["path"] for i in row["inputs"]}:
            raise ValueError("reviewed library oracle lost complete source or imported companion")
        if row.get("target_source_status") != "PENDING_PUBLISHED_DECLARATION_OWNED_LIBRARY_REPLACEMENT":
            target = route.get("target_source_port", {}).get("fixture", identity)
            if row.get("execution_source") != target or row.get("execution_entry") != "test":
                raise ValueError("reviewed library oracle must use full actual test entry")
    legacy = load_module("legacy_consumer_literals", "scripts/source_product_consumer_inventory.py")
    source = legacy.SOURCE.read_text(encoding="utf-8")
    for row in data["cases"]:
        for item in row["inputs"]:
            path = ROOT / item["path"]
            actual = record(path)
            if actual["state"] != "PRESENT" or actual["lf_sha256"] != item["lf_sha256"]:
                raise ValueError(f"oracle input changed: {item['path']}")
            if "original_literal" in item:
                start = source.index("TEST(" + row["legacy_case"] + ")")
                end = legacy.function_end(source, source.index("{", start))
                original = legacy.static_strings(source[start:end])[item["original_literal"]]
                if path.read_bytes().replace(b"\r\n", b"\n") != original:
                    raise ValueError(f"original positive program changed: {row['id']}")
    return {"oracle_cases": len(ids), "product_execution": "NOT_RUN"}


def snapshot() -> dict:
    responsibilities = read_json(HERE / "responsibilities.json")
    validate_responsibilities(responsibilities)
    reg = load_module("legacy_regression_discovery", "scripts/run_regression_tests.py")
    diff = load_module("legacy_diff_discovery", "tests/diff/run_backend_diff.py")
    errors = load_module("legacy_error_discovery", "tests/compile_errors/run_compile_error_tests.py")
    ratchet = load_module("current_corpus_discovery", "scripts/check_regression_corpus.py")
    inventory = load_module("legacy_source_discovery", "scripts/source_product_consumer_inventory.py")
    legacy, generated = inventory.inventory()
    recorded = read_json(ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json")
    if recorded != legacy:
        raise ValueError("legacy responsibility ledger drift; review before accepting a new denominator")
    for path, expected in generated.items():
        if path.read_bytes().replace(b"\r\n", b"\n") != expected:
            raise ValueError(f"legacy projection drift: {relative(path)}")
    anchors = collections.defaultdict(list)
    for family in responsibilities["capabilities"]:
        for path in family["original_anchors"]:
            anchors[path].append(family["id"])

    reviewed_runner_routes = read_json(HERE / "runner_routes.json")
    explicit_routes = {row["case_id"]: row for row in reviewed_runner_routes["cases"]}

    def case_base(path: Path, entry: str) -> dict:
        key = relative(path)
        families = anchors[key]
        route = explicit_routes.get(key)
        return {"case_id": key, "entry": entry, "input": file_record(path),
                "primary_family": route["primary_family"] if route else families[0] if len(families) == 1 else "UNMAPPED",
                "reviewed_anchor_families": families, "execution": "NOT_RUN",
                "explicit_reviewed_route": route}

    regression = []
    for path in reg.collect_cases():
        row = case_base(path, "test" if reg.is_test_module(path) else "run")
        row.update(expected_exit=0, stdout_sidecar=file_record(Path(str(path) + ".expected")),
                   stdout_enforced_by_original_runner=False,
                   independent_oracle="SOURCE_ASSERTIONS_REQUIRE_SEMANTIC_REVIEW")
        regression.append(row)
    selected_reg = {r["case_id"] for r in regression}
    auxiliary = [file_record(p) for p in sorted(reg.TEST_DIR.rglob("*.xr")) if relative(p) not in selected_reg]
    differential = []
    for path in diff.collect_cases("", str(diff.SCRIPT_DIR / "coro_regression_cases.txt")):
        row = case_base(path, "run")
        expected = diff.read_expected_stdout(path)
        row.update(stdout_hex=None if expected is None else expected.hex(),
                   independent_stdout_oracle=expected is not None,
                   arguments=diff.read_args(path), stdin_hex=diff.read_stdin(path).hex(),
                   original_regression_entry=next((r["entry"] for r in regression if r["case_id"] == relative(path)), None))
        differential.append(row)
    negative = []
    for path in errors.collect_cases():
        row = case_base(path, "implicit_compile")
        expected_path, kind = errors.expected_path_for(path)
        row.update(expectation=file_record(expected_path), expectation_kind=kind)
        try:
            assertions = errors.parse_expected(expected_path)
            row.update(assertion_count=len(assertions),
                       diagnostic_position_gap=any(a.misplaced or a.where.degenerate for a in assertions),
                       expectation_parse="PARSED")
        except (errors.ExpectedFormatError, OSError) as exc:
            row.update(expectation_parse="FAILED", expectation_error=str(exc).replace(str(ROOT), "<repository>"))
        negative.append(row)
    actual_entries = collections.defaultdict(list)
    for suite, rows in (("regression", regression), ("differential", differential), ("compile_errors", negative)):
        for row in rows:
            actual_entries[row["case_id"]].append({"suite": suite, "entry": row["entry"]})
    runner_routing_counts = validate_runner_routes(reviewed_runner_routes, responsibilities, actual_entries)
    routing = read_json(HERE / "consumer_routes.json")
    routing_counts = validate_routes(routing, legacy, responsibilities)
    source_routes = {row["case_id"]: row for row in routing["source_cases"]}
    sources = []
    for row in legacy["source"]:
        sources.append({"case_id": row["name"], "legacy_path": row["legacy_path"],
                        "body_lf_sha256": row["body_lf_sha256"], "original_fixture": row["original_fixture"],
                        "projection": row.get("initial_projection"),
                        "candidate_gates": row.get("candidate_replacement_gates", []),
                        "responsibility_detail": "tests/unit/xir/product_consumers/legacy_responsibilities.json#" + row["name"],
                        "execution": "NOT_RUN", "status": "OPEN",
                        "primary_family": source_routes[row["name"]]["primary_family"],
                        "producer_packages": source_routes[row["name"]]["producer_packages"],
                        "tasks": source_routes[row["name"]]["tasks"],
                        "new_entry_status": source_routes[row["name"]]["new_entry_status"]})
    extra = []
    for path in sorted((ROOT / "tests/unit/execution").glob("*.c")):
        extra.append({"input": file_record(path), "tasks": ["315", "316", "318", "322"],
                      "packages": ["P07", "P08", "P09"], "assertion_mapping": "OPEN",
                      "qualification_owner": "P20/P21; read-only for this preparation"})
    baseline = ratchet.read_baseline(ratchet.BASELINE)
    counts = {"registered_corpus_passing_test_baseline": len(baseline), "regression_entries": len(regression), "regression_test": sum(r["entry"] == "test" for r in regression),
              "regression_run": sum(r["entry"] == "run" for r in regression), "regression_auxiliary": len(auxiliary),
              "regression_stdout_sidecars": sum(r["stdout_sidecar"]["state"] == "PRESENT" for r in regression),
              "diff_entries": len(differential), "diff_independent_stdout": sum(r["independent_stdout_oracle"] for r in differential),
              "diff_regression_entry_mismatches": sum(r["original_regression_entry"] == "test" for r in differential),
              "compile_errors": len(negative), "expectation_parse_failures": sum(r["expectation_parse"] == "FAILED" for r in negative),
              "diagnostic_position_gaps": sum(bool(r.get("diagnostic_position_gap")) for r in negative),
              "source_responsibilities": len(sources), "source_fixtures": legacy["source_native_fixtures"],
              "source_projections": sum(r["projection"] is not None for r in sources),
              "other_consumer_files": len(legacy["other_consumer_files"]),
              "other_consumer_functions": sum(len(r["functions"]) for r in legacy["other_consumer_files"].values()),
              "execution_files": len(extra), "allocation_scenarios": legacy["source_allocation_scenarios"]["count"],
              "runner_rows_without_reviewed_primary_family": sum(r["primary_family"] == "UNMAPPED" for r in regression + differential + negative)}
    counts.update(routing_counts)
    counts.update(runner_routing_counts)
    require_full_explicit_runner_coverage(runner_routing_counts)
    counts["prepared_independent_oracles"] = validate_oracles(read_json(HERE / "oracles.json"))["oracle_cases"]
    return {"scope": "PREPARATION_ONLY_NO_PRODUCT_QUALIFICATION", "summary": counts,
            "source_identity": {"seed": "e16797c62636f1d138583ead18d234c86a3cde6e",
                                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                                "working_tree": subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=ROOT, text=True).splitlines(),
                                "baseline_acceptance": "See fixed delivery receipt; preparation checks do not qualify dependencies"},
            "regression": regression, "regression_auxiliary": auxiliary,
            "differential": differential, "compile_errors": negative,
            "source_responsibilities": sources, "other_consumers": legacy["other_consumer_files"],
            "source_allocation_scenarios": legacy["source_allocation_scenarios"],
            "execution_responsibilities": extra, "other_consumer_routes": routing["other_files"],
            "registered_corpus": {"gate": "regression_corpus", "runner": "scripts/check_regression_corpus.py",
                                  "entry": "xray test tests/regression --jobs <registered_jobs> --quiet --report <report>",
                                  "runner_default_timeout_seconds": 900, "ctest_timeout_expression": "600 * XR_TEST_TIMEOUT_SCALE",
                                  "registered_jobs": "min(4, XRAY_HOST_PROCESSOR_COUNT)", "runtime_discovery": "NOT_RUN",
                                  "passing_test_baseline": baseline, "status": "NOT_RUN"},
            "original_baselines": [file_record(ROOT / p) for p in ["tests/regression/baseline_failures.txt", "tests/regression/nondeterministic_cases.txt", "tests/regression/xir_passing.txt"]],
            "discovery_inputs": [file_record(ROOT / p) for p in ["CMakeLists.txt", "scripts/check_regression_corpus.py", "scripts/run_regression_tests.py", "tests/diff/run_backend_diff.py", "tests/diff/coro_regression_cases.txt", "tests/compile_errors/run_compile_error_tests.py", "tests/unit/xir/product_consumers/legacy_responsibilities.json"]],
            "original_anchor_presence": [file_record(ROOT / p) for p in sorted(anchors)],
            "preparation_inputs": [file_record(p) for p in sorted(HERE.rglob("*")) if p.is_file() and "__pycache__" not in p.parts],
            "note": "Runner entries overlap; counts are not a language completion percentage. Unmapped rows and unchanged old failures remain OPEN."}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("snapshot", "check"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    validate_responsibilities(read_json(HERE / "responsibilities.json"))
    result = validate_oracles(read_json(HERE / "oracles.json"))
    result.update(validate_routes(read_json(HERE / "consumer_routes.json"),
                                  read_json(ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json"),
                                  read_json(HERE / "responsibilities.json")))
    if args.action == "snapshot":
        result = snapshot()
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps(result["summary"], sort_keys=True))
    else:
        result.update(validate_runner_routes(read_json(HERE / "runner_routes.json"),
                                             read_json(HERE / "responsibilities.json")))
        require_full_explicit_runner_coverage(result)
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
