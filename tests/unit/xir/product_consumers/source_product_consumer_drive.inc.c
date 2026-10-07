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
typedef struct ConsumerCursor {
    uint64_t epoch, wake;
    size_t resumes;
    bool pending;
} ConsumerCursor;

static unsigned consumer_yield_count(void) {
    return !strcmp(XR_CONSUMER_NAME, "parameter_coroutine") ||
           !strcmp(XR_CONSUMER_NAME, "module_receiver_suspend") ||
           !strcmp(XR_CONSUMER_NAME, "cross_module_coroutine") ||
           !strcmp(XR_CONSUMER_NAME, "cross_module_static_coroutine") ||
           !strcmp(XR_CONSUMER_NAME, "multi_safepoint") ? 2u : 0u;
}

static XrXirCallStatus consumer_advance(XrXirInstance *instance, ConsumerCursor *cursor,
                                      uint64_t quantum) {
    if (cursor->pending) {
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
    CHECK(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
          !wait.subject && !wait.generation && !wait.ticket);
    const size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_resume(instance, result.epoch + 1, result.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake ^ UINT64_C(1)) == XR_XIR_CALL_BAD_STATE);
    const unsigned char untouched_subject = 0;
    XrXirWaitRequest untouched_wait = {.kind = UINT32_MAX, .reserved = UINT32_MAX,
        .after_ms = UINT64_MAX, .subject = &untouched_subject, .generation = UINT64_MAX, .ticket = UINT64_MAX};
    CHECK(xr_xir_instance_wait_request(instance, result.epoch + 1, result.outcome.wake, &untouched_wait) == XR_XIR_CALL_BAD_STATE);
    CHECK(untouched_wait.kind == UINT32_MAX && untouched_wait.reserved == UINT32_MAX &&
          untouched_wait.after_ms == UINT64_MAX && untouched_wait.subject == &untouched_subject &&
          untouched_wait.generation == UINT64_MAX && untouched_wait.ticket == UINT64_MAX);
    CHECK(xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake ^ UINT64_C(1), &untouched_wait) == XR_XIR_CALL_BAD_STATE);
    CHECK(untouched_wait.kind == UINT32_MAX && untouched_wait.reserved == UINT32_MAX &&
          untouched_wait.after_ms == UINT64_MAX && untouched_wait.subject == &untouched_subject &&
          untouched_wait.generation == UINT64_MAX && untouched_wait.ticket == UINT64_MAX);
    XrXirValue untouched_value = {0};
    CHECK(xr_xir_instance_take_result(instance, &untouched_value) == XR_XIR_CALL_BAD_STATE);
    CHECK(!untouched_value.type && !untouched_value.payload && runtime_attempts == attempts);
    cursor->epoch = result.epoch;
    cursor->wake = result.outcome.wake;
    cursor->pending = true;
    /* READY describes the next driver action, which is the host's exact resume. */
    return XR_XIR_CALL_READY;
}
