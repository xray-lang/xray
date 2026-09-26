/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_cases.h - Independent expectations for a real source closure
 *
 * KEY CONCEPT:
 *   Each instance initializes library state once and results outlive instances.
 */
#ifndef XIR_SOURCE_CASES_H
#define XIR_SOURCE_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
typedef struct SourceOutput { uint32_t calls; bool reject_write; } SourceOutput;
static bool source_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    SourceOutput *output = context;
    CHECK(stream == (output->calls == 6 ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(output->calls < 10);
    if (!output->calls) CHECK(length == 8 && !memcmp(bytes, "a\xE4\xB8\xAD 20\n", 8));
    else if (output->calls == 1) CHECK(length == 3 && !memcmp(bytes, "10\n", 3));
    else if (output->calls == 2) CHECK(length == 6 && !memcmp(bytes, "11 12\n", 6));
    else if (output->calls == 3) CHECK(length == 5 && !memcmp(bytes, "true\n", 5));
    else if (output->calls == 4) CHECK(length == 11 && !memcmp(bytes, "13 14 ax y\n", 11));
    else if (output->calls == 5) CHECK(length == 16 && !memcmp(bytes, "aaxy true 42 az\n", 16));
    else if (output->calls == 6) CHECK(length == 4 && !memcmp(bytes, "aerr", 4));
    else if (output->calls == 8) CHECK(length == 4 && !memcmp(bytes, "aout", 4));
    else if (output->calls == 7 && output->reject_write) CHECK(length == 6 && !memcmp(bytes, "false\n", 6));
    else CHECK(length == 5 && !memcmp(bytes, "true\n", 5));
    ++output->calls;
    return !(output->reject_write && output->calls == 7);
}
static bool source_reject(void *context, const XrXirOutputGroup *group) {
    uint32_t *calls = context;
    CHECK(group && group->line && group->stream == XR_XIR_STDOUT && group->count == 2);
    ++*calls; return false;
}
static void source_failed_init(XrXirProgram *program, uint32_t entry, XrXirInstanceConfig config) {
    uint32_t calls = 0;
    config.output = (XrXirOutputProvider) {source_reject, &calls};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED && calls == 1);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_OUTPUT_ERROR && calls == 1);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
static void source_pair(XrXirProgram *program, uint32_t entry, uint32_t result_function,
                        uint32_t advance, XrXirValue *results) {
    SourceOutput outputs[2] = {{0, false}, {0, true}};
    XrXirOutputSink sinks[2] = {{source_bytes, &outputs[0], 65536}, {source_bytes, &outputs[1], 65536}};
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.metadata_limit = 65536; config.value_limit = 65536; config.call_limit = 65536;
    config.poll_limit = 1000; config.depth_limit = 16;
    XrXirInstance *instances[2] = {NULL, NULL};
    for (uint32_t i = 0; i < 2; ++i) {
        config.output = (XrXirOutputProvider) {xr_xir_output_render, &sinks[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instances[i]).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == 0);
        CHECK(outputs[i].calls == 10);
    }
    source_failed_init(program, entry, config);
    for (int64_t expected = 15; expected < 17; ++expected) for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], advance, NULL, 0) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_instance_poll(instances[i]).outcome;
        CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == expected && outputs[i].calls == 10);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], result_function, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instances[i]).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[i], &results[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
}
static void source_result_drop(XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == 2 && !memcmp(bytes, "a!", 2));
    xr_xir_value_drop(value);
}
#endif // XIR_SOURCE_CASES_H
