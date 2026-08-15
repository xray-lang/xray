/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_rows.c - Canonical little-endian TargetPlan table rows
 *
 * KEY CONCEPT:
 *   Wire rows never use native structure layout. One field inventory produces
 *   symmetric encoders, decoders, and exact row widths for every frozen table.
 */

#include "xr_xtp_internal.h"
#include <string.h>

typedef struct XrXtpRowWriter {
    uint8_t *cursor;
} XrXtpRowWriter;

typedef struct XrXtpRowReader {
    const uint8_t *cursor;
} XrXtpRowReader;

XR_FUNC uint16_t xr_xtp_take_u16(const uint8_t *bytes) {
    return (uint16_t) bytes[0] | (uint16_t) ((uint16_t) bytes[1] << 8);
}

XR_FUNC uint32_t xr_xtp_take_u32(const uint8_t *bytes) {
    return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) |
           ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

XR_FUNC uint64_t xr_xtp_take_u64(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t) bytes[i] << (i * 8);
    return value;
}

XR_FUNC void xr_xtp_put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t) value;
    bytes[1] = (uint8_t) (value >> 8);
}

XR_FUNC void xr_xtp_put_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
}

XR_FUNC void xr_xtp_put_u64(uint8_t *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
}

#define XR_XTP_PROFILE_FIELDS(F)                                                                  \
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

#define XR_XTP_MACHINE_REP_FIELDS(F)                                                              \
    F(U32, id) F(U16, kind) F(U16, register_bits) F(U32, memory_size) F(U16, memory_align)         \
    F(U8, signedness) F(U8, root_kind) F(U8, ownership) F(U8, null_encoding) F(U32, detail)        \
    F(U16, lane_count) F(U16, reserved) F(U64, legal_conversion_mask[0])                           \
    F(U64, legal_conversion_mask[1]) F(U64, legal_conversion_mask[2])                              \
    F(U64, legal_conversion_mask[3])
#define XR_XTP_VALUE_REP_FIELDS(F)                                                                 \
    F(U32, semantic_value) F(U16, register_rep) F(U16, memory_rep) F(U32, slot)
#define XR_XTP_EXTENT_FIELDS(F)                                                                    \
    F(U32, id) F(U8, kind) F(U8, operand_count) F(U16, alignment) F(U32, element_layout)           \
    F(U32, stride) F(U32, provider) F(U32, flags)
#define XR_XTP_LAYOUT_FIELDS(F)                                                                    \
    F(U32, id) F(U32, semantic_type) F(U8, kind) F(U8, reserved) F(U16, align)                     \
    F(U32, fixed_prefix_size) F(U32, extent) F(U32, field_begin) F(U16, field_count)               \
    F(U16, root_field_count) F(ID, destructor) F(ID, clone) F(ID, equality_hash) F(FP, fingerprint)
#define XR_XTP_FIELD_FIELDS(F)                                                                     \
    F(U32, layout) F(U32, semantic_field) F(U32, semantic_name) F(U32, offset) F(U32, size)         \
    F(U16, align)                                                                                     \
    F(U16, memory_rep) F(U8, root_kind) F(U8, flags) F(U16, reserved)
#define XR_XTP_STORAGE_FIELDS(F)                                                                   \
    F(U32, id) F(U8, kind) F(U8, ownership) F(U16, flags) F(ID, domain) F(ID, destructor)
#define XR_XTP_ALLOCATION_FIELDS(F)                                                                \
    F(U32, id) F(U32, semantic_operation) F(U32, layout) F(U32, storage) F(U32, operand_begin)    \
    F(U16, operand_count) F(U16, flags)
#define XR_XTP_EXTENT_OPERAND_FIELDS(F)                                                            \
    F(U32, allocation) F(U32, semantic_value) F(U8, ordinal) F(U8, role) F(U16, reserved)
#define XR_XTP_FUNCTION_FIELDS(F)                                                                  \
    F(U32, id) F(U32, semantic_function) F(U32, slot_begin) F(U32, slot_count)                    \
    F(U32, frame_size) F(U16, frame_align) F(U16, reserved) F(U32, root_begin)                    \
    F(U32, root_count) F(U32, cleanup_begin) F(U32, cleanup_count)                                \
    F(U32, coroutine_begin) F(U32, coroutine_count)
#define XR_XTP_SLOT_FIELDS(F)                                                                      \
    F(ID, identity) F(U32, id) F(U32, function) F(U32, semantic_value)                            \
    F(U32, semantic_operation) F(U32, logical_slot) F(U32, offset) F(U32, size)                  \
    F(U16, align) F(U16, register_rep) F(U16, memory_rep) F(U8, role) F(U8, root_kind)           \
    F(U8, ownership) F(U8, reserved) F(U32, debug_variable)
#define XR_XTP_INSTRUCTION_FIELDS(F)                                                               \
    F(U32, id) F(U32, function) F(U32, result_slot) F(U32, operand_slots[0])                      \
    F(U32, operand_slots[1]) F(U64, immediate_bits) F(U8, opcode) F(U8, operand_count)            \
    F(U16, reserved)
#define XR_XTP_CALL_FIELDS(F)                                                                      \
    F(ID, identity) F(U32, id) F(U32, semantic_call_target) F(U32, semantic_operation)           \
    F(U32, caller_function) F(U32, callee_function) F(U32, source_dependency)                    \
    F(U32, source_export) F(ID, source_export_identity) F(ID, source_callee_identity)            \
    F(U32, result_value) F(U32, result_slot)                                                       \
    F(U32, caller_storage_slot) F(U32, error_slot) F(U32, argument_begin) F(U32, adapter_begin)  \
    F(U16, result_register_rep) F(U16, result_memory_rep) F(U16, error_register_rep)             \
    F(U16, error_memory_rep) F(U16, argument_count) F(U16, adapter_count) F(U16, native_abi)     \
    F(U16, flags) F(U8, calling_convention) F(U8, target_kind) F(U8, result_mode)                \
    F(U8, result_ownership) F(U8, error_mode) F(U8, array_intrinsic_kind)                       \
    F(U8, array_element_storage) F(U8, reserved8[0]) F(U8, reserved8[1])                       \
    F(U8, reserved8[2]) F(FP, fingerprint)
#define XR_XTP_CALL_ARGUMENT_FIELDS(F)                                                             \
    F(ID, identity) F(U32, call) F(U32, semantic_operand) F(U32, semantic_value)                  \
    F(U32, callee_parameter) F(U32, caller_slot) F(U32, callee_slot) F(U16, register_rep)        \
    F(U16, memory_rep) F(U16, callee_register_rep) F(U16, callee_memory_rep)                    \
    F(U16, ordinal) F(U8, mode) F(U8, ownership) F(U8, transfer_mode)                            \
    F(U8, flags) F(U8, array_element_storage) F(U8, reserved8[0]) F(U8, reserved8[1])           \
    F(U8, reserved8[2])
#define XR_XTP_ROOT_MAP_FIELDS(F)                                                                  \
    F(U32, id) F(U32, function) F(U32, semantic_operation) F(U32, slot_begin)                    \
    F(U16, slot_count) F(U16, flags)
#define XR_XTP_CLEANUP_FIELDS(F)                                                                   \
    F(U32, id) F(U32, function) F(U32, semantic_operation) F(U32, slot) F(U8, action)            \
    F(U8, flags) F(U16, provider)
#define XR_XTP_ADAPTER_FIELDS(F)                                                                   \
    F(ID, identity) F(U32, id) F(U32, call) F(U32, input_rep) F(U32, output_rep)                 \
    F(U32, layout) F(U16, ordinal) F(U16, flags) F(U8, role) F(U8, kind)                         \
    F(U8, ownership) F(U8, reserved)
#define XR_XTP_CAPABILITY_FIELDS(F)                                                                \
    F(U32, id) F(U32, capability) F(U16, provider) F(U16, flags)
#define XR_XTP_COROUTINE_FIELDS(F)                                                                 \
    F(U32, id) F(U32, function) F(U32, semantic_entity) F(U32, semantic_operation)               \
    F(U32, logical_state) F(U32, suspend_block) F(U32, resume_block)                              \
    F(U32, resume_predecessor) F(U32, direct_call) F(U32, result_slot)                            \
    F(U16, resume_predecessor_ordinal) F(U16, flags)

#define XR_XTP_TYPED_ROWS(F)                                                                       \
    F(TARGET_PROFILE, XrTargetProfileDraft, XR_XTP_PROFILE_FIELDS)                                \
    F(MACHINE_REPS, XrTargetMachineRepRecord, XR_XTP_MACHINE_REP_FIELDS)                          \
    F(VALUE_REPS, XrTargetValueRepRecord, XR_XTP_VALUE_REP_FIELDS)                               \
    F(EXTENTS, XrTargetExtentRecord, XR_XTP_EXTENT_FIELDS)                                       \
    F(LAYOUTS, XrTargetLayoutRecord, XR_XTP_LAYOUT_FIELDS)                                       \
    F(FIELDS, XrTargetFieldRecord, XR_XTP_FIELD_FIELDS)                                          \
    F(STORAGE, XrTargetStorageRecord, XR_XTP_STORAGE_FIELDS)                                     \
    F(ALLOCATIONS, XrTargetAllocationRecord, XR_XTP_ALLOCATION_FIELDS)                           \
    F(EXTENT_OPERANDS, XrTargetExtentOperandRecord, XR_XTP_EXTENT_OPERAND_FIELDS)                 \
    F(FUNCTIONS, XrTargetFunctionRecord, XR_XTP_FUNCTION_FIELDS)                                 \
    F(SLOTS, XrTargetSlotRecord, XR_XTP_SLOT_FIELDS)                                             \
    F(INSTRUCTIONS, XrTargetInstructionRecord, XR_XTP_INSTRUCTION_FIELDS)                         \
    F(CALLS, XrTargetCallRecord, XR_XTP_CALL_FIELDS)                                             \
    F(CALL_ARGUMENTS, XrTargetCallArgumentRecord, XR_XTP_CALL_ARGUMENT_FIELDS)                   \
    F(ROOT_MAPS, XrTargetRootMapRecord, XR_XTP_ROOT_MAP_FIELDS)                                  \
    F(CLEANUPS, XrTargetCleanupRecord, XR_XTP_CLEANUP_FIELDS)                                    \
    F(ADAPTERS, XrTargetAdapterRecord, XR_XTP_ADAPTER_FIELDS)                                    \
    F(CAPABILITIES, XrTargetCapabilityRecord, XR_XTP_CAPABILITY_FIELDS)                          \
    F(COROUTINES, XrTargetCoroutineStateRecord, XR_XTP_COROUTINE_FIELDS)

#define XR_XTP_FIELD_SIZE_U8(field) +1u
#define XR_XTP_FIELD_SIZE_U16(field) +2u
#define XR_XTP_FIELD_SIZE_U32(field) +4u
#define XR_XTP_FIELD_SIZE_U64(field) +8u
#define XR_XTP_FIELD_SIZE_ID(field) +XR_STABLE_ID_BYTES
#define XR_XTP_FIELD_SIZE_FP(field) +XR_FINGERPRINT_BYTES
#define XR_XTP_FIELD_SIZE(kind, field) XR_XTP_FIELD_SIZE_##kind(field)
#define XR_XTP_DECLARE_SIZE(name, type, fields)                                                   \
    enum { XR_XTP_WIRE_##name##_SIZE = 0 fields(XR_XTP_FIELD_SIZE) };
XR_XTP_TYPED_ROWS(XR_XTP_DECLARE_SIZE)

#define XR_XTP_ENCODE_U8(field) *writer.cursor++ = (uint8_t) record->field;
#define XR_XTP_ENCODE_U16(field) xr_xtp_put_u16(writer.cursor, (uint16_t) record->field); writer.cursor += 2;
#define XR_XTP_ENCODE_U32(field) xr_xtp_put_u32(writer.cursor, (uint32_t) record->field); writer.cursor += 4;
#define XR_XTP_ENCODE_U64(field) xr_xtp_put_u64(writer.cursor, (uint64_t) record->field); writer.cursor += 8;
#define XR_XTP_ENCODE_ID(field) memcpy(writer.cursor, record->field.bytes, XR_STABLE_ID_BYTES); writer.cursor += XR_STABLE_ID_BYTES;
#define XR_XTP_ENCODE_FP(field) memcpy(writer.cursor, record->field.bytes, XR_FINGERPRINT_BYTES); writer.cursor += XR_FINGERPRINT_BYTES;
#define XR_XTP_ENCODE_FIELD(kind, field) XR_XTP_ENCODE_##kind(field)

#define XR_XTP_DECODE_U8(field) record->field = *reader.cursor++;
#define XR_XTP_DECODE_U16(field) record->field = xr_xtp_take_u16(reader.cursor); reader.cursor += 2;
#define XR_XTP_DECODE_U32(field) record->field = xr_xtp_take_u32(reader.cursor); reader.cursor += 4;
#define XR_XTP_DECODE_U64(field) record->field = xr_xtp_take_u64(reader.cursor); reader.cursor += 8;
#define XR_XTP_DECODE_ID(field) memcpy(record->field.bytes, reader.cursor, XR_STABLE_ID_BYTES); reader.cursor += XR_STABLE_ID_BYTES;
#define XR_XTP_DECODE_FP(field) memcpy(record->field.bytes, reader.cursor, XR_FINGERPRINT_BYTES); reader.cursor += XR_FINGERPRINT_BYTES;
#define XR_XTP_DECODE_FIELD(kind, field) XR_XTP_DECODE_##kind(field)

#define XR_XTP_DEFINE_ROW_CODECS(name, type, fields)                                              \
    static void encode_##name(const type *records, uint32_t count, uint8_t *bytes) {               \
        XrXtpRowWriter writer = {bytes};                                                           \
        for (uint32_t i = 0; i < count; i++) {                                                    \
            const type *record = &records[i];                                                      \
            fields(XR_XTP_ENCODE_FIELD)                                                           \
        }                                                                                          \
    }                                                                                              \
    static void decode_##name(const uint8_t *bytes, uint32_t count, type *records) {               \
        XrXtpRowReader reader = {bytes};                                                           \
        for (uint32_t i = 0; i < count; i++) {                                                    \
            type *record = &records[i];                                                            \
            fields(XR_XTP_DECODE_FIELD)                                                           \
        }                                                                                          \
    }
XR_XTP_TYPED_ROWS(XR_XTP_DEFINE_ROW_CODECS)

XR_FUNC uint32_t xr_xtp_wire_row_size(XrXtpSectionKind kind) {
    switch (kind) {
#define XR_XTP_SIZE_CASE(name, type, fields) case XR_XTP_SECTION_##name: return XR_XTP_WIRE_##name##_SIZE;
        XR_XTP_TYPED_ROWS(XR_XTP_SIZE_CASE)
        case XR_XTP_SECTION_ROOT_SLOTS: return 4u;
        default: return 0;
    }
}

XR_FUNC uint64_t xr_xtp_table_count_limit(XrXtpSectionKind kind) {
    switch (kind) {
        case XR_XTP_SECTION_TARGET_PROFILE: return 1;
        case XR_XTP_SECTION_MACHINE_REPS: return 256;
        case XR_XTP_SECTION_VALUE_REPS: return 40000000;
        case XR_XTP_SECTION_EXTENTS: return 1000000;
        case XR_XTP_SECTION_LAYOUTS: return 1000000;
        case XR_XTP_SECTION_FIELDS: return 16000000;
        case XR_XTP_SECTION_STORAGE: return 4000000;
        case XR_XTP_SECTION_ALLOCATIONS: return 10000000;
        case XR_XTP_SECTION_EXTENT_OPERANDS: return 40000000;
        case XR_XTP_SECTION_FUNCTIONS: return 100000;
        case XR_XTP_SECTION_SLOTS: return 16000000;
        case XR_XTP_SECTION_INSTRUCTIONS: return 40000000;
        case XR_XTP_SECTION_CALLS: return 10000000;
        case XR_XTP_SECTION_CALL_ARGUMENTS: return 40000000;
        case XR_XTP_SECTION_ROOT_MAPS: return 10000000;
        case XR_XTP_SECTION_ROOT_SLOTS: return 40000000;
        case XR_XTP_SECTION_CLEANUPS: return 40000000;
        case XR_XTP_SECTION_ADAPTERS: return 1000000;
        case XR_XTP_SECTION_CAPABILITIES: return 65536;
        case XR_XTP_SECTION_COROUTINES: return 10000000;
        default: return 0;
    }
}

XR_FUNC bool xr_xtp_encode_rows(XrXtpSectionKind kind, const void *rows, uint32_t count,
                                uint8_t *bytes) {
    if (count && (!rows || !bytes))
        return false;
    switch (kind) {
#define XR_XTP_ENCODE_CASE(name, type, fields)                                                     \
        case XR_XTP_SECTION_##name: encode_##name((const type *) rows, count, bytes); return true;
        XR_XTP_TYPED_ROWS(XR_XTP_ENCODE_CASE)
        case XR_XTP_SECTION_ROOT_SLOTS: {
            const uint32_t *slots = (const uint32_t *) rows;
            for (uint32_t i = 0; i < count; i++)
                xr_xtp_put_u32(bytes + (size_t) i * 4u, slots[i]);
            return true;
        }
        default: return false;
    }
}

XR_FUNC bool xr_xtp_decode_rows(XrXtpSectionKind kind, const uint8_t *bytes, uint32_t count,
                                void *rows) {
    if (count && (!bytes || !rows))
        return false;
    switch (kind) {
#define XR_XTP_DECODE_CASE(name, type, fields)                                                     \
        case XR_XTP_SECTION_##name: decode_##name(bytes, count, (type *) rows); return true;
        XR_XTP_TYPED_ROWS(XR_XTP_DECODE_CASE)
        case XR_XTP_SECTION_ROOT_SLOTS: {
            uint32_t *slots = (uint32_t *) rows;
            for (uint32_t i = 0; i < count; i++)
                slots[i] = xr_xtp_take_u32(bytes + (size_t) i * 4u);
            return true;
        }
        default: return false;
    }
}
