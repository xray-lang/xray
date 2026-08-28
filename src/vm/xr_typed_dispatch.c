/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_dispatch.c - Typed TargetPlan scalar executor
 *
 * KEY CONCEPT:
 *   The dispatcher consumes only one verified TargetPlan and exact typed
 *   slots. Program plans enter through their graph-owned entry function and
 *   direct calls stay inside the same global row namespace. The dispatcher
 *   does not inspect SemanticPlan or Xi and has no legacy bytecode, AOT, or
 *   CGen fallback.
 */

#include "xr_typed_dispatch.h"
#include "xr_typed_frame.h"
#include "xr_vm_decoded_cache.h"
#include "debug/xr_vm_debug_control_internal.h"
#include "debug/xr_vm_trace_internal.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../plan/target/xr_target_plan_internal.h"
#include "../plan/target/xr_target_profile.h"
#include "../plan/target/xr_target_verify.h"
#include "../runtime/value/xtransfer_mode.h"
#include "../shared/xr_bits_core.h"
#include "../shared/xr_compare_core.h"
#include "../shared/xr_int_arith_core.h"
#include "../shared/xr_arith_core.h"
#include "../shared/xr_semantic_owner_ids_gen.h"
#include "../stdlib/xstdlib_target_leaf.h"
#include "../base/xmalloc.h"
#include <stddef.h>
#include <string.h>

_Static_assert(XR_VM_DEBUG_MAX_STACK_DEPTH >= XR_TYPED_DISPATCH_MAX_CALL_DEPTH,
               "debug stack must cover the complete typed call stack");
_Static_assert(sizeof(XrTypedLeafAggregateI64x2) == 16u,
               "leaf aggregate request carrier must stay exactly 16 bytes");
_Static_assert(_Alignof(XrTypedLeafAggregateI64x2) == _Alignof(int64_t),
               "leaf aggregate request carrier must keep i64 alignment");
_Static_assert(sizeof(XrTypedLeafValueProductTuple6) == 48u,
               "leaf value product carrier must stay exactly 48 bytes");
_Static_assert(_Alignof(XrTypedLeafValueProductTuple6) == _Alignof(int64_t),
               "leaf value product carrier must keep i64 alignment");
_Static_assert(offsetof(XrTypedLeafValueProductTuple6, field0) == 0u &&
                   offsetof(XrTypedLeafValueProductTuple6, field1) == 8u &&
                   offsetof(XrTypedLeafValueProductTuple6, field2) == 16u &&
                   offsetof(XrTypedLeafValueProductTuple6, field3) == 24u &&
                   offsetof(XrTypedLeafValueProductTuple6, field4) == 32u &&
                   offsetof(XrTypedLeafValueProductTuple6, field5) == 40u,
               "leaf value product carrier offsets must match schema-55 x64 layout");

typedef union XrTypedAggregateValue {
    XrTypedLeafAggregateI64x2 pair;
    XrTypedLeafValueProductTuple6 product;
} XrTypedAggregateValue;

/* Scalar dispatch owns no lifecycle executor.  Prove that boundary from the
 * verified plan before a frame exists, then repeat it from the allocated
 * frame's exact footprint before destruction. */
static bool function_has_zero_lifecycle(const XrTargetPlan *plan, uint32_t function) {
    uint32_t function_count = 0;
    uint32_t root_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t coroutine_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &function_count);
    (void) xr_target_plan_root_maps(plan, &root_count);
    (void) xr_target_plan_cleanups(plan, &cleanup_count);
    (void) xr_target_plan_coroutines(plan, &coroutine_count);
    const XrTargetFunctionRecord *record =
        functions && function < function_count ? &functions[function] : NULL;
    return record && record->id == function && record->root_begin <= root_count &&
           record->root_count <= root_count - record->root_begin &&
           record->cleanup_begin <= cleanup_count &&
           record->cleanup_count <= cleanup_count - record->cleanup_begin &&
           record->coroutine_begin <= coroutine_count &&
           record->coroutine_count <= coroutine_count - record->coroutine_begin &&
           record->root_count == 0 && record->cleanup_count == 0 && record->coroutine_count == 0;
}

/* A program graph has one externally callable root. Callee rows remain
 * ordinary global function indexes reached only by verified CALL_DIRECT_I64
 * instructions; accepting one of them as a second request root would create
 * an executor-owned graph entry policy. A cold execution repeats the
 * independent verifier. A decoded cache already carries that proof for the
 * exact retained plan, while the intact fingerprint check below still rejects
 * in-place mutation. */
static XrTypedDispatchStatus
require_program_graph_entry(const XrTargetPlan *plan, uint32_t function, bool decoded_cache_exact) {
    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    const XrTargetProgramGraphRecord *graphs = xr_target_plan_program_graphs(plan, &graph_count);
    (void) xr_target_plan_module_partitions(plan, &partition_count);
    if (graph_count == 0 && partition_count == 0)
        return XR_TYPED_DISPATCH_OK;
    if (!xr_target_plan_fingerprint_is_intact(plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!decoded_cache_exact) {
        char error[512] = {0};
        if (!xr_target_plan_verify(plan, error, sizeof(error)))
            return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    }
    if (!graphs || graph_count != 1u || partition_count != 2u || graphs[0].module_count != 2u)
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    return function == graphs[0].entry_target_function ? XR_TYPED_DISPATCH_OK
                                                       : XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
}

static bool function_has_zero_managed_lifecycle(const XrTargetPlan *plan, uint32_t function) {
    uint32_t function_count = 0;
    uint32_t root_count = 0;
    uint32_t cleanup_count = 0;
    uint32_t coroutine_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &function_count);
    (void) xr_target_plan_root_maps(plan, &root_count);
    (void) xr_target_plan_cleanups(plan, &cleanup_count);
    (void) xr_target_plan_coroutines(plan, &coroutine_count);
    const XrTargetFunctionRecord *record =
        functions && function < function_count ? &functions[function] : NULL;
    return record && record->id == function && record->root_begin <= root_count &&
           record->root_count <= root_count - record->root_begin &&
           record->cleanup_begin <= cleanup_count &&
           record->cleanup_count <= cleanup_count - record->cleanup_begin &&
           record->coroutine_begin < coroutine_count &&
           record->coroutine_count <= coroutine_count - record->coroutine_begin &&
           record->root_count == 0 && record->cleanup_count == 0 && record->coroutine_count != 0;
}

static XrTypedDispatchStatus free_scalar_frame(XrTypedFrame **frame) {
    if (!frame)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (!*frame)
        return XR_TYPED_DISPATCH_OK;
    XrTypedFrameMemoryFootprint footprint = {0};
    if (xr_typed_frame_memory_footprint(*frame, &footprint) != XR_TYPED_FRAME_OK ||
        footprint.lifecycle_state_metadata_bytes != 0)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    return xr_typed_frame_free(frame) == XR_TYPED_FRAME_OK ? XR_TYPED_DISPATCH_OK
                                                           : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus describe_i64(XrTypedFrame *frame, uint32_t slot,
                                          XrTypedSlotAccess *access) {
    if (xr_typed_frame_describe_slot(frame, slot, access) != XR_TYPED_FRAME_OK ||
        access->size != sizeof(uint64_t) || access->alignment != sizeof(uint64_t))
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    return XR_TYPED_DISPATCH_OK;
}

/* The truth slot a comparison writes. It is one byte, so it is the second and
 * only other width this executor reads; the width comes from the opcode, and
 * this repeats the plan's own answer rather than choosing one. */
static XrTypedDispatchStatus describe_bool(XrTypedFrame *frame, uint32_t slot,
                                           XrTypedSlotAccess *access) {
    if (xr_typed_frame_describe_slot(frame, slot, access) != XR_TYPED_FRAME_OK ||
        access->size != sizeof(uint8_t) || access->alignment != sizeof(uint8_t))
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus load_i64_bits(XrTypedFrame *frame, uint32_t slot, uint64_t *bits) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_i64(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_load(frame, &access, bits, sizeof(*bits)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus store_i64_bits(XrTypedFrame *frame, uint32_t slot, uint64_t bits) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_i64(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_store(frame, &access, &bits, sizeof(bits)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static const XrTargetSlotRecord *target_slot_record(const XrTargetPlan *plan, uint32_t slot) {
    uint32_t count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &count);
    return slots && slot < count && slots[slot].id == slot ? &slots[slot] : NULL;
}

static bool target_value_rep_binds_slot(const XrTargetPlan *plan, const XrTargetSlotRecord *slot) {
    const XrTargetValueRepRecord *value = slot && slot->semantic_value != XR_SEMANTIC_INDEX_NONE
                                              ? xr_target_plan_value_rep(plan, slot->semantic_value)
                                              : NULL;
    return value && value->semantic_value == slot->semantic_value && value->slot == slot->id &&
           value->register_rep == slot->register_rep && value->memory_rep == slot->memory_rep;
}

static bool target_i64_rep_is_exact(const XrTargetPlan *plan, uint16_t rep_index) {
    const XrTargetMachineRepRecord *rep = xr_target_plan_machine_rep(plan, rep_index);
    return rep && rep->kind == XR_MACHINE_REP_I64 && rep->register_bits == 64 &&
           rep->memory_size == 8 && rep->memory_align == 8 &&
           rep->root_kind == XR_TARGET_ROOT_NONE && rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE;
}

static bool target_u8_rep_is_exact(const XrTargetPlan *plan, uint16_t rep_index) {
    const XrTargetMachineRepRecord *rep = xr_target_plan_machine_rep(plan, rep_index);
    return rep && rep->kind == XR_MACHINE_REP_U8 && rep->register_bits == 8 &&
           rep->memory_size == 1 && rep->memory_align == 1 &&
           rep->root_kind == XR_TARGET_ROOT_NONE && rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE;
}

static XrTypedDispatchStatus describe_exact_u8(const XrTargetPlan *plan, XrTypedFrame *frame,
                                               uint32_t slot, XrTypedSlotAccess *access) {
    const XrTargetSlotRecord *record = target_slot_record(plan, slot);
    if (!record || !target_value_rep_binds_slot(plan, record) ||
        record->register_rep != record->memory_rep ||
        !target_u8_rep_is_exact(plan, record->register_rep) ||
        xr_typed_frame_describe_slot(frame, slot, access) != XR_TYPED_FRAME_OK ||
        access->size != 1u || access->alignment != 1u ||
        access->register_rep != record->register_rep || access->memory_rep != record->memory_rep)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus describe_exact_i64(const XrTargetPlan *plan, XrTypedFrame *frame,
                                                uint32_t slot, XrTypedSlotAccess *access) {
    const XrTargetSlotRecord *record = target_slot_record(plan, slot);
    if (!record || !target_value_rep_binds_slot(plan, record) ||
        record->register_rep != record->memory_rep ||
        !target_i64_rep_is_exact(plan, record->register_rep) ||
        describe_i64(frame, slot, access) != XR_TYPED_DISPATCH_OK ||
        access->register_rep != record->register_rep || access->memory_rep != record->memory_rep)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    return XR_TYPED_DISPATCH_OK;
}

typedef struct XrTypedLeafAggregateSlot {
    XrTypedSlotAccess access;
    const XrTargetSlotRecord *slot;
    const XrTargetLayoutRecord *layout;
    uint32_t layout_index;
} XrTypedLeafAggregateSlot;

static bool target_leaf_field_is_exact(const XrTargetPlan *plan, const XrTargetFieldRecord *field,
                                       uint32_t layout, uint32_t ordinal) {
    return field && field->layout == layout && field->semantic_field == ordinal &&
           field->semantic_name == XR_SEMANTIC_INDEX_NONE &&
           field->offset == ordinal * sizeof(int64_t) && field->size == sizeof(int64_t) &&
           field->align == _Alignof(int64_t) && target_i64_rep_is_exact(plan, field->memory_rep) &&
           field->root_kind == XR_TARGET_ROOT_NONE && field->flags == 0 && field->reserved == 0;
}

static XrTypedDispatchStatus describe_leaf_aggregate(const XrTargetPlan *plan, XrTypedFrame *frame,
                                                     uint32_t slot, XrTypedLeafAggregateSlot *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !frame || !out)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    const XrTargetSlotRecord *record = target_slot_record(plan, slot);
    XrTypedSlotAccess access = {0};
    if (!record || !target_value_rep_binds_slot(plan, record) ||
        record->size != sizeof(XrTypedLeafAggregateI64x2) ||
        record->align != _Alignof(XrTypedLeafAggregateI64x2) ||
        record->register_rep != record->memory_rep || record->root_kind != XR_TARGET_ROOT_NONE ||
        record->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        xr_typed_frame_describe_slot(frame, slot, &access) != XR_TYPED_FRAME_OK ||
        access.size != sizeof(XrTypedLeafAggregateI64x2) ||
        access.alignment != _Alignof(XrTypedLeafAggregateI64x2) ||
        access.register_rep != record->register_rep || access.memory_rep != record->memory_rep)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    const XrTargetMachineRepRecord *rep = xr_target_plan_machine_rep(plan, record->memory_rep);
    uint32_t layout_count = 0;
    uint32_t field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    uint32_t layout_index = rep ? rep->detail : XR_SEMANTIC_INDEX_NONE;
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count && layouts[layout_index].id == layout_index
            ? &layouts[layout_index]
            : NULL;
    if (!rep || rep->kind != XR_MACHINE_REP_AGGREGATE || rep->register_bits != 128 ||
        rep->memory_size != sizeof(XrTypedLeafAggregateI64x2) ||
        rep->memory_align != _Alignof(XrTypedLeafAggregateI64x2) ||
        rep->root_kind != XR_TARGET_ROOT_NONE || rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE || rep->lane_count != 0 || !layout ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->fixed_prefix_size != sizeof(XrTypedLeafAggregateI64x2) ||
        layout->align != _Alignof(XrTypedLeafAggregateI64x2) || layout->field_count != 2 ||
        layout->root_field_count != 0 || !fields || layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        !target_leaf_field_is_exact(plan, &fields[layout->field_begin], layout_index, 0) ||
        !target_leaf_field_is_exact(plan, &fields[layout->field_begin + 1u], layout_index, 1))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    out->access = access;
    out->slot = record;
    out->layout = layout;
    out->layout_index = layout_index;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus load_leaf_aggregate(const XrTargetPlan *plan, XrTypedFrame *frame,
                                                 uint32_t slot, XrTypedLeafAggregateI64x2 *value,
                                                 XrTypedLeafAggregateSlot *described) {
    XrTypedLeafAggregateSlot local = {0};
    XrTypedDispatchStatus status = describe_leaf_aggregate(plan, frame, slot, &local);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    if (xr_typed_frame_load(frame, &local.access, value, sizeof(*value)) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (described)
        *described = local;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus store_leaf_aggregate(const XrTargetPlan *plan, XrTypedFrame *frame,
                                                  uint32_t slot,
                                                  const XrTypedLeafAggregateI64x2 *value,
                                                  XrTypedLeafAggregateSlot *described) {
    XrTypedLeafAggregateSlot local = {0};
    XrTypedDispatchStatus status = describe_leaf_aggregate(plan, frame, slot, &local);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    if (xr_typed_frame_store(frame, &local.access, value, sizeof(*value)) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (described)
        *described = local;
    return XR_TYPED_DISPATCH_OK;
}

typedef struct XrTypedLeafProductSlot {
    XrTypedSlotAccess access;
    const XrTargetLayoutRecord *layout;
    uint32_t layout_index;
} XrTypedLeafProductSlot;

static bool target_product_field_is_exact(const XrTargetPlan *plan,
                                          const XrTargetFieldRecord *field, uint32_t layout,
                                          uint32_t ordinal) {
    bool u8 = ordinal == 2u;
    return field && field->layout == layout && field->semantic_field == ordinal &&
           field->semantic_name == XR_SEMANTIC_INDEX_NONE && field->offset == ordinal * 8u &&
           field->size == (u8 ? 1u : 8u) && field->align == (u8 ? 1u : 8u) &&
           (u8 ? target_u8_rep_is_exact(plan, field->memory_rep)
               : target_i64_rep_is_exact(plan, field->memory_rep)) &&
           field->root_kind == XR_TARGET_ROOT_NONE && field->flags == 0 && field->reserved == 0;
}

static XrTypedDispatchStatus describe_leaf_product(const XrTargetPlan *plan, XrTypedFrame *frame,
                                                   uint32_t slot, XrTypedLeafProductSlot *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrTargetSlotRecord *record = target_slot_record(plan, slot);
    XrTypedSlotAccess access = {0};
    if (!plan || !frame || !out || !record || !target_value_rep_binds_slot(plan, record) ||
        record->size != 48u || record->align != 8u || record->register_rep != record->memory_rep ||
        record->root_kind != XR_TARGET_ROOT_NONE ||
        record->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        xr_typed_frame_describe_slot(frame, slot, &access) != XR_TYPED_FRAME_OK ||
        access.size != 48u || access.alignment != 8u ||
        access.register_rep != record->register_rep || access.memory_rep != record->memory_rep)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    const XrTargetMachineRepRecord *rep = xr_target_plan_machine_rep(plan, record->memory_rep);
    uint32_t layout_count = 0, field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    uint32_t layout_index = rep ? rep->detail : XR_SEMANTIC_INDEX_NONE;
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count && layouts[layout_index].id == layout_index
            ? &layouts[layout_index]
            : NULL;
    if (!rep || rep->kind != XR_MACHINE_REP_AGGREGATE || rep->register_bits != 384u ||
        rep->memory_size != 48u || rep->memory_align != 8u ||
        rep->root_kind != XR_TARGET_ROOT_NONE || rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE || rep->lane_count != 0 || !layout ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->fixed_prefix_size != 48u ||
        layout->align != 8u || layout->field_count != 6u || layout->root_field_count != 0 ||
        !fields || layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    for (uint32_t ordinal = 0; ordinal < 6u; ordinal++)
        if (!target_product_field_is_exact(plan, &fields[layout->field_begin + ordinal],
                                           layout_index, ordinal))
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    out->access = access;
    out->layout = layout;
    out->layout_index = layout_index;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus load_leaf_product(const XrTargetPlan *plan, XrTypedFrame *frame,
                                               uint32_t slot, XrTypedLeafValueProductTuple6 *value,
                                               XrTypedLeafProductSlot *described) {
    XrTypedLeafProductSlot local = {0};
    XrTypedDispatchStatus status = describe_leaf_product(plan, frame, slot, &local);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    if (xr_typed_frame_load(frame, &local.access, value, sizeof(*value)) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (described)
        *described = local;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus store_leaf_product(const XrTargetPlan *plan, XrTypedFrame *frame,
                                                uint32_t slot,
                                                const XrTypedLeafValueProductTuple6 *value) {
    XrTypedLeafProductSlot local = {0};
    XrTypedDispatchStatus status = describe_leaf_product(plan, frame, slot, &local);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_store(frame, &local.access, value, sizeof(*value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus load_bool_byte(XrTypedFrame *frame, uint32_t slot, uint8_t *byte) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_bool(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_load(frame, &access, byte, sizeof(*byte)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus store_bool_byte(XrTypedFrame *frame, uint32_t slot, uint8_t byte) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_bool(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_store(frame, &access, &byte, sizeof(byte)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus describe_value(XrTypedFrame *frame, uint32_t slot,
                                            XrTypedSlotAccess *access) {
    if (xr_typed_frame_describe_slot(frame, slot, access) != XR_TYPED_FRAME_OK ||
        access->size != sizeof(XrValue) || access->alignment != _Alignof(XrValue))
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus load_value(XrTypedFrame *frame, uint32_t slot, XrValue *value) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_value(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_load(frame, &access, value, sizeof(*value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus store_value(XrTypedFrame *frame, uint32_t slot, XrValue value) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_value(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_store(frame, &access, &value, sizeof(value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus take_owned_value(XrTypedFrame *frame, uint32_t slot, XrValue *value) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_value(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_take_owned(frame, &access, value, sizeof(*value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus restore_owned_value(XrTypedFrame *frame, uint32_t slot,
                                                 XrValue value) {
    XrTypedSlotAccess access = {0};
    XrTypedDispatchStatus status = describe_value(frame, slot, &access);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    return xr_typed_frame_restore_owned(frame, &access, &value, sizeof(value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

typedef struct XrTypedDispatchRowContext {
    const int64_t *arguments;
    const XrTypedLeafAggregateI64x2 *aggregate_arguments;
    uint32_t argument_count;
    uint32_t row_count;
    uint32_t *next;
    bool *returned;
    uint64_t *return_bits;
    XrTypedAggregateValue *aggregate_return;
    struct XrTypedDispatchExecution *execution;
    const XrVmDecodedInstruction *decoded;
    bool parameters_prebound;
    uint32_t frame_id;
} XrTypedDispatchRowContext;

typedef XrTypedDispatchStatus (*XrTypedDispatchRowHandler)(
    XrTypedFrame *frame, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract, XrTypedDispatchRowContext *context);

typedef struct XrTypedDispatchFunctionBinding {
    XrTypedDispatchRowHandler handler;
    uint8_t dispatch_kind;
    uint8_t dispatch_argument;
} XrTypedDispatchFunctionBinding;

typedef struct XrTypedDispatchExecution {
    const XrTargetPlan *plan;
    const XrFingerprint *fingerprint;
    XrTypedFrameLimits limits;
    uint32_t remaining_steps;
    uint32_t call_depth;
    uint32_t next_frame_id;
    uint64_t next_event_ordinal;
    const XrVmDebugSession *debug_session;
    const XrVmDecodedCache *decoded_cache;
    const XrVmDynamicEntryContext *dynamic_entries;
    XrModuleGenerationIdentity generation_identity;
    bool generation_identity_present;
    bool use_dynamic_entry_cache;
    bool row_suspended;
    uint32_t row_suspend_state;
    uint32_t resume_start_instruction;
    XrTypedDispatchProvider provider;
    XrValue *value_arguments;
    uint32_t value_argument_count;
    XrTypedArrayPushKernel array_push;
} XrTypedDispatchExecution;

struct XrTypedCoroutineI64 {
    XrTargetPlan *plan;
    XrFingerprint fingerprint;
    XrVmDecodedCache *cache;
    XrTypedFrame *frame;
    int64_t *arguments;
    XrTypedDispatchExecution execution;
    uint64_t return_bits;
    uint32_t function;
    uint32_t argument_count;
    uint32_t next_instruction;
    uint32_t suspended_state;
    bool started;
    bool suspended;
    bool terminal;
};

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static bool generation_identity_equal(const XrModuleGenerationIdentity *left,
                                      const XrModuleGenerationIdentity *right) {
    return left && right && left->schema_version == right->schema_version &&
           left->target_plan_schema_version == right->target_plan_schema_version &&
           left->generation_number == right->generation_number &&
           left->completed_family_mask == right->completed_family_mask &&
           left->required_capability_mask == right->required_capability_mask &&
           memcmp(left->semantic_fingerprint, right->semantic_fingerprint,
                  sizeof(left->semantic_fingerprint)) == 0 &&
           memcmp(left->program_fingerprint, right->program_fingerprint,
                  sizeof(left->program_fingerprint)) == 0 &&
           memcmp(left->program_module_set_fingerprint, right->program_module_set_fingerprint,
                  sizeof(left->program_module_set_fingerprint)) == 0 &&
           memcmp(left->generation_closure_id, right->generation_closure_id,
                  sizeof(left->generation_closure_id)) == 0 &&
           memcmp(left->target_profile_fingerprint, right->target_profile_fingerprint,
                  sizeof(left->target_profile_fingerprint)) == 0 &&
           memcmp(left->target_plan_fingerprint, right->target_plan_fingerprint,
                  sizeof(left->target_plan_fingerprint)) == 0 &&
           memcmp(left->runtime_abi_fingerprint, right->runtime_abi_fingerprint,
                  sizeof(left->runtime_abi_fingerprint)) == 0 &&
           memcmp(left->provider_set_fingerprint, right->provider_set_fingerprint,
                  sizeof(left->provider_set_fingerprint)) == 0 &&
           memcmp(left->object_header_fingerprint, right->object_header_fingerprint,
                  sizeof(left->object_header_fingerprint)) == 0 &&
           memcmp(left->generation_fingerprint, right->generation_fingerprint,
                  sizeof(left->generation_fingerprint)) == 0;
}

static bool generation_identity_matches_plan(const XrModuleGenerationIdentity *identity,
                                             const XrTargetPlan *plan,
                                             XrFingerprint plan_fingerprint) {
    if (!identity)
        return false;
    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    XrFingerprint semantic = xr_target_plan_semantic_fingerprint(plan);
    XrFingerprint profile_fingerprint =
        profile ? xr_target_profile_fingerprint(profile) : (XrFingerprint) {{0}};
    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    const XrTargetProgramGraphRecord *graphs = xr_target_plan_program_graphs(plan, &graph_count);
    (void) xr_target_plan_module_partitions(plan, &partition_count);
    bool program_identity_exact = false;
    if (graph_count == 0 && partition_count == 0) {
        program_identity_exact =
            bytes_are_zero(identity->program_fingerprint, sizeof(identity->program_fingerprint)) &&
            bytes_are_zero(identity->program_module_set_fingerprint,
                           sizeof(identity->program_module_set_fingerprint)) &&
            bytes_are_zero(identity->generation_closure_id,
                           sizeof(identity->generation_closure_id));
    } else if (graphs && graph_count == 1u && partition_count == 2u &&
               graphs[0].module_count == partition_count) {
        XrFingerprint module_set_fingerprint = {{0}};
        program_identity_exact =
            xr_target_plan_program_module_set_fingerprint(plan, &module_set_fingerprint) &&
            memcmp(identity->program_fingerprint, graphs[0].program_fingerprint.bytes,
                   sizeof(identity->program_fingerprint)) == 0 &&
            memcmp(identity->program_module_set_fingerprint, module_set_fingerprint.bytes,
                   sizeof(identity->program_module_set_fingerprint)) == 0 &&
            memcmp(identity->generation_closure_id, graphs[0].generation_identity.bytes,
                   sizeof(identity->generation_closure_id)) == 0;
    }
    return profile && program_identity_exact &&
           identity->schema_version == XR_RUNTIME_GENERATION_SCHEMA_VERSION &&
           identity->target_plan_schema_version == xr_target_plan_schema_version(plan) &&
           identity->completed_family_mask == xr_target_plan_completed_family_mask(plan) &&
           memcmp(identity->semantic_fingerprint, semantic.bytes, sizeof(semantic.bytes)) == 0 &&
           memcmp(identity->target_profile_fingerprint, profile_fingerprint.bytes,
                  sizeof(profile_fingerprint.bytes)) == 0 &&
           memcmp(identity->target_plan_fingerprint, plan_fingerprint.bytes,
                  sizeof(plan_fingerprint.bytes)) == 0 &&
           !bytes_are_zero(identity->generation_fingerprint,
                           sizeof(identity->generation_fingerprint));
}

static XrTypedDispatchStatus
execute_function(XrTypedDispatchExecution *execution, XrTypedFrame *frame, uint32_t function,
                 const int64_t *arguments, const XrTypedLeafAggregateI64x2 *aggregate_arguments,
                 uint32_t argument_count, bool parameters_prebound, uint32_t frame_id,
                 uint32_t parent_frame_id, uint64_t *return_bits,
                 XrTypedAggregateValue *aggregate_return);

static XrVmTraceEvent make_trace_event(XrVmTraceEventKind kind, uint32_t function, uint32_t frame,
                                       uint32_t parent_frame, uint32_t frame_depth) {
    XrVmTraceEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = (uint8_t) kind;
    event.opcode = XR_TARGET_INSTRUCTION_INVALID;
    event.function = function;
    event.related_function = XR_VM_TRACE_ID_NONE;
    event.instruction = XR_VM_TRACE_ID_NONE;
    event.block = XR_VM_TRACE_ID_NONE;
    event.call = XR_VM_TRACE_ID_NONE;
    event.frame = frame;
    event.parent_frame = parent_frame;
    event.related_frame = XR_VM_TRACE_ID_NONE;
    event.frame_depth = frame_depth;
    return event;
}

static XrTypedDispatchStatus emit_trace_event(XrTypedDispatchExecution *execution,
                                              XrVmTraceEvent *event) {
    if (!execution->debug_session)
        return XR_TYPED_DISPATCH_OK;
    if (!xr_typed_debug_attach_event_facts(execution->plan, event))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (!xr_typed_debug_emit(
            execution->debug_session, execution->fingerprint,
            execution->generation_identity_present ? &execution->generation_identity : NULL,
            execution->next_event_ordinal, event))
        return XR_TYPED_DISPATCH_TRACE_REJECTED;
    execution->next_event_ordinal++;
    if (event->kind == XR_VM_TRACE_INSTRUCTION && execution->debug_session->control) {
        XrVmDebugControlEventStatus control_status =
            xr_typed_debug_control_instruction(execution->debug_session->control, event);
        if (control_status == XR_VM_DEBUG_CONTROL_EVENT_TERMINATED)
            return XR_TYPED_DISPATCH_DEBUG_TERMINATED;
        if (control_status == XR_VM_DEBUG_CONTROL_EVENT_STOP_REJECTED)
            return XR_TYPED_DISPATCH_DEBUG_STOP_REJECTED;
        if (control_status != XR_VM_DEBUG_CONTROL_EVENT_OK)
            return XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR;
    }
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus execute_const(XrTypedFrame *frame,
                                           const XrTargetInstructionRecord *row,
                                           const XrTargetInstructionContract *contract,
                                           XrTypedDispatchRowContext *context) {
    (void) contract;
    (void) context;
    return store_i64_bits(frame, row->result_slot, row->immediate_bits);
}

static XrTypedDispatchStatus execute_param(XrTypedFrame *frame,
                                           const XrTargetInstructionRecord *row,
                                           const XrTargetInstructionContract *contract,
                                           XrTypedDispatchRowContext *context) {
    if (context->parameters_prebound)
        return XR_TYPED_DISPATCH_OK;
    if (contract->result_rep == XR_TARGET_INSTRUCTION_REP_AGGREGATE) {
        if (!context->aggregate_arguments || row->immediate_bits >= context->argument_count ||
            !context->execution)
            return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
        return store_leaf_aggregate(context->execution->plan, frame, row->result_slot,
                                    &context->aggregate_arguments[row->immediate_bits], NULL);
    }
    if (contract->result_rep == XR_TARGET_INSTRUCTION_REP_DYN_VALUE) {
        XrTypedDispatchExecution *execution = context->execution;
        if (!execution || !execution->value_arguments ||
            row->immediate_bits >= execution->value_argument_count)
            return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
        XrValue *argument = &execution->value_arguments[row->immediate_bits];
        XrTypedDispatchStatus status = store_value(frame, row->result_slot, *argument);
        if (status == XR_TYPED_DISPATCH_OK &&
            contract->result_ownership == XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_OWNED)
            *argument = xr_null();
        return status;
    }
    if (!context->arguments || row->immediate_bits >= context->argument_count)
        return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
    uint64_t bits = 0;
    memcpy(&bits, &context->arguments[row->immediate_bits], sizeof(bits));
    return store_i64_bits(frame, row->result_slot, bits);
}

static XrTypedDispatchStatus execute_aggregate_get(XrTypedFrame *frame,
                                                   const XrTargetInstructionRecord *row,
                                                   const XrTargetInstructionContract *contract,
                                                   XrTypedDispatchRowContext *context) {
    if (!contract || !context || !context->execution ||
        contract->result_rep != XR_TARGET_INSTRUCTION_REP_I64 ||
        contract->operand_rep[0] != XR_TARGET_INSTRUCTION_REP_AGGREGATE ||
        row->operand_count != 1 || row->immediate_bits > UINT32_MAX)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    bool product =
        xr_target_plan_function_execution_family_mask(context->execution->plan, row->function) ==
        XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6;
    XrTypedLeafAggregateI64x2 aggregate = {{0, 0}};
    XrTypedLeafAggregateSlot source = {0};
    XrTypedLeafValueProductTuple6 product_value = {0};
    XrTypedLeafProductSlot product_source = {0};
    XrTypedDispatchStatus status =
        product ? load_leaf_product(context->execution->plan, frame, row->operand_slots[0],
                                    &product_value, &product_source)
                : load_leaf_aggregate(context->execution->plan, frame, row->operand_slots[0],
                                      &aggregate, &source);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    uint32_t field_count = 0;
    const XrTargetFieldRecord *fields =
        xr_target_plan_fields(context->execution->plan, &field_count);
    uint32_t field_index = (uint32_t) row->immediate_bits;
    const XrTargetLayoutRecord *source_layout = product ? product_source.layout : source.layout;
    uint32_t source_layout_index = product ? product_source.layout_index : source.layout_index;
    if (!fields || field_index >= field_count || field_index < source_layout->field_begin ||
        field_index - source_layout->field_begin >= source_layout->field_count)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    const XrTargetFieldRecord *field = &fields[field_index];
    uint32_t ordinal = field_index - source_layout->field_begin;
    if (product ? (ordinal == 2u || !target_product_field_is_exact(context->execution->plan, field,
                                                                   source_layout_index, ordinal))
                : !target_leaf_field_is_exact(context->execution->plan, field, source_layout_index,
                                              ordinal))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedSlotAccess result = {0};
    if (describe_exact_i64(context->execution->plan, frame, row->result_slot, &result) !=
        XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint64_t bits = 0;
    memcpy(&bits,
           (product ? (const uint8_t *) &product_value : (const uint8_t *) &aggregate) +
               field->offset,
           sizeof(bits));
    return xr_typed_frame_store(frame, &result, &bits, sizeof(bits)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus execute_aggregate_make(XrTypedFrame *frame,
                                                    const XrTargetInstructionRecord *row,
                                                    const XrTargetInstructionContract *contract,
                                                    XrTypedDispatchRowContext *context) {
    if (!contract || !context || !context->execution ||
        contract->result_rep != XR_TARGET_INSTRUCTION_REP_AGGREGATE ||
        contract->operand_rep[0] != XR_TARGET_INSTRUCTION_REP_I64 ||
        contract->operand_rep[1] != XR_TARGET_INSTRUCTION_REP_I64 || row->operand_count != 2 ||
        row->immediate_bits > UINT32_MAX)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedLeafAggregateSlot destination = {0};
    XrTypedDispatchStatus status =
        describe_leaf_aggregate(context->execution->plan, frame, row->result_slot, &destination);
    if (status != XR_TYPED_DISPATCH_OK ||
        destination.layout_index != (uint32_t) row->immediate_bits)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedLeafAggregateI64x2 aggregate = {{0, 0}};
    uint32_t field_count = 0;
    const XrTargetFieldRecord *fields =
        xr_target_plan_fields(context->execution->plan, &field_count);
    if (!fields || destination.layout->field_begin > field_count ||
        destination.layout->field_count > field_count - destination.layout->field_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    for (uint32_t ordinal = 0; ordinal < 2; ordinal++) {
        XrTypedSlotAccess operand = {0};
        if (describe_exact_i64(context->execution->plan, frame, row->operand_slots[ordinal],
                               &operand) != XR_TYPED_DISPATCH_OK)
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
        uint64_t bits = 0;
        if (xr_typed_frame_load(frame, &operand, &bits, sizeof(bits)) != XR_TYPED_FRAME_OK)
            return XR_TYPED_DISPATCH_FRAME_ERROR;
        const XrTargetFieldRecord *field = &fields[destination.layout->field_begin + ordinal];
        if (!target_leaf_field_is_exact(context->execution->plan, field, destination.layout_index,
                                        ordinal))
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
        memcpy((uint8_t *) &aggregate + field->offset, &bits, sizeof(bits));
    }
    return xr_typed_frame_store(frame, &destination.access, &aggregate, sizeof(aggregate)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus execute_copy(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
                                          const XrTargetInstructionContract *contract,
                                          XrTypedDispatchRowContext *context) {
    (void) contract;
    (void) context;
    uint64_t bits = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &bits);
    return status == XR_TYPED_DISPATCH_OK ? store_i64_bits(frame, row->result_slot, bits) : status;
}

static XrTypedDispatchStatus execute_unary(XrTypedFrame *frame,
                                           const XrTargetInstructionRecord *row,
                                           const XrTargetInstructionContract *contract,
                                           XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t bits = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &bits);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    if (contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NEG)
        bits = (uint64_t) (0 - bits);
    else if (contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BNOT)
        bits = ~bits;
    else
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    return store_i64_bits(frame, row->result_slot, bits);
}

static XrTypedDispatchStatus execute_binary(XrTypedFrame *frame,
                                            const XrTargetInstructionRecord *row,
                                            const XrTargetInstructionContract *contract,
                                            XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    switch ((XrTargetInstructionDispatchArgument) contract->dispatch_argument) {
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_ADD:
            left += right;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_SUB:
            left -= right;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_MUL:
            left *= right;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BAND:
            left &= right;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOR:
            left |= right;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BXOR:
            left ^= right;
            break;
        default:
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    return store_i64_bits(frame, row->result_slot, left);
}

static XrTypedDispatchStatus execute_shift(XrTypedFrame *frame,
                                           const XrTargetInstructionRecord *row,
                                           const XrTargetInstructionContract *contract,
                                           XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t value = 0;
    int64_t count = 0;
    memcpy(&value, &left, sizeof(value));
    memcpy(&count, &right, sizeof(count));
    XrShiftKind kind = XR_SHIFT_LEFT;
    if (contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_RIGHT)
        kind = XR_SHIFT_RIGHT_SIGNED;
    else if (contract->dispatch_argument != XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LEFT)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    int64_t shifted =
        XR_SHIFT_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_SHIFT_HI, XR_SEM_OWNER_ID_SHARED_SHIFT_LO,
                             XR_SEM_CONSUMER_VM, kind, value, count);
    memcpy(&left, &shifted, sizeof(left));
    return store_i64_bits(frame, row->result_slot, left);
}

static XrTypedDispatchStatus execute_divmod(XrTypedFrame *frame,
                                            const XrTargetInstructionRecord *row,
                                            const XrTargetInstructionContract *contract,
                                            XrTypedDispatchRowContext *context) {
    (void) context;
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t dividend = 0;
    int64_t divisor = 0;
    memcpy(&dividend, &left, sizeof(dividend));
    memcpy(&divisor, &right, sizeof(divisor));
    bool dividing = contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_DIV;
    if (!dividing && contract->dispatch_argument != XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_MOD)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (divisor == 0) {
        if (contract->error_kind == XR_TARGET_INSTRUCTION_ERROR_DIVIDE_BY_ZERO)
            return XR_TYPED_DISPATCH_DIVIDE_BY_ZERO;
        if (contract->error_kind == XR_TARGET_INSTRUCTION_ERROR_MODULO_BY_ZERO)
            return XR_TYPED_DISPATCH_MODULO_BY_ZERO;
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    XrIntDivModResult evaluated =
        dividing ? XR_INT_DIV_MOD_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,
                                              XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,
                                              XR_SEM_CONSUMER_VM, XR_INT_DIV_MOD_DIV,
                                              XR_INT_DIV_MOD_PROOF_NONZERO, dividend, divisor)
                 : XR_INT_DIV_MOD_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_HI,
                                              XR_SEM_OWNER_ID_SHARED_INT_DIV_MOD_LO,
                                              XR_SEM_CONSUMER_VM, XR_INT_DIV_MOD_MOD,
                                              XR_INT_DIV_MOD_PROOF_NONZERO, dividend, divisor);
    int64_t computed = evaluated.value;
    memcpy(&left, &computed, sizeof(left));
    return store_i64_bits(frame, row->result_slot, left);
}

static XrTypedDispatchStatus execute_compare(XrTypedFrame *frame,
                                             const XrTargetInstructionRecord *row,
                                             const XrTargetInstructionContract *contract,
                                             XrTypedDispatchRowContext *context) {
    (void) context;
    XrCompareKind kind = XR_COMPARE_EQ;
    switch ((XrTargetInstructionDispatchArgument) contract->dispatch_argument) {
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_EQ:
            kind = XR_COMPARE_EQ;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NE:
            kind = XR_COMPARE_NE;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LT:
            kind = XR_COMPARE_LT;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LE:
            kind = XR_COMPARE_LE;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_GT:
            kind = XR_COMPARE_GT;
            break;
        case XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_GE:
            kind = XR_COMPARE_GE;
            break;
        default:
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    uint64_t left = 0;
    uint64_t right = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &left);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, row->operand_slots[1], &right);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t first = 0;
    int64_t second = 0;
    memcpy(&first, &left, sizeof(first));
    memcpy(&second, &right, sizeof(second));
    bool holds = XR_COMPARE_OWNER_APPLY_I64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,
                                            XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_VM,
                                            kind, first, second);
    return store_bool_byte(frame, row->result_slot, holds ? (uint8_t) 1u : (uint8_t) 0u);
}

static XrTypedDispatchStatus execute_overflow(XrTypedFrame *frame,
                                              const XrTargetInstructionRecord *row,
                                              const XrTargetInstructionContract *contract,
                                              XrTypedDispatchRowContext *context) {
    if (!context || !context->execution || !context->execution->plan ||
        contract->dispatch_kind != XR_TARGET_INSTRUCTION_DISPATCH_OVERFLOW ||
        contract->dispatch_argument != XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NONE ||
        row->immediate_bits > UINT32_MAX)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint32_t count = 0;
    const XrTargetI64OverflowPredicateRecord *predicates =
        xr_target_plan_i64_overflow_predicates(context->execution->plan, &count);
    uint32_t predicate_index = (uint32_t) row->immediate_bits;
    const XrTargetI64OverflowPredicateRecord *predicate =
        predicates && predicate_index < count ? &predicates[predicate_index] : NULL;
    if (!predicate || predicate->id != predicate_index || predicate->function != row->function ||
        predicate->result_slot != row->result_slot ||
        predicate->receiver_slot != row->operand_slots[0] ||
        predicate->argument_slot != row->operand_slots[1])
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint64_t receiver_bits = 0, argument_bits = 0;
    XrTypedDispatchStatus status = load_i64_bits(frame, predicate->receiver_slot, &receiver_bits);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    status = load_i64_bits(frame, predicate->argument_slot, &argument_bits);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    int64_t receiver = 0, argument = 0;
    memcpy(&receiver, &receiver_bits, sizeof(receiver));
    memcpy(&argument, &argument_bits, sizeof(argument));
    int holds = 0;
    switch ((XrTargetI64OverflowPredicateKind) predicate->kind) {
        case XR_TARGET_I64_OVERFLOW_PREDICATE_ADD:
            holds = xr_arith_core_add_overflows(receiver, argument);
            break;
        case XR_TARGET_I64_OVERFLOW_PREDICATE_SUB:
            holds = xr_arith_core_sub_overflows(receiver, argument);
            break;
        case XR_TARGET_I64_OVERFLOW_PREDICATE_MUL:
            holds = xr_arith_core_mul_overflows(receiver, argument);
            break;
        default:
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    return store_bool_byte(frame, predicate->result_slot, holds != 0);
}

static XrTypedDispatchStatus execute_return(XrTypedFrame *frame,
                                            const XrTargetInstructionRecord *row,
                                            const XrTargetInstructionContract *contract,
                                            XrTypedDispatchRowContext *context) {
    (void) contract;
    *context->returned = true;
    return load_i64_bits(frame, row->operand_slots[0], context->return_bits);
}

static XrTypedDispatchStatus execute_return_aggregate(XrTypedFrame *frame,
                                                      const XrTargetInstructionRecord *row,
                                                      const XrTargetInstructionContract *contract,
                                                      XrTypedDispatchRowContext *context) {
    if (!contract || !context || !context->execution || !context->aggregate_return ||
        contract->operand_rep[0] != XR_TARGET_INSTRUCTION_REP_AGGREGATE || row->operand_count != 1)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint64_t family =
        xr_target_plan_function_execution_family_mask(context->execution->plan, row->function);
    XrTypedDispatchStatus status =
        family == XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6
            ? load_leaf_product(context->execution->plan, frame, row->operand_slots[0],
                                &context->aggregate_return->product, NULL)
        : family == XR_TARGET_EXECUTION_LEAF_AGGREGATE_I64X2
            ? load_leaf_aggregate(context->execution->plan, frame, row->operand_slots[0],
                                  &context->aggregate_return->pair, NULL)
            : XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (status == XR_TYPED_DISPATCH_OK)
        *context->returned = true;
    return status;
}

static XrTypedDispatchStatus execute_return_unit(XrTypedFrame *frame,
                                                 const XrTargetInstructionRecord *row,
                                                 const XrTargetInstructionContract *contract,
                                                 XrTypedDispatchRowContext *context) {
    (void) frame;
    (void) row;
    if (!contract || contract->result_rep != XR_TARGET_INSTRUCTION_REP_NONE ||
        contract->arity != 0 || !context || !context->returned)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    *context->returned = true;
    *context->return_bits = 0;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus execute_const_u8(XrTypedFrame *frame,
                                              const XrTargetInstructionRecord *row,
                                              const XrTargetInstructionContract *contract,
                                              XrTypedDispatchRowContext *context) {
    if (!context || !context->execution || !contract || row->immediate_bits > UINT8_MAX ||
        contract->result_rep != XR_TARGET_INSTRUCTION_REP_U8 || row->operand_count != 0)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedSlotAccess result = {0};
    if (describe_exact_u8(context->execution->plan, frame, row->result_slot, &result) !=
        XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint8_t value = (uint8_t) row->immediate_bits;
    return xr_typed_frame_store(frame, &result, &value, sizeof(value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus execute_value_product_init(XrTypedFrame *frame,
                                                        const XrTargetInstructionRecord *row,
                                                        const XrTargetInstructionContract *contract,
                                                        XrTypedDispatchRowContext *context) {
    if (!context || !context->execution || !contract || row->immediate_bits > UINT32_MAX ||
        contract->result_rep != XR_TARGET_INSTRUCTION_REP_AGGREGATE || row->operand_count != 0)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedLeafProductSlot destination = {0};
    if (describe_leaf_product(context->execution->plan, frame, row->result_slot, &destination) !=
            XR_TYPED_DISPATCH_OK ||
        destination.layout_index != (uint32_t) row->immediate_bits)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedLeafValueProductTuple6 value = {0};
    return xr_typed_frame_store(frame, &destination.access, &value, sizeof(value)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus execute_value_product_set(XrTypedFrame *frame,
                                                       const XrTargetInstructionRecord *row,
                                                       const XrTargetInstructionContract *contract,
                                                       XrTypedDispatchRowContext *context,
                                                       bool u8) {
    if (!context || !context->execution || !contract || row->operand_count != 2 ||
        row->immediate_bits > UINT32_MAX ||
        contract->operand_rep[0] != XR_TARGET_INSTRUCTION_REP_AGGREGATE ||
        contract->operand_rep[1] !=
            (u8 ? XR_TARGET_INSTRUCTION_REP_U8 : XR_TARGET_INSTRUCTION_REP_I64))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedLeafProductSlot product = {0};
    if (describe_leaf_product(context->execution->plan, frame, row->operand_slots[0], &product) !=
        XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint32_t field_count = 0;
    const XrTargetFieldRecord *fields =
        xr_target_plan_fields(context->execution->plan, &field_count);
    uint32_t field_index = (uint32_t) row->immediate_bits;
    if (!fields || field_index >= field_count || field_index < product.layout->field_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint32_t ordinal = field_index - product.layout->field_begin;
    if (ordinal >= 6u || (ordinal == 2u) != u8 ||
        !target_product_field_is_exact(context->execution->plan, &fields[field_index],
                                       product.layout_index, ordinal))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedSlotAccess source = {0};
    size_t width = u8 ? 1u : 8u;
    if ((u8 ? describe_exact_u8(context->execution->plan, frame, row->operand_slots[1], &source)
            : describe_exact_i64(context->execution->plan, frame, row->operand_slots[1],
                                 &source)) != XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint8_t bytes[8] = {0};
    if (xr_typed_frame_load(frame, &source, bytes, width) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    XrTypedLeafValueProductTuple6 value = {0};
    if (xr_typed_frame_load(frame, &product.access, &value, sizeof(value)) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    memcpy((uint8_t *) &value + fields[field_index].offset, bytes, width);
    return xr_typed_frame_store(frame, &product.access, &value, sizeof(value)) == XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus
execute_value_product_set_i64(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
                              const XrTargetInstructionContract *contract,
                              XrTypedDispatchRowContext *context) {
    return execute_value_product_set(frame, row, contract, context, false);
}

static XrTypedDispatchStatus
execute_value_product_set_u8(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
                             const XrTargetInstructionContract *contract,
                             XrTypedDispatchRowContext *context) {
    return execute_value_product_set(frame, row, contract, context, true);
}

static XrTypedDispatchStatus
execute_value_product_get_u8(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
                             const XrTargetInstructionContract *contract,
                             XrTypedDispatchRowContext *context) {
    if (!context || !context->execution || !contract || row->operand_count != 1 ||
        row->immediate_bits > UINT32_MAX || contract->result_rep != XR_TARGET_INSTRUCTION_REP_U8 ||
        contract->operand_rep[0] != XR_TARGET_INSTRUCTION_REP_AGGREGATE)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedLeafValueProductTuple6 value = {0};
    XrTypedLeafProductSlot product = {0};
    XrTypedDispatchStatus status =
        load_leaf_product(context->execution->plan, frame, row->operand_slots[0], &value, &product);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    uint32_t field_count = 0;
    const XrTargetFieldRecord *fields =
        xr_target_plan_fields(context->execution->plan, &field_count);
    uint32_t field_index = (uint32_t) row->immediate_bits;
    if (!fields || field_index >= field_count || field_index != product.layout->field_begin + 2u ||
        !target_product_field_is_exact(context->execution->plan, &fields[field_index],
                                       product.layout_index, 2u))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedSlotAccess result = {0};
    if (describe_exact_u8(context->execution->plan, frame, row->result_slot, &result) !=
        XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    return xr_typed_frame_store(frame, &result, &value.field2, sizeof(value.field2)) ==
                   XR_TYPED_FRAME_OK
               ? XR_TYPED_DISPATCH_OK
               : XR_TYPED_DISPATCH_FRAME_ERROR;
}

static XrTypedDispatchStatus execute_array_push(XrTypedFrame *frame,
                                                const XrTargetInstructionRecord *row,
                                                const XrTargetInstructionContract *contract,
                                                XrTypedDispatchRowContext *context) {
    if (!contract || contract->error_kind != XR_TARGET_INSTRUCTION_ERROR_ARRAY_PUSH ||
        contract->operand_ownership[0] != XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_BORROW ||
        contract->operand_ownership[1] != XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_CONSUME ||
        !context || !context->execution || !context->execution->array_push)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrValue receiver = xr_null();
    XrValue element = xr_null();
    XrTypedDispatchStatus status = load_value(frame, row->operand_slots[0], &receiver);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    /* Move the frame owner into this operation before mutation. A rejected
     * kernel call restores the exact owner and leaves the frame active; once
     * the kernel succeeds there is no remaining lifecycle operation that can
     * fail after the array has taken ownership. */
    status = take_owned_value(frame, row->operand_slots[1], &element);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    XrArrayPushStatus pushed = context->execution->array_push(receiver, element);
    if (pushed == XR_ARRAY_PUSH_OK)
        return XR_TYPED_DISPATCH_OK;
    if (restore_owned_value(frame, row->operand_slots[1], element) != XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    if (pushed == XR_ARRAY_PUSH_INVALID_ARRAY)
        return XR_TYPED_DISPATCH_ARRAY_PUSH_INVALID_RECEIVER;
    if (pushed == XR_ARRAY_PUSH_SLICE)
        return XR_TYPED_DISPATCH_ARRAY_PUSH_SLICE;
    if (pushed == XR_ARRAY_PUSH_TYPE_MISMATCH)
        return XR_TYPED_DISPATCH_ARRAY_PUSH_TYPE_MISMATCH;
    if (pushed == XR_ARRAY_PUSH_ALLOCATION_FAILED)
        return XR_TYPED_DISPATCH_ARRAY_PUSH_ALLOCATION_FAILED;
    return XR_TYPED_DISPATCH_PROGRAM_INVALID;
}

static XrTypedDispatchStatus execute_suspend(XrTypedFrame *frame,
                                             const XrTargetInstructionRecord *row,
                                             const XrTargetInstructionContract *contract,
                                             XrTypedDispatchRowContext *context) {
    if (!contract || !context || !context->execution ||
        contract->control_kind != XR_TARGET_INSTRUCTION_CONTROL_SUSPEND ||
        xr_typed_frame_bind_coroutine_state(
            frame, XR_TARGET_INSTRUCTION_SUSPEND_STATE(row->immediate_bits)) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    context->execution->row_suspended = true;
    context->execution->row_suspend_state =
        XR_TARGET_INSTRUCTION_SUSPEND_STATE(row->immediate_bits);
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus execute_branch(XrTypedFrame *frame,
                                            const XrTargetInstructionRecord *row,
                                            const XrTargetInstructionContract *contract,
                                            XrTypedDispatchRowContext *context) {
    uint32_t target = context->decoded
                          ? context->decoded->target_if_nonzero
                          : XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(row->immediate_bits);
    if (contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_I64) {
        uint64_t bits = 0;
        XrTypedDispatchStatus status = load_i64_bits(frame, row->operand_slots[0], &bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        if (bits == 0)
            target = context->decoded ? context->decoded->target_if_zero
                                      : XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits);
    } else if (contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOOL) {
        uint8_t truth = 0;
        XrTypedDispatchStatus status = load_bool_byte(frame, row->operand_slots[0], &truth);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        if (truth == 0)
            target = context->decoded ? context->decoded->target_if_zero
                                      : XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits);
    } else if (contract->dispatch_argument != XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_JUMP) {
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
    if (target >= context->row_count)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    *context->next = target;
    return XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus copy_call_arguments(const XrTargetPlan *plan, XrTypedFrame *parent,
                                                 XrTypedFrame *child,
                                                 const XrTargetCallArgumentRecord *arguments,
                                                 uint16_t argument_count, bool aggregate_call) {
    for (uint16_t ordinal = 0; ordinal < argument_count; ordinal++) {
        if (aggregate_call) {
            const XrTargetCallArgumentRecord *argument = &arguments[ordinal];
            XrTypedLeafAggregateI64x2 value = {{0, 0}};
            XrTypedLeafAggregateSlot caller = {0};
            XrTypedLeafAggregateSlot callee = {0};
            XrTypedDispatchStatus status =
                load_leaf_aggregate(plan, parent, argument->caller_slot, &value, &caller);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            status = describe_leaf_aggregate(plan, child, argument->callee_slot, &callee);
            if (status != XR_TYPED_DISPATCH_OK || caller.layout_index != callee.layout_index ||
                caller.slot->register_rep != argument->register_rep ||
                caller.slot->memory_rep != argument->memory_rep ||
                callee.slot->register_rep != argument->callee_register_rep ||
                callee.slot->memory_rep != argument->callee_memory_rep ||
                argument->register_rep != argument->callee_register_rep ||
                argument->memory_rep != argument->callee_memory_rep)
                return XR_TYPED_DISPATCH_PROGRAM_INVALID;
            if (xr_typed_frame_store(child, &callee.access, &value, sizeof(value)) !=
                XR_TYPED_FRAME_OK)
                return XR_TYPED_DISPATCH_FRAME_ERROR;
            continue;
        }
        uint64_t bits = 0;
        XrTypedDispatchStatus status = load_i64_bits(parent, arguments[ordinal].caller_slot, &bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        status = store_i64_bits(child, arguments[ordinal].callee_slot, bits);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
    }
    return XR_TYPED_DISPATCH_OK;
}

static bool leaf_aggregate_call_is_exact(const XrTargetPlan *plan,
                                         const XrTargetInstructionRecord *row,
                                         const XrTargetCallRecord *call,
                                         const XrTargetCallArgumentRecord *arguments,
                                         uint32_t argument_count) {
    if (!plan || !row || !call || !arguments || row->immediate_bits > UINT32_MAX ||
        call->id != (uint32_t) row->immediate_bits || call->caller_function != row->function ||
        call->callee_function == XR_SEMANTIC_INDEX_NONE || call->result_slot != row->result_slot ||
        call->caller_storage_slot != row->result_slot ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !bytes_are_zero(call->source_export_identity.bytes,
                        sizeof(call->source_export_identity.bytes)) ||
        !bytes_are_zero(call->source_callee_identity.bytes,
                        sizeof(call->source_callee_identity.bytes)) ||
        call->result_register_rep != call->result_memory_rep ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 1 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->reserved8[0] != 0 || call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        call->argument_begin >= argument_count)
        return false;
    const XrTargetMachineRepRecord *result_rep =
        xr_target_plan_machine_rep(plan, call->result_memory_rep);
    const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin];
    return result_rep && result_rep->kind == XR_MACHINE_REP_AGGREGATE &&
           result_rep->register_bits == 128 && result_rep->memory_size == 16 &&
           result_rep->memory_align == 8 && argument->call == call->id && argument->ordinal == 0 &&
           argument->mode == XR_TARGET_CALL_VALUE && argument->ownership == XR_TARGET_CALL_READ &&
           argument->transfer_mode == XR_TRANSFER_SHARE && argument->flags == 0 &&
           argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
           argument->reserved8[2] == 0 && argument->register_rep == call->result_register_rep &&
           argument->memory_rep == call->result_memory_rep &&
           argument->callee_register_rep == argument->register_rep &&
           argument->callee_memory_rep == argument->memory_rep;
}

static bool leaf_product_call_is_exact(const XrTargetPlan *plan,
                                       const XrTargetInstructionRecord *row,
                                       const XrTargetCallRecord *call, uint32_t argument_count) {
    if (!plan || !row || !call || row->immediate_bits > UINT32_MAX || argument_count != 0 ||
        call->id != (uint32_t) row->immediate_bits || call->caller_function != row->function ||
        call->callee_function == XR_SEMANTIC_INDEX_NONE || call->result_slot != row->result_slot ||
        call->caller_storage_slot != row->result_slot ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !bytes_are_zero(call->source_export_identity.bytes,
                        sizeof(call->source_export_identity.bytes)) ||
        !bytes_are_zero(call->source_callee_identity.bytes,
                        sizeof(call->source_callee_identity.bytes)) ||
        call->result_register_rep != call->result_memory_rep ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE || call->argument_count != 0 ||
        call->adapter_count != 0 || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->reserved8[0] != 0 || call->reserved8[1] != 0 || call->reserved8[2] != 0)
        return false;
    const XrTargetMachineRepRecord *result_rep =
        xr_target_plan_machine_rep(plan, call->result_memory_rep);
    return result_rep && result_rep->kind == XR_MACHINE_REP_AGGREGATE &&
           result_rep->register_bits == 384u && result_rep->memory_size == 48u &&
           result_rep->memory_align == 8u;
}

static XrTypedDispatchStatus execute_call_common(XrTypedFrame *frame,
                                                 const XrTargetInstructionRecord *row,
                                                 const XrTargetInstructionContract *contract,
                                                 XrTypedDispatchRowContext *context,
                                                 bool aggregate_call) {
    XrTypedDispatchExecution *execution = context->execution;
    if (!execution || execution->call_depth >= XR_TYPED_DISPATCH_MAX_CALL_DEPTH)
        return XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED;
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(execution->plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(execution->plan, &argument_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call = calls && call_index < call_count ? &calls[call_index] : NULL;
    if (!call || call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    bool product_call = aggregate_call && xr_target_plan_function_execution_family_mask(
                                              execution->plan, row->function) ==
                                              XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6;
    if (aggregate_call &&
        (contract->result_rep != XR_TARGET_INSTRUCTION_REP_AGGREGATE ||
         (product_call ? !leaf_product_call_is_exact(execution->plan, row, call, argument_count)
                       : !leaf_aggregate_call_is_exact(execution->plan, row, call, arguments,
                                                       argument_count))))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;

    uint32_t child_frame_id = execution->next_frame_id++;
    XrVmTraceEvent call_enter =
        make_trace_event(XR_VM_TRACE_CALL_ENTER, row->function, context->frame_id,
                         XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
    call_enter.instruction = row->id;
    call_enter.opcode = row->opcode;
    call_enter.call = call->id;
    call_enter.related_function = call->callee_function;
    call_enter.related_frame = child_frame_id;
    XrTypedDispatchStatus status = emit_trace_event(execution, &call_enter);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;

    XrTypedFrame *child = NULL;
    status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (!function_has_zero_lifecycle(execution->plan, call->callee_function))
        goto cleanup;
    if (xr_typed_frame_create(execution->plan, execution->fingerprint, call->callee_function,
                              &execution->limits, &child) != XR_TYPED_FRAME_OK)
        goto cleanup;
    if (execution->generation_identity_present &&
        xr_typed_frame_bind_generation_identity(child, &execution->generation_identity) !=
            XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
        goto cleanup;
    }
    if (xr_typed_frame_link_child(frame, child) != XR_TYPED_FRAME_OK)
        goto cleanup;
    status = copy_call_arguments(execution->plan, frame, child,
                                 call->argument_count ? &arguments[call->argument_begin] : NULL,
                                 call->argument_count, aggregate_call);
    if (status != XR_TYPED_DISPATCH_OK)
        goto cleanup;
    uint64_t child_result = 0;
    XrTypedAggregateValue child_aggregate = {0};
    execution->call_depth++;
    status = execute_function(execution, child, call->callee_function, NULL, NULL,
                              call->argument_count, true, child_frame_id, context->frame_id,
                              &child_result, aggregate_call ? &child_aggregate : NULL);
    execution->call_depth--;
    if (status == XR_TYPED_DISPATCH_OK) {
        status =
            aggregate_call
                ? (product_call
                       ? store_leaf_product(execution->plan, frame, call->caller_storage_slot,
                                            &child_aggregate.product)
                       : store_leaf_aggregate(execution->plan, frame, call->caller_storage_slot,
                                              &child_aggregate.pair, NULL))
                : store_i64_bits(frame, row->result_slot, child_result);
    }

cleanup:
    /* Successful frame cleanup severs the parent link.  A lifecycle refusal
     * leaves the child linked and therefore reachable from its owner. */
    if (free_scalar_frame(&child) != XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent call_return =
            make_trace_event(XR_VM_TRACE_CALL_RETURN, row->function, context->frame_id,
                             XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
        call_return.instruction = row->id;
        call_return.opcode = row->opcode;
        call_return.call = call->id;
        call_return.related_function = call->callee_function;
        call_return.related_frame = child_frame_id;
        call_return.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status = emit_trace_event(execution, &call_return);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            status = trace_status;
    }
    return status;
}

static XrTypedDispatchStatus execute_call(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
                                          const XrTargetInstructionContract *contract,
                                          XrTypedDispatchRowContext *context) {
    return execute_call_common(frame, row, contract, context, false);
}

static XrTypedDispatchStatus execute_call_aggregate(XrTypedFrame *frame,
                                                    const XrTargetInstructionRecord *row,
                                                    const XrTargetInstructionContract *contract,
                                                    XrTypedDispatchRowContext *context) {
    return execute_call_common(frame, row, contract, context, true);
}

static XrTypedDispatchStatus execute_native_leaf(XrTypedFrame *frame,
                                                 const XrTargetInstructionRecord *row,
                                                 const XrTargetInstructionContract *contract,
                                                 XrTypedDispatchRowContext *context) {
    if (!frame || !row || !contract || !context || !context->execution ||
        !context->execution->plan || row->immediate_bits > UINT32_MAX ||
        contract->result_rep != XR_TARGET_INSTRUCTION_REP_I64 || row->operand_count != 0)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;

    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(context->execution->plan, &call_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call = calls && call_index < call_count ? &calls[call_index] : NULL;
    if (!call || call->id != call_index || call->caller_function != row->function ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !bytes_are_zero(call->source_export_identity.bytes,
                        sizeof(call->source_export_identity.bytes)) ||
        !bytes_are_zero(call->source_callee_identity.bytes,
                        sizeof(call->source_callee_identity.bytes)) ||
        bytes_are_zero(call->native_callee_identity.bytes,
                       sizeof(call->native_callee_identity.bytes)) ||
        call->native_leaf <= XR_STDLIB_TARGET_LEAF_NONE ||
        call->native_leaf >= XR_STDLIB_TARGET_LEAF_COUNT ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_NATIVE_TARGET_LEAF_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR ||
        call->argument_count != 0 || call->adapter_count != 0 || call->flags != 0 ||
        call->result_slot != row->result_slot || call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;

    int64_t result = 0;
    if (!xr_stdlib_target_leaf_execute_i64(call->native_leaf, NULL, 0, &result))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    uint64_t result_bits = 0;
    memcpy(&result_bits, &result, sizeof(result_bits));
    return store_i64_bits(frame, row->result_slot, result_bits);
}

static XrTypedDispatchStatus execute_entry_call(XrTypedFrame *frame,
                                                const XrTargetInstructionRecord *row,
                                                const XrTargetInstructionContract *contract,
                                                XrTypedDispatchRowContext *context) {
    (void) contract;
    XrTypedDispatchExecution *execution = context->execution;
    if (!execution || execution->call_depth >= XR_TYPED_DISPATCH_MAX_CALL_DEPTH)
        return XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED;
    if (!execution->dynamic_entries ||
        execution->dynamic_entries->schema_version != XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION ||
        execution->dynamic_entries->reserved != 0 || !execution->dynamic_entries->validate ||
        !execution->dynamic_entries->acquire || !execution->dynamic_entries->retire)
        return XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE;

    uint32_t expectation_count = 0;
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    const XrTargetEntryExpectationRecord *expectations =
        xr_target_plan_entry_expectations(execution->plan, &expectation_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(execution->plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(execution->plan, &argument_count);
    uint32_t expectation_index = (uint32_t) row->immediate_bits;
    const XrTargetEntryExpectationRecord *expectation =
        expectations && row->immediate_bits <= UINT32_MAX && expectation_index < expectation_count
            ? &expectations[expectation_index]
            : NULL;
    const XrTargetCallRecord *call =
        expectation && expectation->call < call_count ? &calls[expectation->call] : NULL;
    if (!expectation || !call || call->argument_count > XR_TARGET_INSTRUCTION_MAX_PARAMETERS ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;

    int64_t child_arguments[XR_TARGET_INSTRUCTION_MAX_PARAMETERS];
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        uint64_t bits = 0;
        XrTypedDispatchStatus load =
            load_i64_bits(frame, arguments[call->argument_begin + ordinal].caller_slot, &bits);
        if (load != XR_TYPED_DISPATCH_OK)
            return load;
        memcpy(&child_arguments[ordinal], &bits, sizeof(child_arguments[ordinal]));
    }

    XrVmDynamicEntryResolution resolution;
    memset(&resolution, 0, sizeof(resolution));
    XrVmDynamicEntryStatus acquire = execution->dynamic_entries->acquire(
        execution->dynamic_entries, execution->plan, execution->fingerprint, expectation,
        execution->use_dynamic_entry_cache, &resolution);
    if (acquire != XR_VM_DYNAMIC_ENTRY_OK) {
        if (acquire == XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED)
            return XR_TYPED_DISPATCH_ENTRY_BUDGET_EXCEEDED;
        if (acquire == XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH)
            return XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH;
        return XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE;
    }
    bool acquired = true;
    XrTypedDispatchStatus status = XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedFrame *child = NULL;
    uint32_t child_frame_id = resolution.adapter.executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM
                                  ? execution->next_frame_id++
                                  : XR_VM_TRACE_ID_NONE;
    uint32_t child_function = resolution.function;
    const XrVmDynamicEntryContext *retire_context = execution->dynamic_entries;
    uint64_t family =
        xr_target_plan_function_execution_family_mask(resolution.plan, resolution.function);
    if (!resolution.plan || !resolution.lease || !xr_target_plan_is_verified(resolution.plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(resolution.plan),
                              resolution.plan_fingerprint) ||
        !xr_target_plan_fingerprint_is_intact(resolution.plan) ||
        !xr_typed_entry_adapter_i64_matches_target(&resolution.adapter, expectation) ||
        (family != XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
         family != XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC) ||
        !function_has_zero_lifecycle(resolution.plan, resolution.function) ||
        !resolution.dynamic_entries ||
        resolution.dynamic_entries->schema_version != XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION ||
        !resolution.dynamic_entries->validate ||
        resolution.dynamic_entries->validate(
            resolution.dynamic_entries, resolution.plan, &resolution.plan_fingerprint,
            &resolution.generation_identity) != XR_VM_DYNAMIC_ENTRY_OK)
        goto cleanup;

    XrVmTraceEvent call_enter =
        make_trace_event(XR_VM_TRACE_CALL_ENTER, row->function, context->frame_id,
                         XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
    call_enter.instruction = row->id;
    call_enter.opcode = row->opcode;
    call_enter.call = call->id;
    call_enter.related_function = resolution.function;
    call_enter.related_frame = child_frame_id;
    status = emit_trace_event(execution, &call_enter);
    if (status != XR_TYPED_DISPATCH_OK)
        goto cleanup;
    if (resolution.adapter.executor_kind == XR_ENTRY_EXECUTOR_NATIVE_I64) {
        int64_t native_result = 0;
        execution->call_depth++;
        XrEntryNativeStatus native_status = xr_typed_entry_adapter_i64_invoke_native(
            &resolution.adapter, call->argument_count ? child_arguments : NULL,
            call->argument_count, &native_result);
        execution->call_depth--;
        if (native_status == XR_ENTRY_NATIVE_OK) {
            uint64_t native_bits = 0;
            memcpy(&native_bits, &native_result, sizeof(native_bits));
            status = store_i64_bits(frame, row->result_slot, native_bits);
        } else if (native_status == XR_ENTRY_NATIVE_CANCELLED) {
            status = XR_TYPED_DISPATCH_ENTRY_CANCELLED;
        } else {
            status = XR_TYPED_DISPATCH_ENTRY_NATIVE_ERROR;
        }
        goto cleanup;
    }
    if (resolution.adapter.executor_kind != XR_ENTRY_EXECUTOR_TYPED_VM)
        goto cleanup;
    if (xr_typed_frame_create(resolution.plan, &resolution.plan_fingerprint, resolution.function,
                              &execution->limits, &child) != XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
        goto cleanup;
    }
    if (xr_typed_frame_bind_generation_identity(child, &resolution.generation_identity) !=
        XR_TYPED_FRAME_OK) {
        status = XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH;
        goto cleanup;
    }

    const XrTargetPlan *saved_plan = execution->plan;
    const XrFingerprint *saved_fingerprint = execution->fingerprint;
    const XrVmDecodedCache *saved_decoded_cache = execution->decoded_cache;
    const XrVmDynamicEntryContext *saved_dynamic_entries = execution->dynamic_entries;
    XrModuleGenerationIdentity saved_generation = execution->generation_identity;
    bool saved_generation_present = execution->generation_identity_present;
    execution->plan = resolution.plan;
    execution->fingerprint = &resolution.plan_fingerprint;
    execution->decoded_cache = resolution.decoded_cache;
    execution->dynamic_entries = resolution.dynamic_entries;
    execution->generation_identity = resolution.generation_identity;
    execution->generation_identity_present = true;
    uint64_t child_result = 0;
    execution->call_depth++;
    status = execute_function(
        execution, child, resolution.function, call->argument_count ? child_arguments : NULL, NULL,
        call->argument_count, false, child_frame_id, context->frame_id, &child_result, NULL);
    execution->call_depth--;
    execution->plan = saved_plan;
    execution->fingerprint = saved_fingerprint;
    execution->decoded_cache = saved_decoded_cache;
    execution->dynamic_entries = saved_dynamic_entries;
    execution->generation_identity = saved_generation;
    execution->generation_identity_present = saved_generation_present;
    if (status == XR_TYPED_DISPATCH_OK)
        status = store_i64_bits(frame, row->result_slot, child_result);

cleanup:;
    /* Frame disposal and lease retirement are independent obligations.  The
     * latter consumes the resolution even when immediate pin release must be
     * deferred to its runtime-owned ledger. */
    if (free_scalar_frame(&child) != XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (acquired) {
        XrVmDynamicEntryStatus retired = retire_context->retire(retire_context, &resolution);
        if (retired != XR_VM_DYNAMIC_ENTRY_OK && status != XR_TYPED_DISPATCH_TRACE_REJECTED)
            status = XR_TYPED_DISPATCH_ENTRY_RETIRE_DEFERRED;
    }
    if (status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent call_return =
            make_trace_event(XR_VM_TRACE_CALL_RETURN, row->function, context->frame_id,
                             XR_VM_TRACE_ID_NONE, execution->call_depth - 1u);
        call_return.instruction = row->id;
        call_return.opcode = row->opcode;
        call_return.call = call->id;
        call_return.related_function = child_function;
        call_return.related_frame = child_frame_id;
        call_return.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status = emit_trace_event(execution, &call_return);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            status = trace_status;
    }
    return status;
}

/* The generated providers consume the same dense registry. The function table
 * deliberately uses only standard function pointers and sequential
 * initialization so it compiles as ordinary C11 under MSVC. */
static const XrTypedDispatchFunctionBinding generated_function_table[] = {
    {NULL, 0, 0},
#define XR_VM_OP(symbol, handler, kind, argument)                                                  \
    {execute_##handler, XR_TARGET_INSTRUCTION_DISPATCH_##kind,                                     \
     XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_##argument},
#include "xr_vm_ops.def"
#undef XR_VM_OP
};

_Static_assert(sizeof(generated_function_table) / sizeof(generated_function_table[0]) ==
                   XR_TARGET_INSTRUCTION_COUNT,
               "generated function table must cover every typed opcode");

static bool optional_text_equal(const char *left, const char *right) {
    if (!left || !right)
        return left == right;
    return strcmp(left, right) == 0;
}

static bool instruction_contract_is_exact(uint16_t opcode,
                                          const XrTargetInstructionContract *contract) {
    const XrTargetInstructionContract *expected = xr_target_instruction_contract(opcode);
    return expected && contract && optional_text_equal(contract->name, expected->name) &&
           optional_text_equal(contract->semantic_name, expected->semantic_name) &&
           contract->arity == expected->arity && contract->terminator == expected->terminator &&
           contract->result_rep == expected->result_rep &&
           contract->operand_rep[0] == expected->operand_rep[0] &&
           contract->operand_rep[1] == expected->operand_rep[1] &&
           contract->result_ownership == expected->result_ownership &&
           contract->operand_ownership[0] == expected->operand_ownership[0] &&
           contract->operand_ownership[1] == expected->operand_ownership[1] &&
           contract->effects == expected->effects && contract->error_kind == expected->error_kind &&
           contract->may_suspend == expected->may_suspend &&
           contract->immediate_kind == expected->immediate_kind &&
           contract->control_kind == expected->control_kind &&
           contract->dispatch_kind == expected->dispatch_kind &&
           contract->dispatch_argument == expected->dispatch_argument;
}

XR_FUNC bool
xr_typed_dispatch_provider_contract_is_exact(XrTypedDispatchProvider provider, uint16_t opcode,
                                             const XrTargetInstructionContract *contract) {
    if (!instruction_contract_is_exact(opcode, contract))
        return false;
    if (provider == XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH) {
        switch ((XrTargetInstructionOpcode) opcode) {
#define XR_VM_OP(symbol, handler, kind, argument)                                                  \
    case XR_TARGET_INSTRUCTION_##symbol:                                                           \
        return contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_##kind &&                 \
               contract->dispatch_argument == XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_##argument;
#include "xr_vm_ops.def"
#undef XR_VM_OP
            default:
                return false;
        }
    }
    if (provider == XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE &&
        opcode < sizeof(generated_function_table) / sizeof(generated_function_table[0])) {
        const XrTypedDispatchFunctionBinding *binding = &generated_function_table[opcode];
        return binding->handler && binding->dispatch_kind == contract->dispatch_kind &&
               binding->dispatch_argument == contract->dispatch_argument;
    }
    return false;
}

static XrTypedDispatchStatus execute_row_with_switch(XrTypedFrame *frame,
                                                     const XrTargetInstructionRecord *row,
                                                     const XrTargetInstructionContract *contract,
                                                     XrTypedDispatchRowContext *context) {
    switch ((XrTargetInstructionOpcode) row->opcode) {
#define XR_VM_OP(symbol, handler, kind, argument)                                                  \
    case XR_TARGET_INSTRUCTION_##symbol:                                                           \
        return execute_##handler(frame, row, contract, context);
#include "xr_vm_ops.def"
#undef XR_VM_OP
        default:
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    }
}

static XrTypedDispatchStatus
execute_row_with_function_table(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
                                const XrTargetInstructionContract *contract,
                                XrTypedDispatchRowContext *context) {
    if (row->opcode >= sizeof(generated_function_table) / sizeof(generated_function_table[0]))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    const XrTypedDispatchFunctionBinding *binding = &generated_function_table[row->opcode];
    return binding->handler ? binding->handler(frame, row, contract, context)
                            : XR_TYPED_DISPATCH_PROGRAM_INVALID;
}

static XrTypedDispatchStatus
execute_row(XrTypedFrame *frame, const XrTargetInstructionRecord *row,
            const XrVmDecodedInstruction *decoded, const int64_t *arguments,
            const XrTypedLeafAggregateI64x2 *aggregate_arguments, uint32_t argument_count,
            uint32_t row_count, uint32_t *next, bool *returned, uint64_t *return_bits,
            XrTypedAggregateValue *aggregate_return, XrTypedDispatchExecution *execution,
            bool parameters_prebound, uint32_t frame_id) {
    const XrTargetInstructionContract *contract =
        decoded ? decoded->contract : xr_target_instruction_contract(row->opcode);
    if (!contract)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    XrTypedDispatchRowContext context = {
        .arguments = arguments,
        .aggregate_arguments = aggregate_arguments,
        .argument_count = argument_count,
        .row_count = row_count,
        .next = next,
        .returned = returned,
        .return_bits = return_bits,
        .aggregate_return = aggregate_return,
        .execution = execution,
        .decoded = decoded,
        .parameters_prebound = parameters_prebound,
        .frame_id = frame_id,
    };
    if (!xr_typed_dispatch_provider_contract_is_exact(execution->provider, row->opcode, contract))
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (execution->provider == XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH)
        return execute_row_with_switch(frame, row, contract, &context);
    if (execution->provider == XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE)
        return execute_row_with_function_table(frame, row, contract, &context);
    return XR_TYPED_DISPATCH_PROGRAM_INVALID;
}

typedef struct XrTypedDispatchFunctionRun {
    uint32_t current_instruction;
    uint32_t last_block;
    uint16_t current_opcode;
} XrTypedDispatchFunctionRun;

static XrTypedDispatchStatus
execute_function_rows(XrTypedDispatchExecution *execution, XrTypedFrame *frame, uint32_t function,
                      const int64_t *arguments,
                      const XrTypedLeafAggregateI64x2 *aggregate_arguments, uint32_t argument_count,
                      bool parameters_prebound, uint32_t frame_id, uint32_t parent_frame_id,
                      uint32_t frame_depth, const XrTargetInstructionRecord *instructions,
                      const XrVmDecodedFunctionView *decoded_function, uint32_t instruction_count,
                      uint64_t *return_bits, XrTypedAggregateValue *aggregate_return,
                      XrTypedDispatchFunctionRun *run) {
    XrVmDebugControl *debug_control =
        execution->debug_session ? execution->debug_session->control : NULL;
    uint32_t current = execution->resume_start_instruction;
    execution->resume_start_instruction = 0;
    bool returned = false;
    bool suspended = false;
    uint32_t suspend_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    run->current_instruction = XR_VM_TRACE_ID_NONE;
    run->last_block = XR_VM_TRACE_ID_NONE;
    run->current_opcode = XR_TARGET_INSTRUCTION_INVALID;
    while (!returned && !suspended) {
        if (current >= instruction_count)
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
        if (execution->remaining_steps == 0)
            return XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED;
        execution->remaining_steps--;
        const XrVmDecodedInstruction *decoded =
            decoded_function->instructions ? &decoded_function->instructions[current] : NULL;
        const XrTargetInstructionRecord *row = decoded ? &decoded->row : &instructions[current];
        run->current_instruction = row->id;
        run->current_opcode = row->opcode;
        uint32_t block_entry_instruction = XR_VM_TRACE_ID_NONE;
        if (decoded) {
            if (decoded->block >= decoded_function->block_count)
                return XR_TYPED_DISPATCH_PROGRAM_INVALID;
            uint32_t block_first = decoded_function->blocks[decoded->block].first_row;
            if (block_first >= instruction_count)
                return XR_TYPED_DISPATCH_PROGRAM_INVALID;
            block_entry_instruction = decoded_function->instructions[block_first].row.id;
            if (xr_typed_frame_enter_decoded_instruction(frame, run->current_instruction,
                                                         block_entry_instruction) !=
                XR_TYPED_FRAME_OK) {
                return XR_TYPED_DISPATCH_FRAME_ERROR;
            }
        } else {
            if (xr_typed_frame_enter_instruction(frame, run->current_instruction) !=
                XR_TYPED_FRAME_OK) {
                return XR_TYPED_DISPATCH_FRAME_ERROR;
            }
            XrTypedFrameContext frame_context;
            if (xr_typed_frame_context(frame, &frame_context) != XR_TYPED_FRAME_OK) {
                return XR_TYPED_DISPATCH_FRAME_ERROR;
            }
            block_entry_instruction = frame_context.block_entry_instruction;
        }
        if (block_entry_instruction != run->last_block) {
            XrVmTraceEvent block_enter = make_trace_event(XR_VM_TRACE_BLOCK_ENTER, function,
                                                          frame_id, parent_frame_id, frame_depth);
            block_enter.instruction = run->current_instruction;
            block_enter.block = block_entry_instruction;
            block_enter.opcode = run->current_opcode;
            XrTypedDispatchStatus status = emit_trace_event(execution, &block_enter);
            if (status != XR_TYPED_DISPATCH_OK)
                return status;
            run->last_block = block_entry_instruction;
        }
        XrVmTraceEvent instruction = make_trace_event(XR_VM_TRACE_INSTRUCTION, function, frame_id,
                                                      parent_frame_id, frame_depth);
        instruction.instruction = run->current_instruction;
        instruction.block = block_entry_instruction;
        instruction.opcode = run->current_opcode;
        XrTypedDispatchStatus status = emit_trace_event(execution, &instruction);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        uint32_t next = current + 1u;
        execution->row_suspended = false;
        execution->row_suspend_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
        status = execute_row(frame, row, decoded, arguments, aggregate_arguments, argument_count,
                             instruction_count, &next, &returned, return_bits, aggregate_return,
                             execution, parameters_prebound, frame_id);
        if (status != XR_TYPED_DISPATCH_OK)
            return status;
        suspended = execution->row_suspended;
        suspend_state = execution->row_suspend_state;
        if (debug_control &&
            xr_typed_debug_control_commit_row(debug_control, row) != XR_VM_DEBUG_CONTROL_EVENT_OK) {
            return XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR;
        }
        current = next;
    }
    execution->row_suspend_state = suspend_state;
    return suspended ? XR_TYPED_DISPATCH_SUSPENDED : XR_TYPED_DISPATCH_OK;
}

static XrTypedDispatchStatus
execute_function(XrTypedDispatchExecution *execution, XrTypedFrame *frame, uint32_t function,
                 const int64_t *arguments, const XrTypedLeafAggregateI64x2 *aggregate_arguments,
                 uint32_t argument_count, bool parameters_prebound, uint32_t frame_id,
                 uint32_t parent_frame_id, uint64_t *return_bits,
                 XrTypedAggregateValue *aggregate_return) {
    XrVmDebugControl *debug_control =
        execution->debug_session ? execution->debug_session->control : NULL;
    if (debug_control &&
        xr_typed_debug_control_push_frame(
            debug_control, execution->plan, execution->fingerprint,
            execution->generation_identity_present ? &execution->generation_identity : NULL, frame,
            function, frame_id, parent_frame_id,
            parameters_prebound) != XR_VM_DEBUG_CONTROL_EVENT_OK)
        return XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR;

    uint32_t frame_depth = execution->call_depth - 1u;
    XrVmTraceEvent frame_enter =
        make_trace_event(XR_VM_TRACE_FRAME_ENTER, function, frame_id, parent_frame_id, frame_depth);
    XrTypedDispatchStatus status = emit_trace_event(execution, &frame_enter);
    XrTypedDispatchFunctionRun run = {XR_VM_TRACE_ID_NONE, XR_VM_TRACE_ID_NONE,
                                      XR_TARGET_INSTRUCTION_INVALID};
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions = NULL;
    XrVmDecodedFunctionView decoded_function = {0};
    if (status != XR_TYPED_DISPATCH_OK)
        goto pop_debug_frame;
    if (execution->decoded_cache) {
        if (!xr_typed_decoded_cache_function(execution->decoded_cache, function,
                                             &decoded_function)) {
            status = XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
            goto report_error;
        }
        instruction_count = decoded_function.instruction_count;
    } else {
        instructions =
            xr_target_plan_function_instructions(execution->plan, function, &instruction_count);
    }
    if ((!instructions && !decoded_function.instructions) || !instruction_count) {
        status = XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
        goto report_error;
    }
    uint32_t declared_parameters =
        decoded_function.instructions ? decoded_function.parameter_count : 0;
    if (!decoded_function.instructions) {
        for (uint32_t i = 0; i < instruction_count; i++) {
            const XrTargetInstructionContract *contract =
                xr_target_instruction_contract(instructions[i].opcode);
            if (!contract) {
                status = XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
                goto report_error;
            }
            declared_parameters +=
                contract->immediate_kind == XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL;
        }
    }
    if (declared_parameters != argument_count) {
        status = XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
        goto report_error;
    }
    status = execute_function_rows(execution, frame, function, arguments, aggregate_arguments,
                                   argument_count, parameters_prebound, frame_id, parent_frame_id,
                                   frame_depth, instructions, &decoded_function, instruction_count,
                                   return_bits, aggregate_return, &run);

report_error:
    if (status != XR_TYPED_DISPATCH_OK && status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent error =
            make_trace_event(XR_VM_TRACE_ERROR, function, frame_id, parent_frame_id, frame_depth);
        error.instruction = run.current_instruction;
        error.opcode = run.current_opcode;
        error.block = run.last_block;
        error.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status = emit_trace_event(execution, &error);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            status = trace_status;
    }

    if (status != XR_TYPED_DISPATCH_TRACE_REJECTED) {
        XrVmTraceEvent frame_exit = make_trace_event(XR_VM_TRACE_FRAME_EXIT, function, frame_id,
                                                     parent_frame_id, frame_depth);
        frame_exit.status = (uint32_t) status;
        XrTypedDispatchStatus trace_status = emit_trace_event(execution, &frame_exit);
        if (trace_status != XR_TYPED_DISPATCH_OK)
            status = trace_status;
    }
pop_debug_frame:
    if (debug_control &&
        xr_typed_debug_control_pop_frame(debug_control, frame_id) != XR_VM_DEBUG_CONTROL_EVENT_OK)
        status = XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR;
    return status;
}

XrTypedDispatchStatus xr_typed_dispatch_execute_i64(const XrTypedDispatchI64Request *request) {
    if (request && request->result)
        *request->result = 0;
    if (!request || !request->verified_plan || !request->required_plan_fingerprint ||
        !request->result || (!request->arguments && request->argument_count) ||
        (request->use_dynamic_entry_cache && !request->dynamic_entries) ||
        (request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH &&
         request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    const XrTargetPlan *verified_plan = request->verified_plan;
    const XrFingerprint *required_plan_fingerprint = request->required_plan_fingerprint;
    if (!xr_target_plan_is_verified(verified_plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    bool decoded_cache_exact = false;
    if (request->decoded_cache) {
        XrVmDecodedCacheStatus cache_status = xr_typed_decoded_cache_require_exact(
            request->decoded_cache, verified_plan, required_plan_fingerprint,
            request->generation_identity);
        if (cache_status == XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH)
            return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
        if (cache_status != XR_VM_DECODED_CACHE_OK)
            return cache_status == XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED
                       ? XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED
                       : XR_TYPED_DISPATCH_PROGRAM_INVALID;
        decoded_cache_exact = true;
    } else {
        char error[512] = {0};
        if (!xr_target_plan_fingerprint_is_intact(verified_plan) ||
            !xr_target_instruction_program_verify(verified_plan, error, sizeof(error)))
            return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    }
    XrTypedDispatchStatus graph_entry_status =
        require_program_graph_entry(verified_plan, request->function, decoded_cache_exact);
    if (graph_entry_status != XR_TYPED_DISPATCH_OK)
        return graph_entry_status;
    if (request->debug_session &&
        !xr_typed_debug_session_matches_plan(request->debug_session,
                                             xr_target_plan_fingerprint(verified_plan)))
        return XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH;
    uint64_t execution_family =
        xr_target_plan_function_execution_family_mask(verified_plan, request->function);
    if ((execution_family != XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
         execution_family != XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC &&
         execution_family != XR_TARGET_EXECUTION_I64_OVERFLOW_PREDICATE) ||
        !function_has_zero_lifecycle(verified_plan, request->function) ||
        (execution_family == XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC &&
         (!request->generation_identity || !request->dynamic_entries ||
          request->dynamic_entries->schema_version != XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION ||
          request->dynamic_entries->reserved != 0 || !request->dynamic_entries->validate ||
          !request->dynamic_entries->acquire || !request->dynamic_entries->retire)))
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    if (request->generation_identity &&
        !generation_identity_matches_plan(request->generation_identity, verified_plan,
                                          *required_plan_fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    if (execution_family == XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC &&
        request->dynamic_entries->validate(request->dynamic_entries, verified_plan,
                                           required_plan_fingerprint,
                                           request->generation_identity) != XR_VM_DYNAMIC_ENTRY_OK)
        return XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH;
    if (request->debug_session && request->debug_session->generation_identity_present &&
        (!request->generation_identity ||
         !generation_identity_equal(&request->debug_session->generation_identity,
                                    request->generation_identity)))
        return XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH;

    XrVmDebugControl *debug_control =
        request->debug_session ? request->debug_session->control : NULL;
    if (debug_control && xr_typed_debug_control_begin_execution(debug_control, verified_plan,
                                                                required_plan_fingerprint) !=
                             XR_VM_DEBUG_CONTROL_EVENT_OK)
        return XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR;

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(verified_plan, required_plan_fingerprint, request->function, &limits,
                              &frame) != XR_TYPED_FRAME_OK) {
        xr_typed_debug_control_end_execution(debug_control);
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    }
    if (request->generation_identity &&
        xr_typed_frame_bind_generation_identity(frame, request->generation_identity) !=
            XR_TYPED_FRAME_OK) {
        XrTypedDispatchStatus bind_status = free_scalar_frame(&frame) == XR_TYPED_DISPATCH_OK
                                                ? XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH
                                                : XR_TYPED_DISPATCH_FRAME_ERROR;
        xr_typed_debug_control_end_execution(debug_control);
        return bind_status;
    }

    uint64_t return_bits = 0;
    XrTypedDispatchExecution execution = {
        .plan = verified_plan,
        .fingerprint = required_plan_fingerprint,
        .limits = limits,
        .remaining_steps = XR_TYPED_DISPATCH_MAX_STEPS,
        .call_depth = 1,
        .next_frame_id = 1,
        .debug_session = request->debug_session,
        .decoded_cache = request->decoded_cache,
        .dynamic_entries = request->dynamic_entries,
        .use_dynamic_entry_cache = request->use_dynamic_entry_cache,
        .provider = request->provider,
    };
    if (request->generation_identity) {
        execution.generation_identity = *request->generation_identity;
        execution.generation_identity_present = true;
    }
    XrTypedDispatchStatus status = execute_function(
        &execution, frame, request->function, request->arguments, NULL, request->argument_count,
        false, 0, XR_VM_TRACE_ID_NONE, &return_bits, NULL);
    if (free_scalar_frame(&frame) != XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    xr_typed_debug_control_end_execution(debug_control);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    memcpy(request->result, &return_bits, sizeof(*request->result));
    return XR_TYPED_DISPATCH_OK;
}

XrTypedDispatchStatus xr_typed_dispatch_execute_leaf_aggregate_i64x2(
    const XrTypedDispatchLeafAggregateI64x2Request *request) {
    if (request && request->result)
        memset(request->result, 0, sizeof(*request->result));
    if (!request || !request->verified_plan || !request->required_plan_fingerprint ||
        !request->result || (!request->arguments && request->argument_count) ||
        (request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH &&
         request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    const XrTargetPlan *plan = request->verified_plan;
    const XrFingerprint *fingerprint = request->required_plan_fingerprint;
    if (!xr_target_plan_is_verified(plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(plan), *fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    char error[512] = {0};
    if (!xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_instruction_program_verify(plan, error, sizeof(error)))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (xr_target_plan_function_execution_family_mask(plan, request->function) !=
            XR_TARGET_EXECUTION_LEAF_AGGREGATE_I64X2 ||
        !function_has_zero_lifecycle(plan, request->function))
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(plan, fingerprint, request->function, &limits, &frame) !=
        XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    XrTypedDispatchExecution execution = {
        .plan = plan,
        .fingerprint = fingerprint,
        .limits = limits,
        .remaining_steps = XR_TYPED_DISPATCH_MAX_STEPS,
        .call_depth = 1,
        .next_frame_id = 1,
        .provider = request->provider,
    };
    uint64_t ignored_scalar_result = 0;
    XrTypedAggregateValue aggregate_result = {0};
    XrTypedDispatchStatus status = execute_function(
        &execution, frame, request->function, NULL, request->arguments, request->argument_count,
        false, 0, XR_VM_TRACE_ID_NONE, &ignored_scalar_result, &aggregate_result);
    if (free_scalar_frame(&frame) != XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (status == XR_TYPED_DISPATCH_OK)
        *request->result = aggregate_result.pair;
    return status;
}

XrTypedDispatchStatus xr_typed_dispatch_execute_leaf_value_product_tuple6(
    const XrTypedDispatchLeafValueProductTuple6Request *request) {
    if (request && request->result)
        memset(request->result, 0, sizeof(*request->result));
    if (!request || !request->verified_plan || !request->required_plan_fingerprint ||
        !request->result ||
        (request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH &&
         request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    const XrTargetPlan *plan = request->verified_plan;
    const XrFingerprint *fingerprint = request->required_plan_fingerprint;
    if (!xr_target_plan_is_verified(plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(plan), *fingerprint))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    char error[512] = {0};
    if (!xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_instruction_program_verify(plan, error, sizeof(error)))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (xr_target_plan_function_execution_family_mask(plan, request->function) !=
            XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6 ||
        !function_has_zero_lifecycle(plan, request->function))
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(plan, fingerprint, request->function, &limits, &frame) !=
        XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    XrTypedDispatchExecution execution = {
        .plan = plan,
        .fingerprint = fingerprint,
        .limits = limits,
        .remaining_steps = XR_TYPED_DISPATCH_MAX_STEPS,
        .call_depth = 1,
        .next_frame_id = 1,
        .provider = request->provider,
    };
    uint64_t ignored_scalar_result = 0;
    XrTypedAggregateValue product_result = {0};
    XrTypedDispatchStatus status =
        execute_function(&execution, frame, request->function, NULL, NULL, 0, false, 0,
                         XR_VM_TRACE_ID_NONE, &ignored_scalar_result, &product_result);
    if (free_scalar_frame(&frame) != XR_TYPED_DISPATCH_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    if (status == XR_TYPED_DISPATCH_OK)
        *request->result = product_result.product;
    return status;
}

XrTypedDispatchStatus xr_typed_dispatch_execute_values(const XrTypedDispatchValueRequest *request) {
    if (!request || !request->verified_plan || !request->required_plan_fingerprint ||
        !request->arguments || !request->array_push || request->argument_count != 2 ||
        (request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH &&
         request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    const XrTargetPlan *plan = request->verified_plan;
    if (!xr_target_plan_is_verified(plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(plan),
                              *request->required_plan_fingerprint) ||
        !xr_target_plan_fingerprint_is_intact(plan))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    char error[512] = {0};
    if (!xr_target_instruction_program_verify(plan, error, sizeof(error)))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (xr_target_plan_function_execution_family_mask(plan, request->function) !=
            XR_TARGET_EXECUTION_MANAGED_ARRAY_PUSH_TAGGED ||
        !function_has_zero_lifecycle(plan, request->function))
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    /* The public XrValue edge is untrusted.  TargetPlan proves the exact
     * source-class type statically; the runtime carrier must still be an
     * instance rather than an arbitrary tagged value before ownership moves. */
    if (!XR_IS_ARRAY(request->arguments[0]))
        return XR_TYPED_DISPATCH_ARRAY_PUSH_INVALID_RECEIVER;
    if (!XR_IS_INSTANCE(request->arguments[1]))
        return XR_TYPED_DISPATCH_ARRAY_PUSH_TYPE_MISMATCH;

    XrValue original_element = request->arguments[1];
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = NULL;
    if (xr_typed_frame_create(plan, request->required_plan_fingerprint, request->function, &limits,
                              &frame) != XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    XrTypedDispatchExecution execution = {
        .plan = plan,
        .fingerprint = request->required_plan_fingerprint,
        .limits = limits,
        .remaining_steps = XR_TYPED_DISPATCH_MAX_STEPS,
        .call_depth = 1,
        .next_frame_id = 1,
        .provider = request->provider,
        .value_arguments = request->arguments,
        .value_argument_count = request->argument_count,
        .array_push = request->array_push,
    };
    uint64_t ignored_result = 0;
    XrTypedDispatchStatus status =
        execute_function(&execution, frame, request->function, NULL, NULL, request->argument_count,
                         false, 0, XR_VM_TRACE_ID_NONE, &ignored_result, NULL);

    /* Once PARAM_DYN_OWNED clears argument 1, every failure must move that
     * exact owner back before the frame can be destroyed. */
    if (status != XR_TYPED_DISPATCH_OK && XR_IS_NULL(request->arguments[1])) {
        uint32_t row_count = 0;
        const XrTargetInstructionRecord *rows =
            xr_target_plan_function_instructions(plan, request->function, &row_count);
        XrValue restored = xr_null();
        if (!rows || row_count != 4 ||
            take_owned_value(frame, rows[1].result_slot, &restored) != XR_TYPED_DISPATCH_OK ||
            memcmp(&restored, &original_element, sizeof(restored)) != 0) {
            status = XR_TYPED_DISPATCH_FRAME_ERROR;
        } else {
            request->arguments[1] = restored;
        }
    }
    if (xr_typed_frame_free(&frame) != XR_TYPED_FRAME_OK)
        status = XR_TYPED_DISPATCH_FRAME_ERROR;
    return status;
}

XrTypedDispatchStatus xr_typed_coroutine_i64_create(const XrTypedCoroutineI64Request *request,
                                                    XrTypedCoroutineI64 **coroutine) {
    if (!coroutine || *coroutine)
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    if (!request || !request->verified_plan || !request->required_plan_fingerprint ||
        (!request->arguments && request->argument_count) ||
        (request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH &&
         request->provider != XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE))
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    if (request->argument_count > XR_TARGET_INSTRUCTION_MAX_PARAMETERS)
        return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
    if (!xr_target_plan_is_verified(request->verified_plan))
        return XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(request->verified_plan),
                              *request->required_plan_fingerprint) ||
        !xr_target_plan_fingerprint_is_intact(request->verified_plan))
        return XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH;
    if (xr_target_plan_function_execution_family_mask(request->verified_plan, request->function) !=
            XR_TARGET_EXECUTION_SCALAR_I64_COROUTINE ||
        !function_has_zero_managed_lifecycle(request->verified_plan, request->function))
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;

    XrTypedCoroutineI64 *created = (XrTypedCoroutineI64 *) xr_calloc(1, sizeof(*created));
    if (!created)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    created->fingerprint = *request->required_plan_fingerprint;
    created->function = request->function;
    created->argument_count = request->argument_count;
    created->next_instruction = 0;
    created->suspended_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    created->plan = xr_target_plan_retain((XrTargetPlan *) request->verified_plan);
    if (!created->plan)
        goto allocation_failed;
    if (xr_typed_decoded_cache_create(created->plan, &created->fingerprint, NULL,
                                      &created->cache) != XR_VM_DECODED_CACHE_OK)
        goto program_invalid;
    XrVmDecodedFunctionView view = {0};
    if (!xr_typed_decoded_cache_function(created->cache, created->function, &view) ||
        view.parameter_count != created->argument_count || !view.instructions ||
        !view.instruction_count)
        goto argument_mismatch;
    if (request->argument_count) {
        created->arguments =
            (int64_t *) xr_malloc((size_t) request->argument_count * sizeof(*created->arguments));
        if (!created->arguments)
            goto allocation_failed;
        memcpy(created->arguments, request->arguments,
               (size_t) request->argument_count * sizeof(*created->arguments));
    }
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    if (xr_typed_frame_create(created->plan, &created->fingerprint, created->function, &limits,
                              &created->frame) != XR_TYPED_FRAME_OK)
        goto allocation_failed;
    created->execution = (XrTypedDispatchExecution) {
        .plan = created->plan,
        .fingerprint = &created->fingerprint,
        .limits = limits,
        .remaining_steps = XR_TYPED_DISPATCH_MAX_STEPS,
        .call_depth = 1,
        .next_frame_id = 1,
        .decoded_cache = created->cache,
        .provider = request->provider,
    };
    *coroutine = created;
    return XR_TYPED_DISPATCH_OK;

argument_mismatch:
    xr_typed_decoded_cache_free(created->cache);
    xr_free(created->arguments);
    xr_target_plan_free(created->plan);
    xr_free(created);
    return XR_TYPED_DISPATCH_ARGUMENT_MISMATCH;
program_invalid:
    xr_free(created->arguments);
    xr_target_plan_free(created->plan);
    xr_free(created);
    return XR_TYPED_DISPATCH_PROGRAM_INVALID;
allocation_failed:
    xr_typed_decoded_cache_free(created->cache);
    xr_free(created->arguments);
    xr_target_plan_free(created->plan);
    xr_free(created);
    return XR_TYPED_DISPATCH_FRAME_ERROR;
}

XrTypedDispatchStatus xr_typed_coroutine_i64_resume(XrTypedCoroutineI64 *coroutine, int64_t *result,
                                                    uint32_t *suspended_state) {
    if (result)
        *result = 0;
    if (suspended_state)
        *suspended_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    if (!coroutine || !result || !suspended_state)
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    if (coroutine->terminal)
        return XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    XrVmDecodedFunctionView view = {0};
    if (!xr_typed_decoded_cache_function(coroutine->cache, coroutine->function, &view) ||
        !view.instructions || coroutine->next_instruction >= view.instruction_count)
        return XR_TYPED_DISPATCH_PROGRAM_INVALID;
    if (coroutine->suspended &&
        xr_typed_frame_resume_coroutine_state(coroutine->frame, coroutine->suspended_state) !=
            XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;

    XrTypedDispatchFunctionRun run = {XR_VM_TRACE_ID_NONE, XR_VM_TRACE_ID_NONE,
                                      XR_TARGET_INSTRUCTION_INVALID};
    coroutine->execution.resume_start_instruction = coroutine->next_instruction;
    XrTypedDispatchStatus status = execute_function_rows(
        &coroutine->execution, coroutine->frame, coroutine->function, coroutine->arguments, NULL,
        coroutine->argument_count, coroutine->started, 0, XR_VM_TRACE_ID_NONE, 0, NULL, &view,
        view.instruction_count, &coroutine->return_bits, NULL, &run);
    uint32_t state = coroutine->execution.row_suspend_state;
    coroutine->started = true;
    coroutine->suspended = false;
    if (status == XR_TYPED_DISPATCH_SUSPENDED) {
        if (run.current_instruction < view.instructions[0].row.id)
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
        uint32_t local = run.current_instruction - view.instructions[0].row.id;
        if (local >= view.instruction_count ||
            view.instructions[local].row.opcode != XR_TARGET_INSTRUCTION_SUSPEND ||
            view.instructions[local].target_if_nonzero >= view.instruction_count ||
            state == XR_TYPED_FRAME_CONTEXT_INDEX_NONE)
            return XR_TYPED_DISPATCH_PROGRAM_INVALID;
        coroutine->next_instruction = view.instructions[local].target_if_nonzero;
        coroutine->suspended_state = state;
        coroutine->suspended = true;
        *suspended_state = state;
        return XR_TYPED_DISPATCH_SUSPENDED;
    }
    coroutine->terminal = true;
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    memcpy(result, &coroutine->return_bits, sizeof(*result));
    return XR_TYPED_DISPATCH_OK;
}

XrTypedDispatchStatus xr_typed_coroutine_i64_cancel(XrTypedCoroutineI64 *coroutine) {
    if (!coroutine)
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    if (coroutine->terminal)
        return XR_TYPED_DISPATCH_OK;
    if (coroutine->suspended &&
        xr_typed_frame_resume_coroutine_state(coroutine->frame, coroutine->suspended_state) !=
            XR_TYPED_FRAME_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    coroutine->suspended = false;
    coroutine->terminal = true;
    return XR_TYPED_DISPATCH_OK;
}

XrTypedDispatchStatus xr_typed_coroutine_i64_free(XrTypedCoroutineI64 **coroutine) {
    if (!coroutine)
        return XR_TYPED_DISPATCH_INVALID_ARGUMENT;
    if (!*coroutine)
        return XR_TYPED_DISPATCH_OK;
    XrTypedDispatchStatus status = xr_typed_coroutine_i64_cancel(*coroutine);
    if (status != XR_TYPED_DISPATCH_OK)
        return status;
    if (free_scalar_frame(&(*coroutine)->frame) != XR_TYPED_DISPATCH_OK)
        return XR_TYPED_DISPATCH_FRAME_ERROR;
    xr_typed_decoded_cache_free((*coroutine)->cache);
    xr_free((*coroutine)->arguments);
    xr_target_plan_free((*coroutine)->plan);
    xr_free(*coroutine);
    *coroutine = NULL;
    return XR_TYPED_DISPATCH_OK;
}
