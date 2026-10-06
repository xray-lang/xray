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

static XrXirCallStatus runtime_drive(XrXirInstance *instance, uint32_t entry, size_t *ticks) {
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    ConsumerCursor cursor = {0};
    while (status == XR_XIR_CALL_READY) {
        CHECK(*ticks < 4096);
        ++*ticks;
        status = consumer_advance(instance, &cursor, 1);
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
    ConsumerOutput output = {0};
    XrXirOutputSink sink = {0};
    XrXirInstanceConfig config = consumer_config(&output, &sink);
    XrXirInstance *instance = NULL;
    probe.status = xr_xir_instance_new(run->program, &config, &instance);
    if (probe.status == XR_XIR_CALL_READY) {
        probe.status = runtime_drive(instance, run->entry, &probe.ticks);
        if (probe.status == XR_XIR_CALL_RETURNED) {
            fixed_result(instance, 0);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                consumer_output_reset(&output);
                probe.status = runtime_drive(instance, run->answer, &probe.ticks);
                if (probe.status != XR_XIR_CALL_RETURNED)
                    break;
                fixed_result(instance, XR_CONSUMER_EXPECTED);
                consumer_output_complete(&output);
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

static XrXirInstance *initialized(const Consumer *run, ConsumerOutput *output, XrXirOutputSink *sink) {
    XrXirInstanceConfig config = consumer_config(output, sink);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue result = execute(instance, run->entry, 0);
    CHECK(result.type == XR_XIR_I64 && !result.payload);
    xr_xir_value_drop(&result);
    return instance;
}

static void cancel_prefixes(unsigned mode) {
    Consumer run = build(mode, NULL, SIZE_MAX, compiler_limits());
    CHECK(run.status == XR_XIR_OK);
    ConsumerOutput output = {0};
    XrXirOutputSink sink = {0};
    XrXirInstance *instance = initialized(&run, &output, &sink);
    size_t ticks = 0;
    CHECK(runtime_drive(instance, run.answer, &ticks) == XR_XIR_CALL_RETURNED && ticks);
    fixed_result(instance, XR_CONSUMER_EXPECTED);
    consumer_output_complete(&output);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
    size_t pending_yields = 0;
    for (size_t prefix = 0; prefix < ticks; ++prefix) {
        instance = initialized(&run, &output, &sink);
        CHECK(xr_xir_instance_start(instance, run.answer, NULL, 0) == XR_XIR_CALL_READY);
        ConsumerCursor cursor = {0};
        for (size_t tick = 0; tick < prefix; ++tick)
            CHECK(consumer_advance(instance, &cursor, 1) == XR_XIR_CALL_READY);
        if (cursor.pending)
            ++pending_yields;
        CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        XrXirCallStatus status;
        size_t drain = 0;
        do {
            CHECK(++drain < 4096);
            status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
        } while (status == XR_XIR_CALL_READY);
        CHECK(status == XR_XIR_CALL_CANCELLED);
        if (cursor.pending)
            CHECK(xr_xir_instance_resume(instance, cursor.epoch, cursor.wake) == XR_XIR_CALL_BAD_STATE);
        XrXirValue untouched = {0};
        CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
        CHECK(!untouched.type && !untouched.payload);
        /* Cancellation preserves initialized module state and future calls. */
        consumer_output_reset(&output);
        XrXirValue result = execute(instance, run.answer, consumer_yield_count());
        CHECK(result.type == XR_XIR_I64 && (int64_t)result.payload == XR_CONSUMER_EXPECTED);
        xr_xir_value_drop(&result);
        consumer_output_complete(&output);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
        printf("cancel prefix=%zu physical=0/0\n", prefix);
    }
    CHECK(pending_yields == consumer_yield_count());
    release(&run);
    printf("cancel-summary case=%s mode=%u prefixes=%zu covered=%zu physical=0/0\n",
        XR_CONSUMER_NAME, mode, ticks, ticks);
    if (pending_yields)
        printf("cancel-yield-summary case=%s mode=%u pending-yields=%zu stale-wakes-rejected=%zu physical=0/0\n",
            XR_CONSUMER_NAME, mode, pending_yields, pending_yields);
}

static void output_statuses(unsigned mode) {
    CHECK(consumer_text_case());
    const XrXirOutputStatus statuses[] = {XR_XIR_OUTPUT_ERROR, XR_XIR_OUTPUT_OOM, XR_XIR_OUTPUT_LIMIT,
        XR_XIR_OUTPUT_BAD_ARGUMENT, XR_XIR_OUTPUT_BAD_ABI, (XrXirOutputStatus)99};
    const XrXirCallStatus expected[] = {XR_XIR_CALL_OUTPUT_ERROR, XR_XIR_CALL_OOM, XR_XIR_CALL_LIMIT,
        XR_XIR_CALL_BAD_ARGUMENT, XR_XIR_CALL_BAD_ABI, XR_XIR_CALL_BAD_ARGUMENT};
    const size_t prefixes[] = {0, 13, 30};
    Consumer run = build(mode, NULL, SIZE_MAX, compiler_limits());
    CHECK(run.status == XR_XIR_OK);
    for (unsigned kind = 0; kind < sizeof(statuses) / sizeof(statuses[0]); ++kind) {
        for (unsigned at = 0; at < 3; ++at) {
            ConsumerOutput output = {0};
            XrXirOutputSink sink = {0};
            XrXirInstance *instance = initialized(&run, &output, &sink);
            output.reject_at = at + 1;
            output.reject_status = statuses[kind];
            size_t ticks = 0;
            CHECK(runtime_drive(instance, run.answer, &ticks) == expected[kind]);
            CHECK(output.rejected && output.groups == at + 1 && output.bytes == prefixes[at]);
            CHECK(!memcmp(output.text, consumer_text_golden, output.bytes));
            CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
            XrXirValue untouched = {0};
            CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
            CHECK(!untouched.type && !untouched.payload);
            consumer_output_reset(&output);
            XrXirValue result = execute(instance, run.answer, consumer_yield_count());
            CHECK(result.type == XR_XIR_I64 && (int64_t)result.payload == XR_CONSUMER_EXPECTED);
            xr_xir_value_drop(&result);
            consumer_output_complete(&output);
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
            printf("output-status kind=%u group=%u status=%u physical=0/0\n", kind, at, expected[kind]);
        }
    }
    release(&run);
    printf("output-status-summary case=%s mode=%u statuses=6 positions=3 covered=18 physical=0/0\n", XR_CONSUMER_NAME, mode);
}
