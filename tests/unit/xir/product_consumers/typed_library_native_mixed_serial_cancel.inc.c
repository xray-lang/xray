/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_native_mixed_serial_cancel.inc.c - Preserved serial cancellation calls
 *
 * KEY CONCEPT:
 *   These unchanged serial-only functions remain independent of the simultaneous
 *   pending receiver and keep their original public cancellation and 42 assertions.
 */

static void drain_clean_cancel(ReceiverRun *run, XrXirInstance *instance, XrXirInstanceResult pending) {
    CHECK(call_completed(run, xr_xir_instance_cancel_current(instance), XR_XIR_CALL_CANCEL_REQUESTED,
        "clean-prefix-public-cancel-current"));
    reject_cancelled_token(run, instance, pending);
    size_t polls = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&result.outcome) && result.epoch == pending.epoch);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_CANCELLED, "clean-prefix-natural-terminal-CANCELLED"));
        CHECK(result.outcome.value.type == XR_XIR_UNIT && !result.outcome.value.reserved &&
            !result.outcome.value.payload && !result.outcome.wake && xr_xir_panic_empty(&result.outcome.panic));
        break;
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    reject_cancelled_token(run, instance, pending);
    CHECK(receiver_native_owner && !receiver_native_releases);
    /* Poll outcomes borrow; cancellation transfers no owned I64 and is never dropped. */
}

static void run_cancel_case(ReceiverRun *run, XrXirInstance *instance, XrXirInstance *peer, unsigned index, unsigned prefix) {
    CHECK(index < 2 && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY &&
        xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY);
    NativeRouteCounts before = native_route_counts(run->roles);
    XrXirInstanceResult pending = reach_cancel_prefix(run, instance, prefix);
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY);
    drain_clean_cancel(run, instance, pending);
    NativeRouteCounts after = native_route_counts(run->roles);
    check_actual_cancel_route(run->mode, prefix, before, after);
    cancelled_output_diagnostics(run, instance);
    ++run->completed_cancel_prefix[index][prefix - 1];
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY && receiver_native_owner && !receiver_native_releases);
    before = native_route_counts(run->roles);
    execute_receiver_i64(run, instance, run->roles.answer, 42, 2);
    CHECK(run->last_call_epoch == pending.epoch + 1);
    reject_cancelled_token(run, instance, pending);
    after = native_route_counts(run->roles); check_actual_route(run->mode, before, after);
    ++run->completed_fixed42[index];
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_READY && receiver_native_owner && !receiver_native_releases);
    printf("typed-library-clean-cancel instance=%u mode=%u prefix=%u actual-SUSPEND=%u actual-earlier-resumes=%u terminal=CANCELLED recovered-owned-I64=42 new-epoch=1 lease=0 concurrent-pending=DEFERRED\n",
        index, run->mode, prefix, prefix, prefix - 1);
}

static void run_receiver_cancel_instances(ReceiverRun *run, const char *c_path) {
    prepare_cancel_instances(run, c_path);
    for (unsigned i = 0; i < 2; ++i) {
        NativeRouteCounts before = native_route_counts(run->roles);
        execute_receiver_i64(run, run->instances[i], run->roles.answer, 42, 2);
        NativeRouteCounts after = native_route_counts(run->roles); check_actual_route(run->mode, before, after);
        ++run->completed_fixed42[i];
        for (unsigned prefix = 1; prefix <= 2; ++prefix)
            run_cancel_case(run, run->instances[i], run->instances[1 - i], i, prefix);
        CHECK(run->completed_fixed42[i] == 3 && run->completed_cancel_prefix[i][0] == 1 &&
            run->completed_cancel_prefix[i][1] == 1 && receiver_native_owner && !receiver_native_releases);
    }
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_READY);
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "clean-prefix-successful-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_native_owner && !receiver_native_releases);
    }
    CHECK(!receiver_native_owner && receiver_native_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}

static void report_cancel_execution(const ReceiverRun *run, bool passed, bool physical_zero) {
    printf("typed-library-clean-prefix result=%s cleanup-error=%u physical-zero=%u instance0-real42=%u instance0-cancel1=%u instance0-cancel2=%u instance1-real42=%u instance1-cancel1=%u instance1-cancel2=%u expected-per-Instance=3-real42+2-CANCELLED full-FI=NOT_RUN axes=NOT_RUN stable-object-lifecycle=OPEN init-failure=OPEN owned-escape=OPEN original-AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN concurrent-pending=DEFERRED\n",
        passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero,
        run->completed_fixed42[0], run->completed_cancel_prefix[0][0], run->completed_cancel_prefix[0][1],
        run->completed_fixed42[1], run->completed_cancel_prefix[1][0], run->completed_cancel_prefix[1][1]);
}
