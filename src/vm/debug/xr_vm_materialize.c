/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_materialize.c - Runtime-only trace fact materialization
 */

#include "xr_vm_materialize.h"
#include <string.h>

static bool materialize_slot(const XrTargetSlotRecord *slots,
                             uint32_t slot_count, uint32_t function,
                             uint32_t slot, XrVmMaterializedSlot *out) {
    out->availability = XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    if (slot == XR_TARGET_INSTRUCTION_SLOT_NONE)
        return true;
    if (!slots || slot >= slot_count || slots[slot].id != slot ||
        slots[slot].function != function)
        return false;
    const XrTargetSlotRecord *source = &slots[slot];
    out->availability = XR_VM_DEBUG_FACT_AVAILABLE;
    out->role = source->role;
    out->root_kind = source->root_kind;
    out->ownership = source->ownership;
    out->id = source->id;
    out->identity = source->identity;
    out->offset = source->offset;
    out->size = source->size;
    out->alignment = source->align;
    out->register_rep = source->register_rep;
    out->memory_rep = source->memory_rep;
    return true;
}

XrVmMaterializeStatus xr_typed_materialize_event(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint,
    const XrVmTraceEvent *event, XrVmMaterializedEvent *materialized) {
    if (materialized)
        memset(materialized, 0, sizeof(*materialized));
    if (!verified_plan || !required_plan_fingerprint || !event ||
        !materialized)
        return XR_VM_MATERIALIZE_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(verified_plan) ||
        !xr_target_plan_fingerprint_is_intact(verified_plan))
        return XR_VM_MATERIALIZE_PLAN_NOT_VERIFIED;
    XrFingerprint actual = xr_target_plan_fingerprint(verified_plan);
    if (!xr_fingerprint_equal(actual, *required_plan_fingerprint) ||
        !xr_fingerprint_equal(actual, event->target_plan_fingerprint))
        return XR_VM_MATERIALIZE_PLAN_IDENTITY_MISMATCH;
    if (event->schema_version != XR_VM_TRACE_SCHEMA_VERSION ||
        event->kind >= XR_VM_TRACE_EVENT_KIND_COUNT)
        return XR_VM_MATERIALIZE_EVENT_INVALID;

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(verified_plan, &function_count);
    if (!functions || event->function >= function_count ||
        functions[event->function].id != event->function)
        return XR_VM_MATERIALIZE_EVENT_INVALID;
    materialized->event = *event;
    materialized->function_identity = XR_VM_DEBUG_FACT_AVAILABLE;
    materialized->generation_identity =
        event->generation_identity_present
            ? XR_VM_DEBUG_FACT_AVAILABLE
            : XR_VM_DEBUG_FACT_CONTEXT_UNAVAILABLE;
    materialized->source_span = XR_VM_DEBUG_FACT_SCHEMA_UNAVAILABLE;
    materialized->owner_identity = XR_VM_DEBUG_FACT_SCHEMA_UNAVAILABLE;
    materialized->layout_identity = XR_VM_DEBUG_FACT_SCHEMA_UNAVAILABLE;
    materialized->semantic_function =
        functions[event->function].semantic_function;
    materialized->result.availability =
        XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    materialized->operands[0].availability =
        XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    materialized->operands[1].availability =
        XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    materialized->call_identity = XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    materialized->callee_function = XR_VM_TRACE_ID_NONE;

    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(verified_plan, &slot_count);
    if (event->instruction != XR_VM_TRACE_ID_NONE) {
        uint32_t instruction_count = 0;
        const XrTargetInstructionRecord *instructions =
            xr_target_plan_instructions(verified_plan, &instruction_count);
        if (!instructions || event->instruction >= instruction_count ||
            instructions[event->instruction].id != event->instruction ||
            instructions[event->instruction].function != event->function ||
            instructions[event->instruction].opcode != event->opcode)
            return XR_VM_MATERIALIZE_EVENT_INVALID;
        const XrTargetInstructionRecord *row =
            &instructions[event->instruction];
        if (!materialize_slot(slots, slot_count, event->function,
                              row->result_slot, &materialized->result) ||
            !materialize_slot(slots, slot_count, event->function,
                              row->operand_slots[0],
                              &materialized->operands[0]) ||
            !materialize_slot(slots, slot_count, event->function,
                              row->operand_slots[1],
                              &materialized->operands[1]))
            return XR_VM_MATERIALIZE_EVENT_INVALID;
    }
    if (event->call != XR_VM_TRACE_ID_NONE) {
        uint32_t call_count = 0;
        const XrTargetCallRecord *calls =
            xr_target_plan_calls(verified_plan, &call_count);
        if (!calls || event->call >= call_count ||
            calls[event->call].id != event->call ||
            calls[event->call].caller_function != event->function)
            return XR_VM_MATERIALIZE_EVENT_INVALID;
        materialized->call_identity = XR_VM_DEBUG_FACT_AVAILABLE;
        materialized->call_stable_identity = calls[event->call].identity;
        materialized->callee_function = calls[event->call].callee_function;
    }
    return XR_VM_MATERIALIZE_OK;
}
