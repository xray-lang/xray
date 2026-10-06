/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_drive.inc.c - Public yield and wake progression
 *
 * KEY CONCEPT:
 *   Polling and host resumption remain separate observable cancellation points.
 */
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u,
    "Consumers require the current Checked packet contract");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Consumers require the current runtime contracts");

typedef struct ConsumerCursor {
    uint64_t epoch, wake;
    size_t resumes;
    bool pending;
    XrXirHostWait host_timer;
} ConsumerCursor;

static unsigned consumer_yield_count(void) {
    return !strcmp(XR_CONSUMER_NAME, "parameter_coroutine") ||
           !strcmp(XR_CONSUMER_NAME, "module_receiver_suspend") ||
           !strcmp(XR_CONSUMER_NAME, "cross_module_coroutine") ||
           !strcmp(XR_CONSUMER_NAME, "cross_module_static_coroutine") ||
           !strcmp(XR_CONSUMER_NAME, "multi_safepoint") ? 2u : 0u;
}

static unsigned consumer_wait_count(void) {
    return consumer_timer_case() ? 1u : consumer_yield_count();
}

static XrXirCallStatus consumer_advance(XrXirInstance *instance, ConsumerCursor *cursor,
                                      uint64_t quantum) {
    if (cursor->pending) {
        if (consumer_timer_case()) consumer_timer_due(&cursor->host_timer);
        CHECK(xr_xir_instance_resume(instance, cursor->epoch, cursor->wake) == XR_XIR_CALL_READY);
        cursor->pending = false;
        ++cursor->resumes;
        return XR_XIR_CALL_READY;
    }
    XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, quantum);
    if (result.outcome.status != XR_XIR_CALL_SUSPENDED)
        return result.outcome.status;
    CHECK(result.epoch && result.epoch != UINT64_MAX && result.outcome.wake);
    XrXirWaitRequest wait = {0};
    CHECK(xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake, &wait) == XR_XIR_CALL_READY);
    if (consumer_timer_case())
        consumer_timer_arm(&cursor->host_timer, &wait, cursor->resumes);
    else
    CHECK(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
        !wait.subject && !wait.generation && !wait.ticket);
    const size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_resume(instance, result.epoch + 1, result.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake ^ UINT64_C(1)) == XR_XIR_CALL_BAD_STATE);
    XrXirWaitRequest untouched_wait;
    unsigned char wait_before[sizeof(untouched_wait)];
    memset(&untouched_wait, 0xa5, sizeof(untouched_wait));
    memcpy(wait_before, &untouched_wait, sizeof(wait_before));
    CHECK(xr_xir_instance_wait_request(instance, result.epoch + 1, result.outcome.wake, &untouched_wait) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&untouched_wait, wait_before, sizeof(untouched_wait)));
    CHECK(xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake ^ UINT64_C(1), &untouched_wait) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&untouched_wait, wait_before, sizeof(untouched_wait)));
    XrXirValue untouched_value;
    unsigned char value_before[sizeof(untouched_value)];
    memset(&untouched_value, 0xa5, sizeof(untouched_value));
    memcpy(value_before, &untouched_value, sizeof(value_before));
    CHECK(xr_xir_instance_take_result(instance, &untouched_value) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&untouched_value, value_before, sizeof(untouched_value)) && runtime_attempts == attempts);
    cursor->epoch = result.epoch;
    cursor->wake = result.outcome.wake;
    cursor->pending = true;
    /* READY describes the next driver action, which is the host's exact resume. */
    return XR_XIR_CALL_READY;
}
