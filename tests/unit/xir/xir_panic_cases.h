/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_panic_cases.h - Independent panic values, suspension and release expectations
 */
#ifndef XIR_PANIC_CASES_H
#define XIR_PANIC_CASES_H
#include "xir/xxir_panic.h"
enum { PANIC_DIRECT, PANIC_BOUND, PANIC_BOUNDS, PANIC_NESTED, PANIC_CHANNELS,
    PANIC_ORDINARY, PANIC_LOCAL, PANIC_INDIRECT, PANIC_SUSPENDED, PANIC_HANDLER,
    PANIC_INFORMATION, PANIC_MATCH, PANIC_UNCAUGHT, PANIC_ENTRY, PANIC_FUNCTIONS };
static XrXirOutputStatus panic_output(void *context, const XrXirOutputGroup *group) {
    unsigned *calls = context;
    const int64_t expected[] = {17, 421, 23, 5};
    CHECK(!*calls && group->stream == XR_XIR_STDOUT && group->line && group->count == 4);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == expected[i]);
    ++*calls; return XR_XIR_OUTPUT_OK;
}
static XrXirCallResult panic_poll(XrXirInstance *instance, unsigned *suspensions) {
    XrXirInstanceResult result = xr_xir_instance_poll(instance);
    while (result.outcome.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(++*suspensions <= 4);
        CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll(instance);
    }
    return result.outcome;
}
static XrXirValue panic_run(XrXirInstance *instance, uint32_t entry, unsigned expected_suspensions) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    unsigned suspensions = 0; XrXirCallResult outcome = panic_poll(instance, &suspensions);
    if (outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "panic entry %u returned status %u code %u\n", entry, outcome.status, outcome.panic.detail.code);
    CHECK(outcome.status == XR_XIR_CALL_RETURNED && suspensions == expected_suspensions);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static void panic_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}
static void panic_failures(XrXirProgram *program, uint32_t entry) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site ? site - 1 : SIZE_MAX;
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
        unsigned calls = 0; config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, panic_output, &calls};
        XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
        if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, entry, NULL, 0);
        unsigned suspensions = 0;
        if (status == XR_XIR_CALL_READY) status = panic_poll(instance, &suspensions).status;
        if (!site) {
            if (status != XR_XIR_CALL_RETURNED)
                fprintf(stderr, "panic baseline entry=%u status=%u\n", entry, status);
            CHECK(status == XR_XIR_CALL_RETURNED); sites = runtime_attempts;
        }
        else {
            if (status != XR_XIR_CALL_OOM)
                fprintf(stderr, "panic OOM entry=%u site=%zu/%zu status=%u\n", entry, site, sites, status);
            CHECK(runtime_attempts > runtime_fail_at && status == XR_XIR_CALL_OOM);
        }
        runtime_fail_at = SIZE_MAX;
        if (instance) CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == live && runtime_bytes == bytes);
    }
    printf("Panic runtime entry %u: %zu allocation failure sites released\n", entry, sites);
}
static void panic_cancel(XrXirProgram *program, uint32_t entry) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
    unsigned calls = 0; config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, panic_output, &calls};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult paused = xr_xir_instance_poll(instance);
    CHECK(paused.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(runtime_live == live && runtime_bytes == bytes);
}
static void panic_cases(XrXirProgram *program, const uint32_t *functions) {
    panic_failures(program, functions[PANIC_INFORMATION]);
    panic_failures(program, functions[PANIC_SUSPENDED]);
    panic_failures(program, functions[PANIC_HANDLER]);
    panic_cancel(program, functions[PANIC_SUSPENDED]);
    panic_cancel(program, functions[PANIC_HANDLER]);
    unsigned calls = 0; XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, panic_output, &calls};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    const unsigned integers[] = {PANIC_DIRECT, PANIC_NESTED, PANIC_CHANNELS,
        PANIC_ORDINARY, PANIC_LOCAL, PANIC_INDIRECT, PANIC_MATCH};
    const int64_t expected[] = {17, 421, 23, 420, 5, 420, 442};
    for (unsigned i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
        XrXirValue value = panic_run(instance, functions[integers[i]], 0);
        CHECK(value.type == XR_XIR_I64 && value.payload == expected[i]); xr_xir_value_drop(&value);
    }
    const unsigned strings[] = {PANIC_BOUND, PANIC_BOUNDS, PANIC_SUSPENDED, PANIC_HANDLER};
    for (unsigned i = 0; i < 4; ++i) {
        XrXirValue value = panic_run(instance, functions[strings[i]], i >= 2);
        panic_text(&value, i == 1 ? "array index out of range: 4 (length 1)" : "division by zero");
        xr_xir_value_drop(&value);
    }
    XrXirValue entry = panic_run(instance, functions[PANIC_ENTRY], 0);
    CHECK(entry.type == XR_XIR_I64 && entry.payload == 0 && calls == 1); xr_xir_value_drop(&entry);
    XrXirValue info = panic_run(instance, functions[PANIC_INFORMATION], 0), copy = {0};
    CHECK(info.type == XR_XIR_PANIC_INFO && xr_xir_value_copy(&info, &copy) == XR_XIR_VALUE_OK);
    CHECK(info.payload == copy.payload);
    CHECK(xr_xir_instance_start(instance, functions[PANIC_UNCAUGHT], NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
    CHECK(outcome.status == XR_XIR_CALL_DIVIDE_BY_ZERO && outcome.panic.detail.code == 420);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_DIVIDE_BY_ZERO);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    XrXirValue recovered = panic_run(instance, functions[PANIC_DIRECT], 0);
    CHECK(recovered.type == XR_XIR_I64 && recovered.payload == 17); xr_xir_value_drop(&recovered);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program); xr_xir_value_drop(&info);
    XrXirFaultDetail detail = {0}; XrXirValue message = {0};
    CHECK(xr_xir_panic_info_detail(&copy, &detail) && detail.code == 420);
    CHECK(xr_xir_panic_info_message(&copy, &message) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&copy); panic_text(&message, "division by zero"); xr_xir_value_drop(&message);
    CHECK(!runtime_live && !runtime_bytes);
}
static void panic_sticky(XrXirProgram *program, uint32_t entry) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instances[i]).outcome;
        CHECK(result.status == XR_XIR_CALL_DIVIDE_BY_ZERO && result.panic.detail.code == 420);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_FAILED);
    }
    for (unsigned i = 0; i < 2; ++i) {
        XrXirCallResult failure = {0};
        CHECK(xr_xir_instance_copy_failure(instances[i], &failure) == XR_XIR_CALL_DIVIDE_BY_ZERO);
        CHECK(failure.panic.detail.code == 420 && failure.value.type == XR_XIR_UNIT);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_DIVIDE_BY_ZERO);
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_DIVIDE_BY_ZERO);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_PANIC_CASES_H
