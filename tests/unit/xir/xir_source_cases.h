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
typedef struct SourceOutput { uint32_t calls; } SourceOutput;
static bool source_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    SourceOutput *output = context;
    CHECK(stream == XR_XIR_STDOUT);
    CHECK(output->calls < 4);
    if (!output->calls) CHECK(length == 8 && !memcmp(bytes, "a\xE4\xB8\xAD 20\n", 8));
    else if (output->calls == 1) CHECK(length == 3 && !memcmp(bytes, "10\n", 3));
    else if (output->calls == 2) CHECK(length == 6 && !memcmp(bytes, "11 12\n", 6));
    else CHECK(length == 5 && !memcmp(bytes, "true\n", 5));
    ++output->calls;
    return true;
}
static XrXirValue source_run(XrXirProgram *program, uint32_t entry, uint32_t result_function, uint32_t advance) {
    SourceOutput output = {0};
    XrXirOutputSink sink = {source_bytes, &output, 65536};
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.metadata_limit = 65536;
    config.value_limit = 65536;
    config.call_limit = 65536;
    config.poll_limit = 1000;
    config.depth_limit = 16;
    config.output = (XrXirOutputProvider) {xr_xir_output_render, &sink};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallResult polled = xr_xir_instance_poll(instance).outcome;
    CHECK(polled.status == XR_XIR_CALL_RETURNED && polled.value.type == XR_XIR_I64 && polled.value.payload == 0);
    CHECK(output.calls == 4);
    for (int64_t expected = 13; expected < 15; ++expected) {
        CHECK(xr_xir_instance_start(instance, advance, NULL, 0) == XR_XIR_CALL_READY);
        polled = xr_xir_instance_poll(instance).outcome;
        CHECK(polled.status == XR_XIR_CALL_RETURNED && polled.value.payload == expected && output.calls == 4);
    }
    CHECK(xr_xir_instance_start(instance, result_function, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    return result;
}
static void source_result_drop(XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == 2 && !memcmp(bytes, "a!", 2));
    xr_xir_value_drop(value);
}
#endif // XIR_SOURCE_CASES_H
