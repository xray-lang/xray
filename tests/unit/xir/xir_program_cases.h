/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_program_cases.h - Independent expected effects and instance values
 *
 * KEY CONCEPT:
 *   VM and native executables each compare to these fixed language outcomes.
 */
#ifndef XIR_PROGRAM_CASES_H
#define XIR_PROGRAM_CASES_H
#include "xir/xxir_program.h"
typedef struct ProgramLog {
    XrXirValue values[32];
    XrXirOutputStream streams[32];
    uint32_t outputs, releases[8], released;
} ProgramLog;
static bool program_write(void *context, XrXirOutputStream stream, const XrXirValue *value) {
    ProgramLog *log = context;
    CHECK(log->outputs < 32);
    log->streams[log->outputs] = stream;
    CHECK(xr_xir_value_copy(value, &log->values[log->outputs++]) == XR_XIR_VALUE_OK);
    return true;
}
static void program_trace(void *context, XrXirLifecycleEvent event, uint32_t index) {
    ProgramLog *log = context;
    if (event == XR_XIR_SLOT_RELEASED) {
        CHECK(log->released < 8); log->releases[log->released++] = index;
    }
}
static void program_bytes(const XrXirValue *value, const char *expected, size_t count) {
    const char *bytes; size_t length;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == count && !memcmp(bytes, expected, count));
}
static void check_program_output(const ProgramLog *log, uint32_t run) {
    uint32_t start = run ? 8 : 3;
    if (!run) {
        program_bytes(&log->values[0], "A\0\xe4\xb8\xad", 5);
        program_bytes(&log->values[1], "B\xf0\x9f\x98\x80", 5);
        program_bytes(&log->values[2], "root", 4);
        for (uint32_t i = 0; i < 3; ++i) CHECK(log->streams[i] == XR_XIR_STDERR);
    }
    CHECK(log->outputs == start + 5);
    program_bytes(&log->values[start], "A\0\xe4\xb8\xad", 5);
    program_bytes(&log->values[start + 1], "B\xf0\x9f\x98\x80", 5);
    CHECK(log->values[start + 2].type == XR_XIR_I64 && log->values[start + 2].payload == (run ? 32 : 30));
    CHECK(log->values[start + 3].type == XR_XIR_BOOL && log->values[start + 3].payload == 1);
    program_bytes(&log->values[start + 4], run ? "updated" : "root", run ? 7 : 4);
    for (uint32_t i = 0; i < 5; ++i)
        CHECK(log->streams[start + i] == (i == 3 ? XR_XIR_STDERR : XR_XIR_STDOUT));
}
static XrXirValue program_result(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static void program_cases(XrXirProgram *program, uint32_t mode) {
    ProgramLog logs[2] = {0}; XrXirInstance *instances[2] = {0};
    XrXirInstanceResult suspended[2] = {0};
    XrXirValue strings[2] = {0}, cells[2] = {0};
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {program_write, &logs[i]};
        config.trace = program_trace; config.trace_context = &logs[i];
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], 3, NULL, 0) == XR_XIR_CALL_READY);
        suspended[i] = xr_xir_instance_poll(instances[i]);
        CHECK(suspended[i].outcome.status == (mode ? XR_XIR_CALL_SUSPENDED : XR_XIR_CALL_RETURNED));
        if (mode) CHECK(logs[i].outputs == 1);
    }
    xr_xir_program_drop(program);
    for (uint32_t i = 0; i < 2; ++i) {
        if (mode) CHECK(xr_xir_instance_resume(instances[i], suspended[i].epoch, suspended[i].outcome.wake) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = xr_xir_instance_poll(instances[i]);
        if (mode == 2) {
            CHECK(result.outcome.status == XR_XIR_CALL_THROWN && result.outcome.value.payload == 91);
            CHECK(xr_xir_instance_start(instances[i], 3, NULL, 0) == XR_XIR_CALL_THROWN);
            CHECK(logs[i].outputs == 1 && logs[i].released == 2);
            CHECK(logs[i].releases[0] == 2 && logs[i].releases[1] == 0);
        } else {
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.payload == 30);
            check_program_output(&logs[i], 0);
            XrXirValue second = program_result(instances[i], 3); CHECK(second.payload == 32);
            check_program_output(&logs[i], 1);
            strings[i] = program_result(instances[i], 6);
            cells[i] = program_result(instances[i], 7);
            CHECK(logs[i].outputs == 14 && logs[i].values[13].type == XR_XIR_I64 && logs[i].values[13].payload == 12);
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        if (mode != 2) {
            const uint32_t releases[] = {4, 3, 1, 2, 0};
            CHECK(logs[i].released == 5 && !memcmp(logs[i].releases, releases, sizeof(releases)));
        }
    }
    if (mode != 2) {
        for (uint32_t i = 0; i < 2; ++i) program_bytes(&strings[i], "updated", 7);
        int64_t previous = 0, other = 0;
        CHECK(xr_xir_atomic_i64_fetch_add(&cells[0], 100, &previous) && previous == 12);
        CHECK(xr_xir_atomic_i64_load(&cells[1], &other) && other == 12);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        xr_xir_value_drop(&strings[i]); xr_xir_value_drop(&cells[i]);
        for (uint32_t j = 0; j < logs[i].outputs; ++j) xr_xir_value_drop(&logs[i].values[j]);
    }
}
#endif
