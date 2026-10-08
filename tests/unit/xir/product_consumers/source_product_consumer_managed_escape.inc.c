/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_managed_escape.inc.c - Managed elements after Array owner death
 *
 * KEY CONCEPT:
 *   Public element reads retain Strings independently of their original Arrays.
 */
static void consumer_managed_string(const XrXirValue *value) {
    const char expected[] = {'a', 0, 'b'}; const char *bytes = NULL; size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && !value->reserved && xr_xir_value_valid(value));
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == sizeof(expected) && !memcmp(bytes, expected, sizeof(expected)));
}

static void consumer_managed_array(const XrXirValue *array, XrXirValue *first) {
    CHECK(!array->reserved && xr_xir_value_valid(array));
    XrXirValueAdmission admission = {xr_xir_value_arena(array), NULL, NULL, NULL, 10000, 65536};
    CHECK(admission.arena); int64_t length = -1;
    CHECK(xr_xir_array_len(array, &admission, &length) == XR_XIR_VALUE_OK && length == 9);
    for (int64_t index = 0; index < 9; ++index) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, index, &admission, &element, &fault) == XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
        consumer_managed_string(&element);
        if (!index && first) CHECK(xr_xir_value_copy(&element, first) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&element);
    }
}

static void consumer_managed_cow(const XrXirValue *original, const XrXirValue *peer, unsigned repeat, unsigned instance) {
    const char expected[] = {'z', 0, 'q'};
    size_t live = runtime_live, bytes = runtime_bytes;
    XrXirValue copy = {0}, replacement = {0}, escaped = {0}; XrXirDomain *domain = NULL;
    CHECK(xr_xir_value_copy(original, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK && domain);
    CHECK(xr_xir_string_new(domain, expected, sizeof(expected), &replacement) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {xr_xir_value_arena(&copy), domain, NULL, NULL, 10000, 65536};
    XrXirValuePlace place = {copy.type, &copy.payload}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&place, 0, &replacement, &admission, &fault) == XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
    xr_xir_value_drop(&replacement); CHECK(!replacement.type && !replacement.reserved && !replacement.payload);
    xr_xir_domain_drop(domain); admission.domain = NULL;
    CHECK(xr_xir_value_valid(&copy) && !copy.reserved); int64_t length = -1;
    CHECK(xr_xir_array_len(&copy, &admission, &length) == XR_XIR_VALUE_OK && length == 9);
    for (int64_t index = 0; index < 9; ++index) {
        XrXirValue element = {0};
        CHECK(xr_xir_array_get(&copy, index, &admission, &element, &fault) == XR_XIR_VALUE_OK && xr_xir_fault_empty(fault));
        if (!index) {
            const char *view = NULL; size_t element_bytes = 0;
            CHECK(element.type == XR_XIR_STRING && !element.reserved && xr_xir_value_valid(&element));
            CHECK(xr_xir_string_view(&element, &view, &element_bytes) && element_bytes == sizeof(expected) && !memcmp(view, expected, sizeof(expected)));
            CHECK(xr_xir_value_copy(&element, &escaped) == XR_XIR_VALUE_OK);
        } else consumer_managed_string(&element);
        xr_xir_value_drop(&element);
    }
    consumer_managed_array(original, NULL); consumer_managed_array(peer, NULL);
    xr_xir_value_drop(&copy); CHECK(!copy.type && !copy.reserved && !copy.payload);
    const char *view = NULL; size_t length_bytes = 0;
    CHECK(escaped.type == XR_XIR_STRING && !escaped.reserved && xr_xir_value_valid(&escaped));
    CHECK(xr_xir_string_view(&escaped, &view, &length_bytes) && length_bytes == sizeof(expected) && !memcmp(view, expected, sizeof(expected)));
    xr_xir_value_drop(&escaped); CHECK(!escaped.type && !escaped.reserved && !escaped.payload);
    CHECK(runtime_live == live && runtime_bytes == bytes);
    printf("managed-cow instance=%u repeat=%u replacement=z-NUL-q original=a-NUL-b peer=a-NUL-b domain-reference-dead=1 replacement-reference-dead=1 escaped-after-mutated-array=1 physical-refund=1 result=PASS\n",
        instance, repeat);
}

static void consumer_managed_escape_normal(Consumer *run) {
    XrXirInstance *instances[2] = {0}; ConsumerOutput outputs[2] = {0}; XrXirOutputSink sinks[2] = {0};
    XrXirValue retained[2][2] = {0}, strings[2][2] = {0};
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
    for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        XrXirValue argument = {XR_XIR_I64, 0, 42};
        CHECK(xr_xir_instance_start(instances[i], run->parameterized_answer, &argument, 1) == XR_XIR_CALL_READY);
        XrXirInstanceResult result; unsigned polls = 0;
        do {
            CHECK(++polls < 4096); result = xr_xir_instance_poll_bounded(instances[i], UINT64_C(1000000));
        } while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && !result.outcome.wake && xr_xir_panic_empty(&result.outcome.panic));
        XrXirValue sentinel = {XR_XIR_I64, 0, 91}, output = sentinel; size_t before = runtime_attempts;
        CHECK(xr_xir_instance_take_result(instances[i], &output) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&output, &sentinel, sizeof(output)) && runtime_attempts == before);
        CHECK(xr_xir_instance_take_result(instances[i], &retained[repeat][i]) == XR_XIR_CALL_RETURNED);
        consumer_managed_array(&retained[repeat][i], NULL); consumer_output_complete(&outputs[i]);
        printf("managed-escape-call instance=%u repeat=%u argument=42 required=1 actual=1 length=9 result=PASS\n", i, repeat);
    }
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    CHECK(runtime_live && runtime_bytes && stats(run).live_bytes > run->baseline.live_bytes);
    for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned i = 0; i < 2; ++i)
        consumer_managed_cow(&retained[repeat][i], &retained[repeat][1 - i], repeat, i);
    for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        consumer_managed_array(&retained[repeat][i], &strings[repeat][i]);
        XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&retained[repeat][i], &copy) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&retained[repeat][i]);
        CHECK(!retained[repeat][i].type && !retained[repeat][i].reserved && !retained[repeat][i].payload);
        consumer_managed_array(&copy, NULL); xr_xir_value_drop(&copy);
        CHECK(!copy.type && !copy.reserved && !copy.payload);
    }
    CHECK(runtime_live && runtime_bytes && stats(run).live_bytes == run->baseline.live_bytes);
    for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        consumer_managed_string(&strings[repeat][i]);
        XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&strings[repeat][i], &copy) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&strings[repeat][i]);
        CHECK(!strings[repeat][i].type && !strings[repeat][i].reserved && !strings[repeat][i].payload);
        consumer_managed_string(&copy); xr_xir_value_drop(&copy);
        CHECK(!copy.type && !copy.reserved && !copy.payload);
    }
    CHECK(!runtime_live && !runtime_bytes && stats(run).live_bytes == run->baseline.live_bytes);
    puts("managed-escape-execution calls=4 instances=2 source-dead=1 caller-program-dead=1 instances-dead=1 arrays-dead=1 owned-arrays=4 copied-arrays=4 escaped-strings=4 copied-strings=4 arena-refund-before-string-drop=1 runtime-physical=0/0");
}
