/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_frame.c - Verified TargetPlan slot arena implementation
 *
 * KEY CONCEPT:
 *   Slot bytes have no runtime tag. The retained verified plan is the sole
 *   authority for layout, representation, identity, and frame ownership.
 */

#include "xr_typed_frame.h"
#include "../base/xmalloc.h"
#include "../plan/target/xr_target_verify.h"
#include <stdint.h>
#include <string.h>

struct XrTypedFrame {
    XrTargetPlan *plan;
    XrFingerprint plan_fingerprint;
    const XrTargetFunctionRecord *function;
    const XrTargetSlotRecord *slots;
    uint8_t *allocation;
    uint8_t *arena;
    uint8_t *states;
    size_t allocation_size;
    uint32_t slot_count;
    bool cleaned;
};

typedef struct XrTypedFrameShape {
    const XrTargetFunctionRecord *function;
    const XrTargetSlotRecord *slots;
    size_t arena_allocation_bytes;
} XrTypedFrameShape;

static bool is_power_of_two(size_t value) {
    return value && (value & (value - 1u)) == 0;
}

static bool checked_add_size(size_t left, size_t right, size_t *result) {
    if (!result || left > SIZE_MAX - right)
        return false;
    *result = left + right;
    return true;
}

static bool supported_rep_kind(uint16_t kind) {
    return kind == XR_MACHINE_REP_VOID ||
           (kind >= XR_MACHINE_REP_I1 && kind <= XR_MACHINE_REP_RUNE);
}

static bool supported_stored_rep(const XrTargetMachineRepRecord *rep) {
    return rep && rep->kind >= XR_MACHINE_REP_I1 &&
           rep->kind <= XR_MACHINE_REP_RUNE &&
           rep->root_kind == XR_TARGET_ROOT_NONE &&
           rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool limits_are_bounded(const XrTypedFrameLimits *limits) {
    return limits && limits->max_arena_bytes <= XR_TYPED_FRAME_MAX_ARENA_BYTES &&
           limits->max_slot_count <= XR_TYPED_FRAME_MAX_SLOT_COUNT &&
           limits->max_total_bytes <= XR_TYPED_FRAME_MAX_TOTAL_BYTES;
}

static XrTypedFrameStatus validate_plan_identity(
    const XrTargetPlan *plan, const XrFingerprint *required_fingerprint) {
    if (!plan || !required_fingerprint)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(plan) ||
        xr_target_plan_schema_version(plan) !=
            XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION)
        return XR_TYPED_FRAME_PLAN_NOT_VERIFIED;
    if (xr_target_plan_completed_family_mask(plan) !=
        XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK)
        return XR_TYPED_FRAME_UNSUPPORTED_FAMILY;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(plan),
                              *required_fingerprint))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    char verifier_error[512] = {0};
    if (!xr_target_plan_verify(plan, verifier_error, sizeof(verifier_error)))
        return XR_TYPED_FRAME_PLAN_NOT_VERIFIED;
    uint32_t rep_count = 0;
    const XrTargetMachineRepRecord *reps =
        xr_target_plan_machine_reps(plan, &rep_count);
    for (uint32_t i = 0; i < rep_count; i++)
        if (!supported_rep_kind(reps[i].kind))
            return XR_TYPED_FRAME_UNSUPPORTED_FAMILY;
    return XR_TYPED_FRAME_OK;
}

static XrTypedFrameStatus validate_shape(const XrTargetPlan *plan,
                                         uint32_t function_index,
                                         const XrTypedFrameLimits *limits,
                                         XrTypedFrameShape *shape) {
    if (!limits_are_bounded(limits) || !shape)
        return limits ? XR_TYPED_FRAME_BUDGET_EXHAUSTED
                      : XR_TYPED_FRAME_INVALID_ARGUMENT;
    uint32_t function_count = 0;
    uint32_t total_slots = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &total_slots);
    if (!functions || function_index >= function_count)
        return XR_TYPED_FRAME_FUNCTION_INVALID;
    const XrTargetFunctionRecord *function = &functions[function_index];
    if (function->id != function_index || function->semantic_function != function_index ||
        function->slot_begin > total_slots ||
        function->slot_count > total_slots - function->slot_begin ||
        !is_power_of_two(function->frame_align) ||
        function->frame_align > XR_TYPED_FRAME_MAX_ALIGNMENT)
        return XR_TYPED_FRAME_FUNCTION_INVALID;
    if (function->frame_size > limits->max_arena_bytes ||
        function->slot_count > limits->max_slot_count)
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;

    size_t arena_allocation = 0;
    if (function->frame_size &&
        !checked_add_size(function->frame_size, function->frame_align - 1u,
                          &arena_allocation))
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;
    if (arena_allocation > limits->max_arena_bytes)
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;
    size_t total_allocation = sizeof(XrTypedFrame);
    if (!checked_add_size(total_allocation, arena_allocation, &total_allocation) ||
        !checked_add_size(total_allocation, function->slot_count,
                          &total_allocation) ||
        total_allocation > limits->max_total_bytes)
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;

    for (uint32_t i = 0; i < function->slot_count; i++) {
        uint32_t global_slot = function->slot_begin + i;
        const XrTargetSlotRecord *slot = &slots[global_slot];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(plan, slot->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(plan, slot->memory_rep);
        if (slot->id != global_slot || slot->function != function_index ||
            !slot->size || !is_power_of_two(slot->align) ||
            slot->align > function->frame_align || slot->offset % slot->align != 0 ||
            slot->offset > function->frame_size ||
            slot->size > function->frame_size - slot->offset ||
            !supported_stored_rep(register_rep) ||
            !supported_stored_rep(memory_rep) ||
            slot->size != memory_rep->memory_size ||
            slot->align != memory_rep->memory_align ||
            slot->root_kind != XR_TARGET_ROOT_NONE ||
            slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
            return XR_TYPED_FRAME_SLOT_INVALID;
    }
    shape->function = function;
    shape->slots = function->slot_count ? slots + function->slot_begin : NULL;
    shape->arena_allocation_bytes = arena_allocation;
    return XR_TYPED_FRAME_OK;
}

static XrTypedFrameStatus allocate_frame(const XrTargetPlan *plan,
                                         const XrTypedFrameShape *shape,
                                         XrTypedFrame **out) {
    XrTypedFrame *frame = (XrTypedFrame *) xr_calloc(1, sizeof(*frame));
    if (!frame)
        return XR_TYPED_FRAME_ALLOCATION_FAILED;
    if (shape->arena_allocation_bytes) {
        frame->allocation = (uint8_t *) xr_malloc(shape->arena_allocation_bytes);
        if (!frame->allocation) {
            xr_free(frame);
            return XR_TYPED_FRAME_ALLOCATION_FAILED;
        }
        memset(frame->allocation, 0, shape->arena_allocation_bytes);
        uintptr_t base = (uintptr_t) frame->allocation;
        uintptr_t mask = (uintptr_t) shape->function->frame_align - 1u;
        if (base > UINTPTR_MAX - mask) {
            xr_free(frame->allocation);
            xr_free(frame);
            return XR_TYPED_FRAME_ALLOCATION_FAILED;
        }
        frame->arena = (uint8_t *) ((base + mask) & ~mask);
    }
    if (shape->function->slot_count) {
        frame->states = (uint8_t *) xr_calloc(shape->function->slot_count, 1);
        if (!frame->states) {
            xr_free(frame->allocation);
            xr_free(frame);
            return XR_TYPED_FRAME_ALLOCATION_FAILED;
        }
        memset(frame->states, XR_TYPED_SLOT_STATE_UNINITIALIZED,
               shape->function->slot_count);
    }
    frame->plan = xr_target_plan_retain((XrTargetPlan *) plan);
    frame->plan_fingerprint = xr_target_plan_fingerprint(plan);
    frame->function = shape->function;
    frame->slots = shape->slots;
    frame->allocation_size = shape->arena_allocation_bytes;
    frame->slot_count = shape->function->slot_count;
    *out = frame;
    return XR_TYPED_FRAME_OK;
}

static bool frame_plan_identity_is_intact(const XrTypedFrame *frame) {
    return frame->plan && xr_target_plan_is_verified(frame->plan) &&
           xr_target_plan_schema_version(frame->plan) ==
               XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION &&
           xr_target_plan_completed_family_mask(frame->plan) ==
               XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK &&
           xr_fingerprint_equal(frame->plan_fingerprint,
                                xr_target_plan_fingerprint(frame->plan));
}

static XrTypedFrameStatus validate_access(const XrTypedFrame *frame,
                                          const XrTypedSlotAccess *access,
                                          uint32_t *local_slot) {
    if (!frame || !access || !local_slot)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (access->slot < frame->function->slot_begin ||
        access->slot - frame->function->slot_begin >= frame->slot_count)
        return XR_TYPED_FRAME_SLOT_INVALID;
    uint32_t local = access->slot - frame->function->slot_begin;
    const XrTargetSlotRecord *slot = &frame->slots[local];
    if (!slot->size || !is_power_of_two(slot->align))
        return XR_TYPED_FRAME_SLOT_INVALID;
    if (!xr_stable_id_equal(access->identity, slot->identity) ||
        access->size != slot->size || access->alignment != slot->align ||
        access->register_rep != slot->register_rep ||
        access->memory_rep != slot->memory_rep || access->reserved != 0)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    if (!frame->arena || slot->offset > frame->function->frame_size ||
        slot->size > frame->function->frame_size - slot->offset ||
        ((uintptr_t) (frame->arena + slot->offset) & (slot->align - 1u)) != 0)
        return XR_TYPED_FRAME_SLOT_INVALID;
    *local_slot = local;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC void xr_typed_frame_limits_default(XrTypedFrameLimits *limits) {
    if (!limits)
        return;
    *limits = (XrTypedFrameLimits) {
        .max_arena_bytes = XR_TYPED_FRAME_MAX_ARENA_BYTES,
        .max_slot_count = XR_TYPED_FRAME_MAX_SLOT_COUNT,
        .max_total_bytes = XR_TYPED_FRAME_MAX_TOTAL_BYTES,
    };
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_create(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    const XrTypedFrameLimits *limits, XrTypedFrame **frame) {
    if (frame)
        *frame = NULL;
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    XrTypedFrameStatus status =
        validate_plan_identity(verified_plan, required_plan_fingerprint);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    XrTypedFrameShape shape = {0};
    status = validate_shape(verified_plan, function, limits, &shape);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    return allocate_frame(verified_plan, &shape, frame);
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_describe_slot(
    const XrTypedFrame *frame, uint32_t slot, XrTypedSlotAccess *access) {
    if (access)
        memset(access, 0, sizeof(*access));
    if (!frame || !access)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (slot < frame->function->slot_begin ||
        slot - frame->function->slot_begin >= frame->slot_count)
        return XR_TYPED_FRAME_SLOT_INVALID;
    const XrTargetSlotRecord *record =
        &frame->slots[slot - frame->function->slot_begin];
    *access = (XrTypedSlotAccess) {
        .identity = record->identity,
        .slot = record->id,
        .size = record->size,
        .alignment = record->align,
        .register_rep = record->register_rep,
        .memory_rep = record->memory_rep,
    };
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_store(
    XrTypedFrame *frame, const XrTypedSlotAccess *access, const void *bytes,
    size_t size) {
    if (!bytes)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    uint32_t local = 0;
    XrTypedFrameStatus status = validate_access(frame, access, &local);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (size != access->size)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    if (frame->states[local] == XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_POISONED;
    memcpy(frame->arena + frame->slots[local].offset, bytes, size);
    frame->states[local] = XR_TYPED_SLOT_STATE_INITIALIZED;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_load(
    const XrTypedFrame *frame, const XrTypedSlotAccess *access, void *bytes,
    size_t size) {
    if (!bytes)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    uint32_t local = 0;
    XrTypedFrameStatus status = validate_access(frame, access, &local);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (size != access->size)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    if (frame->states[local] == XR_TYPED_SLOT_STATE_UNINITIALIZED)
        return XR_TYPED_FRAME_UNINITIALIZED;
    if (frame->states[local] == XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_POISONED;
    if (frame->states[local] != XR_TYPED_SLOT_STATE_INITIALIZED)
        return XR_TYPED_FRAME_SLOT_INVALID;
    memcpy(bytes, frame->arena + frame->slots[local].offset, size);
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_poison(
    XrTypedFrame *frame, const XrTypedSlotAccess *access) {
    uint32_t local = 0;
    XrTypedFrameStatus status = validate_access(frame, access, &local);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (frame->states[local] == XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_POISONED;
    frame->states[local] = XR_TYPED_SLOT_STATE_POISONED;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_slot_state(
    const XrTypedFrame *frame, uint32_t slot, XrTypedSlotState *state) {
    if (state)
        *state = XR_TYPED_SLOT_STATE_INVALID;
    if (!frame || !state)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (slot < frame->function->slot_begin ||
        slot - frame->function->slot_begin >= frame->slot_count)
        return XR_TYPED_FRAME_SLOT_INVALID;
    uint8_t stored = frame->states[slot - frame->function->slot_begin];
    if (stored < XR_TYPED_SLOT_STATE_UNINITIALIZED ||
        stored > XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_SLOT_INVALID;
    *state = (XrTypedSlotState) stored;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC size_t xr_typed_frame_arena_size(const XrTypedFrame *frame) {
    return frame && !frame->cleaned ? frame->function->frame_size : 0;
}

XR_FUNC uint32_t xr_typed_frame_slot_count(const XrTypedFrame *frame) {
    return frame && !frame->cleaned ? frame->slot_count : 0;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_cleanup(XrTypedFrame *frame) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    frame->cleaned = true;
    if (frame->allocation) {
        memset(frame->allocation, 0, frame->allocation_size);
        xr_free(frame->allocation);
    }
    if (frame->states) {
        memset(frame->states, XR_TYPED_SLOT_STATE_INVALID, frame->slot_count);
        xr_free(frame->states);
    }
    xr_target_plan_free(frame->plan);
    frame->plan = NULL;
    frame->function = NULL;
    frame->slots = NULL;
    frame->allocation = NULL;
    frame->arena = NULL;
    frame->states = NULL;
    frame->allocation_size = 0;
    frame->slot_count = 0;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC void xr_typed_frame_free(XrTypedFrame *frame) {
    if (!frame)
        return;
    (void) xr_typed_frame_cleanup(frame);
    xr_free(frame);
}
