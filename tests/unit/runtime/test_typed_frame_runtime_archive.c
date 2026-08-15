/*
 * test_typed_frame_runtime_archive.c - Runtime-only typed frame link boundary
 */

#include "vm/xr_typed_frame.h"
#include "vm/xr_typed_dispatch.h"
#include "vm/debug/xr_vm_materialize.h"
#include "vm/debug/xr_vm_profile.h"
#include "vm/debug/xr_vm_trace.h"
#include <stdio.h>

int main(void) {
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrFingerprint fingerprint = {{0}};
    XrTypedFrame *frame = (XrTypedFrame *) (uintptr_t) 1;
    XrTypedFrameContext context;
    XrTypedFrameMemoryFootprint footprint;
    XrTypedSlotAccess access = {0};
    XrModuleGenerationIdentity generation = {0};
    uint8_t byte = 0;
    int64_t result = 1;
    XrTypedDispatchI64Request request = {
        .required_plan_fingerprint = &fingerprint,
        .result = &result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    XrVmProfile profile;
    XrVmProfileSnapshot profile_snapshot;
    XrVmTraceEvent trace_storage[1];
    XrVmTraceBuffer trace_buffer;
    XrVmTraceSink trace_sink;
    XrVmDebugSession debug_session;
    XrVmMaterializedEvent materialized;
    XrVmTraceEvent trace_event = {0};
    if (XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION != UINT32_C(33) ||
        XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK != XR_TARGET_REQUIRED_FAMILIES ||
        limits.max_arena_bytes != XR_TYPED_FRAME_MAX_ARENA_BYTES ||
        xr_typed_frame_create(NULL, &fingerprint, 0, &limits, &frame) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        frame != NULL ||
        xr_typed_frame_describe_slot(NULL, 0, &access) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_store(NULL, &access, &byte, sizeof(byte)) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_load(NULL, &access, &byte, sizeof(byte)) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_cleanup(NULL) != XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_context(NULL, &context) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_memory_footprint(NULL, &footprint) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_enter_instruction(NULL, 0) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_bind_coroutine_state(NULL, 0) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_bind_generation_identity(NULL, &generation) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_link_child(NULL, NULL) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_unlink_child(NULL, NULL) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        !xr_typed_profile_init(&profile) ||
        !xr_typed_profile_snapshot(&profile, &profile_snapshot) ||
        !xr_typed_trace_buffer_init(&trace_buffer, trace_storage, 1) ||
        (trace_sink = xr_typed_trace_buffer_sink(&trace_buffer)).emit == NULL ||
        xr_typed_debug_session_init(&fingerprint, NULL, &trace_sink, &profile,
                                 &debug_session) !=
            XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH ||
        xr_typed_materialize_event(NULL, &fingerprint, &trace_event,
                                &materialized) !=
            XR_VM_MATERIALIZE_INVALID_ARGUMENT ||
        xr_typed_dispatch_execute_i64(&request) !=
            XR_TYPED_DISPATCH_INVALID_ARGUMENT ||
        result != 0) {
        fputs("runtime-only typed frame boundary failed\n", stderr);
        return 1;
    }
    xr_typed_frame_free(NULL);
    puts("runtime-only typed frame boundary passed");
    return 0;
}
