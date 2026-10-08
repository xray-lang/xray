/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_element_place.inc.c - Exact element bounds after owner death
 *
 * KEY CONCEPT:
 *   Failed stores preserve bounds facts and permit a subsequent independent call.
 */
static void consumer_element_place_normal(Consumer *run) {
    const int64_t indices[6] = {0, 1, -1, INT64_MAX, INT64_MIN, 0};
    XrXirInstance *instances[2] = {0}; ConsumerOutput outputs[2] = {0}; XrXirOutputSink sinks[2] = {0};
    XrXirCallResult faults[5][2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = consumer_config(&outputs[i], &sinks[i]);
        config.metadata_limit = config.call_limit = UINT64_C(1048576); config.depth_limit = 64;
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
    for (unsigned c = 0; c < 7; ++c) for (unsigned i = 0; i < 2; ++i) {
        unsigned which = c == 6 ? 0 : c;
        CHECK(xr_xir_instance_start(instances[i], run->array_entries[which], NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result; unsigned polls = 0;
        do { CHECK(++polls < 4096); result = xr_xir_instance_poll_bounded(instances[i], UINT64_C(1000000)); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        XrXirCallStatus expected = which ? XR_XIR_CALL_BOUNDS : XR_XIR_CALL_RETURNED;
        CHECK(result.outcome.status == expected && !result.outcome.wake);
        if (!which) {
            CHECK(xr_xir_panic_empty(&result.outcome.panic)); XrXirValue value = {0};
            CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
            CHECK(value.type == XR_XIR_I64 && !value.reserved && value.payload == 42); xr_xir_value_drop(&value);
        } else {
            const XrXirPanicPayload *panic = &result.outcome.panic;
            CHECK(xr_xir_panic_valid(panic) && xr_xir_fault_bounds_valid(panic->detail));
            CHECK(panic->detail.code == 430 && panic->detail.index == indices[which] && panic->detail.length == (which == 5 ? 0 : 1));
            XrXirValue empty = {0}, sentinel = {XR_XIR_I64, 0, 91}, occupied = sentinel; size_t before = runtime_attempts;
            CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.reserved && !empty.payload);
            CHECK(xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(!memcmp(&occupied, &sentinel, sizeof(occupied)) && runtime_attempts == before);
            CHECK(xr_xir_call_result_copy(&result.outcome, &faults[which - 1][i]) == XR_XIR_VALUE_OK);
        }
        consumer_output_complete(&outputs[i]);
        printf("element-place-call instance=%u case=%u index=%lld length=%u required=%u actual=%u code=%u value=%u result=PASS\n",
            i, c, (long long)indices[which], which == 5 ? 0u : 1u, expected, result.outcome.status,
            result.outcome.panic.detail.code, which ? 0u : 42u);
    }
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(stats(run).live_bytes == run->baseline.live_bytes);
    for (unsigned c = 1; c < 6; ++c) for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_call_result_valid(&faults[c - 1][i]) && faults[c - 1][i].status == XR_XIR_CALL_BOUNDS);
        CHECK(xr_xir_fault_bounds_valid(faults[c - 1][i].panic.detail) && faults[c - 1][i].panic.detail.index == indices[c]);
        CHECK(faults[c - 1][i].panic.detail.code == 430 && faults[c - 1][i].panic.detail.length == (c == 5 ? 0 : 1));
        xr_xir_call_result_drop(&faults[c - 1][i]); CHECK(xr_xir_call_result_empty(&faults[c - 1][i]));
    }
    CHECK(!runtime_live && !runtime_bytes && stats(run).live_bytes == run->baseline.live_bytes);
    puts("element-place-execution calls=14 instances=2 source-dead=1 caller-program-dead=1 instances-dead=1 retained-bounds=10 recovery=42 runtime-physical=0/0");
}
