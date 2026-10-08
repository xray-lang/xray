/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_array_default.inc.c - Parameterized Array default execution
 *
 * KEY CONCEPT:
 *   Independent length and fault oracles survive source and caller Program death.
 *   Both Instances recover in the original order and owned panic values outlive them.
 */
static void consumer_array_default_normal(Consumer *run) {
    const int64_t counts[6] = {3, 0, -1, INT64_MIN, INT64_MAX, 4};
    const XrXirCallStatus expected[6] = {XR_XIR_CALL_RETURNED, XR_XIR_CALL_RETURNED,
        XR_XIR_CALL_NUMERIC_RANGE, XR_XIR_CALL_NUMERIC_RANGE, XR_XIR_CALL_LIMIT, XR_XIR_CALL_RETURNED};
    XrXirInstance *instances[2] = {0}; ConsumerOutput outputs[2] = {0}; XrXirOutputSink sinks[2] = {0};
    XrXirValue retained[6][2] = {0}; XrXirCallResult faults[6][2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = consumer_config(&outputs[i], &sinks[i]);
        config.metadata_limit = config.call_limit = UINT64_C(1048576);
        config.depth_limit = 64;
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
    for (unsigned c = 0; c < 6; ++c) for (unsigned i = 0; i < 2; ++i) {
        XrXirValue arg = {XR_XIR_I64, 0, counts[c]};
        CHECK(xr_xir_instance_start(instances[i], run->parameterized_answer, &arg, 1) == XR_XIR_CALL_READY);
        XrXirInstanceResult result; unsigned polls = 0;
        do {
            CHECK(++polls < 4096);
            result = xr_xir_instance_poll_bounded(instances[i], UINT64_C(1000000));
        } while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == expected[c] && !result.outcome.wake);
        if (counts[c] >= 0 && counts[c] != INT64_MAX) {
            CHECK(xr_xir_panic_empty(&result.outcome.panic));
            CHECK(xr_xir_instance_take_result(instances[i], &retained[c][i]) == XR_XIR_CALL_RETURNED);
            CHECK(retained[c][i].type == XR_XIR_I64 && retained[c][i].payload == counts[c]);
        } else {
            XrXirValue empty = {0}, sentinel = {XR_XIR_I64, 0, 91}, output = sentinel;
            size_t before = runtime_attempts;
            XrXirCallStatus empty_status = xr_xir_instance_take_result(instances[i], &empty);
            XrXirCallStatus occupied_status = xr_xir_instance_take_result(instances[i], &output);
            if (empty_status != XR_XIR_CALL_BAD_STATE || occupied_status != XR_XIR_CALL_BAD_ARGUMENT)
                fprintf(stderr, "array-default-output count=%lld empty=%u occupied=%u\n", (long long)counts[c], empty_status, occupied_status);
            CHECK(empty_status == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.reserved && !empty.payload);
            CHECK(occupied_status == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(!memcmp(&output, &sentinel, sizeof(output)) && runtime_attempts == before);
            CHECK(xr_xir_call_result_copy(&result.outcome, &faults[c][i]) == XR_XIR_VALUE_OK);
            CHECK(faults[c][i].status == expected[c]);
            if (counts[c] < 0) {
                CHECK(faults[c][i].panic.detail.code == 422 && xr_xir_panic_valid(&faults[c][i].panic));
                CHECK(faults[c][i].panic.message.type == XR_XIR_UNIT &&
                    !faults[c][i].panic.message.reserved && !faults[c][i].panic.message.payload);
            }
        }
        consumer_output_complete(&outputs[i]);
        printf("array-default-call instance=%u case=%u count=%lld required=%u actual=%u panic=%u result=PASS\n",
            i, c, (long long)counts[c], expected[c], result.outcome.status, result.outcome.panic.detail.code);
    }
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    for (unsigned c = 0; c < 6; ++c) for (unsigned i = 0; i < 2; ++i) {
        if (counts[c] >= 0 && counts[c] != INT64_MAX) {
            CHECK(retained[c][i].type == XR_XIR_I64 && retained[c][i].payload == counts[c]);
        } else {
            CHECK(faults[c][i].status == expected[c]);
            if (counts[c] < 0) {
                CHECK(xr_xir_panic_valid(&faults[c][i].panic) && faults[c][i].panic.detail.code == 422);
                CHECK(faults[c][i].panic.message.type == XR_XIR_UNIT &&
                    !faults[c][i].panic.message.reserved && !faults[c][i].panic.message.payload);
            }
        }
        xr_xir_value_drop(&retained[c][i]); xr_xir_call_result_drop(&faults[c][i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("array-default-execution calls=12 instances=2 source-dead=1 caller-program-dead=1 instances-dead=1 owned-results=6 owned-faults=6 recovery-count=4 runtime-physical=0/0");
}
