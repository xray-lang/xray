/*
 * test_typed_frame_runtime_archive.c - Runtime-only typed frame link boundary
 */

#include "vm/xr_typed_frame.h"
#include "vm/xr_typed_dispatch.h"
#include "vm/xr_typed_lifecycle.h"
#include "vm/debug/xr_vm_debug_control.h"
#include "vm/debug/xr_vm_materialize.h"
#include "vm/debug/xr_vm_profile.h"
#include "vm/debug/xr_vm_trace.h"
#include "runtime/xr_dynamic_entry_runtime.h"
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
    XrVmDebugSession *debug_session = (XrVmDebugSession *) (uintptr_t) 1;
    XrVmDebugPlan *debug_plan = (XrVmDebugPlan *) (uintptr_t) 1;
    XrVmDebugControl *debug_control = NULL;
    XrVmMaterializedEvent materialized;
    XrRuntimeDynamicEntryLeaseStats lease_stats;
    XrTypedLifecycleContext lifecycle_context = {0};
    XrVmTraceEvent trace_event = {0};
    uint32_t count = 0;
    if (XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION != UINT32_C(39) ||
        XR_TYPED_LIFECYCLE_CONTEXT_SCHEMA_VERSION != UINT32_C(2) ||
        XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION != UINT32_C(3) ||
        XR_VM_TRACE_SCHEMA_VERSION != UINT32_C(3) ||
        XR_VM_DEBUG_CONTROL_SCHEMA_VERSION != UINT32_C(1) ||
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
        xr_typed_frame_visit_coroutine_roots(NULL, 0, NULL, NULL, &count) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_resume_coroutine_state(NULL, 0) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_execute_cleanups(NULL, 0, 0, NULL, NULL, &count) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_bind_generation_identity(NULL, &generation) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_link_child(NULL, NULL) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_frame_unlink_child(NULL, NULL) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_lifecycle_context_init(
            NULL, &fingerprint, 0, NULL, &lifecycle_context) !=
            XR_TYPED_LIFECYCLE_INVALID_ARGUMENT ||
        xr_typed_lifecycle_execute(
            NULL, NULL, 0, XR_TYPED_LIFECYCLE_EXIT_NORMAL, &count) !=
            XR_TYPED_LIFECYCLE_INVALID_ARGUMENT ||
        !xr_typed_profile_init(&profile) ||
        !xr_typed_profile_snapshot(&profile, &profile_snapshot) ||
        !xr_typed_trace_buffer_init(&trace_buffer, trace_storage, 1) ||
        (trace_sink = xr_typed_trace_buffer_sink(&trace_buffer)).emit == NULL ||
        xr_typed_debug_session_create(&fingerprint, NULL, &trace_sink, &profile,
                                 NULL, &debug_session) !=
            XR_VM_DEBUG_SESSION_PLAN_IDENTITY_MISMATCH ||
        debug_session != NULL ||
        xr_typed_debug_plan_create(NULL, &fingerprint, NULL, 0, &debug_plan) !=
            XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT ||
        debug_plan != NULL ||
        xr_typed_debug_control_create(NULL, NULL, NULL, &debug_control) !=
            XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT ||
        debug_control != NULL ||
        xr_typed_debug_control_arm(NULL, XR_VM_DEBUG_RESUME_CONTINUE) !=
            XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT ||
        xr_typed_debug_control_free(NULL) !=
            XR_VM_DEBUG_CONTROL_INVALID_ARGUMENT ||
        xr_typed_debug_control_matches_plan(NULL, fingerprint) ||
        xr_typed_materialize_event(NULL, &fingerprint, &trace_event,
                                &materialized) !=
            XR_VM_MATERIALIZE_INVALID_ARGUMENT ||
        xr_typed_dispatch_execute_i64(&request) !=
            XR_TYPED_DISPATCH_INVALID_ARGUMENT ||
        xr_runtime_dynamic_entry_lease_stats(NULL, &lease_stats) ||
        result != 0) {
        fputs("runtime-only typed frame boundary failed\n", stderr);
        return 1;
    }
    if (xr_typed_frame_free(NULL) != XR_TYPED_FRAME_INVALID_ARGUMENT)
        return 1;
    xr_typed_debug_plan_free(NULL);
    xr_typed_debug_session_free(NULL);
    xr_typed_lifecycle_context_dispose(NULL);
    puts("runtime-only typed frame boundary passed");
    return 0;
}
