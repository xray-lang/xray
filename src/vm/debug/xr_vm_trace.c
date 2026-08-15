/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_trace.c - Runtime-only typed TargetPlan trace contract
 */

#include "xr_vm_trace_internal.h"
#include "xr_vm_debug_control_internal.h"
#include "xr_vm_profile.h"
#include "../../base/xmalloc.h"
#include <string.h>

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

XrVmDebugSessionStatus
xr_typed_debug_session_create(const XrFingerprint *target_plan_fingerprint,
                              const XrModuleGenerationIdentity *generation_identity,
                              const XrVmTraceSink *trace, XrVmProfile *profile,
                              XrVmDebugControl *control, XrVmDebugSession **session) {
    if (session)
        *session = NULL;
    if (!target_plan_fingerprint || !session || (trace && !trace->emit) ||
        (profile && !xr_typed_profile_is_initialized(profile)))
        return XR_VM_DEBUG_SESSION_INVALID_ARGUMENT;
    if (bytes_are_zero(target_plan_fingerprint->bytes, sizeof(target_plan_fingerprint->bytes)))
        return XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH;
    if (control && !xr_typed_debug_control_matches_plan(control, *target_plan_fingerprint))
        return XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH;
    if (generation_identity &&
        (generation_identity->schema_version != XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
         memcmp(generation_identity->target_plan_fingerprint, target_plan_fingerprint->bytes,
                sizeof(target_plan_fingerprint->bytes)) != 0 ||
         bytes_are_zero(generation_identity->generation_fingerprint,
                        sizeof(generation_identity->generation_fingerprint))))
        return XR_VM_DEBUG_SESSION_GENERATION_IDENTITY_INVALID;
    XrVmDebugSession *created = (XrVmDebugSession *) xr_calloc(1, sizeof(*created));
    if (!created)
        return XR_VM_DEBUG_SESSION_ALLOCATION_FAILED;
    if (control && !xr_typed_debug_control_retain(control)) {
        xr_free(created);
        return XR_VM_DEBUG_SESSION_INVALID_ARGUMENT;
    }
    created->schema_version = XR_VM_TRACE_SCHEMA_VERSION;
    created->target_plan_fingerprint = *target_plan_fingerprint;
    if (trace)
        created->trace = *trace;
    created->profile = profile;
    created->control = control;
    if (generation_identity) {
        created->generation_identity_present = 1;
        created->generation_identity = *generation_identity;
    }
    *session = created;
    return XR_VM_DEBUG_SESSION_OK;
}

void xr_typed_debug_session_free(XrVmDebugSession **session) {
    if (!session || !*session)
        return;
    XrVmDebugSession *owned = *session;
    *session = NULL;
    xr_typed_debug_control_release(owned->control);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
}

bool xr_typed_debug_session_matches_plan(const XrVmDebugSession *session,
                                         XrFingerprint target_plan_fingerprint) {
    return session && session->schema_version == XR_VM_TRACE_SCHEMA_VERSION &&
           (!session->profile || xr_typed_profile_is_initialized(session->profile)) &&
           (!session->control ||
            xr_typed_debug_control_matches_plan(session->control, target_plan_fingerprint)) &&
           xr_fingerprint_equal(session->target_plan_fingerprint, target_plan_fingerprint);
}

bool xr_typed_debug_attach_event_facts(const XrTargetPlan *verified_plan, XrVmTraceEvent *event) {
    if (!verified_plan || !event || !xr_target_plan_is_verified(verified_plan) ||
        !xr_target_plan_fingerprint_is_intact(verified_plan) ||
        event->function == XR_VM_TRACE_ID_NONE)
        return false;
    event->debug_fact = XR_VM_TRACE_ID_NONE;
    event->semantic_operation = XR_VM_TRACE_ID_NONE;
    event->coroutine_state = XR_VM_TRACE_ID_NONE;
    event->source_span_availability = XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    event->owner_availability = XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    event->layout_availability = XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    event->coroutine_availability = XR_VM_DEBUG_FACT_NOT_APPLICABLE;
    if (event->instruction == XR_VM_TRACE_ID_NONE)
        return true;
    uint32_t instruction_count = 0;
    uint32_t fact_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(verified_plan, &instruction_count);
    const XrTargetDebugFactRecord *facts = xr_target_plan_debug_facts(verified_plan, &fact_count);
    if (!instructions || !facts || fact_count != instruction_count ||
        event->instruction >= instruction_count)
        return false;
    const XrTargetInstructionRecord *instruction = &instructions[event->instruction];
    const XrTargetDebugFactRecord *fact = &facts[event->instruction];
    if (instruction->id != event->instruction || instruction->function != event->function ||
        fact->id != event->instruction || fact->instruction != event->instruction ||
        fact->function != event->function ||
        (event->opcode != XR_TARGET_INSTRUCTION_INVALID && instruction->opcode != event->opcode))
        return false;
    event->debug_fact = fact->id;
    event->semantic_operation = fact->semantic_operation;
    event->coroutine_state = fact->coroutine_state;
    event->semantic_operation_identity = fact->semantic_operation_identity;
    event->source_span_identity = fact->source_span_identity;
    event->owner_identity = fact->owner_identity;
    event->coroutine_state_identity = fact->coroutine_state_identity;
    event->layout_fingerprint = fact->layout_fingerprint;
    event->source_start_line = fact->source_start_line;
    event->source_start_column = fact->source_start_column;
    event->source_end_line = fact->source_end_line;
    event->source_end_column = fact->source_end_column;
    if (fact->semantic_operation == XR_VM_TRACE_ID_NONE)
        return true;
    event->source_span_availability =
        bytes_are_zero(fact->source_span_identity.bytes, sizeof(fact->source_span_identity.bytes))
            ? XR_VM_DEBUG_FACT_CONTEXT_UNAVAILABLE
            : XR_VM_DEBUG_FACT_AVAILABLE;
    event->owner_availability =
        bytes_are_zero(fact->owner_identity.bytes, sizeof(fact->owner_identity.bytes))
            ? XR_VM_DEBUG_FACT_NOT_APPLICABLE
            : XR_VM_DEBUG_FACT_AVAILABLE;
    event->layout_availability =
        bytes_are_zero(fact->layout_fingerprint.bytes, sizeof(fact->layout_fingerprint.bytes))
            ? XR_VM_DEBUG_FACT_NOT_APPLICABLE
            : XR_VM_DEBUG_FACT_AVAILABLE;
    event->coroutine_availability = fact->coroutine_state == XR_VM_TRACE_ID_NONE
                                        ? XR_VM_DEBUG_FACT_NOT_APPLICABLE
                                        : XR_VM_DEBUG_FACT_AVAILABLE;
    return true;
}

bool xr_typed_debug_emit(const XrVmDebugSession *session,
                         const XrFingerprint *target_plan_fingerprint,
                         const XrModuleGenerationIdentity *generation_identity, uint64_t ordinal,
                         XrVmTraceEvent *event) {
    if (!session)
        return true;
    if (!event || !target_plan_fingerprint ||
        !xr_typed_debug_session_matches_plan(session, session->target_plan_fingerprint) ||
        (generation_identity &&
         (generation_identity->schema_version != XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
          memcmp(generation_identity->target_plan_fingerprint, target_plan_fingerprint->bytes,
                 sizeof(target_plan_fingerprint->bytes)) != 0)))
        return false;
    event->schema_version = XR_VM_TRACE_SCHEMA_VERSION;
    event->ordinal = ordinal;
    event->target_plan_fingerprint = *target_plan_fingerprint;
    event->generation_identity_present = generation_identity != NULL;
    if (generation_identity) {
        event->generation_number = generation_identity->generation_number;
        memcpy(event->generation_fingerprint.bytes, generation_identity->generation_fingerprint,
               sizeof(event->generation_fingerprint.bytes));
    }
    if (session->trace.emit && !session->trace.emit(session->trace.context, event))
        return false;
    if (session->profile)
        xr_typed_profile_record_event(session->profile, event);
    return true;
}

static bool trace_buffer_emit(void *context, const XrVmTraceEvent *event) {
    XrVmTraceBuffer *buffer = (XrVmTraceBuffer *) context;
    if (!buffer || !event || buffer->count >= buffer->capacity) {
        if (buffer)
            buffer->capacity_exceeded = true;
        return false;
    }
    buffer->events[buffer->count++] = *event;
    return true;
}

bool xr_typed_trace_buffer_init(XrVmTraceBuffer *buffer, XrVmTraceEvent *storage, size_t capacity) {
    if (buffer)
        memset(buffer, 0, sizeof(*buffer));
    if (!buffer || (!storage && capacity))
        return false;
    buffer->events = storage;
    buffer->capacity = capacity;
    return true;
}

XrVmTraceSink xr_typed_trace_buffer_sink(XrVmTraceBuffer *buffer) {
    XrVmTraceSink sink = {0};
    if (buffer) {
        sink.emit = trace_buffer_emit;
        sink.context = buffer;
    }
    return sink;
}
