/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_native_mixed_class_observation.inc.c - Private owned Worker lifecycle stream
 *
 * KEY CONCEPT:
 *   The real observation owner and serial identify each constructed Worker.
 *   Natural return or cancellation must reclaim it before Instance disposal.
 */
static void class_observation_collect(ReceiverRun *run, unsigned index) {
    CHECK(index < 2 && run->class_observations[index] && run->class_counts[index] < 128);
    uint32_t amount = 0, remaining = 128 - run->class_counts[index]; bool overflow = false;
    XrXirValueStatus status = xr_xir_class_observation_drain(run->class_observations[index],
        &run->class_cursors[index], run->class_events[index] + run->class_counts[index], remaining, &amount, &overflow);
    if (status != XR_XIR_VALUE_OK)
        record_failure("PRODUCT_VALUE_STATUS", "owned-class-observation-drain", true, XR_XIR_VALUE_OK, status, NULL);
    CHECK(status == XR_XIR_VALUE_OK && !overflow && amount <= remaining);
    run->class_counts[index] += amount;
    CHECK(run->class_cursors[index] == run->class_counts[index]);
}
static void class_construct_pending(ReceiverRun *run, unsigned index, uint32_t first) {
    class_observation_collect(run, index);
    CHECK(run->class_counts[index] == first + 1);
    const XrXirClassEvent *event = &run->class_events[index][first];
    CHECK(event->serial && event->kind == XR_XIR_CLASS_CONSTRUCT && !event->flags &&
        !event->initialized_fields && event->status == XR_XIR_VALUE_OK);
    CHECK(receiver_native_owner && receiver_native_owner->lowered && !receiver_native_releases);
    const XrXirModule *module = xr_xir_compile_artifact_module(receiver_native_owner->lowered);
    const XrXirTypeNode *node = xr_xir_type_node(module->types, (XrXirType)event->type);
    CHECK(node && xr_xir_type_is_class(module->types, (XrXirType)event->type) &&
        node->nominal.declaration == run->roles.nominal && event->declaration == run->roles.nominal);
    if (first) CHECK(run->class_events[index][first - 1].serial < event->serial);
    /* The other observation owner may legitimately reuse the same numeric serial. */
}
static void class_terminal_events(ReceiverRun *run, unsigned index, uint32_t first) {
    class_observation_collect(run, index);
    CHECK(run->class_counts[index] == first + 3);
    const XrXirClassEvent *events = run->class_events[index] + first;
    const uint32_t kinds[3] = {XR_XIR_CLASS_CONSTRUCT, XR_XIR_CLASS_FINALIZE, XR_XIR_CLASS_RECLAIM};
    CHECK(events[0].serial);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(events[i].serial == events[0].serial && events[i].type == events[0].type &&
            events[i].declaration == run->roles.nominal && events[i].kind == kinds[i]);
        CHECK(!events[i].flags && !events[i].initialized_fields && events[i].status == XR_XIR_VALUE_OK);
    }
}
static void class_reject_cancelled_wake(ReceiverRun *run, unsigned index, uint64_t epoch, uint64_t wake) {
    XrXirWaitRequest wait; uint8_t before[sizeof(wait)];
    memset(&wait, 0xa5, sizeof(wait)); memcpy(before, &wait, sizeof(wait));
    CHECK(call_completed(run, xr_xir_instance_wait_request(run->instances[index], epoch, wake, &wait),
        XR_XIR_CALL_BAD_STATE, "class-events-cancelled-wake-wait-reject"));
    CHECK(!memcmp(before, &wait, sizeof(wait)));
    CHECK(call_completed(run, xr_xir_instance_resume(run->instances[index], epoch, wake),
        XR_XIR_CALL_BAD_STATE, "class-events-cancelled-wake-resume-reject"));
}
static void class_wait_request(ReceiverRun *run, unsigned index, XrXirInstanceResult step) {
    XrXirWaitRequest wait = {0};
    CHECK(call_completed(run, xr_xir_instance_wait_request(run->instances[index], step.epoch, step.outcome.wake, &wait),
        XR_XIR_CALL_READY, "class-events-actual-YIELD-request"));
    CHECK(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
        !wait.subject && !wait.generation && !wait.ticket);
}
static void class_cancel_terminal(ReceiverRun *run, unsigned index, uint64_t epoch, uint64_t wake) {
    CHECK(call_completed(run, xr_xir_instance_cancel_current(run->instances[index]),
        XR_XIR_CALL_CANCEL_REQUESTED, "class-events-public-cancel-current"));
    class_reject_cancelled_wake(run, index, epoch, wake);
    size_t polls = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult step = xr_xir_instance_poll_bounded(run->instances[index], 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&step.outcome) && step.epoch == epoch);
        if (step.outcome.status == XR_XIR_CALL_READY) continue;
        CHECK(call_completed(run, step.outcome.status, XR_XIR_CALL_CANCELLED, "class-events-natural-CANCELLED"));
        CHECK(step.outcome.value.type == XR_XIR_UNIT && !step.outcome.value.reserved && !step.outcome.value.payload &&
            !step.outcome.wake && xr_xir_panic_empty(&step.outcome.panic));
        break;
    }
    CHECK(xr_xir_instance_state(run->instances[index]) == XR_XIR_INSTANCE_READY);
    class_reject_cancelled_wake(run, index, epoch, wake);
    /* Clean cancellation returns no owned I64. This output guard is auxiliary. */
    CHECK(call_completed(run, xr_xir_instance_take_result(run->instances[index], &run->value),
        XR_XIR_CALL_BAD_STATE, "class-events-clean-cancel-empty-take-auxiliary"));
    CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload);
}
static uint64_t class_observed_activation(ReceiverRun *run, unsigned index, unsigned cancel_prefix) {
    CHECK(index < 2 && cancel_prefix <= 2 && xr_xir_instance_state(run->instances[index]) == XR_XIR_INSTANCE_READY);
    CHECK(xr_xir_instance_state(run->instances[1 - index]) == XR_XIR_INSTANCE_READY);
    uint32_t first = run->class_counts[index];
    NativeRouteCounts before = native_route_counts(run->roles);
    CHECK(call_completed(run, xr_xir_instance_start(run->instances[index], run->roles.answer, NULL, 0),
        XR_XIR_CALL_READY, "class-events-original-Worker40-child2-start"));
    uint64_t epoch = 0, previous_wake = 0; unsigned suspended = 0, resumed = 0; size_t polls = 0;
    for (;;) {
        CHECK(++polls < 4096);
        XrXirInstanceResult step = xr_xir_instance_poll_bounded(run->instances[index], 64);
        CHECK(!receiver_observer_failed && xr_xir_call_result_valid(&step.outcome) && step.epoch && step.epoch != UINT64_MAX);
        if (!epoch) epoch = step.epoch;
        CHECK(step.epoch == epoch && xr_xir_instance_state(run->instances[1 - index]) == XR_XIR_INSTANCE_READY);
        if (step.outcome.status == XR_XIR_CALL_READY) continue;
        if (step.outcome.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(suspended < 2 && step.outcome.wake && step.outcome.wake != previous_wake);
            ++suspended; class_wait_request(run, index, step); class_construct_pending(run, index, first);
            if (cancel_prefix == suspended) {
                CHECK(resumed == cancel_prefix - 1);
                class_cancel_terminal(run, index, epoch, step.outcome.wake); break;
            }
            CHECK(call_completed(run, xr_xir_instance_resume(run->instances[index], epoch, step.outcome.wake),
                XR_XIR_CALL_READY, "class-events-consume-real-YIELD-once"));
            previous_wake = step.outcome.wake; ++resumed; continue;
        }
        CHECK(!cancel_prefix && suspended == 2 && resumed == 2);
        CHECK(call_completed(run, step.outcome.status, XR_XIR_CALL_RETURNED, "class-events-normal-terminal"));
        CHECK(xr_xir_instance_state(run->instances[index]) == XR_XIR_INSTANCE_READY);
        CHECK(call_completed(run, xr_xir_instance_take_result(run->instances[index], &run->value),
            XR_XIR_CALL_RETURNED, "class-events-owned-I64-take"));
        CHECK(xr_xir_value_valid(&run->value) && run->value.type == XR_XIR_I64 && !run->value.reserved && run->value.payload == 42);
        xr_xir_value_drop(&run->value);
        CHECK(run->value.type == XR_XIR_UNIT && !run->value.reserved && !run->value.payload); break;
    }
    class_terminal_events(run, index, first);
    NativeRouteCounts after = native_route_counts(run->roles);
    if (cancel_prefix) {
        check_actual_cancel_route(run->mode, cancel_prefix, before, after);
        ++run->completed_class_cancel[index][cancel_prefix - 1];
    } else { check_actual_route(run->mode, before, after); ++run->completed_class_normal[index]; }
    CHECK(receiver_native_owner && !receiver_native_releases &&
        xr_xir_instance_state(run->instances[1 - index]) == XR_XIR_INSTANCE_READY);
    printf("typed-library-class-events instance=%u mode=%u cancel-prefix=%u serial=%llu construct=1 finalize=1 reclaim=1 before-Instance-free=1 actual-SUSPEND=%u consumed-resumes=%u terminal=%s\n",
        index, run->mode, cancel_prefix, (unsigned long long)run->class_events[index][first].serial,
        suspended, resumed, cancel_prefix ? "CANCELLED" : "RETURNED-I64-42");
    return epoch;
}
static void prepare_class_observation_instances(ReceiverRun *run, const char *c_path) {
    seal_receiver_native(run, c_path);
    XrXirInstanceConfig config;
    CHECK(call_completed(run, xr_xir_instance_config_init(&config, sizeof(config)), XR_XIR_CALL_READY, "class-events-config-init"));
    config.value_limit = UINT64_C(1048576);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(call_completed(run, xr_xir_instance_new(run->program, &config, &run->instances[i]),
            XR_XIR_CALL_READY, "class-events-Instance-new") && run->instances[i]);
        CHECK(xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW);
        CHECK(call_completed(run, xr_xir_instance_class_observation_open(run->instances[i], 128, &run->class_observations[i]),
            XR_XIR_CALL_READY, "class-events-real-owned-stream-open-before-first-start") && run->class_observations[i]);
    }
    CHECK(run->instances[0] != run->instances[1] && run->class_observations[0] != run->class_observations[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    CHECK(receiver_native_owner && !receiver_native_releases);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(call_completed(run, xr_xir_instance_start(run->instances[i], run->roles.private_answer, NULL, 0),
            XR_XIR_CALL_BAD_ARGUMENT, "class-events-private-NEW-answer-permission-reject"));
        CHECK(runtime_attempts == attempts && xr_xir_instance_state(run->instances[i]) == XR_XIR_INSTANCE_NEW);
        execute_receiver_i64(run, run->instances[i], run->roles.entry, 0, 0);
        class_observation_collect(run, i); CHECK(!run->class_counts[i] && !run->class_cursors[i]);
    }
}
static void release_class_observations(ReceiverRun *run) {
    /* Every pointer lives in static Run, including partial open and first-error paths. */
    for (unsigned i = 0; i < 2; ++i) {
        xr_xir_class_observation_drop(&run->class_observations[i]);
        if (run->class_observations[i]) {
            receiver_cleanup_failed = true;
            record_failure("PRODUCT_VALUE_CLEANUP", "class-events-owned-stream-drop-unproved", false, 0, 0, NULL);
        }
    }
}
static void finish_class_observation_instances(ReceiverRun *run) {
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(run->class_counts[i] == 15 && run->class_cursors[i] == 15);
        CHECK(run->completed_class_normal[i] == 3 && run->completed_class_cancel[i][0] == 1 && run->completed_class_cancel[i][1] == 1);
        XrXirCallStatus released = xr_xir_instance_free(run->instances[i]);
        if (released != XR_XIR_CALL_BUSY) run->instances[i] = NULL;
        CHECK(call_completed(run, released, XR_XIR_CALL_READY, "class-events-successful-Instance-free"));
        CHECK(!receiver_observer_failed);
        if (i == 0) CHECK(receiver_native_owner && !receiver_native_releases);
        else CHECK(!receiver_native_owner && receiver_native_releases == 1);
        class_observation_collect(run, i); CHECK(run->class_counts[i] == 15 && run->class_cursors[i] == 15);
    }
    /* Both observations still own their Domain leases after the last code lease drop. */
    for (unsigned i = 0; i < 2; ++i) {
        class_observation_collect(run, i); CHECK(run->class_counts[i] == 15 && run->class_cursors[i] == 15);
    }
    release_class_observations(run);
    CHECK(!receiver_cleanup_failed && !run->class_observations[0] && !run->class_observations[1]);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
}
static void run_receiver_class_observation_instances(ReceiverRun *run, const char *c_path) {
    prepare_class_observation_instances(run, c_path);
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t epoch = class_observed_activation(run, i, 0);
        for (unsigned prefix = 1; prefix <= 2; ++prefix) {
            uint64_t cancelled_epoch = class_observed_activation(run, i, prefix);
            CHECK(cancelled_epoch == epoch + 1);
            epoch = class_observed_activation(run, i, 0); CHECK(epoch == cancelled_epoch + 1);
        }
    }
    finish_class_observation_instances(run);
}
static void report_class_observation_execution(const ReceiverRun *run, bool passed, bool physical_zero) {
    printf("typed-library-private24-class-events result=%s cleanup-error=%u physical-zero=%u instance0-real42=%u instance1-real42=%u instance0-events=%u instance1-events=%u expected-per-owner=5-construct+5-finalize+5-reclaim capacity=128 overflow=0 private-contract=Value24 NOT_ADOPTED full-FI=NOT_RUN axes=NOT_RUN init-failure=OPEN owned-escape=OPEN original-AOT=OPEN deterministic-C=OPEN SourceDelete=OPEN SDK-cross-DLL=OPEN strong-dual-pending-events=DEFERRED\n",
        passed ? "PASS" : "FAIL", (unsigned)receiver_cleanup_failed, (unsigned)physical_zero,
        run->completed_class_normal[0], run->completed_class_normal[1], run->class_counts[0], run->class_counts[1]);
}
