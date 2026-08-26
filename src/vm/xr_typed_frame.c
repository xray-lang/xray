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
#include "../ir/xi_ops_gen.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/value/xtype.h"
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
    uint32_t pending_resume_instruction;
    uint64_t generation_number;
    XrFingerprint generation_fingerprint;
    struct XrTypedFrame *parent;
    struct XrTypedFrame *child;
    uint8_t *allocation;
    uint8_t *arena;
    uint32_t *lifecycle_slots;
    uint8_t *lifecycle_states;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    uint8_t *states;
#endif
    size_t allocation_size;
    size_t arena_size;
    uint32_t slot_begin;
    uint32_t slot_count;
    uint32_t lifecycle_count;
    uint16_t frame_alignment;
    bool generation_bound;
    bool terminal;
    bool cleaned;
};

typedef struct XrTypedFrameShape {
    const XrTargetFunctionRecord *function;
    const XrTargetSlotRecord *slots;
    size_t arena_allocation_bytes;
    uint32_t lifecycle_count;
    size_t lifecycle_metadata_bytes;
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

typedef enum XrTypedLifecycleState {
    XR_TYPED_LIFECYCLE_UNMANAGED = 0,
    XR_TYPED_LIFECYCLE_EMPTY,
    XR_TYPED_LIFECYCLE_ACTIVE,
    XR_TYPED_LIFECYCLE_RELEASED,
    XR_TYPED_LIFECYCLE_TRANSFERRED,
} XrTypedLifecycleState;

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

static bool function_root_partition(
    const XrTargetPlan *plan, uint32_t function,
    const XrTargetRootMapRecord **rows, uint32_t *count) {
    if (rows)
        *rows = NULL;
    if (count)
        *count = 0;
    uint32_t function_count = 0;
    uint32_t root_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    const XrTargetRootMapRecord *roots =
        xr_target_plan_root_maps(plan, &root_count);
    const XrTargetFunctionRecord *record =
        functions && function < function_count ? &functions[function] : NULL;
    if (!rows || !count || !record || record->id != function ||
        record->root_begin > root_count ||
        record->root_count > root_count - record->root_begin ||
        (record->root_count && !roots))
        return false;
    *rows = record->root_count ? roots + record->root_begin : NULL;
    *count = record->root_count;
    return true;
}

static bool slot_has_root_map_entry(const XrTargetPlan *plan,
                                    const XrTargetSlotRecord *slot,
                                    uint32_t *semantic_operation) {
    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    const XrTargetRootMapRecord *roots = NULL;
    const uint32_t *root_slots =
        xr_target_plan_root_slots(plan, &root_slot_count);
    if (!slot || !function_root_partition(plan, slot->function, &roots,
                                          &root_count))
        return false;
    uint32_t matches = 0;
    uint32_t operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < root_count; i++) {
        const XrTargetRootMapRecord *root = &roots[i];
        if (root->function != slot->function ||
            root->flags != (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL |
                            XR_TARGET_ROOT_EXIT) ||
            root->slot_begin > root_slot_count ||
            root->slot_count > root_slot_count - root->slot_begin)
            continue;
        for (uint32_t j = 0; j < root->slot_count; j++) {
            if (root_slots[root->slot_begin + j] != slot->id)
                continue;
            matches++;
            operation = root->semantic_operation;
        }
    }
    if (matches != 1)
        return false;
    if (semantic_operation)
        *semantic_operation = operation;
    return true;
}

static bool function_cleanup_partition(
    const XrTargetPlan *plan, uint32_t function,
    const XrTargetCleanupRecord **rows, uint32_t *count) {
    if (rows)
        *rows = NULL;
    if (count)
        *count = 0;
    uint32_t function_count = 0;
    uint32_t cleanup_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    const XrTargetCleanupRecord *cleanups =
        xr_target_plan_cleanups(plan, &cleanup_count);
    const XrTargetFunctionRecord *record =
        functions && function < function_count ? &functions[function] : NULL;
    if (!rows || !count || !record || record->id != function ||
        record->cleanup_begin > cleanup_count ||
        record->cleanup_count > cleanup_count - record->cleanup_begin ||
        (record->cleanup_count && !cleanups))
        return false;
    *rows = record->cleanup_count
                ? cleanups + record->cleanup_begin
                : NULL;
    *count = record->cleanup_count;
    return true;
}

static bool slot_lifecycle_contract_is_exact(
    const XrTargetPlan *plan, const XrTargetSlotRecord *slot) {
    const XrTargetMachineRepRecord *register_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    if (!slot || !register_rep || !memory_rep ||
        register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        register_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        memory_rep->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        !reps_are_storage_compatible(register_rep, memory_rep) ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != XR_TARGET_OWNERSHIP_OWNED ||
        slot->size != memory_rep->memory_size ||
        slot->align != memory_rep->memory_align)
        return false;
    uint32_t state_operation = XR_SEMANTIC_INDEX_NONE;
    if (!slot_has_root_map_entry(plan, slot, &state_operation))
        return false;
    uint32_t cleanup_count = 0;
    const XrTargetCleanupRecord *cleanups = NULL;
    if (!function_cleanup_partition(plan, slot->function, &cleanups,
                                    &cleanup_count))
        return false;
    uint32_t terminal = 0;
    uint32_t normal = 0;
    for (uint32_t i = 0; i < cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup = &cleanups[i];
        if (cleanup->function != slot->function || cleanup->slot != slot->id ||
            cleanup->action != XR_TARGET_CLEANUP_RELEASE ||
            cleanup->provider != 0)
            continue;
        terminal += cleanup->semantic_operation == state_operation &&
                    cleanup->flags == (XR_TARGET_CLEANUP_CANCEL |
                                       XR_TARGET_CLEANUP_EXIT);
        normal += cleanup->semantic_operation != state_operation &&
                  cleanup->flags == 0;
    }
    return terminal == 1 && normal == 1;
}

/* Array.push owns one short-lived managed parameter without inventing root or
 * cleanup rows: its executable CONSUME operand is the transfer authority.  A
 * borrowed receiver is admitted as managed transport but never enters the
 * lifecycle ledger; the owned element does, and the frame cannot be freed
 * until execution either transfers or returns it. */
static bool slot_managed_array_push_parameter_is_exact(
    const XrTargetPlan *plan, const XrTargetSlotRecord *slot, bool *owned) {
    if (owned)
        *owned = false;
    if (!plan || !slot || slot->role != XR_TARGET_SLOT_PARAMETER ||
        xr_target_plan_function_execution_family_mask(plan, slot->function) !=
            XR_TARGET_EXECUTION_MANAGED_ARRAY_PUSH_TAGGED)
        return false;
    uint32_t count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(plan, slot->function, &count);
    bool is_borrowed = rows && count == 4 && rows[0].result_slot == slot->id &&
                       rows[0].opcode == XR_TARGET_INSTRUCTION_PARAM_DYN_BORROW;
    bool is_owned = rows && count == 4 && rows[1].result_slot == slot->id &&
                    rows[1].opcode == XR_TARGET_INSTRUCTION_PARAM_DYN_OWNED;
    if (!is_borrowed && !is_owned)
        return false;
    uint8_t ownership = is_owned ? XR_TARGET_OWNERSHIP_OWNED
                                 : XR_TARGET_OWNERSHIP_BORROWED;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(plan, slot->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(plan, slot->memory_rep);
    bool exact = register_rep && memory_rep &&
                 register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                 memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                 register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                 memory_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                 register_rep->ownership == ownership &&
                 memory_rep->ownership == ownership &&
                 reps_are_storage_compatible(register_rep, memory_rep) &&
                 slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                 slot->ownership == ownership && slot->size == memory_rep->memory_size &&
                 slot->align == memory_rep->memory_align;
    if (exact && owned)
        *owned = is_owned;
    return exact;
}

/* Immutable String literals are the already-frozen prerequisite carrier for
 * the concat inputs.  They own no frame-local lifecycle: their exact
 * BORROWED_STATIC SemanticPlan identity is sufficient, while every fresh
 * owned dynamic String still requires the root/drop contract above. */
static bool slot_is_exact_immutable_string_literal(
    const XrTargetPlan *plan, const XrTargetSlotRecord *slot) {
    const XrSemanticPlan *semantic =
        plan ? xr_target_plan_semantic_plan(plan) : NULL;
    const XrSemanticOperationRecord *operation =
        semantic && slot ? xr_semantic_plan_operation(
                               semantic, slot->semantic_operation)
                         : NULL;
    const XrSemanticConstantRecord *constant =
        operation ? xr_semantic_plan_constant(semantic, operation->constant)
                  : NULL;
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type)
                  : NULL;
    const XrTargetMachineRepRecord *register_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    XrStableId zero = {{0}};
    return semantic && operation && constant && type && register_rep &&
           memory_rep && operation->opcode == XI_CONST &&
           operation->operand_count == 0 && operation->allocation_key == NULL &&
           xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->result_value == slot->semantic_value &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_complete == 1 &&
           constant->kind == XR_SEM_CONST_STRING && constant->string &&
           constant->type == operation->result_type &&
           type->kind == XR_KIND_STRING && type->child_count == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                           XR_SEM_TYPE_BORROW_VIEW |
                           XR_SEM_TYPE_AGGREGATE_EXACT)) == 0 &&
           (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                           XR_SEM_TYPE_OWNERSHIP_ROOT)) ==
               (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           register_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
           register_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           memory_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           register_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           memory_rep->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           reps_are_storage_compatible(register_rep, memory_rep) &&
           slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
           slot->ownership == XR_TARGET_OWNERSHIP_OWNED &&
           slot->size == memory_rep->memory_size &&
           slot->align == memory_rep->memory_align;
}

static bool slot_rep_contract_is_exact(
    const XrTargetPlan *plan, const XrTargetSlotRecord *slot) {
    const XrTargetMachineRepRecord *register_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        slot ? xr_target_plan_machine_rep(plan, slot->memory_rep) : NULL;
    bool transportable = stored_rep_is_transportable(register_rep) &&
                         stored_rep_is_transportable(memory_rep);
    bool managed = slot_lifecycle_contract_is_exact(plan, slot);
    bool immutable_literal =
        slot_is_exact_immutable_string_literal(plan, slot);
    bool managed_push_parameter =
        slot_managed_array_push_parameter_is_exact(plan, slot, NULL);
    return slot && (transportable || managed || immutable_literal || managed_push_parameter) &&
           reps_are_storage_compatible(register_rep, memory_rep) &&
           register_rep->id == slot->register_rep &&
           memory_rep->id == slot->memory_rep &&
           slot->size == memory_rep->memory_size &&
           slot->align == memory_rep->memory_align &&
           slot->root_kind == memory_rep->root_kind &&
           slot->ownership == memory_rep->ownership;
}

/* A frozen function may retain planning-only values that its verified typed
 * instruction closure does not execute. Only instruction operands/results and
 * call arguments are frame transport authority. Keeping every other slot
 * inaccessible is narrower than pretending the executor owns its roots,
 * cleanup, or representation contract. */
static bool slot_is_executed(const XrTargetPlan *plan, uint32_t function,
                             uint32_t slot) {
    uint32_t row_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(plan, function, &row_count);
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    uint32_t expectation_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetEntryExpectationRecord *expectations =
        xr_target_plan_entry_expectations(plan, &expectation_count);
    for (uint32_t i = 0; i < row_count; i++) {
        const XrTargetInstructionRecord *row = &rows[i];
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(row->opcode);
        if (!contract)
            return true;
        if (row->result_slot == slot)
            return true;
        for (uint8_t operand = 0; operand < contract->arity; operand++) {
            if (row->operand_slots[operand] == slot)
                return true;
        }
        uint32_t call_index = XR_SEMANTIC_INDEX_NONE;
        if ((row->opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
             row->opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE) &&
            row->immediate_bits <= UINT32_MAX)
            call_index = (uint32_t) row->immediate_bits;
        else if (row->opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64 &&
                 row->immediate_bits < expectation_count)
            call_index = expectations[row->immediate_bits].call;
        if (call_index >= call_count)
            continue;
        const XrTargetCallRecord *call = &calls[call_index];
        if (call->argument_begin > argument_count ||
            call->argument_count > argument_count - call->argument_begin)
            return true;
        for (uint16_t argument = 0; argument < call->argument_count;
             argument++) {
            if (arguments[call->argument_begin + argument].caller_slot == slot)
                return true;
        }
    }
    return false;
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
    bool has_verified_instruction_closure =
        xr_target_plan_function_execution_family_mask(plan, function_index) !=
        0;
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
    uint32_t lifecycle_count = 0;
    for (uint32_t i = 0; i < function->slot_count; i++) {
        bool push_owner = false;
        const XrTargetSlotRecord *slot = &slots[function->slot_begin + i];
        lifecycle_count += slot_lifecycle_contract_is_exact(plan, slot) ||
                           (slot_managed_array_push_parameter_is_exact(
                                plan, slot, &push_owner) && push_owner);
    }
    size_t lifecycle_metadata = 0;
    if (lifecycle_count > SIZE_MAX /
                              (sizeof(uint32_t) + sizeof(uint8_t)))
        return XR_TYPED_FRAME_BUDGET_EXHAUSTED;
    lifecycle_metadata = (size_t) lifecycle_count *
                         (sizeof(uint32_t) + sizeof(uint8_t));
    size_t total_allocation = sizeof(XrTypedFrame);
    if (!checked_add_size(total_allocation, arena_allocation,
                          &total_allocation) ||
        !checked_add_size(total_allocation, lifecycle_metadata,
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
            ((!has_verified_instruction_closure ||
              slot_is_executed(plan, function_index, global_slot)) &&
             !slot_rep_contract_is_exact(plan, slot)))
            return XR_TYPED_FRAME_SLOT_INVALID;
    }
    shape->function = function;
    shape->slots = function->slot_count ? slots + function->slot_begin : NULL;
    shape->arena_allocation_bytes = arena_allocation;
    shape->lifecycle_count = lifecycle_count;
    shape->lifecycle_metadata_bytes = lifecycle_metadata;
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
    if (shape->lifecycle_count) {
        frame->lifecycle_slots = (uint32_t *) xr_malloc(
            (size_t) shape->lifecycle_count * sizeof(*frame->lifecycle_slots));
        frame->lifecycle_states =
            (uint8_t *) xr_calloc(shape->lifecycle_count, 1);
        if (!frame->lifecycle_slots || !frame->lifecycle_states) {
            xr_free(frame->lifecycle_slots);
            xr_free(frame->lifecycle_states);
            xr_free(frame->allocation);
            xr_free(frame);
            return XR_TYPED_FRAME_ALLOCATION_FAILED;
        }
        uint32_t next = 0;
        for (uint32_t i = 0; i < shape->function->slot_count; i++) {
            bool push_owner = false;
            if (!slot_lifecycle_contract_is_exact(plan, &shape->slots[i]) &&
                !(slot_managed_array_push_parameter_is_exact(
                      plan, &shape->slots[i], &push_owner) && push_owner))
                continue;
            frame->lifecycle_slots[next] = shape->slots[i].id;
            frame->lifecycle_states[next] = XR_TYPED_LIFECYCLE_EMPTY;
            next++;
        }
        if (next != shape->lifecycle_count) {
            xr_free(frame->lifecycle_slots);
            xr_free(frame->lifecycle_states);
            xr_free(frame->allocation);
            xr_free(frame);
            return XR_TYPED_FRAME_SLOT_INVALID;
        }
    }
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    if (shape->function->slot_count) {
        frame->states = (uint8_t *) xr_calloc(shape->function->slot_count, 1);
        if (!frame->states) {
            xr_free(frame->lifecycle_slots);
            xr_free(frame->lifecycle_states);
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
    frame->pending_resume_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->allocation_size = shape->arena_allocation_bytes;
    frame->arena_size = shape->function->frame_size;
    frame->slot_begin = shape->function->slot_begin;
    frame->slot_count = shape->function->slot_count;
    frame->lifecycle_count = shape->lifecycle_count;
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
    if (frame->terminal)
        return XR_TYPED_FRAME_TERMINAL;
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

static int32_t frame_lifecycle_index(const XrTypedFrame *frame,
                                     uint32_t global_slot) {
    uint32_t low = 0;
    uint32_t high = frame ? frame->lifecycle_count : 0;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (frame->lifecycle_slots[middle] < global_slot)
            low = middle + 1u;
        else
            high = middle;
    }
    return frame && low < frame->lifecycle_count &&
                   frame->lifecycle_slots[low] == global_slot
               ? (int32_t) low
               : -1;
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
    int32_t lifecycle_index = frame->lifecycle_count
        ? frame_lifecycle_index(frame, resolved.record->id)
        : -1;
    XrTypedLifecycleState lifecycle = lifecycle_index >= 0
        ? (XrTypedLifecycleState) frame->lifecycle_states[lifecycle_index]
        : XR_TYPED_LIFECYCLE_UNMANAGED;
    if (lifecycle != XR_TYPED_LIFECYCLE_UNMANAGED) {
        if (transfer == XR_TYPED_FRAME_TRANSFER_STORE &&
            lifecycle == XR_TYPED_LIFECYCLE_ACTIVE)
            return XR_TYPED_FRAME_LIFECYCLE_ACTIVE;
        if (transfer == XR_TYPED_FRAME_TRANSFER_STORE &&
            (lifecycle == XR_TYPED_LIFECYCLE_RELEASED ||
             lifecycle == XR_TYPED_LIFECYCLE_TRANSFERRED))
            return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
        if (transfer == XR_TYPED_FRAME_TRANSFER_LOAD &&
            lifecycle != XR_TYPED_LIFECYCLE_ACTIVE)
            return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
    }
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
        if (lifecycle == XR_TYPED_LIFECYCLE_EMPTY)
            frame->lifecycle_states[lifecycle_index] =
                XR_TYPED_LIFECYCLE_ACTIVE;
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

XR_FUNC XrTypedFrameStatus xr_typed_frame_take_owned(
    XrTypedFrame *frame, const XrTypedSlotAccess *access, void *bytes,
    size_t size) {
    if (!frame || !access || !bytes)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    XrTypedResolvedSlot resolved = {0};
    XrTypedFrameStatus status = resolve_slot(frame, access->slot, access, &resolved);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (size != resolved.record->size)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    bool push_owner = false;
    if (!slot_managed_array_push_parameter_is_exact(
            frame->plan, resolved.record, &push_owner) || !push_owner)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    int32_t lifecycle = frame_lifecycle_index(frame, resolved.record->id);
    if (lifecycle < 0 ||
        frame->lifecycle_states[lifecycle] != XR_TYPED_LIFECYCLE_ACTIVE)
        return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    if (frame->states[resolved.local_slot] != XR_TYPED_SLOT_STATE_INITIALIZED)
        return XR_TYPED_FRAME_UNINITIALIZED;
#endif
    memcpy(bytes, resolved.bytes, size);
    memset(resolved.bytes, 0, size);
    frame->lifecycle_states[lifecycle] = XR_TYPED_LIFECYCLE_TRANSFERRED;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    frame->states[resolved.local_slot] = XR_TYPED_SLOT_STATE_POISONED;
#endif
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_restore_owned(
    XrTypedFrame *frame, const XrTypedSlotAccess *access, const void *bytes,
    size_t size) {
    if (!frame || !access || !bytes)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    XrTypedResolvedSlot resolved = {0};
    XrTypedFrameStatus status = resolve_slot(frame, access->slot, access, &resolved);
    if (status != XR_TYPED_FRAME_OK)
        return status;
    if (size != resolved.record->size)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    bool push_owner = false;
    if (!slot_managed_array_push_parameter_is_exact(
            frame->plan, resolved.record, &push_owner) || !push_owner)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
    int32_t lifecycle = frame_lifecycle_index(frame, resolved.record->id);
    if (lifecycle < 0 ||
        frame->lifecycle_states[lifecycle] != XR_TYPED_LIFECYCLE_TRANSFERRED)
        return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    if (frame->states[resolved.local_slot] != XR_TYPED_SLOT_STATE_POISONED)
        return XR_TYPED_FRAME_ACCESS_MISMATCH;
#endif
    memcpy(resolved.bytes, bytes, size);
    frame->lifecycle_states[lifecycle] = XR_TYPED_LIFECYCLE_ACTIVE;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    frame->states[resolved.local_slot] = XR_TYPED_SLOT_STATE_INITIALIZED;
#endif
    return XR_TYPED_FRAME_OK;
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
        case XR_TARGET_INSTRUCTION_CONTROL_SUSPEND:
            return frame->coroutine_state ==
                       XR_TYPED_FRAME_CONTEXT_INDEX_NONE &&
                   frame->pending_resume_instruction == rows[next_local].id;
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
        .terminal = frame->terminal,
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
    frame->pending_resume_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
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
    uint32_t count = 0;
    const XrTargetInstructionRecord *rows =
        frame_instruction_group(frame, &count);
    uint32_t local = 0;
    if (!instruction_local_index(rows, count, instruction, &local) ||
        instruction_block_entry(rows, local) != block_entry_instruction)
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    if (!instruction_transition_allowed(frame, rows, count, local))
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    frame->current_instruction = instruction;
    frame->current_block_instruction = block_entry_instruction;
    frame->pending_resume_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_bind_coroutine_state(
    XrTypedFrame *frame, uint32_t coroutine_state) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (frame->terminal)
        return XR_TYPED_FRAME_TERMINAL;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    uint32_t count = 0;
    const XrTargetCoroutineStateRecord *states =
        xr_target_plan_coroutines(frame->plan, &count);
    if (!states || coroutine_state >= count ||
        states[coroutine_state].id != coroutine_state ||
        states[coroutine_state].function != frame->function_index)
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    if (xr_target_plan_function_execution_family_mask(
            frame->plan, frame->function_index) ==
        XR_TARGET_EXECUTION_SCALAR_I64_COROUTINE) {
        uint32_t row_count = 0;
        const XrTargetInstructionRecord *rows =
            frame_instruction_group(frame, &row_count);
        uint32_t current_local = 0;
        if (!instruction_local_index(rows, row_count,
                                     frame->current_instruction,
                                     &current_local) ||
            rows[current_local].opcode != XR_TARGET_INSTRUCTION_SUSPEND ||
            XR_TARGET_INSTRUCTION_SUSPEND_STATE(
                rows[current_local].immediate_bits) != coroutine_state ||
            frame->pending_resume_instruction !=
                XR_TYPED_FRAME_CONTEXT_INDEX_NONE)
            return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    }
    /* The current TargetPlan freezes state identity but not a complete state
     * transition graph. Bind one verified persisted state and refuse to invent
     * movement between states until that upstream authority exists. */
    if (frame->coroutine_state != XR_TYPED_FRAME_CONTEXT_INDEX_NONE &&
        frame->coroutine_state != coroutine_state)
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    const XrTargetRootMapRecord *roots = NULL;
    const uint32_t *root_slots =
        xr_target_plan_root_slots(frame->plan, &root_slot_count);
    if (!function_root_partition(frame->plan, frame->function_index, &roots,
                                 &root_count) ||
        root_count != frame->function->root_count)
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    for (uint32_t root = 0; root < root_count; root++) {
        const XrTargetRootMapRecord *map = &roots[root];
        if (map->function != frame->function_index ||
            map->semantic_operation != states[coroutine_state].semantic_operation)
            continue;
        if (map->flags != (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL |
                           XR_TARGET_ROOT_EXIT) ||
            map->slot_begin > root_slot_count ||
            map->slot_count > root_slot_count - map->slot_begin)
            return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
        for (uint32_t slot = 0; slot < map->slot_count; slot++) {
            uint32_t global_slot = root_slots[map->slot_begin + slot];
            int32_t lifecycle_index =
                frame_lifecycle_index(frame, global_slot);
            if (global_slot < frame->slot_begin ||
                global_slot - frame->slot_begin >= frame->slot_count ||
                lifecycle_index < 0 ||
                frame->lifecycle_states[lifecycle_index] !=
                    XR_TYPED_LIFECYCLE_ACTIVE)
                return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
        }
    }
    frame->coroutine_state = coroutine_state;
    return XR_TYPED_FRAME_OK;
}

static const XrTargetCoroutineStateRecord *frame_coroutine_state(
    const XrTypedFrame *frame, uint32_t coroutine_state) {
    uint32_t count = 0;
    const XrTargetCoroutineStateRecord *states =
        frame ? xr_target_plan_coroutines(frame->plan, &count) : NULL;
    return states && coroutine_state < count &&
                   states[coroutine_state].id == coroutine_state &&
                   states[coroutine_state].function == frame->function_index
               ? &states[coroutine_state]
               : NULL;
}

static bool frame_resume_instruction(
    const XrTypedFrame *frame, const XrTargetCoroutineStateRecord *state,
    uint32_t *instruction) {
    if (instruction)
        *instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    uint32_t row_count = 0;
    const XrTargetInstructionRecord *rows =
        frame_instruction_group(frame, &row_count);
    uint32_t current = 0;
    if (!frame || !state || !instruction || !rows || !row_count ||
        !instruction_local_index(rows, row_count, frame->current_instruction,
                                 &current) ||
        rows[current].opcode != XR_TARGET_INSTRUCTION_SUSPEND ||
        XR_TARGET_INSTRUCTION_SUSPEND_STATE(
            rows[current].immediate_bits) != state->id)
        return false;
    uint32_t target = XR_TARGET_INSTRUCTION_SUSPEND_RESUME(
        rows[current].immediate_bits);
    if (target != state->resume_instruction || target >= row_count ||
        instruction_block_entry(rows, target) !=
                                   rows[target].id)
        return false;
    *instruction = rows[target].id;
    return true;
}

static const XrTargetRootMapRecord *frame_state_root_map(
    const XrTypedFrame *frame, const XrTargetCoroutineStateRecord *state,
    const uint32_t **root_slots_out, uint32_t *root_slot_count_out) {
    uint32_t root_count = 0;
    uint32_t root_slot_count = 0;
    const XrTargetRootMapRecord *roots = NULL;
    const uint32_t *root_slots = frame
        ? xr_target_plan_root_slots(frame->plan, &root_slot_count)
        : NULL;
    if (!frame ||
        !function_root_partition(frame->plan, frame->function_index, &roots,
                                 &root_count) ||
        root_count != frame->function->root_count)
        return NULL;
    const XrTargetRootMapRecord *matched = NULL;
    for (uint32_t i = 0; state && i < root_count; i++) {
        if (roots[i].function != frame->function_index ||
            roots[i].semantic_operation != state->semantic_operation)
            continue;
        if (matched || roots[i].flags !=
                           (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL |
                            XR_TARGET_ROOT_EXIT) ||
            roots[i].slot_begin > root_slot_count ||
            roots[i].slot_count > root_slot_count - roots[i].slot_begin)
            return NULL;
        matched = &roots[i];
    }
    if (matched) {
        if (root_slots_out)
            *root_slots_out = root_slots + matched->slot_begin;
        if (root_slot_count_out)
            *root_slot_count_out = matched->slot_count;
    }
    return matched;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_visit_coroutine_roots(
    XrTypedFrame *frame, uint32_t coroutine_state,
    XrTypedFrameRootVisitor visitor, void *context, uint32_t *visited) {
    if (visited)
        *visited = 0;
    if (!frame || !visitor || !visited)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (frame->terminal)
        return XR_TYPED_FRAME_TERMINAL;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    const XrTargetCoroutineStateRecord *state =
        frame_coroutine_state(frame, coroutine_state);
    if (!state || frame->coroutine_state != coroutine_state)
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    const uint32_t *root_slots = NULL;
    uint32_t root_slot_count = 0;
    if (!frame_state_root_map(frame, state, &root_slots, &root_slot_count))
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    for (uint32_t i = 0; i < root_slot_count; i++) {
        int32_t lifecycle_index =
            frame_lifecycle_index(frame, root_slots[i]);
        if (root_slots[i] < frame->slot_begin ||
            root_slots[i] - frame->slot_begin >= frame->slot_count ||
            lifecycle_index < 0 ||
            frame->lifecycle_states[lifecycle_index] !=
                XR_TYPED_LIFECYCLE_ACTIVE)
            return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
    }
    for (uint32_t i = 0; i < root_slot_count; i++) {
        XrTypedResolvedSlot resolved = {0};
        XrTypedFrameStatus status =
            resolve_slot(frame, root_slots[i], NULL, &resolved);
        if (status != XR_TYPED_FRAME_OK)
            return status;
        XrTypedSlotAccess access = {
            .identity = resolved.record->identity,
            .slot = resolved.record->id,
            .size = resolved.record->size,
            .alignment = resolved.record->align,
            .register_rep = resolved.record->register_rep,
            .memory_rep = resolved.record->memory_rep,
        };
        visitor(context, &access, resolved.bytes);
        (*visited)++;
    }
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_resume_coroutine_state(
    XrTypedFrame *frame, uint32_t coroutine_state) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (frame->terminal)
        return XR_TYPED_FRAME_TERMINAL;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (!frame_coroutine_state(frame, coroutine_state) ||
        frame->coroutine_state != coroutine_state)
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    uint32_t resume_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    if (xr_target_plan_function_execution_family_mask(
            frame->plan, frame->function_index) ==
            XR_TARGET_EXECUTION_SCALAR_I64_COROUTINE &&
        !frame_resume_instruction(
            frame, frame_coroutine_state(frame, coroutine_state),
            &resume_instruction))
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    frame->coroutine_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->pending_resume_instruction = resume_instruction;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_execute_cleanups(
    XrTypedFrame *frame, uint32_t semantic_operation, uint8_t event_flags,
    XrTypedFrameCleanupExecutor executor, void *context, uint32_t *executed) {
    if (executed)
        *executed = 0;
    if (!frame || !executor || !executed ||
        (event_flags != 0 && event_flags != XR_TARGET_CLEANUP_CANCEL &&
         event_flags != XR_TARGET_CLEANUP_EXIT))
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (frame->cleaned)
        return XR_TYPED_FRAME_CLEANED;
    if (frame->terminal)
        return XR_TYPED_FRAME_TERMINAL;
    if (!frame_plan_identity_is_intact(frame))
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    if (event_flags) {
        const XrTargetCoroutineStateRecord *state =
            frame_coroutine_state(frame, frame->coroutine_state);
        if (!state || state->semantic_operation != semantic_operation)
            return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    } else if (frame->coroutine_state != XR_TYPED_FRAME_CONTEXT_INDEX_NONE) {
        return XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID;
    }
    uint32_t cleanup_count = 0;
    const XrTargetCleanupRecord *cleanups = NULL;
    if (!function_cleanup_partition(frame->plan, frame->function_index,
                                    &cleanups, &cleanup_count) ||
        cleanup_count != frame->function->cleanup_count)
        return XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH;
    uint32_t matches = 0;
    uint32_t active_matches = 0;
    for (uint32_t i = 0; i < cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup = &cleanups[i];
        bool event_match = event_flags
            ? cleanup->flags == (XR_TARGET_CLEANUP_CANCEL |
                                 XR_TARGET_CLEANUP_EXIT) &&
                  (cleanup->flags & event_flags) != 0
            : cleanup->flags == 0;
        if (cleanup->function != frame->function_index ||
            cleanup->semantic_operation != semantic_operation || !event_match)
            continue;
        int32_t lifecycle_index =
            frame_lifecycle_index(frame, cleanup->slot);
        if (cleanup->action != XR_TARGET_CLEANUP_RELEASE ||
            cleanup->provider != 0 || cleanup->slot < frame->slot_begin ||
            cleanup->slot - frame->slot_begin >= frame->slot_count ||
            lifecycle_index < 0)
            return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
        XrTypedLifecycleState state =
            (XrTypedLifecycleState) frame->lifecycle_states[lifecycle_index];
        if (state != XR_TYPED_LIFECYCLE_ACTIVE &&
            state != XR_TYPED_LIFECYCLE_RELEASED)
            return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
        active_matches += state == XR_TYPED_LIFECYCLE_ACTIVE;
        matches++;
    }
    if (!matches)
        return XR_TYPED_FRAME_CONTEXT_UNAVAILABLE;
    if (!active_matches)
        return XR_TYPED_FRAME_LIFECYCLE_INACTIVE;
    for (uint32_t i = 0; i < cleanup_count; i++) {
        const XrTargetCleanupRecord *cleanup = &cleanups[i];
        bool event_match = event_flags
            ? cleanup->flags == (XR_TARGET_CLEANUP_CANCEL |
                                 XR_TARGET_CLEANUP_EXIT) &&
                  (cleanup->flags & event_flags) != 0
            : cleanup->flags == 0;
        if (cleanup->function != frame->function_index ||
            cleanup->semantic_operation != semantic_operation || !event_match)
            continue;
        int32_t lifecycle_index =
            frame_lifecycle_index(frame, cleanup->slot);
        if (lifecycle_index < 0)
            return XR_TYPED_FRAME_SLOT_INVALID;
        if (frame->lifecycle_states[lifecycle_index] ==
            XR_TYPED_LIFECYCLE_RELEASED)
            continue;
        XrTypedResolvedSlot resolved = {0};
        XrTypedFrameStatus status =
            resolve_slot(frame, cleanup->slot, NULL, &resolved);
        if (status != XR_TYPED_FRAME_OK)
            return status;
        XrTypedSlotAccess access = {
            .identity = resolved.record->identity,
            .slot = resolved.record->id,
            .size = resolved.record->size,
            .alignment = resolved.record->align,
            .register_rep = resolved.record->register_rep,
            .memory_rep = resolved.record->memory_rep,
        };
        status = executor(context, cleanup->action, &access, resolved.bytes);
        if (status != XR_TYPED_FRAME_OK)
            return status;
        memset(resolved.bytes, 0, resolved.record->size);
        frame->lifecycle_states[lifecycle_index] =
            XR_TYPED_LIFECYCLE_RELEASED;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
        frame->states[resolved.local_slot] = XR_TYPED_SLOT_STATE_UNINITIALIZED;
#endif
        (*executed)++;
    }
    if (event_flags) {
        frame->coroutine_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
        frame->pending_resume_instruction =
            XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
        frame->terminal = true;
    }
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
        .lifecycle_state_metadata_bytes =
            (size_t) frame->lifecycle_count *
            (sizeof(*frame->lifecycle_slots) +
             sizeof(*frame->lifecycle_states)),
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
        .slot_state_metadata_bytes = frame->slot_count,
#endif
    };
    size_t total = measured.fixed_frame_bytes;
    if (!checked_add_size(total, measured.arena_allocation_bytes, &total) ||
        !checked_add_size(total, measured.alignment_padding_bytes, &total) ||
        !checked_add_size(total, measured.slot_state_metadata_bytes, &total) ||
        !checked_add_size(total, measured.lifecycle_state_metadata_bytes,
                          &total))
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
    if (frame->child)
        return XR_TYPED_FRAME_CHILD_ACTIVE;
    for (uint32_t i = 0; i < frame->lifecycle_count; i++)
        if (frame->lifecycle_states[i] == XR_TYPED_LIFECYCLE_ACTIVE)
            return XR_TYPED_FRAME_LIFECYCLE_ACTIVE;
    frame->cleaned = true;
    if (frame->parent)
        frame->parent->child = NULL;
    frame->parent = NULL;
    if (frame->allocation) {
        memset(frame->allocation, 0, frame->allocation_size);
        xr_free(frame->allocation);
    }
    if (frame->lifecycle_states) {
        memset(frame->lifecycle_states, XR_TYPED_LIFECYCLE_UNMANAGED,
               frame->lifecycle_count);
        xr_free(frame->lifecycle_slots);
        xr_free(frame->lifecycle_states);
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
    frame->lifecycle_states = NULL;
    frame->lifecycle_slots = NULL;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    frame->states = NULL;
#endif
    frame->function_index = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->current_block_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->current_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->coroutine_state = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->pending_resume_instruction = XR_TYPED_FRAME_CONTEXT_INDEX_NONE;
    frame->generation_number = 0;
    memset(&frame->generation_fingerprint, 0,
           sizeof(frame->generation_fingerprint));
    frame->generation_bound = false;
    frame->terminal = false;
    frame->allocation_size = 0;
    frame->arena_size = 0;
    frame->slot_begin = 0;
    frame->slot_count = 0;
    frame->lifecycle_count = 0;
    frame->frame_alignment = 0;
    return XR_TYPED_FRAME_OK;
}

XR_FUNC XrTypedFrameStatus xr_typed_frame_free(XrTypedFrame **frame) {
    if (!frame)
        return XR_TYPED_FRAME_INVALID_ARGUMENT;
    if (!*frame)
        return XR_TYPED_FRAME_OK;
    if (!(*frame)->cleaned) {
        XrTypedFrameStatus status = xr_typed_frame_cleanup(*frame);
        if (status != XR_TYPED_FRAME_OK)
            return status;
    }
    xr_free(*frame);
    *frame = NULL;
    return XR_TYPED_FRAME_OK;
}
