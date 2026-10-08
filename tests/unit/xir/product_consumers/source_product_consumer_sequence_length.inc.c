/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_sequence_length.inc.c - Unicode String results after Instance death
 *
 * KEY CONCEPT:
 *   Six scalars remain readable after source, executor and original value death.
 */
static void consumer_sequence_string(const XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0; int64_t scalars = -1;
    CHECK(value->type == XR_XIR_STRING && !value->reserved && xr_xir_value_valid(value));
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == sizeof(sequence_bytes) && !memcmp(bytes, sequence_bytes, sizeof(sequence_bytes)));
    CHECK(xr_xir_string_length(value, &scalars) == XR_XIR_VALUE_OK && scalars == 6);
}
static void consumer_sequence_normal(Consumer *run) {
    XrXirInstance *instances[2] = {0}; ConsumerOutput outputs[2] = {0}; XrXirOutputSink sinks[2] = {0};
    XrXirValue retained[2][2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = consumer_config(&outputs[i], &sinks[i]);
        CHECK(xr_xir_instance_new(run->program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        size_t before = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], run->private_answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == before);
        XrXirValue entry = execute(instances[i], run->entry, 0);
        CHECK(entry.type == XR_XIR_I64 && !entry.payload); xr_xir_value_drop(&entry);
    }
    for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        XrXirValue length = execute(instances[i], run->answer, 0);
        CHECK(length.type == XR_XIR_I64 && (int64_t)length.payload == 6); xr_xir_value_drop(&length);
        CHECK(xr_xir_instance_start(instances[i], run->parameterized_answer, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result; unsigned polls = 0;
        do { CHECK(++polls < 4096); result = xr_xir_instance_poll_bounded(instances[i], UINT64_C(1000000)); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && !result.outcome.wake && xr_xir_panic_empty(&result.outcome.panic));
        XrXirValue sentinel = {XR_XIR_I64, 0, 91}, output = sentinel; size_t before = runtime_attempts;
        CHECK(xr_xir_instance_take_result(instances[i], &output) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&output, &sentinel, sizeof(output)) && runtime_attempts == before);
        CHECK(xr_xir_instance_take_result(instances[i], &retained[repeat][i]) == XR_XIR_CALL_RETURNED);
        consumer_sequence_string(&retained[repeat][i]); consumer_output_complete(&outputs[i]);
        printf("sequence-call instance=%u repeat=%u length=6 bytes=13 string-result=1 occupied-preserved=1 result=PASS\n", i, repeat);
    }
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes && stats(run).live_bytes == run->baseline.live_bytes);
    for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        consumer_sequence_string(&retained[repeat][i]);
        XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&retained[repeat][i], &copy) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&retained[repeat][i]);
        CHECK(!retained[repeat][i].type && !retained[repeat][i].reserved && !retained[repeat][i].payload);
        consumer_sequence_string(&copy); xr_xir_value_drop(&copy); CHECK(!copy.type && !copy.reserved && !copy.payload);
    }
    CHECK(!runtime_live && !runtime_bytes && stats(run).live_bytes == run->baseline.live_bytes);
    puts("sequence-execution calls=8 instances=2 scalar-results=4 owned-strings=4 copied-strings=4 bytes=13 scalars=6 source-dead=1 caller-program-dead=1 instances-dead=1 compiler-arena-refund=1 runtime-physical=0/0");
}
