/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_identity_cases.h - Independent root authority and lifetime oracles
 */
#ifndef XIR_ROOT_IDENTITY_CASES_H
#define XIR_ROOT_IDENTITY_CASES_H
static XrXirInstanceResult ri_poll(RiWitness *w) {
    XrXirInstanceResult result = {0};
    unsigned steps = 0;
    do {
        result = xr_xir_instance_poll_bounded(w->instance, 1);
        CHECK(++steps < 3000);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}
static void ri_expired_view(RiWitness *w) {
    CHECK(w->saved_root_live && w->saved_root.activation);
    ri_denied_read(&w->saved_root);
}
static void ri_start_root(RiWitness *w) {
    w->root_call = NULL; w->saved_root_live = false; w->child_local = 0;
    CHECK(xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0) == XR_XIR_CALL_READY);
}
static XrXirInstanceResult ri_suspended(RiWitness *w) {
    XrXirInstanceResult suspended = ri_poll(w);
    CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED && suspended.epoch && suspended.outcome.wake);
    CHECK(w->await_count && w->host_suspend_count && w->child_local == 1);
    CHECK(w->calls[RI_CHILD_EXIT] && w->releases[RI_CHILD_EXIT]);
    CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_READY);
    ri_expired_view(w);
    return suspended;
}
static void ri_returned(RiWitness *w) {
    XrXirInstanceResult result = ri_poll(w);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && !result.outcome.wake);
    CHECK(result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.reserved && result.outcome.value.payload == 42);
    XrXirValue owned = {0};
    CHECK(xr_xir_instance_take_result(w->instance, &owned) == XR_XIR_CALL_RETURNED);
    CHECK(owned.type == XR_XIR_I64 && !owned.reserved && owned.payload == 42);
    xr_xir_value_drop(&owned);
    CHECK(w->root_cleanup_reads && w->child_cleanup_denials);
    ri_expired_view(w);
}
static int64_t ri_counter(RiWitness *w) {
    CHECK(w->function.type == RI_FUNCTION_TYPE);
    /* A successful new start releases the previous Call. Its borrowed view
     * must never be probed again, even when allocator addresses happen to match. */
    w->saved_root_live = false;
    CHECK(xr_xir_instance_start_function(w->instance, &w->function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = ri_poll(w);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(w->instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(value.type == XR_XIR_I64 && !value.reserved);
    int64_t counter = value.payload; xr_xir_value_drop(&value);
    return counter;
}
static void ri_output_trace(const RiWitness *w, unsigned repeats) {
    CHECK(w->output_count == repeats * 3 && w->output_denials == w->output_count);
    for (unsigned i = 0; i < repeats; ++i) {
        CHECK(w->outputs[i * 3] == 6);
        CHECK(w->outputs[i * 3 + 1] == 42);
        CHECK(w->outputs[i * 3 + 2] == 5);
    }
}
static void ri_lifecycle_trace(const RiWitness *w, bool failed) {
    const RiTrace complete[] = {
        {XR_XIR_MODULE_BEGIN, 0}, {XR_XIR_SLOT_PUBLISHED, 0}, {XR_XIR_SLOT_PUBLISHED, 1},
        {XR_XIR_MODULE_READY, 0}, {XR_XIR_SLOT_RELEASED, 1}, {XR_XIR_SLOT_RELEASED, 0}
    };
    const RiTrace failure[] = {
        {XR_XIR_MODULE_BEGIN, 0}, {XR_XIR_SLOT_PUBLISHED, 0}, {XR_XIR_SLOT_PUBLISHED, 1},
        {XR_XIR_SLOT_RELEASED, 1}, {XR_XIR_SLOT_RELEASED, 0}
    };
    const RiTrace *expected = failed ? failure : complete;
    unsigned count = failed ? 5 : 6;
    CHECK(w->lifecycle_count == count && w->trace_denials == 2);
    for (unsigned i = 0; i < count; ++i)
        CHECK(w->lifecycle[i].event == expected[i].event && w->lifecycle[i].index == expected[i].index);
}
static void ri_cross_gate(RiFixture *f) {
    RiWitness *a = &f->witnesses[0], *b = &f->witnesses[1];
    CHECK(a->function.type == RI_FUNCTION_TYPE && b->function.type == RI_FUNCTION_TYPE);
    unsigned before[RI_FUNCTIONS]; memcpy(before, b->calls, sizeof(before));
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_start_function(b->instance, &a->function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(before, b->calls, sizeof(before)) && runtime_attempts == attempts);
    CHECK(xr_xir_instance_start_function(a->instance, &b->function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
}
static void ri_finish(RiFixture *f, bool failed) {
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &f->witnesses[i];
        w->saved_root_live = false; w->returned_view = NULL;
        CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_READY);
        w->instance = NULL; f->witnesses[1 - i].peer = NULL;
        ri_lifecycle_trace(w, failed);
        CHECK(w->copied_view_denials && w->altered_view_denials == w->copied_view_denials * 8);
        unsigned releases = 0;
        for (unsigned j = 0; j < RI_FUNCTIONS; ++j) releases += w->releases[j];
        CHECK(releases == w->release_denials && w->releases[RI_INIT] == 1);
    }
    if (!failed) CHECK(!f->code_releases && runtime_live && runtime_bytes);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) xr_xir_value_drop(&f->witnesses[i].function);
    CHECK(f->code_releases == 1 && !f->lowered);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    ri_observed = NULL;
}
static void ri_identity_case(void) {
    RiFixture fixture; ri_fixture(&fixture, false); ri_instances(&fixture);
    for (unsigned round = 0; round < 2; ++round) {
        XrXirInstanceResult suspended[RI_INSTANCES];
        for (unsigned i = 0; i < RI_INSTANCES; ++i) {
            RiWitness *w = &fixture.witnesses[i]; ri_start_root(w);
            suspended[i] = ri_suspended(w);
        }
        ri_cross_gate(&fixture);
        for (unsigned i = 0; i < RI_INSTANCES; ++i) {
            RiWitness *w = &fixture.witnesses[i];
            CHECK(xr_xir_instance_resume(w->instance, suspended[i].epoch, suspended[i].outcome.wake) == XR_XIR_CALL_READY);
            ri_returned(w); CHECK(ri_counter(w) == (round == 0 ? 11 : 22));
            ri_output_trace(w, round + 1);
            CHECK(w->root_cleanup_slot == (round == 0 ? 11 : 22));
        }
    }
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        CHECK(fixture.witnesses[i].releases[RI_ROOT] == 2 && fixture.witnesses[i].releases[RI_CHILD] == 2);
        CHECK(fixture.witnesses[i].releases[RI_ROOT_EXIT] == 2 && fixture.witnesses[i].releases[RI_CHILD_EXIT] == 2);
        CHECK(fixture.witnesses[i].releases[RI_READ] == 2);
    }
    ri_finish(&fixture, false);
    puts("root identity VM: two interleaved Instances each11/22, child42, output6/42/5, gates, physical=0/0");
}
static void ri_bad_wait(RiWitness *w, XrXirInstanceResult suspended) {
    unsigned before[RI_FUNCTIONS]; memcpy(before, w->calls, sizeof(before));
    CHECK(xr_xir_instance_start(w->instance, RI_READ, NULL, 0) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_resume(w->instance, suspended.epoch + 1, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(w->instance, suspended.epoch, suspended.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(w->instance, suspended.epoch, 0) == XR_XIR_CALL_BAD_STATE);
    XrXirWaitRequest sentinel, saved; memset(&sentinel, 0xa5, sizeof(sentinel)); saved = sentinel;
    CHECK(xr_xir_instance_wait_request(w->instance, suspended.epoch + 1, suspended.outcome.wake,
        &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(sentinel)));
    CHECK(!memcmp(before, w->calls, sizeof(before)));
    XrXirWaitRequest request = {0};
    CHECK(xr_xir_instance_wait_request(w->instance, suspended.epoch, suspended.outcome.wake,
        &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
        !request.subject && !request.generation && !request.ticket);
}
static void ri_resume_case(void) {
    RiFixture fixture; ri_fixture(&fixture, false); ri_instances(&fixture);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &fixture.witnesses[i]; ri_start_root(w);
        XrXirInstanceResult old = ri_suspended(w); ri_bad_wait(w, old);
        CHECK(xr_xir_instance_cancel_current(w->instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(ri_poll(w).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(w->root_cleanup_slot == 11 && xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_READY);
        ri_output_trace(w, 1); ri_expired_view(w);
        ri_start_root(w);
        XrXirInstanceResult fresh = ri_suspended(w);
        CHECK(fresh.epoch == old.epoch + 1 && fresh.outcome.wake == old.outcome.wake);
        CHECK(xr_xir_instance_resume(w->instance, old.epoch, old.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_resume(w->instance, fresh.epoch, fresh.outcome.wake) == XR_XIR_CALL_READY);
        ri_returned(w); CHECK(ri_counter(w) == 22); ri_output_trace(w, 2);
    }
    ri_cross_gate(&fixture); ri_finish(&fixture, false);
    puts("root identity VM: scoped wrong/stale wake refusal, cancel cleanup, retry22, physical=0/0");
}
static void ri_closing_case(void) {
    RiFixture fixture; ri_fixture(&fixture, false); ri_instances(&fixture);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &fixture.witnesses[i]; ri_start_root(w);
        unsigned steps = 0;
        while (!w->await_count || !w->calls[RI_CHILD]) {
            CHECK(xr_xir_instance_poll_bounded(w->instance, 1).outcome.status == XR_XIR_CALL_READY);
            CHECK(++steps < 3000);
        }
        CHECK(!w->calls[RI_CHILD_EXIT] && !w->output_count);
        XrXirTaskWaitToken waiting = {0};
        CHECK(xr_xir_call_state(w->root_call) == XR_XIR_CALL_SUSPENDED);
        CHECK(xr_xir_call_task_wait_token(w->root_call, &waiting));
        CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
        CHECK(w->output_count == 2 && w->outputs[0] == 6 && w->outputs[1] == 5 && w->output_denials == 2);
        CHECK(w->root_cleanup_slot == 11 && w->root_cleanup_reads && w->child_cleanup_denials);
        CHECK(w->releases[RI_ROOT] == 1 && w->releases[RI_CHILD] == 1);
        CHECK(w->releases[RI_ROOT_EXIT] == 1 && w->releases[RI_CHILD_EXIT] == 1);
        ri_expired_view(w);
        unsigned before[RI_FUNCTIONS]; memcpy(before, w->calls, sizeof(before));
        CHECK(xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_start_function(w->instance, &w->function, NULL, 0) == XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(before, w->calls, sizeof(before)));
        CHECK(xr_xir_instance_resume(w->instance, 1, 1) == XR_XIR_CALL_BAD_STATE);
    }
    ri_finish(&fixture, false);
    puts("root identity VM: root awaiting child at stop, original cleanup principals, output6/5, closed gate, physical=0/0");
}
static void ri_initialization_failure_case(void) {
    RiFixture fixture; ri_fixture(&fixture, true); ri_instances(&fixture);
    XrXirCallResult escaped[RI_INSTANCES] = {0};
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &fixture.witnesses[i]; ri_start_root(w);
        XrXirInstanceResult result = ri_poll(w);
        CHECK(result.outcome.status == XR_XIR_CALL_MATCH_FAILURE && result.outcome.panic.detail.code == 442);
        CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_FAILED);
        CHECK(!w->calls[RI_ROOT] && !w->calls[RI_CHILD] && !w->output_count && !w->function.type);
        unsigned before[RI_FUNCTIONS]; memcpy(before, w->calls, sizeof(before));
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            CHECK(xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0) == XR_XIR_CALL_MATCH_FAILURE);
            XrXirInstanceResult repeated = ri_poll(w);
            CHECK(repeated.outcome.status == XR_XIR_CALL_MATCH_FAILURE && repeated.epoch == result.epoch);
            CHECK(!memcmp(&repeated.outcome.panic.detail, &result.outcome.panic.detail, sizeof(result.outcome.panic.detail)));
            XrXirValue occupied = {XR_XIR_I64, 0, 909}, saved = occupied;
            CHECK(xr_xir_instance_take_result(w->instance, &occupied) == XR_XIR_CALL_BAD_STATE);
            CHECK(!memcmp(&occupied, &saved, sizeof(occupied)));
        }
        CHECK(!memcmp(before, w->calls, sizeof(before)));
        CHECK(xr_xir_instance_copy_failure(w->instance, &escaped[i]) == XR_XIR_CALL_MATCH_FAILURE);
        CHECK(escaped[i].status == XR_XIR_CALL_MATCH_FAILURE && !escaped[i].wake &&
            escaped[i].panic.detail.code == 442 && !escaped[i].panic.detail.reserved &&
            !escaped[i].panic.detail.index && !escaped[i].panic.detail.length);
    }
    ri_finish(&fixture, true);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        CHECK(escaped[i].status == XR_XIR_CALL_MATCH_FAILURE && escaped[i].panic.detail.code == 442);
        xr_xir_call_result_drop(&escaped[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("root identity VM: initialization failure442 sticky, no root body, reverse publication release, physical=0/0");
}
#endif // XIR_ROOT_IDENTITY_CASES_H
