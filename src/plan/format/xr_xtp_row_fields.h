/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_row_fields.h - Reader-side field inventory for frozen wire rows
 *
 * KEY CONCEPT:
 *   The codec owns the authoritative field inventory. A reader that wants to
 *   name fields rather than print bytes needs the same list, and the frozen
 *   artifact contract keeps the codec's copy under a digest gate rather than
 *   in a shared header. So this reader-side list is checked instead of
 *   trusted: every entry contributes its wire width, and the renderer refuses
 *   any table whose recorded row width disagrees. A drifted list therefore
 *   stops the reader instead of silently naming stale fields.
 */

#ifndef XR_XTP_ROW_FIELDS_H
#define XR_XTP_ROW_FIELDS_H

#include "../target/xr_target_plan.h"
#include "../target/xr_target_profile_internal.h"

#define XR_XTP_TEXT_PROFILE_FIELDS(F)                                                                  \
    F(U32, schema_version)                                                                        \
    F(U16, machine.architecture)                                                                  \
    F(U16, machine.operating_system)                                                              \
    F(U16, machine.environment)                                                                   \
    F(U16, machine.native_abi)                                                                    \
    F(U8, machine.runtime_profile)                                                                \
    F(U8, machine.reserved8[0])                                                                   \
    F(U8, machine.reserved8[1])                                                                   \
    F(U8, machine.reserved8[2])                                                                   \
    F(U32, machine.data_layout.i8.size) F(U32, machine.data_layout.i8.align)                       \
    F(U32, machine.data_layout.u8.size) F(U32, machine.data_layout.u8.align)                       \
    F(U32, machine.data_layout.i16.size) F(U32, machine.data_layout.i16.align)                     \
    F(U32, machine.data_layout.u16.size) F(U32, machine.data_layout.u16.align)                     \
    F(U32, machine.data_layout.i32.size) F(U32, machine.data_layout.i32.align)                     \
    F(U32, machine.data_layout.u32.size) F(U32, machine.data_layout.u32.align)                     \
    F(U32, machine.data_layout.i64.size) F(U32, machine.data_layout.i64.align)                     \
    F(U32, machine.data_layout.u64.size) F(U32, machine.data_layout.u64.align)                     \
    F(U32, machine.data_layout.f32.size) F(U32, machine.data_layout.f32.align)                     \
    F(U32, machine.data_layout.f64.size) F(U32, machine.data_layout.f64.align)                     \
    F(U32, machine.data_layout.boolean.size) F(U32, machine.data_layout.boolean.align)             \
    F(U32, machine.data_layout.pointer.size) F(U32, machine.data_layout.pointer.align)             \
    F(U32, machine.data_layout.isize.size) F(U32, machine.data_layout.isize.align)                 \
    F(U32, machine.data_layout.usize.size) F(U32, machine.data_layout.usize.align)                 \
    F(U32, machine.data_layout.xr_value.size) F(U32, machine.data_layout.xr_value.align)           \
    F(U32, machine.data_layout.endian)                                                             \
    F(U32, machine.data_layout.abi_id)                                                             \
    F(U64, machine.data_layout.stable_hash)                                                        \
    F(U64, machine.atomic_width_mask)                                                              \
    F(U64, machine.atomic_order_mask)                                                              \
    F(U64, machine.float_feature_mask)                                                             \
    F(U64, machine.vector_feature_mask)                                                            \
    F(U16, machine.maximum_vector_bits)                                                            \
    F(U16, machine.reserved16)                                                                     \
    F(U64, provider_mask)                                                                          \
    F(FP, provider_set_fingerprint)                                                                \
    F(FP, object_header_fingerprint)                                                               \
    F(FP, runtime_abi_fingerprint)                                                               \
    F(U32, string_literal.schema_version)                                                        \
    F(U8, string_literal.dynamic_tag)                                                            \
    F(U8, string_literal.has_object_header)                                                      \
    F(U8, string_literal.owns_utf8_bytes)                                                        \
    F(U8, string_literal.nul_terminated)                                                         \
    F(U32, string_literal.literal_flag)                                                          \
    F(U32, string_literal.semantic_domain)                                                       \
    F(U32, string_literal.backend_materialization)                                               \
    F(U32, string_literal.view_size)                                                             \
    F(U16, string_literal.view_alignment)                                                        \
    F(U16, string_literal.field_count)                                                           \
    F(U16, string_literal.fields[0].role) F(U16, string_literal.fields[0].flags)                 \
    F(U32, string_literal.fields[0].offset) F(U32, string_literal.fields[0].width)               \
    F(U32, string_literal.fields[0].reserved)                                                    \
    F(U16, string_literal.fields[1].role) F(U16, string_literal.fields[1].flags)                 \
    F(U32, string_literal.fields[1].offset) F(U32, string_literal.fields[1].width)               \
    F(U32, string_literal.fields[1].reserved)                                                    \
    F(U16, string_literal.fields[2].role) F(U16, string_literal.fields[2].flags)                 \
    F(U32, string_literal.fields[2].offset) F(U32, string_literal.fields[2].width)               \
    F(U32, string_literal.fields[2].reserved)                                                    \
    F(U16, string_literal.fields[3].role) F(U16, string_literal.fields[3].flags)                 \
    F(U32, string_literal.fields[3].offset) F(U32, string_literal.fields[3].width)               \
    F(U32, string_literal.fields[3].reserved)                                                    \
    F(U16, string_literal.fields[4].role) F(U16, string_literal.fields[4].flags)                 \
    F(U32, string_literal.fields[4].offset) F(U32, string_literal.fields[4].width)               \
    F(U32, string_literal.fields[4].reserved)                                                    \
    F(FP, string_literal.fingerprint)                                                            \
    F(U64, string_literal.reserved[0]) F(U64, string_literal.reserved[1])

#define XR_XTP_TEXT_MACHINE_REP_FIELDS(F)                                                              \
    F(U32, id) F(U16, kind) F(U16, register_bits) F(U32, memory_size) F(U16, memory_align)         \
    F(U8, signedness) F(U8, root_kind) F(U8, ownership) F(U8, null_encoding) F(U32, detail)        \
    F(U16, lane_count) F(U16, reserved) F(U64, legal_conversion_mask[0])                           \
    F(U64, legal_conversion_mask[1]) F(U64, legal_conversion_mask[2])                              \
    F(U64, legal_conversion_mask[3])
#define XR_XTP_TEXT_VALUE_REP_FIELDS(F)                                                                 \
    F(U32, semantic_value) F(U16, register_rep) F(U16, memory_rep) F(U32, slot)
#define XR_XTP_TEXT_EXTENT_FIELDS(F)                                                                    \
    F(U32, id) F(U8, kind) F(U8, operand_count) F(U16, alignment) F(U32, element_layout)           \
    F(U32, stride) F(U32, provider) F(U32, flags)
#define XR_XTP_TEXT_LAYOUT_FIELDS(F)                                                                    \
    F(U32, id) F(U32, semantic_type) F(U8, kind) F(U8, array_element_storage) F(U16, align)        \
    F(U32, fixed_prefix_size) F(U32, extent) F(U32, field_begin) F(U16, field_count)               \
    F(U16, root_field_count) F(ID, destructor) F(ID, clone) F(ID, equality_hash) F(FP, fingerprint)
#define XR_XTP_TEXT_FIELD_FIELDS(F)                                                                     \
    F(U32, layout) F(U32, semantic_field) F(U32, semantic_name) F(U32, offset) F(U32, size)         \
    F(U16, align)                                                                                     \
    F(U16, memory_rep) F(U8, root_kind) F(U8, flags) F(U16, reserved)
#define XR_XTP_TEXT_STORAGE_FIELDS(F)                                                                   \
    F(U32, id) F(U8, kind) F(U8, ownership) F(U16, flags) F(ID, domain) F(ID, destructor)
#define XR_XTP_TEXT_ALLOCATION_FIELDS(F)                                                                \
    F(U32, id) F(U32, semantic_operation) F(U32, layout) F(U32, storage) F(U32, operand_begin)    \
    F(U16, operand_count) F(U16, flags)
#define XR_XTP_TEXT_EXTENT_OPERAND_FIELDS(F)                                                            \
    F(U32, allocation) F(U32, semantic_value) F(U8, ordinal) F(U8, role) F(U16, reserved)
#define XR_XTP_TEXT_FUNCTION_FIELDS(F)                                                                  \
    F(U32, id) F(U32, semantic_function) F(U32, slot_begin) F(U32, slot_count)                    \
    F(U32, frame_size) F(U16, frame_align) F(U16, reserved) F(U32, root_begin)                    \
    F(U32, root_count) F(U32, cleanup_begin) F(U32, cleanup_count)                                \
    F(U32, coroutine_begin) F(U32, coroutine_count)
#define XR_XTP_TEXT_SLOT_FIELDS(F)                                                                      \
    F(ID, identity) F(U32, id) F(U32, function) F(U32, semantic_value)                            \
    F(U32, semantic_operation) F(U32, logical_slot) F(U32, offset) F(U32, size)                  \
    F(U16, align) F(U16, register_rep) F(U16, memory_rep) F(U8, role) F(U8, root_kind)           \
    F(U8, ownership) F(U8, reserved) F(U32, debug_variable)
#define XR_XTP_TEXT_INSTRUCTION_FIELDS(F)                                                               \
    F(U32, id) F(U32, function) F(U32, result_slot) F(U32, operand_slots[0])                      \
    F(U32, operand_slots[1]) F(U64, immediate_bits) F(U16, opcode) F(U8, operand_count)           \
    F(U8, reserved)
#define XR_XTP_TEXT_CALL_FIELDS(F)                                                                      \
    F(ID, identity) F(U32, id) F(U32, semantic_call_target) F(U32, semantic_operation)           \
    F(U32, caller_function) F(U32, callee_function) F(U32, source_dependency)                    \
    F(U32, source_export) F(ID, source_export_identity) F(ID, source_callee_identity)            \
    F(U32, result_value) F(U32, result_slot)                                                       \
    F(U32, caller_storage_slot) F(U32, error_slot) F(U32, argument_begin) F(U32, adapter_begin)  \
    F(U16, result_register_rep) F(U16, result_memory_rep) F(U16, error_register_rep)             \
    F(U16, error_memory_rep) F(U16, argument_count) F(U16, adapter_count) F(U16, native_abi)     \
    F(U16, flags) F(U8, calling_convention) F(U8, target_kind) F(U8, result_mode)                \
    F(U8, result_ownership) F(U8, error_mode) F(U8, array_intrinsic_kind)                       \
    F(U8, array_element_storage) F(U8, array_hof_kind) F(U8, array_result_element_storage)     \
    F(U8, reserved8[0]) F(U8, reserved8[1])                                                    \
    F(U8, reserved8[2]) F(FP, fingerprint)
#define XR_XTP_TEXT_CALL_ARGUMENT_FIELDS(F)                                                             \
    F(ID, identity) F(U32, call) F(U32, semantic_operand) F(U32, semantic_value)                  \
    F(U32, callee_parameter) F(U32, caller_slot) F(U32, callee_slot) F(U16, register_rep)        \
    F(U16, memory_rep) F(U16, callee_register_rep) F(U16, callee_memory_rep)                    \
    F(U16, ordinal) F(U8, mode) F(U8, ownership) F(U8, transfer_mode)                            \
    F(U8, flags) F(U8, array_element_storage) F(U8, reserved8[0]) F(U8, reserved8[1])           \
    F(U8, reserved8[2])
#define XR_XTP_TEXT_ROOT_MAP_FIELDS(F)                                                                  \
    F(U32, id) F(U32, function) F(U32, semantic_operation) F(U32, slot_begin)                    \
    F(U16, slot_count) F(U16, flags)
#define XR_XTP_TEXT_CLEANUP_FIELDS(F)                                                                   \
    F(U32, id) F(U32, function) F(U32, semantic_operation) F(U32, slot) F(U8, action)            \
    F(U8, flags) F(U16, provider)
#define XR_XTP_TEXT_ADAPTER_FIELDS(F)                                                                   \
    F(ID, identity) F(U32, id) F(U32, call) F(U32, input_rep) F(U32, output_rep)                 \
    F(U32, layout) F(U16, ordinal) F(U16, flags) F(U8, role) F(U8, kind)                         \
    F(U8, ownership) F(U8, reserved)
#define XR_XTP_TEXT_CAPABILITY_FIELDS(F)                                                                \
    F(U32, id) F(U32, capability) F(U16, provider) F(U16, flags)
#define XR_XTP_TEXT_COROUTINE_FIELDS(F)                                                                 \
    F(U32, id) F(U32, function) F(U32, semantic_entity) F(U32, semantic_operation)               \
    F(U32, logical_state) F(U32, suspend_block) F(U32, resume_block)                              \
    F(U32, resume_predecessor) F(U32, resume_instruction) F(U32, direct_call)                     \
    F(U32, result_slot)                                                                            \
    F(U16, resume_predecessor_ordinal) F(U16, flags)
#define XR_XTP_TEXT_ENTRY_EXPECTATION_FIELDS(F)                                                         \
    F(ID, identity) F(U32, id) F(U32, call) F(U32, abi_schema_version)                                 \
    F(U16, parameter_count) F(U16, native_abi) F(U8, value_kind) F(U8, adapter_kind)                   \
    F(U16, flags) F(U32, reserved32) F(U64, target_data_layout)                                        \
    F(FP, target_profile_fingerprint) F(FP, entry_abi_fingerprint) F(FP, adapter_fingerprint)
#define XR_XTP_TEXT_DEBUG_FACT_FIELDS(F)                                                               \
    F(U32, id) F(U32, instruction) F(U32, function) F(U32, semantic_operation)                       \
    F(U32, coroutine_state) F(U32, source_start_line) F(U32, source_start_column)                     \
    F(U32, source_end_line) F(U32, source_end_column) F(ID, semantic_operation_identity)              \
    F(ID, source_span_identity) F(ID, owner_identity) F(ID, coroutine_state_identity)                 \
    F(FP, layout_fingerprint)

#define XR_XTP_TEXT_TYPED_ROWS(F)                                                                       \
    F(TARGET_PROFILE, XrTargetProfileDraft, XR_XTP_TEXT_PROFILE_FIELDS)                                \
    F(MACHINE_REPS, XrTargetMachineRepRecord, XR_XTP_TEXT_MACHINE_REP_FIELDS)                          \
    F(VALUE_REPS, XrTargetValueRepRecord, XR_XTP_TEXT_VALUE_REP_FIELDS)                               \
    F(EXTENTS, XrTargetExtentRecord, XR_XTP_TEXT_EXTENT_FIELDS)                                       \
    F(LAYOUTS, XrTargetLayoutRecord, XR_XTP_TEXT_LAYOUT_FIELDS)                                       \
    F(FIELDS, XrTargetFieldRecord, XR_XTP_TEXT_FIELD_FIELDS)                                          \
    F(STORAGE, XrTargetStorageRecord, XR_XTP_TEXT_STORAGE_FIELDS)                                     \
    F(ALLOCATIONS, XrTargetAllocationRecord, XR_XTP_TEXT_ALLOCATION_FIELDS)                           \
    F(EXTENT_OPERANDS, XrTargetExtentOperandRecord, XR_XTP_TEXT_EXTENT_OPERAND_FIELDS)                 \
    F(FUNCTIONS, XrTargetFunctionRecord, XR_XTP_TEXT_FUNCTION_FIELDS)                                 \
    F(SLOTS, XrTargetSlotRecord, XR_XTP_TEXT_SLOT_FIELDS)                                             \
    F(INSTRUCTIONS, XrTargetInstructionRecord, XR_XTP_TEXT_INSTRUCTION_FIELDS)                         \
    F(CALLS, XrTargetCallRecord, XR_XTP_TEXT_CALL_FIELDS)                                             \
    F(CALL_ARGUMENTS, XrTargetCallArgumentRecord, XR_XTP_TEXT_CALL_ARGUMENT_FIELDS)                   \
    F(ROOT_MAPS, XrTargetRootMapRecord, XR_XTP_TEXT_ROOT_MAP_FIELDS)                                  \
    F(CLEANUPS, XrTargetCleanupRecord, XR_XTP_TEXT_CLEANUP_FIELDS)                                    \
    F(ADAPTERS, XrTargetAdapterRecord, XR_XTP_TEXT_ADAPTER_FIELDS)                                    \
    F(CAPABILITIES, XrTargetCapabilityRecord, XR_XTP_TEXT_CAPABILITY_FIELDS)                          \
    F(COROUTINES, XrTargetCoroutineStateRecord, XR_XTP_TEXT_COROUTINE_FIELDS)                         \
    F(ENTRY_EXPECTATIONS, XrTargetEntryExpectationRecord, XR_XTP_TEXT_ENTRY_EXPECTATION_FIELDS)       \
    F(DEBUG_FACTS, XrTargetDebugFactRecord, XR_XTP_TEXT_DEBUG_FACT_FIELDS)

#endif  // XR_XTP_ROW_FIELDS_H
