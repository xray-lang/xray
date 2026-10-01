/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_program_cases.h - Backend-independent Array result and fault oracles
 *
 * KEY CONCEPT:
 *   Each backend checks fixed bytes, instance isolation and escaped ownership.
 */
#ifndef XIR_ARRAY_PROGRAM_CASES_H
#define XIR_ARRAY_PROGRAM_CASES_H
#include "xir/xxir_array.h"
typedef struct ArrayProgramLog {
    char bytes[128];
    size_t length;
    uint32_t releases[3], released;
} ArrayProgramLog;
static XrXirOutputStatus array_program_output(void *context, const XrXirOutputGroup *group) {
    ArrayProgramLog *log = context;
    CHECK(group->line && group->count == 1 && group->stream == XR_XIR_STDOUT);
    const XrXirValue *value = group->values;
    const char *bytes = NULL; size_t length = 0; char number[32];
    if (value->type == XR_XIR_STRING) CHECK(xr_xir_string_view(value, &bytes, &length));
    else {
        CHECK(value->type == XR_XIR_I64);
        int written = snprintf(number, sizeof(number), "%lld", (long long) value->payload);
        CHECK(written > 0 && (size_t) written < sizeof(number));
        bytes = number; length = (size_t) written;
    }
    CHECK(log->length + length + 1 < sizeof(log->bytes));
    memcpy(log->bytes + log->length, bytes, length); log->length += length;
    log->bytes[log->length++] = '\n';
    return XR_XIR_OUTPUT_OK;
}
static void array_program_trace(void *context, XrXirLifecycleEvent event, uint32_t slot) {
    ArrayProgramLog *log = context;
    if (event == XR_XIR_SLOT_RELEASED) {
        CHECK(log->released < 3);
        log->releases[log->released++] = slot;
    }
}
static XrXirValue array_program_result(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
    return value;
}
static void array_program_string(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length == strlen(expected) && !memcmp(bytes,expected,length));
}
static void array_program_cases(XrXirProgram *program, bool fail_init) {
    XrXirInstance *instances[2] = {0};
    XrXirValue escaped[2] = {0};
    ArrayProgramLog logs[2] = {0};
    XrXirCallResult failures[2] = {0};
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, array_program_output, &logs[i]};
        config.trace = array_program_trace; config.trace_context = &logs[i];
        CHECK(xr_xir_instance_new(program,&config,&instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i],2,NULL,0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = xr_xir_instance_poll(instances[i]);
        if (fail_init) {
            CHECK(result.outcome.status == XR_XIR_CALL_BOUNDS && !logs[i].length);
            CHECK(result.outcome.panic.detail.code == 430 && result.outcome.panic.detail.index == -1 && result.outcome.panic.detail.length == 2);
            CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_FAILED);
            for (uint32_t repeat = 0; repeat < 3; ++repeat) {
                CHECK(xr_xir_instance_start(instances[i],2,NULL,0) == XR_XIR_CALL_BOUNDS);
                XrXirCallResult copy = {0};
                CHECK(xr_xir_instance_copy_failure(instances[i],&copy) == XR_XIR_CALL_BOUNDS);
                CHECK(!memcmp(&copy.panic.detail,&result.outcome.panic.detail,sizeof(copy.panic.detail)));
                failures[i] = copy;
            }
            CHECK(logs[i].released == 1 && logs[i].releases[0] == 0);
        } else {
            static const char expected[] = "red\nblue\n2\ngreen\nblue\ngreen\n3\n";
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.outcome.value.payload == 3);
            CHECK(logs[i].length == sizeof(expected)-1 && !memcmp(logs[i].bytes,expected,sizeof(expected)-1));
            for (uint32_t entry = 6; entry <= 7; ++entry) {
                XrXirValue changed = array_program_result(instances[i],entry);
                array_program_string(&changed,"green"); xr_xir_value_drop(&changed);
            }
            const int64_t bad[] = {INT64_MIN,-1,3,INT64_MAX};
            for (uint32_t index = 0; index < 4; ++index) {
                XrXirValue argument = {XR_XIR_I64,0,bad[index]};
                CHECK(xr_xir_instance_start(instances[i],5,&argument,1) == XR_XIR_CALL_READY);
                XrXirCallResult failed = xr_xir_instance_poll(instances[i]).outcome;
                CHECK(failed.status == XR_XIR_CALL_BOUNDS && failed.panic.detail.code == 430 &&
                    failed.panic.detail.index == bad[index] && failed.panic.detail.length == 3 &&
                    failed.value.type == XR_XIR_UNIT && !failed.value.payload && !failed.wake);
                CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_READY);
            }
            escaped[i] = array_program_result(instances[i],4);
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        if (!fail_init) CHECK(logs[i].released == 3 && logs[i].releases[0] == 2 &&
            logs[i].releases[1] == 1 && logs[i].releases[2] == 0);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (fail_init) CHECK(failures[i].panic.detail.index == -1 && failures[i].panic.detail.length == 2);
        else {
            XrXirValueAdmission admission = {xr_xir_value_arena(&escaped[i]),NULL,NULL,NULL,10000,65536};
            int64_t length = -1; XrXirValue element = {0}; XrXirFaultDetail fault = {0};
            CHECK(xr_xir_array_len(&escaped[i],&admission,&length) == XR_XIR_VALUE_OK && length == 3);
            CHECK(xr_xir_array_get(&escaped[i],2,&admission,&element,&fault) == XR_XIR_VALUE_OK);
            array_program_string(&element,"green");
            xr_xir_value_drop(&escaped[i]);
            array_program_string(&element,"green"); xr_xir_value_drop(&element);
        }
    }
}
#endif // XIR_ARRAY_PROGRAM_CASES_H
