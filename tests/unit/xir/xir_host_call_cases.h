/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_host_call_cases.h - Independent host suspension, cleanup and lifetime oracles
 */
#ifndef XIR_HOST_CALL_CASES_H
#define XIR_HOST_CALL_CASES_H
typedef struct HostTrace {
    XrXirHostCall *call;
    int64_t values[8];
    uint32_t count, reentries;
} HostTrace;
static XrXirOutputStatus host_output(void *context, const XrXirOutputGroup *group) {
    HostTrace *trace = context;
    CHECK(group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(trace->count < 8);
    if (group->values[0].type == XR_XIR_STRING) {
        const char *text = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&group->values[0], &text, &length) && length == 7 && !memcmp(text, "cleanup", 7));
        trace->values[trace->count++] = 7;
    } else {
        CHECK(group->values[0].type == XR_XIR_I64);
        trace->values[trace->count++] = (int64_t)group->values[0].payload;
    }
    if (trace->call) {
        XrXirCallResult result = {0};
        CHECK(xr_xir_host_call_step_bounded(trace->call, UINT64_MAX).outcome.status == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_host_call_resume(trace->call, 1, 1) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_host_call_request_cancel(trace->call) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_host_call_take(trace->call, &result) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_host_call_drop(trace->call) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_call_result_empty(&result));
        ++trace->reentries;
    }
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceConfig host_config(HostTrace *trace) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, host_output, trace};
    return config;
}
static void host_fatal_cleanup(XrXirProgram *program, uint32_t entry) {
    HostTrace trace = {0}; XrXirInstanceConfig config = host_config(&trace);
    XrXirHostExecutionRequest request = {program, &config, entry, NULL, 0};
    XrXirHostCall *call = NULL;
    CHECK(xr_xir_host_call_begin(&request, &call) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_host_call_step_bounded(call, UINT64_MAX).outcome.status == XR_XIR_CALL_SUSPENDED);
    (void)xr_xir_host_call_drop(call);
    fputs("fatal cleanup incorrectly returned to the host\n", stderr);
    exit(99);
}
static void host_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}
static void host_copy_limit(XrXirHostCall *call, const XrXirValue *value) {
    XirObject *object = object_pointer(value);
    uint32_t references = atomic_load(&object->references);
    atomic_store(&object->references, UINT32_MAX);
    XrXirCallResult output = {0};
    CHECK(xr_xir_host_call_take(call, &output) == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&output));
    atomic_store(&object->references, references);
}
static XrXirInstanceResult host_resume(XrXirHostCall *call, XrXirInstanceResult suspended) {
    CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED && suspended.epoch && suspended.outcome.wake);
    CHECK(xr_xir_host_call_resume(call, suspended.epoch + 1, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_host_call_resume(call, suspended.epoch, suspended.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    XrXirInstanceResult repeated = xr_xir_host_call_step_bounded(call, UINT64_MAX);
    CHECK(repeated.epoch == suspended.epoch && repeated.outcome.wake == suspended.outcome.wake);
    CHECK(xr_xir_host_call_resume(call, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_host_call_resume(call, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    return xr_xir_host_call_step_bounded(call, UINT64_MAX);
}
static void host_faults(XrXirProgram *program, const uint32_t entries[3], unsigned mode) {
    size_t live = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t point = 0; point <= sites; ++point) {
        HostTrace trace = {0}; XrXirInstanceConfig config = host_config(&trace);
        XrXirHostExecutionRequest request = {program, &config, entries[mode == 3 ? 1 : mode == 4 ? 2 : 0], NULL, 0};
        runtime_attempts = 0; runtime_fail_at = point ? point - 1 : SIZE_MAX;
        XrXirHostCall *call = NULL; XrXirCallResult owned = {0};
        XrXirCallStatus status = xr_xir_host_call_begin(&request, &call);
        trace.call = call;
        if (status == XR_XIR_CALL_READY) {
            XrXirInstanceResult result = xr_xir_host_call_step_bounded(call, UINT64_MAX);
            status = result.outcome.status;
            if (status == XR_XIR_CALL_SUSPENDED && (mode == 1 || mode == 2 || mode == 4)) {
                if (mode == 1) {
                    CHECK(xr_xir_host_call_request_cancel(call) == XR_XIR_CALL_CANCEL_REQUESTED);
                    status = xr_xir_host_call_step_bounded(call, UINT64_MAX).outcome.status;
                }
                else { status = xr_xir_host_call_drop(call); call = NULL; }
            } else {
                while (result.outcome.status == XR_XIR_CALL_SUSPENDED) result = host_resume(call, result);
                status = result.outcome.status;
            }
            if (call) CHECK(xr_xir_host_call_take(call, &owned) == status);
        }
        if (!point) {
            const XrXirCallStatus expected[] = {XR_XIR_CALL_RETURNED, XR_XIR_CALL_CANCELLED,
                XR_XIR_CALL_READY, XR_XIR_CALL_ASSERTION, XR_XIR_CALL_READY};
            CHECK(status == expected[mode]); sites = runtime_attempts;
            if (mode == 0) host_text(&owned.value, "held-result");
            if (mode == 3) host_text(&owned.panic.message, "owned-panic");
            if (mode <= 2) CHECK(trace.count == (mode ? 2u : 3u) && trace.values[0] == 1 &&
                trace.values[1] == (mode ? 1 : 11) && (mode || trace.values[2] == 11));
            CHECK(trace.count == trace.reentries);
        } else {
            if (status != XR_XIR_CALL_OOM) fprintf(stderr, "host mode=%u OOM=%zu/%zu status=%u\n", mode, point, sites, status);
            CHECK(status == XR_XIR_CALL_OOM && runtime_attempts > runtime_fail_at);
        }
        runtime_fail_at = SIZE_MAX;
        CHECK(xr_xir_host_call_drop(call) == XR_XIR_CALL_READY);
        xr_xir_call_result_drop(&owned);
        CHECK(runtime_live == live && runtime_bytes == bytes);
    }
    printf("Host mode=%u allocation failures=%zu; physical baseline PASS\n", mode, sites);
}
static void host_rejections(XrXirProgram *program, uint32_t entry) {
    HostTrace trace = {0}; XrXirInstanceConfig config = host_config(&trace);
    XrXirHostExecutionRequest request = {program, &config, entry, NULL, 0};
    XrXirHostCall *call = NULL;
    CHECK(xr_xir_host_call_begin(&request, &call) == XR_XIR_CALL_READY);
    XrXirHostCall *before = call; size_t attempts = runtime_attempts;
    CHECK(xr_xir_host_call_begin(&request, &call) == XR_XIR_CALL_BAD_ARGUMENT && call == before && runtime_attempts == attempts);
    XrXirCallResult owned = {0};
    CHECK(xr_xir_host_call_take(call, &owned) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&owned));
    CHECK(xr_xir_host_call_request_cancel(call) == XR_XIR_CALL_CANCEL_REQUESTED && !trace.count);
    CHECK(xr_xir_host_call_step_bounded(call, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED && !trace.count);
    CHECK(xr_xir_host_call_request_cancel(call) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_host_call_take(call, &owned) == XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_host_call_drop(call) == XR_XIR_CALL_READY); call = NULL;
    xr_xir_call_result_drop(&owned);
    config.metadata_limit = 0;
    CHECK(xr_xir_host_call_begin(&request, &call) == XR_XIR_CALL_LIMIT && !call);
    config = host_config(&trace); request.entry = UINT32_MAX;
    CHECK(xr_xir_host_call_begin(&request, &call) == XR_XIR_CALL_BAD_ARGUMENT && !call);
    CHECK(xr_xir_host_call_step_bounded(NULL, UINT64_MAX).outcome.status == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_host_call_drop(NULL) == XR_XIR_CALL_READY);
}
/* Consumes the producer's Program reference before either instance executes. */
static void host_lifetimes(XrXirProgram *program, const uint32_t entries[3]) {
    HostTrace traces[3] = {0}; XrXirHostCall *calls[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        XrXirInstanceConfig config = host_config(&traces[i]);
        XrXirHostExecutionRequest request = {program, &config, entries[i == 2 ? 1 : 0], NULL, 0};
        CHECK(xr_xir_host_call_begin(&request, &calls[i]) == XR_XIR_CALL_READY);
        traces[i].call = calls[i]; memset(&config, 0xCC, sizeof(config)); memset(&request, 0xCC, sizeof(request));
    }
    xr_xir_compile_program_drop(program);
    XrXirInstanceResult first = xr_xir_host_call_step_bounded(calls[0], UINT64_MAX), second = xr_xir_host_call_step_bounded(calls[1], UINT64_MAX);
    CHECK(first.outcome.status == XR_XIR_CALL_SUSPENDED && second.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(traces[0].count == 1 && traces[0].values[0] == 1 && traces[1].count == 1 && traces[1].values[0] == 1);
    { CHECK(xr_xir_host_call_request_cancel(calls[0]) == XR_XIR_CALL_CANCEL_REQUESTED); CHECK(xr_xir_host_call_step_bounded(calls[0], UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED); }
    CHECK(traces[0].count == 2 && traces[0].values[1] == 1);
    CHECK(xr_xir_host_call_resume(calls[0], first.epoch, first.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_host_call_step_bounded(calls[0], UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED && traces[0].count == 2);
    XrXirCallResult owned[3] = {0};
    CHECK(xr_xir_host_call_take(calls[0], &owned[0]) == XR_XIR_CALL_CANCELLED);
    second = host_resume(calls[1], second);
    CHECK(second.outcome.status == XR_XIR_CALL_SUSPENDED && traces[1].count == 2 && traces[1].values[1] == 11);
    second = host_resume(calls[1], second);
    CHECK(second.outcome.status == XR_XIR_CALL_RETURNED && traces[1].count == 3 && traces[1].values[2] == 11);
    XrXirCallResult held = {XR_XIR_CALL_RETURNED, {XR_XIR_I64, 0, 7}, 0, {0}}, held_before = held;
    CHECK(xr_xir_host_call_take(calls[1], &held) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&held, &held_before, sizeof(held)));
    host_copy_limit(calls[1], &second.outcome.value);
    CHECK(xr_xir_host_call_take(calls[1], &owned[1]) == XR_XIR_CALL_RETURNED);
    XrXirCallResult duplicate = {0};
    CHECK(xr_xir_host_call_take(calls[1], &duplicate) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&duplicate));
    CHECK(xr_xir_host_call_step_bounded(calls[1], UINT64_MAX).outcome.status == XR_XIR_CALL_BAD_STATE);
    XrXirInstanceResult panic = host_resume(calls[2], xr_xir_host_call_step_bounded(calls[2], UINT64_MAX));
    CHECK(panic.outcome.status == XR_XIR_CALL_ASSERTION && traces[2].count == 1 && traces[2].values[0] == 7);
    host_copy_limit(calls[2], &panic.outcome.panic.message);
    CHECK(xr_xir_host_call_take(calls[2], &owned[2]) == XR_XIR_CALL_ASSERTION);
    for (unsigned i = 0; i < 3; ++i) CHECK(xr_xir_host_call_drop(calls[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes);
    host_text(&owned[1].value, "held-result"); host_text(&owned[2].panic.message, "owned-panic");
    for (unsigned i = 0; i < 3; ++i) xr_xir_call_result_drop(&owned[i]);
    CHECK(!runtime_live && !runtime_bytes);
}
static void host_cases(XrXirProgram *program, const uint32_t entries[3]) {
    host_rejections(program, entries[0]);
    for (unsigned mode = 0; mode < 5; ++mode) host_faults(program, entries, mode);
    host_lifetimes(program, entries);
    puts("Host cancellation/resume, once-only cleanup, isolated instances and owned panic/results PASS");
}
#endif // XIR_HOST_CALL_CASES_H
