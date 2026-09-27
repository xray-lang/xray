/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_float_cases.h - Independent source Float execution expectations
 *
 * KEY CONCEPT:
 *   Each executor must match logical values, evaluation order and physical release.
 */
#ifndef XIR_SOURCE_FLOAT_CASES_H
#define XIR_SOURCE_FLOAT_CASES_H
#include "xir/xxir_output.h"

enum { FLOAT_ENTRY, FLOAT_RESULT, FLOAT_ADVANCE, FLOAT_CURRENT, FLOAT_CAPTURED,
    FLOAT_SUSPENDED, FLOAT_FUNCTION_COUNT };
typedef struct SourceFloatOutput { unsigned count; bool reject; } SourceFloatOutput;
static bool source_float_bytes(void *pointer, XrXirOutputStream stream, const char *bytes, size_t length) {
    static const char *const expected[] = {"3.5 1.5\n", "4.0 1.75\n", "inf -inf nan -0.0\n"};
    SourceFloatOutput *output = pointer;
    CHECK(stream == XR_XIR_STDOUT && output->count < sizeof(expected) / sizeof(expected[0]));
    const char *text = expected[output->count++];
    CHECK(length == strlen(text) && !memcmp(bytes, text, length));
    return !output->reject;
}
static void source_float_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}
static XrXirValue source_float_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult outcome = xr_xir_instance_poll(instance).outcome;
    if (outcome.status != XR_XIR_CALL_RETURNED)
        fprintf(stderr, "float function %u status %u fault %u\n", function, (unsigned) outcome.status, outcome.fault.code);
    CHECK(outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static XrXirValue source_float_resume_one(XrXirInstance *instance, uint32_t function) {
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
typedef struct SourceFloatFaultOutput {
    XrXirOutputSink sink;
    bool failed_allocation;
} SourceFloatFaultOutput;
static bool source_float_fault_render(void *pointer, const XrXirOutputGroup *group) {
    SourceFloatFaultOutput *output = pointer;
    SourceFloatOutput *bytes = output->sink.context;
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
static void source_float_runtime_failures(XrXirProgram *program, uint32_t entry) {
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site ? site - 1 : SIZE_MAX;
        SourceFloatOutput output = {0};
        SourceFloatFaultOutput rendering = {{source_float_bytes, &output, 4096}, false};
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {source_float_fault_render, &rendering};
        XrXirInstance *instance = NULL;
        const char *phase = "new";
        XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
        if (status == XR_XIR_CALL_READY) { phase = "start"; status = xr_xir_instance_start(instance, entry, NULL, 0); }
        if (status == XR_XIR_CALL_READY) { phase = "poll"; status = xr_xir_instance_poll(instance).outcome.status; }
        if (!site) { CHECK(status == XR_XIR_CALL_RETURNED && output.count == 3); sites = runtime_attempts; }
        else {
            XrXirCallStatus expected = rendering.failed_allocation ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_OOM;
            if (status != expected) fprintf(stderr,
                "Float runtime allocation site=%zu/%zu fail_at=%zu attempts=%zu phase=%s status=%u outputs=%u live=%zu/%zu\n",
                site, sites, runtime_fail_at, runtime_attempts, phase, (unsigned) status, output.count, runtime_live, baseline_live);
            CHECK(runtime_attempts > runtime_fail_at && status == expected);
        }
        runtime_fail_at = SIZE_MAX;
        if (instance) CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    }
    printf("Source Float runtime: %zu allocation failure sites leave no live instance ownership\n", sites);
}
static void source_float_bits(XrXirValue value, uint32_t type, uint64_t bits) {
    CHECK(value.type == type && (uint64_t)value.payload == bits);
    xr_xir_value_drop(&value);
}
static void source_float_output_failures(XrXirProgram *program, uint32_t entry) {
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    for (unsigned mode = 0; mode < 2; ++mode) {
        SourceFloatOutput output = {0, mode != 0};
        XrXirOutputSink sink = {source_float_bytes, &output, mode ? 4096 : 1};
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {xr_xir_output_render, &sink};
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
        CHECK(output.count == mode);
        for (unsigned again = 0; again < 2; ++again) {
            XrXirCallResult failure = {0};
            CHECK(xr_xir_instance_copy_failure(instance, &failure) == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(failure.status == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(output.count == mode);
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    }
}
static void source_float_call_failures(XrXirProgram *program, const uint32_t *functions) {
    const unsigned calls[] = {FLOAT_RESULT, FLOAT_CAPTURED, FLOAT_SUSPENDED};
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    for (unsigned f = 0; f < sizeof(calls) / sizeof(calls[0]); ++f) {
        size_t sites = 0;
        for (size_t site = 0; site <= sites; ++site) {
            SourceFloatOutput output = {0};
            XrXirOutputSink sink = {source_float_bytes, &output, 4096};
            XrXirInstanceConfig config = xr_xir_instance_defaults();
            config.output = (XrXirOutputProvider) {xr_xir_output_render, &sink};
            XrXirInstance *instance = NULL;
            CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
            XrXirValue value = source_float_run(instance, functions[FLOAT_ENTRY]);
            xr_xir_value_drop(&value);
            CHECK(output.count == 3);
            runtime_attempts = 0; runtime_fail_at = site ? site - 1 : SIZE_MAX;
            XrXirCallStatus status = xr_xir_instance_start(instance, functions[calls[f]], NULL, 0);
            if (status == XR_XIR_CALL_READY) {
                XrXirInstanceResult outcome = xr_xir_instance_poll(instance);
                if (outcome.outcome.status == XR_XIR_CALL_SUSPENDED) {
                    CHECK(calls[f] == FLOAT_SUSPENDED);
                    CHECK(xr_xir_instance_resume(instance, outcome.epoch, outcome.outcome.wake) == XR_XIR_CALL_READY);
                    outcome = xr_xir_instance_poll(instance);
                }
                status = outcome.outcome.status;
            }
            if (!site) {
                CHECK(status == XR_XIR_CALL_RETURNED);
                CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
                sites = runtime_attempts;
                if (calls[f] == FLOAT_RESULT) { source_float_text(&value, "float result"); xr_xir_value_drop(&value); }
                else source_float_bits(value, XR_XIR_F32, calls[f] == FLOAT_CAPTURED ? UINT64_C(0x3fa00000) : UINT64_C(0x40600000));
            } else {
                if (status != XR_XIR_CALL_OOM)
                    fprintf(stderr, "float call %u site %zu/%zu status %u attempts %zu\n", calls[f], site, sites, status, runtime_attempts);
                CHECK(runtime_attempts > runtime_fail_at && status == XR_XIR_CALL_OOM);
            }
            runtime_fail_at = SIZE_MAX;
            CHECK(output.count == 3);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
        }
        CHECK(sites > 0);
        printf("Float call %u: %zu allocation failures cleaned up\n", calls[f], sites);
    }
}

static void source_float_program_cases(XrXirProgram *program, const uint32_t *functions) {
    source_float_call_failures(program, functions);
    source_float_runtime_failures(program, functions[FLOAT_ENTRY]);
    source_float_output_failures(program, functions[FLOAT_ENTRY]);
    const size_t baseline_live = runtime_live, baseline_bytes = runtime_bytes;
    SourceFloatOutput output[2] = {{0}, {0}};
    XrXirOutputSink sinks[2] = {{source_float_bytes, &output[0], 4096}, {source_float_bytes, &output[1], 4096}};
    XrXirInstance *instances[2] = {0};
    XrXirValue retained[2] = {{0}, {0}};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = xr_xir_instance_defaults();
        config.output = (XrXirOutputProvider) {xr_xir_output_render, &sinks[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        source_float_bits(source_float_run(instances[i], functions[FLOAT_ENTRY]), XR_XIR_I64, 0);
        CHECK(output[i].count == 3);
        retained[i] = source_float_run(instances[i], functions[FLOAT_RESULT]);
    }
    source_float_bits(source_float_run(instances[0], functions[FLOAT_ADVANCE]), XR_XIR_F32, UINT64_C(0x40800000));
    source_float_bits(source_float_run(instances[0], functions[FLOAT_CURRENT]), XR_XIR_F64, UINT64_C(0x4012000000000000));
    source_float_bits(source_float_run(instances[1], functions[FLOAT_CURRENT]), XR_XIR_F64, UINT64_C(0x4010000000000000));
    for (unsigned i = 0; i < 2; ++i) {
        source_float_bits(source_float_run(instances[i], functions[FLOAT_CAPTURED]), XR_XIR_F32, UINT64_C(0x3fa00000));
        source_float_bits(source_float_resume_one(instances[i], functions[FLOAT_SUSPENDED]), XR_XIR_F32, UINT64_C(0x40600000));
        CHECK(xr_xir_instance_start(instances[i], functions[FLOAT_SUSPENDED], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult paused = xr_xir_instance_poll(instances[i]);
        CHECK(paused.outcome.status == XR_XIR_CALL_SUSPENDED && paused.epoch && paused.outcome.wake);
        CHECK(xr_xir_instance_stop(instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_resume(instances[i], paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(runtime_live > baseline_live && runtime_bytes > baseline_bytes);
    xr_xir_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        source_float_text(&retained[i], "float result");
        xr_xir_value_drop(&retained[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_SOURCE_FLOAT_CASES_H
