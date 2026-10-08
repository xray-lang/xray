/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_array_escape.inc.c - Heap Array results after execution owner death
 *
 * KEY CONCEPT:
 *   Escaped values own their type arena and data until the final public drop.
 */
static void consumer_array_zeros(const XrXirValue *array, int64_t count) {
    CHECK(count >= 0 && count <= 4 && !array->reserved && xr_xir_value_valid(array));
    XrXirValueAdmission admission = {xr_xir_value_arena(array), NULL, NULL, NULL, 10000, 65536};
    CHECK(admission.arena); int64_t length = -1;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == count);
    for (int64_t index = 0; index < count; ++index) {
        XrXirValue value = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, index, &admission, &value, &fault) == XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
        CHECK(value.type == XR_XIR_U8 && !value.reserved && !value.payload); xr_xir_value_drop(&value);
    }
}

static void consumer_array_cow(const XrXirValue *original, const XrXirValue *peer, int64_t count, unsigned instance) {
    CHECK(count > 0 && count <= 4);
    size_t live = runtime_live, bytes = runtime_bytes;
    XrXirValue copy = {0}; XrXirDomain *domain = NULL;
    CHECK(xr_xir_value_copy(original, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK && domain);
    XrXirValueAdmission admission = {xr_xir_value_arena(&copy), domain, NULL, NULL, 10000, 65536};
    XrXirValuePlace place = {copy.type, &copy.payload};
    XrXirValue replacement = {XR_XIR_U8, 0, 7}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&place, 0, &replacement, &admission, &fault) == XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
    xr_xir_domain_drop(domain); admission.domain = NULL;
    CHECK(xr_xir_value_valid(&copy) && !copy.reserved);
    int64_t length = -1;
    CHECK(xr_xir_array_len(&copy, &admission, &length) == XR_XIR_VALUE_OK && length == count);
    for (int64_t index = 0; index < count; ++index) {
        XrXirValue element = {0};
        CHECK(xr_xir_array_get(&copy, index, &admission, &element, &fault) == XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
        CHECK(element.type == XR_XIR_U8 && !element.reserved && element.payload == (index ? 0 : 7));
        xr_xir_value_drop(&element);
    }
    consumer_array_zeros(original, count); consumer_array_zeros(peer, count);
    xr_xir_value_drop(&copy); CHECK(!copy.type && !copy.reserved && !copy.payload);
    CHECK(runtime_live == live && runtime_bytes == bytes);
    printf("array-cow instance=%u count=%lld replacement=7 original=zero peer=zero domain-reference-dead=1 physical-refund=1 result=PASS\n",
        instance, (long long)count);
}

static void consumer_array_escape_normal(Consumer *run) {
    const int64_t counts[6] = {3, 0, -1, INT64_MIN, INT64_MAX, 4};
    const XrXirCallStatus expected[6] = {XR_XIR_CALL_RETURNED, XR_XIR_CALL_RETURNED,
        XR_XIR_CALL_NUMERIC_RANGE, XR_XIR_CALL_NUMERIC_RANGE, XR_XIR_CALL_LIMIT, XR_XIR_CALL_RETURNED};
    XrXirInstance *instances[2] = {0}; ConsumerOutput outputs[2] = {0}; XrXirOutputSink sinks[2] = {0};
    XrXirValue retained[6][2] = {0}; XrXirCallResult faults[6][2] = {0};
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
    for (unsigned c = 0; c < 6; ++c) for (unsigned i = 0; i < 2; ++i) {
        XrXirValue argument = {XR_XIR_I64, 0, counts[c]};
        CHECK(xr_xir_instance_start(instances[i], run->parameterized_answer, &argument, 1) == XR_XIR_CALL_READY);
        XrXirInstanceResult result; unsigned polls = 0;
        do {
            CHECK(++polls < 4096); result = xr_xir_instance_poll_bounded(instances[i], UINT64_C(1000000));
        } while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == expected[c] && !result.outcome.wake);
        if (expected[c] == XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_panic_empty(&result.outcome.panic));
            CHECK(xr_xir_instance_take_result(instances[i], &retained[c][i]) == XR_XIR_CALL_RETURNED);
            consumer_array_zeros(&retained[c][i], counts[c]);
        } else {
            XrXirValue empty = {0}, sentinel = {XR_XIR_I64, 0, 91}, output = sentinel;
            size_t before = runtime_attempts;
            CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE);
            CHECK(!empty.type && !empty.reserved && !empty.payload);
            CHECK(xr_xir_instance_take_result(instances[i], &output) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(!memcmp(&output, &sentinel, sizeof(output)) && runtime_attempts == before);
            CHECK(xr_xir_call_result_copy(&result.outcome, &faults[c][i]) == XR_XIR_VALUE_OK);
            if (counts[c] < 0) CHECK(faults[c][i].panic.detail.code == 422 && xr_xir_panic_valid(&faults[c][i].panic));
        }
        consumer_output_complete(&outputs[i]);
        printf("array-escape-call instance=%u case=%u count=%lld required=%u actual=%u result=PASS\n",
            i, c, (long long)counts[c], expected[c], result.outcome.status);
    }
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes && stats(run).live_bytes > run->baseline.live_bytes);
    for (unsigned c = 0; c < 6; ++c) if (expected[c] == XR_XIR_CALL_RETURNED && counts[c])
        for (unsigned i = 0; i < 2; ++i) consumer_array_cow(&retained[c][i], &retained[c][1 - i], counts[c], i);
    for (unsigned c = 0; c < 6; ++c) for (unsigned i = 0; i < 2; ++i) {
        if (expected[c] == XR_XIR_CALL_RETURNED) {
            consumer_array_zeros(&retained[c][i], counts[c]);
            XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&retained[c][i], &copy) == XR_XIR_VALUE_OK);
            xr_xir_value_drop(&retained[c][i]);
            CHECK(!retained[c][i].type && !retained[c][i].reserved && !retained[c][i].payload);
            consumer_array_zeros(&copy, counts[c]); xr_xir_value_drop(&copy);
            CHECK(!copy.type && !copy.reserved && !copy.payload);
        } else {
            CHECK(faults[c][i].status == expected[c] && xr_xir_call_result_valid(&faults[c][i]));
            if (counts[c] < 0) CHECK(faults[c][i].panic.detail.code == 422 && xr_xir_panic_valid(&faults[c][i].panic));
        }
        xr_xir_call_result_drop(&faults[c][i]); CHECK(xr_xir_call_result_empty(&faults[c][i]));
    }
    CHECK(!runtime_live && !runtime_bytes && stats(run).live_bytes == run->baseline.live_bytes);
    puts("array-escape-execution calls=12 instances=2 source-dead=1 caller-program-dead=1 instances-dead=1 owned-arrays=6 copied-arrays=6 retained-faults=6 recovery-count=4 arena-pinned=1 compiler-stock-refund=1 runtime-physical=0/0");
}
