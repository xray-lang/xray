/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_source_cases.h - Independent source cleanup and escaped-value witnesses
 */
#ifndef XIR_CLEANUP_SOURCE_CASES_H
#define XIR_CLEANUP_SOURCE_CASES_H
#include "xir/xxir_array.h"
enum { CLEANUP_SCOPES, CLEANUP_LOOPS, CLEANUP_ERRORS, CLEANUP_PANIC, CLEANUP_CANCELLED,
    CLEANUP_GENERICS, CLEANUP_SNAPSHOT, CLEANUP_CONSTRUCTOR, CLEANUP_CONSTRUCTOR_NESTED, CLEANUP_CONSTRUCTOR_BARE, CLEANUP_CONSTRUCTOR_UNCAPTURED, CLEANUP_MEMBER, CLEANUP_MEMBER_OPERATORS, CLEANUP_MEMBER_RESUME, CLEANUP_MEMBER_CANCEL, CLEANUP_MEMBER_FAILURE, CLEANUP_SOURCE_FUNCTIONS };
typedef struct CleanupExpected { XrXirType type; int64_t number; const char *text; } CleanupExpected;
typedef struct CleanupSourceLog { const CleanupExpected *values; uint32_t count, at; } CleanupSourceLog;
static void cleanup_source_string(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}
static XrXirOutputStatus cleanup_source_output(void *context, const XrXirOutputGroup *group) {
    CleanupSourceLog *log = context;
    CHECK(group->line && group->stream == XR_XIR_STDOUT && group->count == 1 && log->at < log->count);
    const CleanupExpected *expected = &log->values[log->at++];
    CHECK(group->values[0].type == (uint32_t)expected->type);
    if (expected->type == XR_XIR_STRING) cleanup_source_string(&group->values[0], expected->text);
    else CHECK(group->values[0].payload == expected->number);
    return XR_XIR_OUTPUT_OK;
}
static void cleanup_source_cases(XrXirProgram *program, const uint32_t *functions) {
    const CleanupExpected scopes[] = {{XR_XIR_STRING,0,"nested"},{XR_XIR_STRING,0,"inner"},
        {XR_XIR_I64,1,NULL},{XR_XIR_STRING,0,"after"},{XR_XIR_I64,2,NULL}};
    const CleanupExpected loops[] = {{XR_XIR_I64,1,NULL},{XR_XIR_I64,2,NULL}};
    const CleanupExpected errors[] = {{XR_XIR_STRING,0,"error cleanup"},{XR_XIR_STRING,0,"caught"}};
    const CleanupExpected panic[] = {{XR_XIR_STRING,0,"panic cleanup"},{XR_XIR_STRING,0,"handled"}};
    const CleanupExpected cancel[] = {{XR_XIR_STRING,0,"cancelled"}};
    const CleanupExpected generic[] = {{XR_XIR_STRING,0,"generic"}};
    const CleanupExpected array[] = {{XR_XIR_I64,7,NULL}};
    const CleanupExpected constructor[] = {{XR_XIR_I64,2,NULL}};
    const CleanupExpected constructed_nested[] = {{XR_XIR_STRING,0,"ctor"},{XR_XIR_STRING,0,"ctor"}};
    const CleanupExpected uncaptured[] = {{XR_XIR_I64,1,NULL},{XR_XIR_I64,1,NULL},{XR_XIR_STRING,0,"plain"}};
    const CleanupExpected member[] = {{XR_XIR_STRING,0,"rhs"},{XR_XIR_I64,5,NULL},{XR_XIR_I64,9,NULL},{XR_XIR_I64,3,NULL},{XR_XIR_I64,6,NULL},{XR_XIR_I64,12,NULL}};
    const CleanupExpected operators[] = {{XR_XIR_I64,18,NULL},{XR_XIR_I64,-128,NULL},{XR_XIR_BOOL,1,NULL},{XR_XIR_STRING,0,"abc"}};
    const CleanupExpected resumed[] = {{XR_XIR_I64,5,NULL}}, abandoned[] = {{XR_XIR_I64,100,NULL}}, failure[] = {{XR_XIR_I64,100,NULL},{XR_XIR_I64,100,NULL}};
    CleanupSourceLog logs[] = {{scopes,5,0},{loops,2,0},{errors,2,0},{panic,2,0},{cancel,1,0},{generic,1,0},{array,1,0},{constructor,1,0},{constructed_nested,2,0},{NULL,0,0},{uncaptured,3,0},{member,6,0},{operators,4,0},{resumed,1,0},{abandoned,1,0},{failure,2,0}};
    XrXirInstance *instances[CLEANUP_SOURCE_FUNCTIONS] = {0};
    XrXirValue results[CLEANUP_SOURCE_FUNCTIONS] = {{0}};
    for (unsigned i = 0; i < CLEANUP_SOURCE_FUNCTIONS; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, cleanup_source_output, &logs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (unsigned i = 0; i < CLEANUP_SOURCE_FUNCTIONS; ++i) {
        CHECK(xr_xir_instance_start(instances[i], functions[i], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult step = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX);
        XrXirCallResult outcome = step.outcome;
        if (i == CLEANUP_CANCELLED || i == CLEANUP_MEMBER_CANCEL) {
            CHECK(outcome.status == XR_XIR_CALL_SUSPENDED && !logs[i].at);
            CHECK(xr_xir_instance_stop(instances[i]) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
        } else {
            if (i == CLEANUP_MEMBER_RESUME) {
                CHECK(outcome.status == XR_XIR_CALL_SUSPENDED && !logs[i].at);
                CHECK(xr_xir_instance_resume(instances[i], step.epoch, outcome.wake) == XR_XIR_CALL_READY);
                outcome = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome;
            }
            if (outcome.status != XR_XIR_CALL_RETURNED) fprintf(stderr, "cleanup source case %u failed: %u\n", i, outcome.status);
            CHECK(outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        }
        CHECK(logs[i].at == logs[i].count);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    cleanup_source_string(&results[CLEANUP_SCOPES], "kept");
    cleanup_source_string(&results[CLEANUP_GENERICS], "g");
    CHECK(results[CLEANUP_CONSTRUCTOR].payload == 2);
    CHECK(results[CLEANUP_CONSTRUCTOR_BARE].payload == 9);
    CHECK(results[CLEANUP_CONSTRUCTOR_UNCAPTURED].payload == 6);
    CHECK(results[CLEANUP_MEMBER].payload == 10);
    CHECK(results[CLEANUP_MEMBER_RESUME].payload == 14 && results[CLEANUP_MEMBER_FAILURE].payload == 18);
    cleanup_source_string(&results[CLEANUP_MEMBER_OPERATORS], "ab");
    cleanup_source_string(&results[CLEANUP_CONSTRUCTOR_NESTED], "afterafter");
    CHECK(results[CLEANUP_LOOPS].payload == 2 && results[CLEANUP_ERRORS].payload == 7 && results[CLEANUP_PANIC].payload == 9);
    XrXirValueAdmission admission = {xr_xir_value_arena(&results[CLEANUP_SNAPSHOT]),NULL,NULL,NULL,10000,65536};
    XrXirValue element = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&results[CLEANUP_SNAPSHOT], 0, &admission, &element, &fault) == XR_XIR_VALUE_OK);
    CHECK(element.type == XR_XIR_I64 && element.payload == 1); xr_xir_value_drop(&element);
    for (unsigned i = 0; i < CLEANUP_SOURCE_FUNCTIONS; ++i) xr_xir_value_drop(&results[i]);
}
static XrXirOutputStatus cleanup_allocation_output(void *context, const XrXirOutputGroup *group) {
    (void)context;
    CHECK(group && group->count == 1);
    return XR_XIR_OUTPUT_OK;
}
static void cleanup_source_allocations(XrXirProgram *program, const uint32_t *functions) {
    size_t baseline = runtime_live, bytes = runtime_bytes, total = 0;
    for (unsigned f = 0; f < CLEANUP_SOURCE_FUNCTIONS; ++f) {
        size_t sites = 0;
        for (size_t pass = 0; pass <= sites; ++pass) {
            runtime_attempts = 0; runtime_fail_at = pass ? pass - 1 : SIZE_MAX;
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, cleanup_allocation_output, NULL};
            XrXirInstance *instance = NULL; XrXirValue value = {0};
            XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
            if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, functions[f], NULL, 0);
            XrXirInstanceResult step = {0};
            if (status == XR_XIR_CALL_READY) { step = xr_xir_instance_poll_bounded(instance, UINT64_MAX); status = step.outcome.status; }
            if (status == XR_XIR_CALL_SUSPENDED) {
                CHECK(f == CLEANUP_CANCELLED || f == CLEANUP_MEMBER_CANCEL || f == CLEANUP_MEMBER_RESUME);
                if (f == CLEANUP_MEMBER_RESUME) {
                    status = xr_xir_instance_resume(instance, step.epoch, step.outcome.wake);
                    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
                }
                else {
                    status = xr_xir_instance_stop(instance);
                    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
                }
            }
            if (status == XR_XIR_CALL_RETURNED) status = xr_xir_instance_take_result(instance, &value);
            if (!pass) {
                CHECK(status == ((f == CLEANUP_CANCELLED || f == CLEANUP_MEMBER_CANCEL) ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
                sites = runtime_attempts; total += sites;
            } else {
                if (status != XR_XIR_CALL_OOM) fprintf(stderr, "cleanup allocation function %u site %zu/%zu status %u\n", f, pass, sites, status);
                CHECK(runtime_attempts > runtime_fail_at && status == XR_XIR_CALL_OOM);
            }
            runtime_fail_at = SIZE_MAX;
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            xr_xir_value_drop(&value);
            CHECK(runtime_live == baseline && runtime_bytes == bytes);
        }
    }
    printf("Source cleanup physical release: %zu allocation failure sites\n", total);
}
static void cleanup_source_fatal(XrXirProgram *program, uint32_t function) {
    XrXirInstance *instance = NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
    if (result.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
    }
    fprintf(stderr, "cleanup escape incorrectly returned status %u\n", result.status);
    exit(1);
}
#endif // XIR_CLEANUP_SOURCE_CASES_H
