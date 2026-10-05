/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_program_cases.h - Independent cleanup order and result witnesses
 */
#ifndef XIR_CLEANUP_PROGRAM_CASES_H
#define XIR_CLEANUP_PROGRAM_CASES_H
typedef struct CleanupProgramLog { int64_t values[8]; uint32_t count; } CleanupProgramLog;
static XrXirOutputStatus cleanup_program_write(void *context, const XrXirOutputGroup *group) {
    CleanupProgramLog *log = context;
    CHECK(group->stream == XR_XIR_STDOUT && !group->line && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && log->count < 8);
    log->values[log->count++] = group->values[0].payload;
    return XR_XIR_OUTPUT_OK;
}
static void cleanup_program_cases(XrXirProgram *program, unsigned mode) {
    CleanupProgramLog log = {0}; XrXirInstance *instance = NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, cleanup_program_write, &log};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance, 1, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    if (mode == 1) {
        CHECK(result.outcome.status == XR_XIR_CALL_SUSPENDED && !log.count);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
    } else {
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue owned = {0};
        CHECK(xr_xir_instance_take_result(instance, &owned) == XR_XIR_CALL_RETURNED);
        CHECK(owned.type == XR_XIR_I64 && owned.payload == (mode == 5 ? 0 : 10));
        xr_xir_value_drop(&owned);
    }
    const int64_t ordinary[] = {20, 99, 10}, full[] = {20, 10, 99}, early[] = {0, 99};
    uint32_t count = mode == 1 || mode == 4 || mode == 5 ? 2u : 3u;
    const int64_t *expected = mode == 5 ? early : mode == 1 || mode == 2 || mode == 4 ? full : ordinary;
    CHECK(log.count == count && !memcmp(log.values, expected, count * sizeof(*expected)));
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(log.count == count);
}
#endif // XIR_CLEANUP_PROGRAM_CASES_H
