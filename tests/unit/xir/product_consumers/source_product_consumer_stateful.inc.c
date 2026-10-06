/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_stateful.inc.c - Original mutable module state
 *
 * KEY CONCEPT:
 *   Repeated calls preserve the original state and escaped assertion owner.
 */
static bool consumer_stateful_case(void) {
    return !strcmp(XR_CONSUMER_NAME, "narrow_array");
}

static unsigned consumer_repeat_count(void) {
    return consumer_stateful_case() ? 1 : 2;
}

static void consumer_stateful_finish(XrXirInstance *instance, uint32_t answer, unsigned ordinal) {
    CHECK(consumer_stateful_case());
    CHECK(xr_xir_instance_start(instance, answer, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult failed = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    CHECK(failed.outcome.status == XR_XIR_CALL_ASSERTION);
    CHECK(failed.outcome.panic.detail.code == XR_XIR_PANIC_ASSERTION);
    XrXirCallResult owned = {0};
    CHECK(xr_xir_call_result_copy(&failed.outcome, &owned) == XR_XIR_VALUE_OK);
    const char *before_bytes = NULL;
    size_t before_length = 0;
    CHECK(xr_xir_panic_valid(&owned.panic));
    CHECK(xr_xir_string_view(&owned.panic.message, &before_bytes, &before_length));
    char golden[256];
    CHECK(before_length < sizeof(golden));
    if (before_length)
        memcpy(golden, before_bytes, before_length);
    XrXirValue untouched = {0};
    CHECK(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
    CHECK(!untouched.type && !untouched.payload);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    const char *after_bytes = NULL;
    size_t after_length = 0;
    CHECK(xr_xir_panic_valid(&owned.panic));
    CHECK(xr_xir_string_view(&owned.panic.message, &after_bytes, &after_length));
    CHECK(after_length == before_length);
    if (after_length)
        CHECK(!memcmp(golden, after_bytes, after_length));
    printf("stateful-repeat instance=%u status=%u panic=%u owned-message=%zu\n",
        ordinal, owned.status, owned.panic.detail.code, after_length);
    xr_xir_call_result_drop(&owned);
}
