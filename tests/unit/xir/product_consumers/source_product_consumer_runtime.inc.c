/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_runtime.inc.c - Real runtime faults and cancellation
 *
 * KEY CONCEPT:
 *   Every allocation and every active prefix retains its original oracle.
 */
typedef struct RuntimeProbe {
    XrXirCallStatus status;
    size_t sites, ticks;
} RuntimeProbe;

static XrXirInstanceConfig runtime_config(void) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = UINT64_C(1048576);
    return config;
}

static XrXirCallStatus runtime_drive(XrXirInstance *instance, uint32_t entry, size_t *ticks) {
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    while (status == XR_XIR_CALL_READY) {
        CHECK(*ticks < 4096);
        ++*ticks;
        status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
    }
    return status;
}

static void fixed_result(XrXirInstance *instance, int64_t expected) {
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(value.type == XR_XIR_I64 && (int64_t)value.payload == expected);
    xr_xir_value_drop(&value);
}

static RuntimeProbe runtime_probe(const Consumer *run, size_t failure) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    CHECK(!live && !bytes);
    runtime_attempts = 0;
    runtime_fail_at = failure;
    RuntimeProbe probe = {0};
    XrXirInstanceConfig config = runtime_config();
    XrXirInstance *instance = NULL;
    probe.status = xr_xir_instance_new(run->program, &config, &instance);
    if (probe.status == XR_XIR_CALL_READY) {
        probe.status = runtime_drive(instance, run->entry, &probe.ticks);
        if (probe.status == XR_XIR_CALL_RETURNED) {
            fixed_result(instance, 0);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                probe.status = runtime_drive(instance, run->answer, &probe.ticks);
                if (probe.status != XR_XIR_CALL_RETURNED)
                    break;
                fixed_result(instance, XR_CONSUMER_EXPECTED);
            }
        }
        if (probe.status != XR_XIR_CALL_RETURNED) {
            CHECK(failure != SIZE_MAX && probe.status == XR_XIR_CALL_OOM);
            XrXirValue untouched = {0};
            CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
            CHECK(!untouched.type && !untouched.payload);
            if (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED) {
                CHECK(xr_xir_instance_start(instance, run->answer, NULL, 0) == XR_XIR_CALL_OOM);
                XrXirCallResult owned = {0};
                CHECK(xr_xir_instance_copy_failure(instance, &owned) == XR_XIR_CALL_OOM);
                CHECK(owned.status == XR_XIR_CALL_OOM);
                xr_xir_call_result_drop(&owned);
            }
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    } else {
        CHECK(!instance && failure != SIZE_MAX && probe.status == XR_XIR_CALL_OOM);
    }
    probe.sites = runtime_attempts;
    runtime_fail_at = SIZE_MAX;
    CHECK(runtime_live == live && runtime_bytes == bytes);
    return probe;
}

static void runtime_faults(unsigned mode) {
    Consumer run = build(mode, NULL, SIZE_MAX, compiler_limits());
    CHECK(run.status == XR_XIR_OK);
    RuntimeProbe baseline = runtime_probe(&run, SIZE_MAX);
    CHECK(baseline.status == XR_XIR_CALL_RETURNED && baseline.sites && baseline.ticks);
    for (size_t ordinal = 0; ordinal < baseline.sites; ++ordinal) {
        RuntimeProbe fault = runtime_probe(&run, ordinal);
        if (fault.status != XR_XIR_CALL_OOM || fault.sites <= ordinal)
            fprintf(stderr, "runtime case=%s mode=%u ordinal=%zu/%zu status=%u attempts=%zu\n",
                XR_CONSUMER_NAME, mode, ordinal, baseline.sites, fault.status, fault.sites);
        CHECK(fault.status == XR_XIR_CALL_OOM && fault.sites > ordinal);
        printf("runtime ordinal=%zu physical=0/0\n", ordinal);
    }
    release(&run);
    printf("runtime-summary case=%s mode=%u sites=%zu covered=%zu ticks=%zu physical=0/0\n",
        XR_CONSUMER_NAME, mode, baseline.sites, baseline.sites, baseline.ticks);
}

static XrXirInstance *initialized(const Consumer *run) {
    XrXirInstanceConfig config = runtime_config();
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue result = execute(instance, run->entry);
    CHECK(result.type == XR_XIR_I64 && !result.payload);
    xr_xir_value_drop(&result);
    return instance;
}

static void cancel_prefixes(unsigned mode) {
    Consumer run = build(mode, NULL, SIZE_MAX, compiler_limits());
    CHECK(run.status == XR_XIR_OK);
    XrXirInstance *instance = initialized(&run);
    size_t ticks = 0;
    CHECK(runtime_drive(instance, run.answer, &ticks) == XR_XIR_CALL_RETURNED && ticks);
    fixed_result(instance, XR_CONSUMER_EXPECTED);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
    for (size_t prefix = 0; prefix < ticks; ++prefix) {
        instance = initialized(&run);
        CHECK(xr_xir_instance_start(instance, run.answer, NULL, 0) == XR_XIR_CALL_READY);
        for (size_t tick = 0; tick < prefix; ++tick)
            CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        XrXirCallStatus status;
        size_t drain = 0;
        do {
            CHECK(++drain < 4096);
            status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
        } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_CANCELLED);
        XrXirValue untouched = {0};
        CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
        CHECK(!untouched.type && !untouched.payload);
        /* Cancellation preserves initialized module state and future calls. */
        XrXirValue result = execute(instance, run.answer);
        CHECK(result.type == XR_XIR_I64 && (int64_t)result.payload == XR_CONSUMER_EXPECTED);
        xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
        printf("cancel prefix=%zu physical=0/0\n", prefix);
    }
    release(&run);
    printf("cancel-summary case=%s mode=%u prefixes=%zu covered=%zu physical=0/0\n",
        XR_CONSUMER_NAME, mode, ticks, ticks);
}
