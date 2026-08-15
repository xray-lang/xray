/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_debug_control.c - TargetPlan-driven typed debugger control
 */

#include "xr_vm_debug_control_internal.h"
#include "../../base/xmalloc.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrVmDebugBreakpointRow {
    uint32_t function;
    uint32_t instruction;
} XrVmDebugBreakpointRow;

struct XrVmDebugPlan {
    uint32_t schema_version;
    _Atomic uint32_t references;
    XrFingerprint target_plan_fingerprint;
    XrVmDebugBreakpointRow *breakpoints;
    uint32_t breakpoint_count;
};

typedef struct XrVmDebugActiveFrame {
    const XrTargetPlan *plan;
    XrTypedFrame *frame;
    XrFingerprint plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    bool generation_identity_present;
    uint32_t function;
    uint32_t semantic_function;
    uint32_t frame_id;
    uint32_t parent_frame_id;
    uint32_t slot_begin;
    uint32_t slot_count;
    uint8_t *initialized;
    XrVmTraceEvent instruction;
} XrVmDebugActiveFrame;

typedef enum XrVmDebugExecutionState {
    XR_VM_DEBUG_EXECUTION_IDLE = 0,
    XR_VM_DEBUG_EXECUTION_ARMING,
    XR_VM_DEBUG_EXECUTION_ACTIVE,
} XrVmDebugExecutionState;

struct XrVmDebugControl {
    uint32_t schema_version;
    _Atomic uint32_t references;
    _Atomic uint8_t execution_state;
    XrVmDebugPlan *plan;
    XrVmDebugStopFn stop;
    void *context;
    _Atomic uint8_t armed_command;
    XrVmDebugResumeCommand command;
    bool anchor_present;
    uint64_t anchor_ordinal;
    uint32_t anchor_depth;
    uint32_t frame_count;
    uint32_t tracked_slot_count;
    XrVmDebugActiveFrame frames[XR_VM_DEBUG_MAX_STACK_DEPTH];
};

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static int compare_stable_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes));
}

static int compare_breakpoint_request(const void *left, const void *right) {
    const XrVmDebugBreakpointRequest *a = (const XrVmDebugBreakpointRequest *) left;
    const XrVmDebugBreakpointRequest *b = (const XrVmDebugBreakpointRequest *) right;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    return compare_stable_id(a->identity, b->identity);
}

static int64_t find_breakpoint_request(const XrVmDebugBreakpointRequest *requests, uint32_t count,
                                       XrVmDebugBreakpointKind kind, XrStableId identity) {
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const XrVmDebugBreakpointRequest *candidate = &requests[middle];
        int order = candidate->kind == kind ? compare_stable_id(candidate->identity, identity)
                                            : (candidate->kind < kind ? -1 : 1);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low >= count || requests[low].kind != kind ||
        compare_stable_id(requests[low].identity, identity) != 0)
        return -1;
    return (int64_t) low;
}

static bool fact_matches_requests(const XrTargetDebugFactRecord *fact,
                                  const XrVmDebugBreakpointRequest *requests,
                                  uint32_t request_count, uint8_t *matched) {
    bool found = false;
    int64_t index = find_breakpoint_request(
        requests, request_count, XR_VM_DEBUG_BREAKPOINT_SOURCE_SPAN, fact->source_span_identity);
    if (index >= 0) {
        matched[index] = 1;
        found = true;
    }
    index =
        find_breakpoint_request(requests, request_count, XR_VM_DEBUG_BREAKPOINT_SEMANTIC_OPERATION,
                                fact->semantic_operation_identity);
    if (index >= 0) {
        matched[index] = 1;
        found = true;
    }
    return found;
}

static bool debug_plan_retain(XrVmDebugPlan *plan) {
    if (!plan)
        return false;
    uint32_t references = atomic_load_explicit(&plan->references, memory_order_relaxed);
    while (references && references != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&plan->references, &references, references + 1u,
                                                  memory_order_relaxed, memory_order_relaxed))
            return true;
    }
    return false;
}

static void debug_plan_release(XrVmDebugPlan *plan) {
    if (!plan || atomic_fetch_sub_explicit(&plan->references, 1u, memory_order_acq_rel) != 1u)
        return;
    xr_free(plan->breakpoints);
    memset(plan, 0, sizeof(*plan));
    xr_free(plan);
}

XrVmDebugControlStatus xr_typed_debug_plan_create(const XrTargetPlan *verified_plan,
                                                  const XrFingerprint *required_plan_fingerprint,
                                                  const XrVmDebugBreakpointRequest *breakpoints,
                                                  uint32_t breakpoint_count, XrVmDebugPlan **plan) {
    if (plan)
        *plan = NULL;
    if (!plan || !verified_plan || !required_plan_fingerprint || (!breakpoints && breakpoint_count))
        return XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT;
    if (breakpoint_count > XR_VM_DEBUG_MAX_BREAKPOINTS)
        return XR_VM_DEBUG_CONTROL_RESOURCE_LIMIT;
    if (!xr_target_plan_is_verified(verified_plan) ||
        !xr_target_plan_fingerprint_is_intact(verified_plan))
        return XR_VM_DEBUG_CONTROL_PLAN_NOT_VERIFIED;
    if (!xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_VM_DEBUG_CONTROL_PLAN_IDENTITY_MISMATCH;
    XrVmDebugBreakpointRequest *sorted = NULL;
    uint8_t *matched = NULL;
    if (breakpoint_count) {
        sorted =
            (XrVmDebugBreakpointRequest *) xr_malloc((size_t) breakpoint_count * sizeof(*sorted));
        matched = (uint8_t *) xr_calloc(breakpoint_count, 1);
        if (!sorted || !matched) {
            xr_free(sorted);
            xr_free(matched);
            return XR_VM_DEBUG_CONTROL_ALLOCATION_FAILED;
        }
        memcpy(sorted, breakpoints, (size_t) breakpoint_count * sizeof(*sorted));
        for (uint32_t i = 0; i < breakpoint_count; i++) {
            if (sorted[i].kind <= XR_VM_DEBUG_BREAKPOINT_INVALID ||
                sorted[i].kind >= XR_VM_DEBUG_BREAKPOINT_KIND_COUNT || sorted[i].reserved[0] ||
                sorted[i].reserved[1] || sorted[i].reserved[2] ||
                bytes_are_zero(sorted[i].identity.bytes, sizeof(sorted[i].identity.bytes))) {
                xr_free(sorted);
                xr_free(matched);
                return XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT;
            }
        }
        qsort(sorted, breakpoint_count, sizeof(*sorted), compare_breakpoint_request);
        for (uint32_t i = 1; i < breakpoint_count; i++) {
            if (compare_breakpoint_request(&sorted[i - 1u], &sorted[i]) == 0) {
                xr_free(sorted);
                xr_free(matched);
                return XR_VM_DEBUG_CONTROL_BREAKPOINT_DUPLICATE;
            }
        }
    }

    uint32_t instruction_count = 0;
    uint32_t fact_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(verified_plan, &instruction_count);
    const XrTargetDebugFactRecord *facts = xr_target_plan_debug_facts(verified_plan, &fact_count);
    if (!instructions || !facts || instruction_count != fact_count) {
        xr_free(sorted);
        xr_free(matched);
        return XR_VM_DEBUG_CONTROL_PLAN_NOT_VERIFIED;
    }
    uint32_t row_count = 0;
    for (uint32_t i = 0; i < fact_count; i++) {
        if (facts[i].id != i || facts[i].instruction != instructions[i].id ||
            facts[i].function != instructions[i].function) {
            xr_free(sorted);
            xr_free(matched);
            return XR_VM_DEBUG_CONTROL_PLAN_NOT_VERIFIED;
        }
        if (breakpoint_count &&
            fact_matches_requests(&facts[i], sorted, breakpoint_count, matched)) {
            if (row_count == XR_VM_DEBUG_MAX_BREAKPOINTS) {
                xr_free(sorted);
                xr_free(matched);
                return XR_VM_DEBUG_CONTROL_RESOURCE_LIMIT;
            }
            row_count++;
        }
    }
    for (uint32_t i = 0; i < breakpoint_count; i++) {
        if (!matched[i]) {
            xr_free(sorted);
            xr_free(matched);
            return XR_VM_DEBUG_CONTROL_BREAKPOINT_NOT_FOUND;
        }
    }
    XrVmDebugPlan *created = (XrVmDebugPlan *) xr_calloc(1, sizeof(*created));
    if (!created) {
        xr_free(sorted);
        xr_free(matched);
        return XR_VM_DEBUG_CONTROL_ALLOCATION_FAILED;
    }
    if (row_count) {
        created->breakpoints =
            (XrVmDebugBreakpointRow *) xr_calloc(row_count, sizeof(*created->breakpoints));
        if (!created->breakpoints) {
            xr_free(sorted);
            xr_free(matched);
            xr_free(created);
            return XR_VM_DEBUG_CONTROL_ALLOCATION_FAILED;
        }
    }
    created->schema_version = XR_VM_DEBUG_CONTROL_SCHEMA_VERSION;
    atomic_init(&created->references, 1u);
    created->target_plan_fingerprint = *required_plan_fingerprint;
    created->breakpoint_count = row_count;
    uint32_t row = 0;
    if (breakpoint_count) {
        memset(matched, 0, breakpoint_count);
        for (uint32_t i = 0; i < fact_count; i++) {
            if (!fact_matches_requests(&facts[i], sorted, breakpoint_count, matched))
                continue;
            created->breakpoints[row++] = (XrVmDebugBreakpointRow) {
                .function = facts[i].function,
                .instruction = facts[i].instruction,
            };
        }
    }
    xr_free(sorted);
    xr_free(matched);
    *plan = created;
    return XR_VM_DEBUG_CONTROL_OK;
}

void xr_typed_debug_plan_free(XrVmDebugPlan **plan) {
    if (!plan || !*plan)
        return;
    XrVmDebugPlan *owned = *plan;
    *plan = NULL;
    debug_plan_release(owned);
}

XrVmDebugControlStatus xr_typed_debug_control_create(const XrVmDebugPlan *plan,
                                                     XrVmDebugStopFn stop, void *context,
                                                     XrVmDebugControl **control) {
    if (control)
        *control = NULL;
    if (!control || !plan || !stop || plan->schema_version != XR_VM_DEBUG_CONTROL_SCHEMA_VERSION)
        return XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT;
    XrVmDebugControl *created = (XrVmDebugControl *) xr_calloc(1, sizeof(*created));
    if (!created)
        return XR_VM_DEBUG_CONTROL_ALLOCATION_FAILED;
    created->schema_version = XR_VM_DEBUG_CONTROL_SCHEMA_VERSION;
    atomic_init(&created->references, 1u);
    atomic_init(&created->execution_state, XR_VM_DEBUG_EXECUTION_IDLE);
    created->plan = (XrVmDebugPlan *) plan;
    if (!debug_plan_retain(created->plan)) {
        xr_free(created);
        return XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT;
    }
    created->stop = stop;
    created->context = context;
    atomic_init(&created->armed_command, XR_VM_DEBUG_RESUME_CONTINUE);
    *control = created;
    return XR_VM_DEBUG_CONTROL_OK;
}

XrVmDebugControlStatus xr_typed_debug_control_arm(XrVmDebugControl *control,
                                                  XrVmDebugResumeCommand initial_command) {
    if (!control || control->schema_version != XR_VM_DEBUG_CONTROL_SCHEMA_VERSION ||
        (initial_command != XR_VM_DEBUG_RESUME_CONTINUE &&
         initial_command != XR_VM_DEBUG_RESUME_STEP_INTO))
        return XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT;
    uint8_t expected = XR_VM_DEBUG_EXECUTION_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&control->execution_state, &expected,
                                                 XR_VM_DEBUG_EXECUTION_ARMING, memory_order_acq_rel,
                                                 memory_order_acquire))
        return XR_VM_DEBUG_CONTROL_ACTIVE;
    atomic_store_explicit(&control->armed_command, (uint8_t) initial_command, memory_order_release);
    atomic_store_explicit(&control->execution_state, XR_VM_DEBUG_EXECUTION_IDLE,
                          memory_order_release);
    return XR_VM_DEBUG_CONTROL_OK;
}

XrVmDebugControlStatus xr_typed_debug_control_free(XrVmDebugControl **control) {
    if (!control || !*control)
        return XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT;
    XrVmDebugControl *owned = *control;
    *control = NULL;
    xr_typed_debug_control_release(owned);
    return XR_VM_DEBUG_CONTROL_OK;
}

bool xr_typed_debug_control_retain(XrVmDebugControl *control) {
    if (!control || control->schema_version != XR_VM_DEBUG_CONTROL_SCHEMA_VERSION)
        return false;
    uint32_t references = atomic_load_explicit(&control->references, memory_order_relaxed);
    while (references && references != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&control->references, &references,
                                                  references + 1u, memory_order_acquire,
                                                  memory_order_relaxed))
            return true;
    }
    return false;
}

void xr_typed_debug_control_release(XrVmDebugControl *control) {
    if (!control || atomic_fetch_sub_explicit(&control->references, 1u, memory_order_acq_rel) != 1u)
        return;
    debug_plan_release(control->plan);
    memset(control, 0, sizeof(*control));
    xr_free(control);
}

bool xr_typed_debug_control_matches_plan(const XrVmDebugControl *control,
                                         XrFingerprint target_plan_fingerprint) {
    return control && control->schema_version == XR_VM_DEBUG_CONTROL_SCHEMA_VERSION &&
           control->plan && control->plan->schema_version == XR_VM_DEBUG_CONTROL_SCHEMA_VERSION &&
           xr_fingerprint_equal(control->plan->target_plan_fingerprint, target_plan_fingerprint);
}

XrVmDebugControlEventStatus
xr_typed_debug_control_begin_execution(XrVmDebugControl *control, const XrTargetPlan *verified_plan,
                                       const XrFingerprint *required_plan_fingerprint) {
    if (!xr_typed_debug_control_retain(control))
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    if (!verified_plan || !required_plan_fingerprint ||
        !xr_target_plan_is_verified(verified_plan) ||
        !xr_target_plan_fingerprint_is_intact(verified_plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint) ||
        !xr_typed_debug_control_matches_plan(control, *required_plan_fingerprint)) {
        xr_typed_debug_control_release(control);
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    }
    uint8_t expected = XR_VM_DEBUG_EXECUTION_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&control->execution_state, &expected,
                                                 XR_VM_DEBUG_EXECUTION_ACTIVE, memory_order_acq_rel,
                                                 memory_order_acquire)) {
        xr_typed_debug_control_release(control);
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    }
    control->command = (XrVmDebugResumeCommand) atomic_load_explicit(&control->armed_command,
                                                                     memory_order_acquire);
    control->anchor_present = false;
    control->anchor_ordinal = 0;
    control->anchor_depth = 0;
    control->frame_count = 0;
    control->tracked_slot_count = 0;
    return XR_VM_DEBUG_CONTROL_EVENT_OK;
}

static const XrTargetFunctionRecord *find_function(const XrTargetPlan *plan, uint32_t function) {
    uint32_t count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &count);
    return functions && function < count && functions[function].id == function
               ? &functions[function]
               : NULL;
}

XrVmDebugControlEventStatus
xr_typed_debug_control_push_frame(XrVmDebugControl *control, const XrTargetPlan *verified_plan,
                                  const XrFingerprint *required_plan_fingerprint,
                                  const XrModuleGenerationIdentity *generation_identity,
                                  XrTypedFrame *frame, uint32_t function, uint32_t frame_id,
                                  uint32_t parent_frame_id, bool parameters_prebound) {
    if (!control ||
        atomic_load_explicit(&control->execution_state, memory_order_acquire) !=
            XR_VM_DEBUG_EXECUTION_ACTIVE ||
        !verified_plan || !required_plan_fingerprint || !frame ||
        control->frame_count >= XR_VM_DEBUG_MAX_STACK_DEPTH ||
        !xr_target_plan_is_verified(verified_plan) ||
        !xr_target_plan_fingerprint_is_intact(verified_plan) ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    const XrTargetFunctionRecord *function_row = find_function(verified_plan, function);
    XrTypedFrameContext frame_context;
    if (!function_row || xr_typed_frame_context(frame, &frame_context) != XR_TYPED_FRAME_OK ||
        function_row->slot_count > XR_VM_DEBUG_MAX_TRACKED_SLOTS - control->tracked_slot_count ||
        frame_context.function_identity.function != function ||
        frame_context.function_identity.semantic_function != function_row->semantic_function ||
        !xr_fingerprint_equal(frame_context.function_identity.plan_fingerprint,
                              *required_plan_fingerprint) ||
        frame_context.generation_bound != (generation_identity != NULL))
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    if (generation_identity &&
        (!frame_context.generation_bound ||
         frame_context.generation_number != generation_identity->generation_number ||
         memcmp(frame_context.generation_fingerprint.bytes,
                generation_identity->generation_fingerprint,
                sizeof(frame_context.generation_fingerprint.bytes)) != 0))
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    uint8_t *initialized = NULL;
    if (function_row->slot_count) {
        initialized = (uint8_t *) xr_calloc(function_row->slot_count, 1);
        if (!initialized)
            return XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED;
    }
    XrVmDebugActiveFrame *active = &control->frames[control->frame_count];
    memset(active, 0, sizeof(*active));
    active->plan = verified_plan;
    active->frame = frame;
    active->plan_fingerprint = *required_plan_fingerprint;
    active->generation_identity_present = generation_identity != NULL;
    if (generation_identity)
        active->generation_identity = *generation_identity;
    active->function = function;
    active->semantic_function = function_row->semantic_function;
    active->frame_id = frame_id;
    active->parent_frame_id = parent_frame_id;
    active->slot_begin = function_row->slot_begin;
    active->slot_count = function_row->slot_count;
    active->initialized = initialized;
    active->instruction.instruction = XR_VM_TRACE_ID_NONE;
    control->frame_count++;
    control->tracked_slot_count += function_row->slot_count;
    if (parameters_prebound) {
        uint32_t count = 0;
        const XrTargetInstructionRecord *rows =
            xr_target_plan_function_instructions(verified_plan, function, &count);
        if (!rows) {
            xr_typed_debug_control_pop_frame(control, frame_id);
            return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (rows[i].opcode != XR_TARGET_INSTRUCTION_PARAM_I64)
                continue;
            if (rows[i].result_slot < active->slot_begin ||
                rows[i].result_slot - active->slot_begin >= active->slot_count) {
                xr_typed_debug_control_pop_frame(control, frame_id);
                return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
            }
            active->initialized[rows[i].result_slot - active->slot_begin] = 1;
        }
    }
    return XR_VM_DEBUG_CONTROL_EVENT_OK;
}

static bool instruction_is_breakpoint(const XrVmDebugControl *control,
                                      const XrVmTraceEvent *event) {
    if (!xr_fingerprint_equal(event->target_plan_fingerprint,
                              control->plan->target_plan_fingerprint))
        return false;
    uint32_t low = 0;
    uint32_t high = control->plan->breakpoint_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (control->plan->breakpoints[middle].instruction < event->instruction)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < control->plan->breakpoint_count &&
           control->plan->breakpoints[low].instruction == event->instruction &&
           control->plan->breakpoints[low].function == event->function;
}

static bool step_is_due(const XrVmDebugControl *control, const XrVmTraceEvent *event) {
    if (control->command == XR_VM_DEBUG_RESUME_STEP_INTO)
        return !control->anchor_present || event->ordinal > control->anchor_ordinal;
    if (!control->anchor_present || event->ordinal <= control->anchor_ordinal)
        return false;
    if (control->command == XR_VM_DEBUG_RESUME_STEP_OVER)
        return event->frame_depth <= control->anchor_depth;
    if (control->command == XR_VM_DEBUG_RESUME_STEP_OUT)
        return event->frame_depth < control->anchor_depth;
    return false;
}

static XrVmDebugControlEventStatus
capture_stop(XrVmDebugControl *control, const XrVmTraceEvent *event, XrVmDebugStopReason reason) {
    uint32_t local_count = 0;
    size_t byte_count = 0;
    for (uint32_t frame_index = 0; frame_index < control->frame_count; frame_index++) {
        const XrVmDebugActiveFrame *active = &control->frames[frame_index];
        uint32_t slot_table_count = 0;
        const XrTargetSlotRecord *slots = xr_target_plan_slots(active->plan, &slot_table_count);
        if (!slots || active->slot_begin > slot_table_count ||
            active->slot_count > slot_table_count - active->slot_begin)
            return XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED;
        for (uint32_t i = 0; i < active->slot_count; i++) {
            if (!active->initialized[i])
                continue;
            const XrTargetSlotRecord *slot = &slots[active->slot_begin + i];
            if (!xr_typed_debug_snapshot_budget_reserve(&local_count, &byte_count, slot->size))
                return XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED;
        }
    }
    XrVmDebugFrameSnapshot *frames =
        (XrVmDebugFrameSnapshot *) xr_calloc(control->frame_count, sizeof(*frames));
    XrVmDebugLocalSnapshot *locals =
        local_count ? (XrVmDebugLocalSnapshot *) xr_calloc(local_count, sizeof(*locals)) : NULL;
    uint8_t *bytes = byte_count ? (uint8_t *) xr_malloc(byte_count) : NULL;
    if (!frames || (local_count && !locals) || (byte_count && !bytes)) {
        xr_free(frames);
        xr_free(locals);
        xr_free(bytes);
        return XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED;
    }
    for (uint32_t frame_index = 0; frame_index < control->frame_count; frame_index++) {
        if (control->frames[frame_index].instruction.instruction == XR_VM_TRACE_ID_NONE) {
            xr_free(frames);
            xr_free(locals);
            xr_free(bytes);
            return XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED;
        }
        frames[frame_index].instruction = control->frames[frame_index].instruction;
        frames[frame_index].semantic_function = control->frames[frame_index].semantic_function;
    }
    uint32_t local = 0;
    uint32_t offset = 0;
    for (uint32_t frame_index = 0; frame_index < control->frame_count; frame_index++) {
        XrVmDebugActiveFrame *active = &control->frames[frame_index];
        uint32_t slot_table_count = 0;
        const XrTargetSlotRecord *slots = xr_target_plan_slots(active->plan, &slot_table_count);
        for (uint32_t i = 0; i < active->slot_count; i++) {
            if (!active->initialized[i])
                continue;
            const XrTargetSlotRecord *slot = &slots[active->slot_begin + i];
            XrTypedSlotAccess access;
            if (xr_typed_frame_describe_slot(active->frame, slot->id, &access) !=
                    XR_TYPED_FRAME_OK ||
                access.size != slot->size || access.alignment != slot->align ||
                access.register_rep != slot->register_rep ||
                access.memory_rep != slot->memory_rep ||
                xr_typed_frame_load(active->frame, &access, bytes + offset, slot->size) !=
                    XR_TYPED_FRAME_OK) {
                xr_free(frames);
                xr_free(locals);
                xr_free(bytes);
                return XR_VM_DEBUG_CONTROL_EVENT_CAPTURE_FAILED;
            }
            locals[local++] = (XrVmDebugLocalSnapshot) {
                .identity = slot->identity,
                .frame = active->frame_id,
                .slot = slot->id,
                .semantic_value = slot->semantic_value,
                .semantic_operation = slot->semantic_operation,
                .value_offset = offset,
                .value_size = slot->size,
                .alignment = slot->align,
                .register_rep = slot->register_rep,
                .memory_rep = slot->memory_rep,
                .role = slot->role,
                .root_kind = slot->root_kind,
                .ownership = slot->ownership,
            };
            offset += slot->size;
        }
    }
    XrVmDebugStop snapshot = {
        .schema_version = XR_VM_DEBUG_CONTROL_SCHEMA_VERSION,
        .reason = (uint8_t) reason,
        .instruction = *event,
        .frames = frames,
        .frame_count = control->frame_count,
        .locals = locals,
        .local_count = local_count,
        .local_bytes = bytes,
        .local_bytes_size = (uint32_t) byte_count,
    };
    XrVmDebugResumeCommand resume = XR_VM_DEBUG_RESUME_INVALID;
    bool accepted = control->stop(control->context, &snapshot, &resume);
    xr_free(frames);
    xr_free(locals);
    xr_free(bytes);
    if (!accepted || resume <= XR_VM_DEBUG_RESUME_INVALID ||
        resume >= XR_VM_DEBUG_RESUME_COMMAND_COUNT)
        return XR_VM_DEBUG_CONTROL_EVENT_STOP_REJECTED;
    if (resume == XR_VM_DEBUG_RESUME_STEP_OUT && event->frame_depth == 0)
        return XR_VM_DEBUG_CONTROL_EVENT_STOP_REJECTED;
    if (resume == XR_VM_DEBUG_RESUME_TERMINATE)
        return XR_VM_DEBUG_CONTROL_EVENT_TERMINATED;
    control->command = resume;
    control->anchor_present = resume != XR_VM_DEBUG_RESUME_CONTINUE;
    control->anchor_ordinal = event->ordinal;
    control->anchor_depth = event->frame_depth;
    return XR_VM_DEBUG_CONTROL_EVENT_OK;
}

XrVmDebugControlEventStatus xr_typed_debug_control_instruction(XrVmDebugControl *control,
                                                               const XrVmTraceEvent *event) {
    if (!control || !event ||
        atomic_load_explicit(&control->execution_state, memory_order_acquire) !=
            XR_VM_DEBUG_EXECUTION_ACTIVE ||
        event->kind != XR_VM_TRACE_INSTRUCTION || event->instruction == XR_VM_TRACE_ID_NONE ||
        !control->frame_count)
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    XrVmDebugActiveFrame *active = &control->frames[control->frame_count - 1u];
    if (event->frame != active->frame_id || event->function != active->function ||
        !xr_fingerprint_equal(event->target_plan_fingerprint, active->plan_fingerprint))
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    active->instruction = *event;
    bool breakpoint = instruction_is_breakpoint(control, event);
    bool step = step_is_due(control, event);
    if (!breakpoint && !step)
        return XR_VM_DEBUG_CONTROL_EVENT_OK;
    return capture_stop(control, event,
                        breakpoint ? XR_VM_DEBUG_STOP_BREAKPOINT : XR_VM_DEBUG_STOP_STEP);
}

XrVmDebugControlEventStatus
xr_typed_debug_control_commit_row(XrVmDebugControl *control, const XrTargetInstructionRecord *row) {
    if (!control || !row ||
        atomic_load_explicit(&control->execution_state, memory_order_acquire) !=
            XR_VM_DEBUG_EXECUTION_ACTIVE ||
        !control->frame_count)
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    if (row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE)
        return XR_VM_DEBUG_CONTROL_EVENT_OK;
    XrVmDebugActiveFrame *active = &control->frames[control->frame_count - 1u];
    if (row->function != active->function || row->result_slot < active->slot_begin ||
        row->result_slot - active->slot_begin >= active->slot_count)
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    active->initialized[row->result_slot - active->slot_begin] = 1;
    return XR_VM_DEBUG_CONTROL_EVENT_OK;
}

XrVmDebugControlEventStatus xr_typed_debug_control_pop_frame(XrVmDebugControl *control,
                                                             uint32_t frame_id) {
    if (!control ||
        atomic_load_explicit(&control->execution_state, memory_order_acquire) !=
            XR_VM_DEBUG_EXECUTION_ACTIVE ||
        !control->frame_count)
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    XrVmDebugActiveFrame *active = &control->frames[control->frame_count - 1u];
    if (active->frame_id != frame_id)
        return XR_VM_DEBUG_CONTROL_EVENT_INVALID;
    control->tracked_slot_count -= active->slot_count;
    xr_free(active->initialized);
    memset(active, 0, sizeof(*active));
    control->frame_count--;
    return XR_VM_DEBUG_CONTROL_EVENT_OK;
}

void xr_typed_debug_control_end_execution(XrVmDebugControl *control) {
    if (!control || atomic_load_explicit(&control->execution_state, memory_order_acquire) !=
                        XR_VM_DEBUG_EXECUTION_ACTIVE)
        return;
    while (control->frame_count) {
        XrVmDebugActiveFrame *active = &control->frames[control->frame_count - 1u];
        xr_free(active->initialized);
        memset(active, 0, sizeof(*active));
        control->frame_count--;
    }
    control->command = XR_VM_DEBUG_RESUME_CONTINUE;
    control->anchor_present = false;
    control->tracked_slot_count = 0;
    atomic_store_explicit(&control->execution_state, XR_VM_DEBUG_EXECUTION_IDLE,
                          memory_order_release);
    xr_typed_debug_control_release(control);
}
