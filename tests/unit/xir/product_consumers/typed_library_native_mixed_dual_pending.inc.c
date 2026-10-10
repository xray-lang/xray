/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_native_mixed_dual_pending.inc.c - Independent pending cancellation isolation
 *
 * KEY CONCEPT:
 *   Both initialized Instances own a real suspended answer call. Cancellation
 *   and natural unwind of one preserve the other's exact pending host request.
 *   Each peer completes its own two yields and 42 before the victim recovers 42.
 */
typedef struct ReceiverPendingSnapshot {
    XrXirInstanceResult result;
    XrXirWaitRequest request;
} ReceiverPendingSnapshot;

static ReceiverPendingSnapshot capture_pending_snapshot(ReceiverRun *run, XrXirInstance *instance,
    XrXirInstanceResult pending) {
    ReceiverPendingSnapshot snapshot = {0}; snapshot.result = pending;
    CHECK(pending.outcome.status == XR_XIR_CALL_SUSPENDED && pending.epoch &&
        pending.epoch != UINT64_MAX && pending.outcome.wake);
    CHECK(call_completed(run, xr_xir_instance_wait_request(instance, pending.epoch,
        pending.outcome.wake, &snapshot.request), XR_XIR_CALL_READY, "dual-pending-original-public-request"));
    CHECK(snapshot.request.kind == XR_XIR_WAIT_YIELD && !snapshot.request.reserved &&
        !snapshot.request.after_ms && !snapshot.request.subject && !snapshot.request.generation && !snapshot.request.ticket);
    return snapshot;
}

static void check_pending_snapshot(ReceiverRun *run, XrXirInstance *instance,
    const ReceiverPendingSnapshot *snapshot) {
    NativeRouteCounts before = native_route_counts(run->roles);
    XrXirInstanceResult actual = xr_xir_instance_poll_bounded(instance, 64);
    CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&actual.outcome));
    CHECK(call_completed(run, actual.outcome.status, XR_XIR_CALL_SUSPENDED, "dual-peer-still-real-SUSPENDED"));
    CHECK(actual.epoch == snapshot->result.epoch && actual.outcome.wake == snapshot->result.outcome.wake);
    CHECK(actual.outcome.value.type == XR_XIR_UNIT && !actual.outcome.value.reserved &&
        !actual.outcome.value.payload && xr_xir_panic_empty(&actual.outcome.panic));
    XrXirWaitRequest request = {0};
    CHECK(call_completed(run, xr_xir_instance_wait_request(instance, actual.epoch, actual.outcome.wake,
        &request), XR_XIR_CALL_READY, "dual-peer-exact-pending-request-unchanged"));
    CHECK(!memcmp(&request, &snapshot->request, sizeof(request)));
    NativeRouteCounts after = native_route_counts(run->roles);
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(after.native[n] == before.native[n] && after.vm[n] == before.vm[n]);
        CHECK(after.child_suspends[n] == before.child_suspends[n]);
    }
    CHECK(receiver_native_owner && !receiver_native_releases);
}

static void check_cancel_unwind_route(unsigned mode, NativeRouteCounts before, NativeRouteCounts after) {
    for (unsigned n = 0; n < 2; ++n) {
        bool native = mode == 1 || (mode == 2 ? n == 0 : n == 1);
        CHECK(native ? after.native[n] >= before.native[n] : after.vm[n] >= before.vm[n]);
        CHECK(native ? after.vm[n] == before.vm[n] : after.native[n] == before.native[n]);
        CHECK(after.child_suspends[n] == before.child_suspends[n]);
    }
}

static void drain_dual_cancel(ReceiverRun *run, XrXirInstance *instance, XrXirInstanceResult pending,
    XrXirInstance *peer, const ReceiverPendingSnapshot *snapshot) {
    check_pending_snapshot(run, peer, snapshot);
    CHECK(call_completed(run, xr_xir_instance_cancel_current(instance), XR_XIR_CALL_CANCEL_REQUESTED,
        "dual-pending-public-cancel-only-victim"));
    reject_cancelled_token(run, instance, pending);
    check_pending_snapshot(run, peer, snapshot);
    size_t polls = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&result.outcome) && result.epoch == pending.epoch);
        check_pending_snapshot(run, peer, snapshot);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_CANCELLED, "dual-victim-natural-terminal-CANCELLED"));
        CHECK(result.outcome.value.type == XR_XIR_UNIT && !result.outcome.value.reserved &&
            !result.outcome.value.payload && !result.outcome.wake && xr_xir_panic_empty(&result.outcome.panic));
        break;
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    reject_cancelled_token(run, instance, pending);
    check_pending_snapshot(run, peer, snapshot);
    CHECK(receiver_native_owner && !receiver_native_releases);
}

static unsigned complete_pending_peer(ReceiverRun *run, XrXirInstance *instance,
    XrXirInstanceResult pending, unsigned prefix) {
    CHECK(prefix == 1 || prefix == 2);
    CHECK(call_completed(run, xr_xir_instance_resume(instance, pending.epoch, pending.outcome.wake),
        XR_XIR_CALL_READY, "dual-peer-consume-its-own-original-wake-once"));
    unsigned resumed = 1; size_t polls = 0;
    uint64_t previous_wake = pending.outcome.wake;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&result.outcome) && result.epoch == pending.epoch);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        if (result.outcome.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(prefix - 1 + resumed < 2 && result.outcome.wake && result.outcome.wake != previous_wake);
            XrXirWaitRequest request = {0};
            CHECK(call_completed(run, xr_xir_instance_wait_request(instance, result.epoch, result.outcome.wake,
                &request), XR_XIR_CALL_READY, "dual-peer-own-next-real-YIELD-request"));
            CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
                !request.subject && !request.generation && !request.ticket);
            CHECK(call_completed(run, xr_xir_instance_resume(instance, result.epoch, result.outcome.wake),
                XR_XIR_CALL_READY, "dual-peer-consume-its-own-next-wake-once"));
            previous_wake = result.outcome.wake; ++resumed; continue;
        }
        CHECK(call_completed(run, result.outcome.status, XR_XIR_CALL_RETURNED, "dual-peer-real-42-terminal"));
        CHECK(prefix - 1 + resumed == 2); break;
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(call_completed(run, xr_xir_instance_take_result(instance, &run->value), XR_XIR_CALL_RETURNED,
        "dual-peer-transfer-owned-independent-42"));
    CHECK(xr_xir_value_valid(&run->value) && run->value.type == XR_XIR_I64 && !run->value.reserved &&
        (int64_t)run->value.payload == 42);
    xr_xir_value_drop(&run->value);
    CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload);
    return resumed;
}

static void check_pending_completion_route(unsigned mode, unsigned prefix,
    NativeRouteCounts before, NativeRouteCounts after) {
    CHECK(prefix == 1 || prefix == 2);
    for (unsigned n = 0; n < 2; ++n) {
        bool native = mode == 1 || (mode == 2 ? n == 0 : n == 1);
        CHECK(native ? after.native[n] >= before.native[n] : after.vm[n] >= before.vm[n]);
        CHECK(native ? after.vm[n] == before.vm[n] : after.native[n] == before.native[n]);
        if (n == 1) CHECK(native ? after.native[n] > before.native[n] : after.vm[n] > before.vm[n]);
    }
    unsigned selected = mode == 1 || mode == 3 ? 0u : 1u;
    CHECK(after.child_suspends[selected] >= before.child_suspends[selected] &&
        after.child_suspends[selected] - before.child_suspends[selected] == 2 - prefix);
    CHECK(after.child_suspends[1 - selected] == before.child_suspends[1 - selected]);
}

static void run_dual_pending_case(ReceiverRun *run, unsigned victim, unsigned prefix) {
    CHECK(victim < 2 && (prefix == 1 || prefix == 2)); unsigned peer = 1 - victim;
    CHECK(xr_xir_instance_state(run->instances[victim]) == XR_XIR_INSTANCE_READY &&
        xr_xir_instance_state(run->instances[peer]) == XR_XIR_INSTANCE_READY);
    NativeRouteCounts before = native_route_counts(run->roles);
    XrXirInstanceResult pending = reach_cancel_prefix(run, run->instances[victim], prefix);
    check_actual_cancel_route(run->mode, prefix, before, native_route_counts(run->roles));
    ReceiverPendingSnapshot victim_snapshot = capture_pending_snapshot(run, run->instances[victim], pending);
    before = native_route_counts(run->roles);
    XrXirInstanceResult peer_pending = reach_cancel_prefix(run, run->instances[peer], prefix);
    check_actual_cancel_route(run->mode, prefix, before, native_route_counts(run->roles));
    ReceiverPendingSnapshot peer_snapshot = capture_pending_snapshot(run, run->instances[peer], peer_pending);
    /* Host tokens belong to their Instance; their numeric values may coincide. */
    check_pending_snapshot(run, run->instances[victim], &victim_snapshot);
    check_pending_snapshot(run, run->instances[peer], &peer_snapshot);
    before = native_route_counts(run->roles);
    drain_dual_cancel(run, run->instances[victim], pending, run->instances[peer], &peer_snapshot);
    check_cancel_unwind_route(run->mode, before, native_route_counts(run->roles));
    cancelled_output_diagnostics(run, run->instances[victim]);
    ++run->completed_dual_cancel[victim][prefix - 1];
    check_pending_snapshot(run, run->instances[peer], &peer_snapshot);
    before = native_route_counts(run->roles);
    unsigned peer_resumes = complete_pending_peer(run, run->instances[peer], peer_pending, prefix);
    check_pending_completion_route(run->mode, prefix, before, native_route_counts(run->roles));
    CHECK(peer_resumes == 3 - prefix);
    ++run->completed_dual_peer42[peer][prefix - 1]; ++run->completed_dual_fixed42[peer];
    before = native_route_counts(run->roles);
    execute_receiver_i64(run, run->instances[victim], run->roles.answer, 42, 2);
    CHECK(run->last_call_epoch == pending.epoch + 1);
    reject_cancelled_token(run, run->instances[victim], pending);
    check_actual_route(run->mode, before, native_route_counts(run->roles));
    ++run->completed_dual_recovered42[victim][prefix - 1]; ++run->completed_dual_fixed42[victim];
    CHECK(xr_xir_instance_state(run->instances[victim]) == XR_XIR_INSTANCE_READY &&
        xr_xir_instance_state(run->instances[peer]) == XR_XIR_INSTANCE_READY && receiver_native_owner && !receiver_native_releases);
    printf("typed-library-dual-pending mode=%u victim=%u peer=%u prefix=%u both-real-SUSPENDED=1 peer-request-bytes-unchanged=1 victim-terminal=CANCELLED peer-completion-resumes=%u peer-owned-I64=42 recovered-owned-I64=42 victim-new-epoch=1 lease=0 cross-Instance-token-uniqueness=NOT_ASSUMED\n",
        run->mode, victim, peer, prefix, peer_resumes);
}

static void run_receiver_dual_pending_instances(ReceiverRun *run, const char *c_path) {
    prepare_cancel_instances(run, c_path);
    for (unsigned victim = 0; victim < 2; ++victim)
        for (unsigned prefix = 1; prefix <= 2; ++prefix) run_dual_pending_case(run, victim, prefix);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(run->completed_dual_fixed42[i] == 4);
        for (unsigned prefix = 0; prefix < 2; ++prefix) {
            CHECK(run->completed_dual_cancel[i][prefix] == 1 && run->completed_dual_peer42[i][prefix] == 1 &&
                run->completed_dual_recovered42[i][prefix] == 1);
        }
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_READY);
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "dual-successful-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_native_owner && !receiver_native_releases);
    }
    CHECK(!receiver_native_owner && receiver_native_releases == 1);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}

static void report_dual_pending_execution(const ReceiverRun *run, bool passed, bool physical_zero) {
    printf("typed-library-dual-pending result=%s cleanup-error=%u physical-zero=%u instance0-real42=%u instance1-real42=%u expected-per-Instance=4-real42+2-CANCELLED both-pending-isolation=%s full-FI=NOT_RUN axes=NOT_RUN stable-object-lifecycle=OPEN init-failure=OPEN owned-escape=OPEN original-AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN\n",
        passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero,
        run->completed_dual_fixed42[0], run->completed_dual_fixed42[1], passed ? "PASS" : "UNQUALIFIED");
}
