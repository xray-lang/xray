/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_entries_shadow.h - Ordinary same-spelling nominal methods
 *
 * KEY CONCEPT:
 *   A user Array class and its read methods have no governed Array authority.
 */
#ifndef XIR_ARRAY_ENTRIES_SHADOW_H
#define XIR_ARRAY_ENTRIES_SHADOW_H
static XrXirInstance *shadow_open(EntriesCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576;
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue result = entries_run(instance, run->program->declarations->entry_function);
    CHECK(result.type == XR_XIR_I64 && result.payload == 0); xr_xir_value_drop(&result); return instance;
}
static void entries_goldens(EntriesCompile *run) {
    for (unsigned row = 0; row < 2; ++row) {
        const uint32_t function = entries_find(run->module, row ? "case1" : "case0");
        XrXirInstance *instance = shadow_open(run); runtime_attempts = 0;
        XrXirValue result = entries_run(instance, function);
        CHECK(result.type == (uint32_t)(row ? XR_XIR_BOOL : XR_XIR_I64) && result.payload == (row ? 1 : 16));
        const size_t sites = runtime_attempts;
        xr_xir_value_drop(&result); CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
        for (size_t failure = 0; failure < sites; ++failure) {
            instance = shadow_open(run); runtime_attempts = 0; runtime_fail_at = failure;
            XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
            while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
            CHECK(status == XR_XIR_CALL_OOM && runtime_attempts > failure); runtime_fail_at = SIZE_MAX;
            result = (XrXirValue){0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_BAD_STATE && !result.type && !result.payload);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
        }
        printf("user Array ordinary read-entries case%u runtimeOOM%zu physical0\n", row, sites);
    }
}
#endif // XIR_ARRAY_ENTRIES_SHADOW_H
