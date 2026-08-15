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
#include "../plan/target/xr_target_profile.h"
#include <stdint.h>
#include <string.h>

struct XrTypedFrame {
    XrTargetPlan *plan;
    XrFingerprint plan_fingerprint;
    const XrTargetFunctionRecord *function;
    const XrTargetSlotRecord *slots;
    uint32_t function_index;
    uint32_t current_block_instruction;
    uint32_t current_instruction;
    uint32_t coroutine_state;
    uint64_t generation_number;
    XrFingerprint generation_fingerprint;
    struct XrTypedFrame *parent;
    struct XrTypedFrame *child;
    uint8_t *allocation;
    uint8_t *arena;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    uint8_t *states;
#endif
    size_t allocation_size;
    size_t arena_size;
    uint32_t slot_begin;
    uint32_t slot_count;
    uint16_t frame_alignment;
    bool generation_bound;
    bool cleaned;
};

typedef struct XrTypedFrameShape {
    const XrTargetFunctionRecord *function;
    const XrTargetSlotRecord *slots;
    size_t arena_allocation_bytes;
} XrTypedFrameShape;

typedef enum XrTypedFrameTransfer {
    XR_TYPED_FRAME_TRANSFER_LOAD = 0,
    XR_TYPED_FRAME_TRANSFER_STORE,
} XrTypedFrameTransfer;

typedef struct XrTypedResolvedSlot {
    const XrTargetSlotRecord *record;
    uint8_t *bytes;
    uint32_t local_slot;
} XrTypedResolvedSlot;

static bool is_power_of_two(size_t value) {
    return value && (value & (value - 1u)) == 0;
}

static bool checked_add_size(size_t left, size_t right, size_t *result) {
    if (!result || left > SIZE_MAX - right)
        return false;
    *result = left + right;
    return true;
}

static bool reps_are_storage_compatible(
    const XrTargetMachineRepRecord *register_rep,
    const XrTargetMachineRepRecord *memory_rep) {
    return register_rep && memory_rep &&
           register_rep->kind == memory_rep->kind &&
           register_rep->register_bits == memory_rep->register_bits &&
           register_rep->memory_size == memory_rep->memory_size &&
           register_rep->memory_align == memory_rep->memory_align &&
           register_rep->signedness == memory_rep->signedness &&
           register_rep->root_kind == memory_rep->root_kind &&
           register_rep->ownership == memory_rep->ownership &&
           register_rep->null_encoding == memory_rep->null_encoding &&
           register_rep->detail == memory_rep->detail &&
           register_rep->lane_count == memory_rep->lane_count;
}

/* This is a representation transport boundary, not a lifecycle authority.
 * Rooted, owned, and borrowed bytes remain fail closed until the executor has
 * exact root, lifetime, and cleanup operations. A transported raw pointer is
 * opaque object representation: this function never dereferences it. */
static bool stored_rep_is_transportable(
    const XrTargetMachineRepRecord *rep) {
    if (!rep || rep->id > UINT16_MAX || rep->reserved != 0 ||
        rep->kind >= XR_MACHINE_REP_COUNT ||
        !rep->memory_size || !is_power_of_two(rep->memory_align) ||
        rep->memory_align > rep->memory_size || rep->lane_count != 0)
        return false;
    if (rep->kind >= XR_MACHINE_REP_I1 &&
        rep->kind <= XR_MACHINE_REP_RUNE)
        return rep->root_kind == XR_TARGET_ROOT_NONE &&
               rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
    switch (rep->kind) {
        case XR_MACHINE_REP_ENUM_ORDINAL:
        case XR_MACHINE_REP_AGGREGATE:
            return rep->root_kind == XR_TARGET_ROOT_NONE &&
                   rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
        case XR_MACHINE_REP_RAW_PTR:
            return rep->root_kind == XR_TARGET_ROOT_NONE &&
                   rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
        default:
            return false;
    }
}

static bool slot_rep_contract_is_exact(
    const XrTargetPlan *plan, const XrTargetSlotRecord *slot) {
    const XrTargetMachineRepRecord *register_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    return slot && stored_rep_is_transportable(register_rep) &&
           stored_rep_is_transportable(memory_rep) &&
           reps_are_storage_compatible(register_rep, memory_rep) &&
           register_rep->id == slot->register_rep &&
           memory_rep->id == slot->memory_rep &&
           slot->size == memory_rep->memory_size &&
           slot->align == memory_rep->memory_align &&
           slot->root_kind == memory_rep->root_kind &&
           slot->ownership == memory_rep->ownership;
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
    if (!xr_target_plan_fingerprint_is_intact(plan))
        return XR_TYPED_FRAME_PLAN_NOT_VERIFIED;
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
    if (!checked_add_size(total_allocation, arena_allocation,
                          &total_allocation) ||
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
        !checked_add_size(total_allocation, function->slot_count,
                          &total_allocation) ||
#endif
        total_allocation > limits->max_total_bytes)
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;

    for (uint32_t i = 0; i < function->slot_count; i++) {
        uint32_t global_slot = function->slot_begin + i;
        const XrTargetSlotRecord *slot = &slots[global_slot];
        if (slot->id != global_slot || slot->function != function_index ||
            slot->reserved != 0 || !slot->size ||
            !is_power_of_two(slot->align) ||
            slot->align > function->frame_align || slot->offset % slot->align != 0 ||
            slot->offset > function->frame_size ||
            slot->size > function->frame_size - slot->offset ||
            !slot_rep_contract_is_exact(plan, slot))
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
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
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
#endif
    frame->plan = xr_target_plan_retain((XrTargetPlan *) plan);
    frame->plan_fingerprint = xr_target_plan_fingerprint(plan);
    frame->function = shape->function;
    frame->slots = shape->slots;
    frame->function_index = shape->function->id;
    frame->current_block_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->current_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->coroutine_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->allocation_size = shape->arena_allocation_bytes;
    frame->arena_size = shape->function->frame_size;
    frame->slot_begin = shape->function->slot_begin;
    frame->slot_count = shape->function->slot_count;
    frame->frame_alignment = shape->function->frame_align;
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

static XrTypedFrameStatus resolve_slot(const XrTypedFrame *frame,
                                       uint32_t slot_index,
                                       const XrTypedSlotAccess *access,
                                       XrTypedResolvedSlot *resolved) {
    if (resolved)
        memset(resolved, 0, sizeof(*resolved));
    if (!frame || !resolved)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (slot_index < frame->slot_begin ||
        slot_index - frame->slot_begin >= frame->slot_count)
        return XR_TYPED_FRAME_SLOT_INVALID;
    uint32_t local = slot_index - frame->slot_begin;
    const XrTargetSlotRecord *slot = &frame->slots[local];
    if (slot->id != slot_index || slot->function != frame->function_index ||
        slot->reserved != 0 || !slot->size ||
        !is_power_of_two(frame->frame_alignment) ||
        !is_power_of_two(slot->align) ||
        slot->align > frame->frame_alignment ||
        slot->offset % slot->align != 0 ||
        !slot_rep_contract_is_exact(frame->plan, slot))
        return XR_TYPED_FRAME_SLOT_INVALID;
    if (access &&
        (!xr_stable_id_equal(access->identity, slot->identity) ||
         access->slot != slot->id || access->size != slot->size ||
         access->alignment != slot->align ||
         access->register_rep != slot->register_rep ||
         access->memory_rep != slot->memory_rep || access->reserved != 0))
        return XR_TYPED_FRAME_ACCESS_MISMATCH;

    size_t slot_end = 0;
    if (!checked_add_size(slot->offset, slot->size, &slot_end) ||
        slot_end > frame->arena_size || !frame->allocation ||
        !frame->arena)
        return XR_TYPED_FRAME_SLOT_INVALID;
    uintptr_t allocation_address = (uintptr_t) frame->allocation;
    uintptr_t arena_address = (uintptr_t) frame->arena;
    if (arena_address < allocation_address)
        return XR_TYPED_FRAME_SLOT_INVALID;
    uintptr_t prefix_value = arena_address - allocation_address;
    if (prefix_value > SIZE_MAX || prefix_value >= frame->frame_alignment)
        return XR_TYPED_FRAME_SLOT_INVALID;
    size_t arena_prefix = (size_t) prefix_value;
    size_t allocation_end = 0;
    size_t byte_offset = 0;
    if (!checked_add_size(arena_prefix, slot_end, &allocation_end) ||
        allocation_end > frame->allocation_size ||
        !checked_add_size(arena_prefix, slot->offset, &byte_offset) ||
        byte_offset >= frame->allocation_size)
        return XR_TYPED_FRAME_SLOT_INVALID;
    uint8_t *slot_bytes = frame->allocation + byte_offset;
    if (((uintptr_t) slot_bytes & (slot->align - 1u)) != 0)
        return XR_TYPED_FRAME_SLOT_INVALID;
    resolved->record = slot;
    resolved->bytes = slot_bytes;
    resolved->local_slot = local;
    return XR_TYPED_FRAME_OK;
}

static XrTypedFrameStatus transfer_slot(
    const XrTypedFrame *frame, const XrTypedSlotAccess *access,
    const void *source, void *destination, size_t size,
    XrTypedFrameTransfer transfer) {
    if ((transfer == XR_TYPED_FRAME_TRANSFER_STORE && !source) ||
        (transfer == XR_TYPED_FRAME_TRANSFER_LOAD && !destination))
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (!access)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    XrTypedResolvedSlot resolved = {0};
    XrTypedFrameStatus status =
        resolve_slot(frame, access->slot, access, &resolved);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (size != resolved.record->size)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    XrTypedSlotState state =
        (XrTypedSlotState) frame->states[resolved.local_slot];
    if (state == XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_POISONED;
    if (transfer == XR_TYPED_FRAME_TRANSFER_LOAD &&
        state == XR_TYPED_SLOT_STATE_UNINITIALIZED)
        return XR_TYPED_FRAME_UNINITIALIZED;
    if (transfer == XR_TYPED_FRAME_TRANSFER_LOAD &&
        state != XR_TYPED_SLOT_STATE_INITIALIZED)
        return XR_TYPED_FRAME_SLOT_INVALID;
#endif
    if (transfer == XR_TYPED_FRAME_TRANSFER_STORE) {
        memcpy(resolved.bytes, source, size);
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
        frame->states[resolved.local_slot] = XR_TYPED_SLOT_STATE_INITIALIZED;
#endif
    } else {
        memcpy(destination, resolved.bytes, size);
    }
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
    XrTypedResolvedSlot resolved = {0};
    XrTypedFrameStatus status = resolve_slot(frame, slot, NULL, &resolved);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    const XrTargetSlotRecord *record = resolved.record;
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
    return transfer_slot(frame, access, bytes, NULL, size,
                         XR_TYPED_FRAME_TRANSFER_STORE);
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_load(
    const XrTypedFrame *frame, const XrTypedSlotAccess *access, void *bytes,
    size_t size) {
    return transfer_slot(frame, access, NULL, bytes, size,
                         XR_TYPED_FRAME_TRANSFER_LOAD);
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_poison(
    XrTypedFrame *frame, const XrTypedSlotAccess *access) {
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    if (!access)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    XrTypedResolvedSlot resolved = {0};
    XrTypedFrameStatus status =
        resolve_slot(frame, access->slot, access, &resolved);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (frame->states[resolved.local_slot] == XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_POISONED;
    frame->states[resolved.local_slot] = XR_TYPED_SLOT_STATE_POISONED;
    return XR_TYPED_FRAME_OK;
#else
    if (!access)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    XrTypedResolvedSlot resolved = {0};
    XrTypedFrameStatus status =
        resolve_slot(frame, access->slot, access, &resolved);
    return status == XR_TYPED_FRAME_OK
               ? XR_TYPED_FRAME_DEBUG_METADATA_UNAVAILABLE
               : status;
#endif
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
    if (slot < frame->slot_begin ||
        slot - frame->slot_begin >= frame->slot_count)
        return XR_TYPED_FRAME_SLOT_INVALID;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    uint8_t stored = frame->states[slot - frame->slot_begin];
    if (stored < XR_TYPED_SLOT_STATE_UNINITIALIZED ||
        stored > XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_SLOT_INVALID;
    *state = (XrTypedSlotState) stored;
    return XR_TYPED_FRAME_OK;
#else
    return XR_TYPED_FRAME_DEBUG_METADATA_UNAVAILABLE;
#endif
}

static bool fingerprint_bytes_match(const uint8_t *bytes,
                                    XrFingerprint fingerprint) {
    return bytes &&
           memcmp(bytes, fingerprint.bytes, sizeof(fingerprint.bytes)) == 0;
}

static bool fingerprint_bytes_nonzero(const uint8_t *bytes, size_t size) {
    if (!bytes)
        return false;
    for (size_t i = 0; i < size; i++)
        if (bytes[i] != 0)
            return true;
    return false;
}

static const XrTargetInstructionRecord *frame_instruction_group(
    const XrTypedFrame *frame, uint32_t *count) {
    if (count)
        *count = 0;
    if (!frame || frame->cleaned || !frame_plan_identity_is_intact(frame))
        return NULL;
    return xr_target_plan_function_instructions(frame->plan,
                                                frame->function_index, count);
}

static bool instruction_local_index(
    const XrTargetInstructionRecord *rows, uint32_t count,
    uint32_t instruction, uint32_t *local) {
    if (!rows || !count || !local || instruction < rows[0].id)
        return false;
    uint32_t index = instruction - rows[0].id;
    if (index >= count || rows[index].id != instruction)
        return false;
    *local = index;
    return true;
}

static uint32_t instruction_block_entry(
    const XrTargetInstructionRecord *rows, uint32_t local) {
    uint32_t block = rows[0].id;
    for (uint32_t i = 1; i <= local; i++)
        if (xr_target_instruction_is_terminator(rows[i - 1u].opcode))
            block = rows[i].id;
    return block;
}

static bool instruction_transition_allowed(
    const XrTypedFrame *frame, const XrTargetInstructionRecord *rows,
    uint32_t count, uint32_t next_local) {
    if (frame->current_instruction == XR_TYPED_FRAME_CONTEXT_INDEX_NONE)
        return next_local == 0;
    uint32_t current_local = 0;
    if (!instruction_local_index(rows, count, frame->current_instruction,
                                 &current_local))
        return false;
    const XrTargetInstructionRecord *current = &rows[current_local];
    const XrTargetInstructionContract *contract =
        xr_target_instruction_contract(current->opcode);
    if (!contract)
        return false;
    switch ((XrTargetInstructionControlKind) contract->control_kind) {
        case XR_TARGET_INSTRUCTION_CONTROL_NONE:
            return current_local + 1u < count &&
                   next_local == current_local + 1u;
        case XR_TARGET_INSTRUCTION_CONTROL_RETURN:
            return false;
        case XR_TARGET_INSTRUCTION_CONTROL_JUMP:
            return next_local == XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(
                                     current->immediate_bits);
        case XR_TARGET_INSTRUCTION_CONTROL_BRANCH:
            return next_local == XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(
                                     current->immediate_bits) ||
                   next_local == XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(
                                     current->immediate_bits);
    }
    return false;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_context(
    const XrTypedFrame *frame, XrTypedFrameContext *context) {
    if (context)
        memset(context, 0, sizeof(*context));
    if (!frame || !context)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    *context = (XrTypedFrameContext) {
        .function_identity =
            {
                .plan_fingerprint = frame->plan_fingerprint,
                .function = frame->function_index,
                .semantic_function = frame->function->semantic_function,
            },
        .block_entry_instruction = frame->current_block_instruction,
        .instruction = frame->current_instruction,
        .coroutine_state = frame->coroutine_state,
        .generation_number = frame->generation_number,
        .generation_fingerprint = frame->generation_fingerprint,
        .generation_bound = frame->generation_bound,
        .has_parent = frame->parent != NULL,
        .has_child = frame->child != NULL,
    };
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_enter_instruction(
    XrTypedFrame *frame, uint32_t instruction) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (frame->child)
        return XR_TYPED_FRAME_CHILD_ACTIVE;
    uint32_t count = 0;
    const XrTargetInstructionRecord *rows =
        frame_instruction_group(frame, &count);
    uint32_t local = 0;
    if (!instruction_local_index(rows, count, instruction, &local))
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    if (!instruction_transition_allowed(frame, rows, count, local))
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    frame->current_instruction = instruction;
    frame->current_block_instruction = instruction_block_entry(rows, local);
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_enter_decoded_instruction(
    XrTypedFrame *frame, uint32_t instruction,
    uint32_t block_entry_instruction) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (frame->child)
        return XR_TYPED_FRAME_CHILD_ACTIVE;
    if (instruction == XR_TYPED_FRAME_CONTEXT_INDEX_NONE ||
        block_entry_instruction == XR_TYPED_FRAME_CONTEXT_INDEX_NONE ||
        block_entry_instruction > instruction)
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    frame->current_instruction = instruction;
    frame->current_block_instruction = block_entry_instruction;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_bind_coroutine_state(
    XrTypedFrame *frame, uint32_t coroutine_state) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    uint32_t count = 0;
    const XrTargetCoroutineStateRecord *states =
        xr_target_plan_coroutines(frame->plan, &count);
    if (!states || coroutine_state >= count ||
        states[coroutine_state].id != coroutine_state ||
        states[coroutine_state].function != frame->function_index)
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    /* The current TargetPlan freezes state identity but not a complete state
     * transition graph. Bind one verified persisted state and refuse to invent
     * movement between states until that upstream authority exists. */
    if (frame->coroutine_state != XR_TYPED_FRAME_CONTEXT_INDEX_NONE &&
        frame->coroutine_state != coroutine_state)
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    frame->coroutine_state = coroutine_state;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_bind_generation_identity(
    XrTypedFrame *frame, const XrModuleGenerationIdentity *identity) {
    if (!frame || !identity)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    const XrTargetProfile *profile = xr_target_plan_profile(frame->plan);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(profile);
    if (identity->schema_version != XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
        identity->target_plan_schema_version !=
            XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION ||
        identity->generation_number == 0 ||
        identity->completed_family_mask != XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK ||
        !fingerprint_bytes_match(identity->semantic_fingerprint,
                                 xr_target_plan_semantic_fingerprint(frame->plan)) ||
        !fingerprint_bytes_match(identity->target_profile_fingerprint,
                                 profile_fingerprint) ||
        !fingerprint_bytes_match(identity->target_plan_fingerprint,
                                 frame->plan_fingerprint) ||
        !fingerprint_bytes_nonzero(identity->generation_fingerprint,
                                   sizeof(identity->generation_fingerprint)))
        return XR_TYPED_FRAME_GENERATION_IDENTITY_MISMATCH;
    if (frame->generation_bound &&
        (frame->generation_number != identity->generation_number ||
         memcmp(frame->generation_fingerprint.bytes,
                identity->generation_fingerprint,
                sizeof(frame->generation_fingerprint.bytes)) != 0))
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    if (frame->generation_bound)
        return XR_TYPED_FRAME_OK;
    /* Linking requires identical generation contexts. Refuse a late bind on
     * either side instead of briefly creating an internally inconsistent call
     * chain; callers bind both frames before linking them. */
    if (frame->parent || frame->child)
        return XR_TYPED_FRAME_CALL_LINK_INVALID;
    frame->generation_number = identity->generation_number;
    memcpy(frame->generation_fingerprint.bytes,
           identity->generation_fingerprint,
           sizeof(frame->generation_fingerprint.bytes));
    frame->generation_bound = true;
    return XR_TYPED_FRAME_OK;
}

static bool generation_contexts_match(const XrTypedFrame *left,
                                      const XrTypedFrame *right) {
    return left->generation_bound == right->generation_bound &&
           (!left->generation_bound ||
            (left->generation_number == right->generation_number &&
             xr_fingerprint_equal(left->generation_fingerprint,
                                  right->generation_fingerprint)));
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_link_child(
    XrTypedFrame *parent, XrTypedFrame *child) {
    if (!parent || !child || parent == child)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (parent->cleaned || child->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(parent) ||
        !frame_plan_identity_is_intact(child))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (parent->child || child->parent ||
        !xr_fingerprint_equal(parent->plan_fingerprint,
                              child->plan_fingerprint) ||
        !generation_contexts_match(parent, child))
        return XR_TYPED_FRAME_CALL_LINK_INVALID;
    const XrTypedFrame *ancestor = parent;
    for (uint32_t depth = 0; ancestor; depth++) {
        if (ancestor == child || depth >= XR_TYPED_FRAME_MAX_PARENT_DEPTH)
            return XR_TYPED_FRAME_CALL_LINK_INVALID;
        ancestor = ancestor->parent;
    }
    parent->child = child;
    child->parent = parent;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_unlink_child(
    XrTypedFrame *parent, XrTypedFrame *child) {
    if (!parent || !child || parent == child)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (parent->cleaned || child->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (parent->child != child || child->parent != parent)
        return XR_TYPED_FRAME_CALL_LINK_INVALID;
    parent->child = NULL;
    child->parent = NULL;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC size_t xr_typed_frame_arena_size(const XrTypedFrame *frame) {
    return frame && !frame->cleaned ? frame->arena_size : 0;
}

XR_FUNC uint32_t xr_typed_frame_slot_count(const XrTypedFrame *frame) {
    return frame && !frame->cleaned ? frame->slot_count : 0;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_memory_footprint(
    const XrTypedFrame *frame, XrTypedFrameMemoryFootprint *footprint) {
    if (footprint)
        memset(footprint, 0, sizeof(*footprint));
    if (!frame || !footprint)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    size_t arena_bytes = frame->arena_size;
    if (frame->allocation_size < arena_bytes)
        return XR_TYPED_FRAME_SLOT_INVALID;
    XrTypedFrameMemoryFootprint measured = {
        .fixed_frame_bytes = sizeof(*frame),
        .arena_allocation_bytes = arena_bytes,
        .alignment_padding_bytes = frame->allocation_size - arena_bytes,
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
        .slot_state_metadata_bytes = frame->slot_count,
#endif
    };
    size_t total = measured.fixed_frame_bytes;
    if (!checked_add_size(total, measured.arena_allocation_bytes, &total) ||
        !checked_add_size(total, measured.alignment_padding_bytes, &total) ||
        !checked_add_size(total, measured.slot_state_metadata_bytes, &total))
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;
    measured.total_bytes = total;
    *footprint = measured;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_cleanup(XrTypedFrame *frame) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    frame->cleaned = true;
    if (frame->parent)
        frame->parent->child = NULL;
    if (frame->child)
        frame->child->parent = NULL;
    frame->parent = NULL;
    frame->child = NULL;
    if (frame->allocation) {
        memset(frame->allocation, 0, frame->allocation_size);
        xr_free(frame->allocation);
    }
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    if (frame->states) {
        memset(frame->states, XR_TYPED_SLOT_STATE_INVALID, frame->slot_count);
        xr_free(frame->states);
    }
#endif
    xr_target_plan_free(frame->plan);
    frame->plan = NULL;
    frame->function = NULL;
    frame->slots = NULL;
    frame->allocation = NULL;
    frame->arena = NULL;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    frame->states = NULL;
#endif
    frame->function_index = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->current_block_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->current_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->coroutine_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->generation_number = 0;
    memset(&frame->generation_fingerprint, 0,
           sizeof(frame->generation_fingerprint));
    frame->generation_bound = false;
    frame->allocation_size = 0;
    frame->arena_size = 0;
    frame->slot_begin = 0;
    frame->slot_count = 0;
    frame->frame_alignment = 0;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC void xr_typed_frame_free(XrTypedFrame *frame) {
    if (!frame)
        return;
    (void) xr_typed_frame_cleanup(frame);
    xr_free(frame);
}
