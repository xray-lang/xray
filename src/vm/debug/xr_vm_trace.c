/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_trace.c - Runtime-only typed TargetPlan trace contract
 */

#include "xr_vm_trace.h"
#include "xr_vm_profile.h"
#include <string.h>

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

XrVmDebugSessionStatus xr_typed_debug_session_init(
    const XrFingerprint *target_plan_fingerprint,
    const XrModuleGenerationIdentity *generation_identity,
    const XrVmTraceSink *trace, XrVmProfile *profile,
    XrVmDebugSession *session) {
    if (session)
        memset(session, 0, sizeof(*session));
    if (!target_plan_fingerprint || !session ||
        (trace && !trace->emit) ||
        (profile && !xr_typed_profile_is_initialized(profile)))
        return XR_VM_DEBUG_SESSION_INVALID_ARGUMENT;
    if (bytes_are_zero(target_plan_fingerprint->bytes,
                       sizeof(target_plan_fingerprint->bytes)))
        return XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH;

    session->schema_version = XR_VM_TRACE_SCHEMA_VERSION;
    session->target_plan_fingerprint = *target_plan_fingerprint;
    if (trace)
        session->trace = *trace;
    session->profile = profile;
    if (!generation_identity)
        return XR_VM_DEBUG_SESSION_OK;
    if (generation_identity->schema_version !=
            XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
        memcmp(generation_identity->target_plan_fingerprint,
               target_plan_fingerprint->bytes,
               sizeof(target_plan_fingerprint->bytes)) != 0 ||
        bytes_are_zero(generation_identity->generation_fingerprint,
                       sizeof(generation_identity->generation_fingerprint))) {
        memset(session, 0, sizeof(*session));
        return XR_VM_DEBUG_SESSION_GENERATION_IDENTITY_INVALID;
    }
    session->generation_identity_present = 1;
    session->generation_identity = *generation_identity;
    return XR_VM_DEBUG_SESSION_OK;
}

bool xr_typed_debug_session_matches_plan(
    const XrVmDebugSession *session,
    XrFingerprint target_plan_fingerprint) {
    return session && session->schema_version == XR_VM_TRACE_SCHEMA_VERSION &&
           (!session->profile ||
            xr_typed_profile_is_initialized(session->profile)) &&
           xr_fingerprint_equal(session->target_plan_fingerprint,
                                target_plan_fingerprint);
}

bool xr_typed_debug_emit(
    const XrVmDebugSession *session,
    const XrFingerprint *target_plan_fingerprint,
    const XrModuleGenerationIdentity *generation_identity,
    uint64_t ordinal, XrVmTraceEvent *event) {
    if (!session)
        return true;
    if (!event || !target_plan_fingerprint ||
        !xr_typed_debug_session_matches_plan(
            session, session->target_plan_fingerprint) ||
        (generation_identity &&
         (generation_identity->schema_version !=
              XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
          memcmp(generation_identity->target_plan_fingerprint,
                 target_plan_fingerprint->bytes,
                 sizeof(target_plan_fingerprint->bytes)) != 0)))
        return false;
    event->schema_version = XR_VM_TRACE_SCHEMA_VERSION;
    event->ordinal = ordinal;
    event->target_plan_fingerprint = *target_plan_fingerprint;
    event->generation_identity_present = generation_identity != NULL;
    if (generation_identity) {
        event->generation_number =
            generation_identity->generation_number;
        memcpy(event->generation_fingerprint.bytes,
               generation_identity->generation_fingerprint,
               sizeof(event->generation_fingerprint.bytes));
    }
    if (session->trace.emit &&
        !session->trace.emit(session->trace.context, event))
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

bool xr_typed_trace_buffer_init(XrVmTraceBuffer *buffer,
                             XrVmTraceEvent *storage, size_t capacity) {
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
