/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_lifecycle_cases.h - Late events and retained results across cleanup providers
 *
 * KEY CONCEPT:
 *   Stopping one instance must not consume a peer's resume or result ownership.
 */
#ifndef XIR_MIXED_LIFECYCLE_CASES_H
#define XIR_MIXED_LIFECYCLE_CASES_H

static void mixed_lifecycle_rejected_events(XrXirInstance *instance,
    XrXirInstanceResult suspended, const CleanupSourceLog *log) {
    CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(suspended.epoch && suspended.outcome.wake && !log->at);
    const size_t attempts = runtime_attempts, live = runtime_live, bytes = runtime_bytes;
    CHECK(xr_xir_instance_resume(instance, suspended.epoch + 1, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, suspended.epoch, suspended.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    XrXirInstanceResult repeated = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    CHECK(repeated.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(repeated.epoch == suspended.epoch && repeated.outcome.wake == suspended.outcome.wake);
    CHECK(!log->at && runtime_attempts == attempts && runtime_live == live && runtime_bytes == bytes);
}

static void mixed_lifecycle_stopped(XrXirInstance *instance,
    XrXirInstanceResult suspended, uint32_t entry, CleanupSourceLog *log) {
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
    CHECK(log->at == log->count);
    CHECK(xr_xir_instance_resume(instance, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(instance, &empty) == XR_XIR_CALL_BAD_STATE);
    CHECK(!empty.type && !empty.reserved && !empty.payload && log->at == log->count);
}

static void mixed_lifecycle_case(unsigned native_cleanup) {
    CHECK(native_cleanup < 2 && !runtime_live && !runtime_bytes);
    XrXirCompileContext context = cleanup_source_context((XrCompileResourceLimits){67108864,8388608,128000000});
    XrCompileResourceStats baseline = cleanup_source_stats(&context);
    XrXirProgram *program = NULL;
    const unsigned releases = cleanup_mixed_releases;
    CHECK(cleanup_mixed_build(&context, native_cleanup, &program) == XR_XIR_OK && program);
    const CleanupExpected expected[] = {{XR_XIR_STRING,0,"cancelled"},{XR_XIR_STRING,0,"cancelled"}};
    CleanupSourceLog logs[] = {{expected,1,0},{expected,2,0}};
    XrXirInstance *instances[2] = {0};
    XrXirInstanceResult suspended[2] = {0};
    const uint32_t entry = cleanup_source_functions[CLEANUP_CANCELLED];
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,cleanup_source_output,&logs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    /* Only the two execution owners retain the program before either starts. */
    xr_xir_compile_program_drop(program); program = NULL;
    CHECK(cleanup_mixed_releases == releases);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
        suspended[i] = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);
        mixed_lifecycle_rejected_events(instances[i], suspended[i], &logs[i]);
    }
    mixed_lifecycle_stopped(instances[0], suspended[0], entry, &logs[0]);
    CHECK(!logs[1].at && cleanup_mixed_releases == releases);
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY); instances[0] = NULL;
    CHECK(cleanup_mixed_releases == releases && !logs[1].at);
    CHECK(xr_xir_instance_resume(instances[1], suspended[1].epoch, suspended[1].outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(instances[1], suspended[1].epoch, suspended[1].outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_poll_bounded(instances[1], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(logs[1].at == 1 && logs[0].at == 1);
    CHECK(xr_xir_instance_resume(instances[1], suspended[1].epoch, suspended[1].outcome.wake) == XR_XIR_CALL_BAD_STATE);
    XrXirValue occupied = {XR_XIR_I64,0,INT64_C(0x12345678)}, original = occupied;
    CHECK(xr_xir_instance_take_result(instances[1], &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied, &original, sizeof(occupied)));
    XrXirValue retained = {0}, empty = {0};
    CHECK(xr_xir_instance_take_result(instances[1], &retained) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instances[1], &empty) == XR_XIR_CALL_BAD_STATE);
    CHECK(!empty.type && !empty.reserved && !empty.payload);
    cleanup_source_string(&retained, "resumed");
    CHECK(xr_xir_instance_start(instances[1], entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult fresh = xr_xir_instance_poll_bounded(instances[1], UINT64_MAX);
    CHECK(fresh.outcome.status == XR_XIR_CALL_SUSPENDED && fresh.epoch != suspended[1].epoch);
    CHECK(xr_xir_instance_resume(instances[1], suspended[1].epoch, suspended[1].outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(logs[1].at == 1);
    mixed_lifecycle_stopped(instances[1], fresh, entry, &logs[1]);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY); instances[1] = NULL;
    CHECK(cleanup_mixed_releases == releases + 1);
    /* Independent string bytes keep only their value domain after code retirement. */
    CHECK(runtime_live && runtime_bytes);
    cleanup_source_string(&retained, "resumed");
    xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !retained.type && !retained.payload);
    cleanup_source_owner_free(&context, baseline);
    printf("Mixed lifecycle nativeCleanup=%u stale/duplicate rejected, cleanup=1/2, codeRelease=1, retained=resumed, physical=0/0\n", native_cleanup);
}
#endif // XIR_MIXED_LIFECYCLE_CASES_H
