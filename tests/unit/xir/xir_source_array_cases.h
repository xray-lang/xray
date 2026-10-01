/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_array_cases.h - Independent source Array execution expectations
 *
 * KEY CONCEPT:
 *   Each executor must match logical values, evaluation order and physical release.
 */
#ifndef XIR_SOURCE_ARRAY_CASES_H
#define XIR_SOURCE_ARRAY_CASES_H
#include "xir/xxir_output.h"
#include "xir/xxir_array.h"
#include "xir/xxir_struct.h"
enum {
    ARRAY_ENTRY, ARRAY_RESULT, ARRAY_ORIGINAL, ARRAY_ADVANCE, ARRAY_REBOUND,
    ARRAY_SNAPSHOT, ARRAY_METHOD_SNAPSHOT, ARRAY_REBOUND_FAULT, ARRAY_CURRENT,
    ARRAY_ASSIGN_RESULT, ARRAY_LOCAL, ARRAY_CAPTURED, ARRAY_GENERIC, ARRAY_NESTED,
    ARRAY_IMPORTED_LENGTH, ARRAY_LOCAL_LENGTH, ARRAY_SELF_PUSH, ARRAY_SET_ORDER,
    ARRAY_ORDER_TRACE, ARRAY_SUSPENDED_SNAPSHOT, ARRAY_SUSPENDED_SET,
    ARRAY_AGGREGATE, ARRAY_AGGREGATE_NESTED, ARRAY_EMPTY_AGGREGATE, ARRAY_ENUM_AGGREGATE,
    ARRAY_AGGREGATE_RESULT, ARRAY_PATH, ARRAY_PATH_SET, ARRAY_PATH_PUSH, ARRAY_PATH_COMPOUND,
    ARRAY_PATH_SUSPENDED, ARRAY_PATH_WRITE_FAULT, ARRAY_PATH_FIELD_FAULT, ARRAY_PATH_COMPOUND_FAULT,
    ARRAY_PATH_STATE, ARRAY_PATH_CAUGHT, ARRAY_FUNCTION_COUNT
};
typedef struct SourceArrayOutput { unsigned count; } SourceArrayOutput;
static bool source_array_bytes(void *pointer, XrXirOutputStream stream, const char *bytes, size_t length) {
    static const char *const expected[] = {"red\n", "blue\n", "2\n", "green\n", "blue\n", "green\n", "3\n"};
    SourceArrayOutput *output = pointer;
    CHECK(stream == XR_XIR_STDOUT && output->count < sizeof(expected) / sizeof(expected[0]));
    const char *text = expected[output->count++];
    CHECK(length == strlen(text) && !memcmp(bytes, text, length));
    return true;
}
static void source_array_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}
static XrXirValue source_array_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
    if (outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "array function %u status %u fault %u\n", function, (unsigned) outcome.status, outcome.panic.detail.code);
    CHECK(outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static void source_array_self_push(XrXirInstance *instance, uint32_t function) {
    const size_t first = runtime_attempts;
    XrXirValue value = source_array_run(instance, function);
    const size_t allocations = runtime_attempts - first;
    /* Include the call frame, initializer and its first legal detach, but exclude
     * instance initialization and result inspection. Repeated receiver snapshots
     * would force at least one backing allocation per append. The loose bound
     * leaves room for different geometric growth policies and frame layouts. */
    printf("Source Array self-push: 1024 appends, %zu allocations/reallocations (bound <64)\n", allocations);
    CHECK(allocations < 64);
    XrXirValueAdmission admission = {xr_xir_value_arena(&value), NULL, NULL, NULL, 10000, 65536};
    int64_t length = 0;
    CHECK(xr_xir_array_len(&value, &admission, &length) == XR_XIR_VALUE_OK && length == 1025);
    const int64_t positions[] = {0, 1024};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(&value, positions[i], &admission, &element, &fault) == XR_XIR_VALUE_OK);
        source_array_text(&element, "seed"); xr_xir_value_drop(&element);
    }
    xr_xir_value_drop(&value);
}
static XrXirValue source_array_resume_one(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult paused = xr_xir_instance_poll(instance);
    CHECK(paused.outcome.status == XR_XIR_CALL_SUSPENDED && paused.outcome.wake && paused.epoch);
    CHECK(paused.outcome.value.type == XR_XIR_UNIT && !paused.outcome.value.payload);
    XrXirInstanceResult repeated = xr_xir_instance_poll(instance);
    CHECK(repeated.outcome.status == XR_XIR_CALL_SUSPENDED && repeated.epoch == paused.epoch &&
        repeated.outcome.wake == paused.outcome.wake);
    CHECK(xr_xir_instance_resume(instance, paused.epoch + 1, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
    CHECK(outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static void source_array_order_cases(XrXirInstance *instance, const uint32_t *functions) {
    XrXirValue value = source_array_run(instance, functions[ARRAY_SET_ORDER]);
    source_array_text(&value, "ordered"); xr_xir_value_drop(&value);
    value = source_array_run(instance, functions[ARRAY_ORDER_TRACE]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 12); xr_xir_value_drop(&value);
    const unsigned suspended[] = {ARRAY_SUSPENDED_SNAPSHOT, ARRAY_SUSPENDED_SET};
    const char *results[] = {"oldGet", "stored"};
    const char *roots[] = {"newGet", "newSet"};
    const int64_t traces[] = {12, 123};
    for (unsigned i = 0; i < sizeof(suspended) / sizeof(suspended[0]); ++i) {
        value = source_array_resume_one(instance, functions[suspended[i]]);
        source_array_text(&value, results[i]); xr_xir_value_drop(&value);
        value = source_array_run(instance, functions[ARRAY_CURRENT]);
        source_array_text(&value, roots[i]); xr_xir_value_drop(&value);
        value = source_array_run(instance, functions[ARRAY_ORDER_TRACE]);
        CHECK(value.type == XR_XIR_I64 && value.payload == traces[i]); xr_xir_value_drop(&value);
    }
}
static void source_array_result_cases(XrXirInstance *instance, const uint32_t *functions) {
    const unsigned strings[] = {ARRAY_ORIGINAL, ARRAY_REBOUND, ARRAY_SNAPSHOT, ARRAY_METHOD_SNAPSHOT,
        ARRAY_ASSIGN_RESULT, ARRAY_GENERIC, ARRAY_NESTED, ARRAY_IMPORTED_LENGTH, ARRAY_LOCAL_LENGTH};
    const char *expected[] = {"red", "changed", "before", "before", "result", "generic", "nested", "shadow!", "shadow?"};
    for (unsigned i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
        XrXirValue value = source_array_run(instance, functions[strings[i]]);
        source_array_text(&value, expected[i]); xr_xir_value_drop(&value);
    }
    XrXirValue value = source_array_run(instance, functions[ARRAY_LOCAL]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 17); xr_xir_value_drop(&value);
    value = source_array_run(instance, functions[ARRAY_CAPTURED]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 2); xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_start(instance, functions[ARRAY_REBOUND_FAULT], NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
    CHECK(outcome.status == XR_XIR_CALL_BOUNDS && outcome.panic.detail.code == 430 &&
        outcome.panic.detail.index == 1 && outcome.panic.detail.length == 1);
    value = source_array_run(instance, functions[ARRAY_CURRENT]);
    source_array_text(&value, "only"); xr_xir_value_drop(&value);
    source_array_self_push(instance, functions[ARRAY_SELF_PUSH]);
    source_array_order_cases(instance, functions);
    value = source_array_run(instance, functions[ARRAY_AGGREGATE]);
    source_array_text(&value, "before:after:before:local"); xr_xir_value_drop(&value);
    value = source_array_run(instance, functions[ARRAY_AGGREGATE_NESTED]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 17); xr_xir_value_drop(&value);
    value = source_array_run(instance, functions[ARRAY_EMPTY_AGGREGATE]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 9); xr_xir_value_drop(&value);
    value = source_array_run(instance, functions[ARRAY_ENUM_AGGREGATE]);
    source_array_text(&value, "enum"); xr_xir_value_drop(&value);
    const unsigned paths[] = {ARRAY_PATH,ARRAY_PATH_SET,ARRAY_PATH_PUSH};
    const int64_t path_expected[] = {123110,123110,122791};
    for (unsigned i = 0; i < 3; ++i) {
        value = source_array_run(instance,functions[paths[i]]);
        CHECK(value.type == XR_XIR_I64 && value.payload == path_expected[i]); xr_xir_value_drop(&value);
    }
    value = source_array_run(instance,functions[ARRAY_PATH_COMPOUND]);
    source_array_text(&value,"oldx:old"); xr_xir_value_drop(&value);
    value = source_array_run(instance,functions[ARRAY_ORDER_TRACE]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 12); xr_xir_value_drop(&value);
    value = source_array_resume_one(instance,functions[ARRAY_PATH_SUSPENDED]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 123110); xr_xir_value_drop(&value);
    const unsigned faults[] = {ARRAY_PATH_WRITE_FAULT,ARRAY_PATH_FIELD_FAULT,ARRAY_PATH_COMPOUND_FAULT};
    value = source_array_run(instance,functions[ARRAY_PATH_CAUGHT]);
    CHECK(value.type == XR_XIR_I64 && value.payload == 4301234); xr_xir_value_drop(&value);
    const int64_t states[] = {123000,12001,1001};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(xr_xir_instance_start(instance,functions[faults[i]],NULL,0) == XR_XIR_CALL_READY);
        XrXirCallResult failed = xr_xir_instance_poll(instance).outcome;
        CHECK(failed.status == XR_XIR_CALL_BOUNDS && failed.panic.detail.code == 430);
        CHECK(failed.panic.detail.index == (i ? 1 : 0) && failed.panic.detail.length == (i ? 1 : 0));
        CHECK(failed.value.type == XR_XIR_UNIT && !failed.value.payload && !failed.wake);
        value = source_array_run(instance,functions[ARRAY_PATH_STATE]);
        CHECK(value.type == XR_XIR_I64 && value.payload == states[i]); xr_xir_value_drop(&value);
    }
}
typedef struct SourceArrayFaultOutput {
    XrXirOutputSink sink;
    bool failed_allocation;
} SourceArrayFaultOutput;
static bool source_array_fault_render(void *pointer, const XrXirOutputGroup *group) {
    SourceArrayFaultOutput *output = pointer;
    SourceArrayOutput *bytes = output->sink.context;
    size_t first = runtime_attempts;
    unsigned groups = bytes->count;
    bool accepted = xr_xir_output_render(&output->sink, group);
    bool failed_here = runtime_fail_at >= first && runtime_fail_at < runtime_attempts;
    if (failed_here) {
        CHECK(!accepted && bytes->count == groups);
        output->failed_allocation = true;
    }
    return accepted;
}
static void source_array_runtime_failures(XrXirProgram *program, uint32_t entry) {
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site ? site - 1 : SIZE_MAX;
        SourceArrayOutput output = {0};
        SourceArrayFaultOutput rendering = {{source_array_bytes, &output, 4096}, false};
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {source_array_fault_render, &rendering};
        XrXirInstance *instance = NULL;
        const char *phase = "new";
        XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
        if (status == XR_XIR_CALL_READY) { phase = "start"; status = xr_xir_instance_start(instance, entry, NULL, 0); }
        if (status == XR_XIR_CALL_READY) { phase = "poll"; status = xr_xir_instance_poll(instance).outcome.status; }
        if (!site) { CHECK(status == XR_XIR_CALL_RETURNED && output.count == 7); sites = runtime_attempts; }
        else {
            XrXirCallStatus expected = rendering.failed_allocation ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_OOM;
            if (status != expected) fprintf(stderr,
                "Array runtime allocation site=%zu/%zu fail_at=%zu attempts=%zu phase=%s status=%u outputs=%u live=%zu/%zu\n",
                site, sites, runtime_fail_at, runtime_attempts, phase, (unsigned) status, output.count, runtime_live, baseline_live);
            CHECK(runtime_attempts > runtime_fail_at && status == expected);
        }
        runtime_fail_at = SIZE_MAX;
        if (instance) CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    }
    printf("Source Array runtime: %zu allocation failure sites leave no live instance ownership\n", sites);
}
static void source_array_sticky_bounds(XrXirProgram *program, uint32_t entry) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
    CHECK(outcome.status == XR_XIR_CALL_BOUNDS && outcome.panic.detail.code == 430 &&
        outcome.panic.detail.index == -1 && outcome.panic.detail.length == 1);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirCallResult failure = {0};
        CHECK(xr_xir_instance_copy_failure(instance, &failure) == XR_XIR_CALL_BOUNDS);
        CHECK(failure.status == XR_XIR_CALL_BOUNDS && failure.panic.detail.code == 430 &&
            failure.panic.detail.index == -1 && failure.panic.detail.length == 1 && !failure.panic.detail.reserved);
        CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_BOUNDS);
        outcome = xr_xir_instance_poll(instance).outcome;
        CHECK(outcome.status == XR_XIR_CALL_BOUNDS && outcome.panic.detail.index == -1 && outcome.panic.detail.length == 1);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
}
static void source_array_program_cases(XrXirProgram *program, const uint32_t *functions) {
    source_array_runtime_failures(program, functions[ARRAY_ENTRY]);
    const unsigned aggregates[] = {ARRAY_AGGREGATE, ARRAY_AGGREGATE_NESTED, ARRAY_EMPTY_AGGREGATE,
        ARRAY_ENUM_AGGREGATE, ARRAY_AGGREGATE_RESULT, ARRAY_PATH, ARRAY_PATH_SET, ARRAY_PATH_PUSH, ARRAY_PATH_COMPOUND,
        ARRAY_PATH_CAUGHT};
    for (unsigned i = 0; i < sizeof(aggregates) / sizeof(aggregates[0]); ++i)
        source_array_runtime_failures(program, functions[aggregates[i]]);
    const unsigned cancelled[] = {ARRAY_SUSPENDED_SNAPSHOT, ARRAY_SUSPENDED_SET, ARRAY_PATH_SUSPENDED};
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    for (unsigned i = 0; i < sizeof(cancelled) / sizeof(cancelled[0]); ++i) {
        SourceArrayOutput output = {0}; XrXirOutputSink sink = {source_array_bytes, &output, 4096};
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {xr_xir_output_render, &sink};
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        XrXirValue entry = source_array_run(instance, functions[ARRAY_ENTRY]);
        CHECK(entry.type == XR_XIR_I64 && entry.payload == 0 && output.count == 7);
        xr_xir_value_drop(&entry);
        CHECK(xr_xir_instance_start(instance, functions[cancelled[i]], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult paused = xr_xir_instance_poll(instance);
        CHECK(paused.outcome.status == XR_XIR_CALL_SUSPENDED && paused.epoch && paused.outcome.wake);
        CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    }
    SourceArrayOutput output[2] = {{0}, {0}};
    XrXirOutputSink sinks[2] = {{source_array_bytes, &output[0], 4096}, {source_array_bytes, &output[1], 4096}};
    XrXirInstance *instances[2] = {0};
    XrXirValue retained[2] = {{0}, {0}};
    XrXirValue aggregate_retained[2] = {{0}, {0}};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {xr_xir_output_render, &sinks[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        XrXirValue entry = source_array_run(instances[i], functions[ARRAY_ENTRY]);
        CHECK(entry.type == XR_XIR_I64 && entry.payload == 0 && output[i].count == 7);
        xr_xir_value_drop(&entry);
        retained[i] = source_array_run(instances[i], functions[ARRAY_RESULT]);
        aggregate_retained[i] = source_array_run(instances[i], functions[ARRAY_AGGREGATE_RESULT]);
    }
    for (int64_t want = 4; want <= 5; ++want) for (unsigned i = 0; i < 2; ++i) {
        XrXirValue length = source_array_run(instances[i], functions[ARRAY_ADVANCE]);
        CHECK(length.type == XR_XIR_I64 && length.payload == want); xr_xir_value_drop(&length);
    }
    for (unsigned i = 0; i < 2; ++i) {
        source_array_result_cases(instances[i], functions);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirDomain *receiver = NULL;
        CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
        XrXirValueAdmission receiving = {xr_xir_value_arena(&aggregate_retained[i]), receiver, NULL, NULL, 10000, 65536};
        XrXirValue item = {0}, text = {0}; XrXirFaultDetail aggregate_fault = {0};
        CHECK(xr_xir_array_get(&aggregate_retained[i], 0, &receiving, &item, &aggregate_fault) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&aggregate_retained[i]);
        CHECK(xr_xir_struct_get(&item, 0, &receiving, &text) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&item); xr_xir_domain_drop(receiver);
        source_array_text(&text, "escaped"); xr_xir_value_drop(&text);
        int64_t length = 0;
        XrXirValueAdmission admission = {xr_xir_value_arena(&retained[i]), NULL, NULL, NULL, 10000, 65536};
        CHECK(xr_xir_array_len(&retained[i], &admission, &length) == XR_XIR_VALUE_OK);
        CHECK(length == 3);
        const char *texts[] = {"green", "blue", "green"};
        for (int64_t at = 0; at < length; ++at) {
            XrXirValue value = {0}; XrXirFaultDetail fault = {0};
            CHECK(xr_xir_array_get(&retained[i], at, &admission, &value, &fault) == XR_XIR_VALUE_OK);
            source_array_text(&value, texts[at]); xr_xir_value_drop(&value);
        }
        xr_xir_value_drop(&retained[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_SOURCE_ARRAY_CASES_H
