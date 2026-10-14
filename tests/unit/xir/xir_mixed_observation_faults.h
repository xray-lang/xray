/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_observation_faults.h - Complete runtime faults and finite observed ledgers
 */
#ifndef XIR_MIXED_OBSERVATION_FAULTS_H
#define XIR_MIXED_OBSERVATION_FAULTS_H

static void observation_partial(ObservationRecord *record) {
    for (unsigned i = 0; i < record->count; ++i) {
        const ObservationFrame *frame = &record->frames[i];
        CHECK(frame->left && !frame->dispatch && frame->ends >= frame->begins);
        CHECK(frame->leave == XR_XIR_CALL_RETURNED || frame->leave == observation_expected_fault);
        if (frame->code == XR_XIR_CALL_CODE_UNKNOWN) CHECK(!frame->begins && !frame->ends && !frame->codes);
        else CHECK(frame->code == (frame->key.entry == record->code->functions ?
            XR_XIR_CALL_CODE_NATIVE : record->code->expected[frame->key.entry]));
    }
}
static void observation_cost(ObservationRecord *record, XrXirDomain *domain,
    XrXirDomainBudgetStats *previous, bool completed, unsigned round) {
    CHECK(record->instance->domain == domain);
    XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
    CHECK(spent.work >= previous->work && spent.requested_bytes >= previous->requested_bytes &&
        spent.requested_call_bytes >= previous->requested_call_bytes);
    if (completed && round) CHECK(spent.work > previous->work && spent.requested_call_bytes > previous->requested_call_bytes);
    *previous = spent;
}
static bool observation_operation(XrXirProgram *program, TerminalBackend backend,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    ObservationCode *code = observation_code;
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    observation_expected_fault = limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM;
    memset(code->records, 0, sizeof(code->records));
    XrXirDomain *domains[OBS_INSTANCES] = {0}; XrXirDomainBudgetStats previous[OBS_INSTANCES] = {{0}};
    bool complete[OBS_INSTANCES] = {false, false}, driven[OBS_INSTANCES] = {false, false};
    XrXirCallStatus statuses[OBS_INSTANCES] = {XR_XIR_CALL_READY, XR_XIR_CALL_READY};
    for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
        ObservationRecord *record = &code->records[i]; record->code = code; record->trace.kind = 3;
        record->observer = (XrXirCallObserver){record, observation_event};
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (limits) config = *limits;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, observation_output, record};
        statuses[i] = xr_xir_instance_new(program, &config, &record->instance);
        complete[i] = statuses[i] == XR_XIR_CALL_READY;
        CHECK(complete[i] || (statuses[i] == observation_expected_fault && !record->instance));
        if (record->instance) { domains[i] = record->instance->domain; CHECK(xr_xir_domain_retain(domains[i])); }
    }
    for (unsigned round = 0; round < 2; ++round) {
        bool done[OBS_INSTANCES] = {true, true}; unsigned polls = 0;
        for (unsigned i = 0; i < OBS_INSTANCES; ++i) if (complete[i]) {
            ObservationRecord *record = &code->records[i]; record->trace.count = 0;
            statuses[i] = xr_xir_instance_start(record->instance, code->roles[0], NULL, 0);
            complete[i] = statuses[i] == XR_XIR_CALL_READY; done[i] = !complete[i];
            CHECK(complete[i] || statuses[i] == observation_expected_fault);
        }
        while (!done[0] || !done[1]) for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
            if (done[i]) continue;
            ObservationRecord peer = code->records[1-i];
            statuses[i] = xr_xir_instance_poll_bounded(code->records[i].instance, 1).outcome.status;
            CHECK(++polls < 4096 && !memcmp(&peer, &code->records[1-i], sizeof(peer)));
            CHECK(statuses[i] == XR_XIR_CALL_READY || statuses[i] == XR_XIR_CALL_RETURNED || statuses[i] == observation_expected_fault);
            done[i] = statuses[i] != XR_XIR_CALL_READY;
            if (done[i]) { complete[i] = statuses[i] == XR_XIR_CALL_RETURNED; driven[i] = !complete[i]; }
        }
        for (unsigned i = 0; i < OBS_INSTANCES; ++i) if (code->records[i].instance) {
            ObservationRecord *record = &code->records[i];
            if (complete[i]) {
                CHECK(record->trace.count == 6 && record->trace.root_bodies == round + 1 && record->trace.callee_bodies == round + 1);
                CHECK(record->trace.callback_bodies == 2*(round + 1));
                XrXirValue value = {0}, occupied = {XR_XIR_I64, 0, 73}, before = occupied;
                CHECK(xr_xir_instance_take_result(record->instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
                CHECK(xr_xir_instance_take_result(record->instance, &value) == XR_XIR_CALL_RETURNED && value.type == XR_XIR_I64 && value.payload == 42);
                xr_xir_value_drop(&value);
                CHECK(xr_xir_instance_take_result(record->instance, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
            }
            observation_cost(record, domains[i], &previous[i], complete[i], round);
        }
    }
    bool all_complete = complete[0] && complete[1];
    for (unsigned i = 0; i < OBS_INSTANCES; ++i) if (code->records[i].instance) {
        ObservationRecord *record = &code->records[i];
        XrXirCallStatus expected = driven[i] || record->instance->budget.exhausted ? observation_expected_fault : XR_XIR_CALL_READY;
        XrXirCallStatus freed = xr_xir_instance_free(record->instance);
        if (freed != expected) fprintf(stderr, "Observed retirement site=%zu index=%u driven=%u status=%u expected=%u\n",
            runtime_fail_at, i, (unsigned)driven[i], freed, expected);
        CHECK(freed == expected); record->instance = NULL;
        if (complete[i]) observation_verify(record, backend); else observation_partial(record);
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domains[i]);
        CHECK(spent.work >= previous[i].work && spent.requested_bytes >= previous[i].requested_bytes &&
            spent.requested_call_bytes >= previous[i].requested_call_bytes);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes == sizeof(*domains[i]));
        if (cost) {
            if (!i) *cost = spent;
            else CHECK(spent.work == cost->work && spent.requested_bytes == cost->requested_bytes && spent.requested_call_bytes == cost->requested_call_bytes);
        }
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !terminal_code_releases);
    observation_expected_fault = XR_XIR_CALL_READY;
    return all_complete;
}
static void observation_axes(XrXirProgram *program, TerminalBackend backend) {
    XrXirDomainBudgetStats cost = {0}; CHECK(observation_operation(program, backend, NULL, &cost));
    const uint64_t exact[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(exact[axis]);
        for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            const uint64_t budget = exact[axis] - minus;
            if (!axis) config.requested_value_limit = budget;
            else if (axis == 1) config.requested_call_limit = budget;
            else config.work_limit = budget;
            CHECK(observation_operation(program, backend, &config, NULL) == !minus);
        }
    }
    printf("Observed cumulative axes backend=%s value=%llu call=%llu work=%llu, exact/minus1 same Domain two rounds\n",
        terminal_backend_names[backend], (unsigned long long)exact[0], (unsigned long long)exact[1], (unsigned long long)exact[2]);
}
static void observation_faults(XrXirProgram *program, TerminalBackend backend) {
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(observation_operation(program, backend, NULL, NULL));
    const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
    for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!observation_operation(program, backend, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Observed complete runtime FI backend=%s sites=%zu every point hit, typed OOM and physical0/0\n", terminal_backend_names[backend], sites);
}
#endif
