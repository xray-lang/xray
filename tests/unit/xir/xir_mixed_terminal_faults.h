/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_terminal_faults.h - Terminal transactions retain one cumulative owner
 *
 * KEY CONCEPT:
 *   Real source bodies execute twice on one Domain before its physical retirement.
 */
#ifndef XIR_MIXED_TERMINAL_FAULTS_H
#define XIR_MIXED_TERMINAL_FAULTS_H

static const char *terminal_fault_names[] = {
    "return", "throw", "panic", "cancel-return", "cancel-throw", "cancel-panic", "crossed", "closure"
};
typedef struct TerminalFaultTrace { unsigned scenario, count; } TerminalFaultTrace;
static XrXirOutputStatus terminal_fault_output(void *pointer, const XrXirOutputGroup *group) {
    TerminalFaultTrace *trace = pointer;
    CHECK(trace->scenario < 7 && group && group->count == 1 && group->line && group->stream == XR_XIR_STDOUT);
    CHECK(group->values[0].type == XR_XIR_I64);
    int64_t value = group->values[0].payload;
    if (trace->scenario == 6) {
        const int64_t expected[] = {39,40,41,42,41,43};
        CHECK(trace->count < 6 && value == expected[trace->count]);
        ++trace->count;
    } else {
        int64_t outer = (int64_t)(trace->scenario % 3 + 1) * 10;
        /* An early failure may precede registration of the inner cleanup. */
        CHECK((!trace->count && value == outer + 1) || (trace->count <= 1 && value == outer));
        trace->count = value == outer ? 2u : 1u;
    }
    return XR_XIR_OUTPUT_OK;
}
static void terminal_fault_payload(unsigned scenario, const XrXirCallResult *outcome) {
    if (!scenario || scenario == 7) terminal_text(&outcome->value, scenario ? "captured" : "held");
    else if (scenario == 1) {
        XrXirEnumBorrow error = {0};
        CHECK(xr_xir_enum_borrow(&outcome->value, &error) == XR_XIR_VALUE_OK);
        CHECK(error.name.length == 13 && !memcmp(error.name.bytes, "TerminalFault", 13));
        CHECK(error.member.length == 6 && !memcmp(error.member.bytes, "Failed", 6) && error.field_count == 1);
        terminal_text(&error.fields[0], "held-error");
    } else if (scenario == 2) CHECK(outcome->panic.detail.code == 420 && outcome->value.type == XR_XIR_UNIT);
    else if (scenario == 6) CHECK(outcome->value.type == XR_XIR_I64 && outcome->value.payload == 42);
}
static XrXirCallResult terminal_fault_poll(XrXirInstance *instance, unsigned *polls) {
    XrXirCallResult result;
    do {
        result = xr_xir_instance_poll_bounded(instance, 1).outcome;
        CHECK(++*polls < 4096);
    } while (result.status == XR_XIR_CALL_READY);
    return result;
}
static XrXirCallStatus terminal_fault_copy(const XrXirCallResult *outcome, XrXirCallResult *retained) {
    XrXirValueStatus copied = xr_xir_call_result_copy(outcome, retained);
    if (copied == XR_XIR_VALUE_OK) return outcome->status;
    CHECK((copied == XR_XIR_VALUE_LIMIT || copied == XR_XIR_VALUE_OOM) && xr_xir_call_result_empty(retained));
    return copied == XR_XIR_VALUE_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM;
}
static XrXirCallStatus terminal_fault_closure(XrXirInstance *instance, XrXirValue *function,
    XrXirCallResult *retained, bool *driven_fault) {
    CHECK(xr_xir_instance_take_result(instance, function) == XR_XIR_CALL_RETURNED);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(function);
    CHECK(binding && binding->capture_count == 1);
    terminal_text(&binding->captures[0], "captured");
    XrXirCallStatus status = xr_xir_instance_start_function(instance, function, NULL, 0);
    if (status != XR_XIR_CALL_READY) return status;
    unsigned polls = 0; XrXirCallResult outcome = terminal_fault_poll(instance, &polls);
    *driven_fault = outcome.status == XR_XIR_CALL_OOM || outcome.status == XR_XIR_CALL_LIMIT;
    if (outcome.status != XR_XIR_CALL_RETURNED) return outcome.status;
    status = terminal_fault_copy(&outcome, retained);
    if (status != XR_XIR_CALL_RETURNED) return status;
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    terminal_text(&value, "captured"); xr_xir_value_drop(&value);
    return outcome.status;
}
static bool terminal_fault_operation(XrXirProgram *program, unsigned scenario,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    const XrXirCallStatus expected[] = {XR_XIR_CALL_RETURNED, XR_XIR_CALL_THROWN, XR_XIR_CALL_DIVIDE_BY_ZERO,
        XR_XIR_CALL_CANCELLED, XR_XIR_CALL_CANCELLED, XR_XIR_CALL_CANCELLED, XR_XIR_CALL_RETURNED, XR_XIR_CALL_RETURNED};
    const XrXirCallStatus fault = limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM;
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    TerminalFaultTrace trace = {scenario,0}; XrXirInstanceConfig config;
    CHECK(scenario < 8 && xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,terminal_fault_output,&trace};
    XrXirInstance *instance = NULL; XrXirDomain *domain = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    bool completed = status == XR_XIR_CALL_READY, driven_fault = false;
    XrXirCallResult retained[2] = {{0}}; XrXirValue functions[2] = {{0}};
    if (instance) { domain = instance->domain; CHECK(xr_xir_domain_retain(domain)); }
    XrXirDomainBudgetStats previous = {0};
    for (unsigned round = 0; completed && round < 2; ++round) {
        trace.count = 0;
        unsigned entry = scenario < 6 ? scenario % 3 : scenario - 3;
        status = xr_xir_instance_start(instance, terminal_fixture_functions[entry], NULL, 0);
        if (status == XR_XIR_CALL_READY) {
            unsigned polls = 0; XrXirCallResult outcome = terminal_fault_poll(instance, &polls);
            if (outcome.status == XR_XIR_CALL_SUSPENDED) {
                CHECK(scenario < 6 && !trace.count);
                uint64_t epoch = instance->epoch, wake = outcome.wake;
                CHECK(epoch && wake);
                CHECK(xr_xir_instance_resume(instance, epoch + 1, wake) == XR_XIR_CALL_BAD_STATE);
                CHECK(xr_xir_instance_resume(instance, epoch, wake + 1) == XR_XIR_CALL_BAD_STATE);
                CHECK((scenario >= 3 ? xr_xir_instance_cancel_current(instance) :
                    xr_xir_instance_resume(instance, epoch, wake)) ==
                    (scenario >= 3 ? XR_XIR_CALL_CANCEL_REQUESTED : XR_XIR_CALL_READY));
                outcome = terminal_fault_poll(instance, &polls);
                CHECK(xr_xir_instance_resume(instance, epoch, wake) == XR_XIR_CALL_BAD_STATE);
            }
            status = outcome.status;
            driven_fault = status == fault;
            if (status == expected[scenario]) {
                CHECK(trace.count == (scenario < 6 ? 2u : scenario == 6 ? 6u : 0u));
                if (scenario == 7) status = terminal_fault_closure(instance, &functions[round], &retained[round], &driven_fault);
                else {
                    status = terminal_fault_copy(&outcome, &retained[round]);
                    if (status == expected[scenario]) terminal_fault_payload(scenario, &retained[round]);
                    if (status == expected[scenario] && (scenario == 0 || scenario == 1 || scenario == 6)) {
                        XrXirValue occupied = {XR_XIR_I64,0,73}, original = occupied, value = {0};
                        CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
                        CHECK(!memcmp(&occupied, &original, sizeof(occupied)));
                        CHECK(xr_xir_instance_take_result(instance, &value) == expected[scenario]);
                        xr_xir_value_drop(&value);
                        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
                    }
                }
            }
        }
        completed = status == expected[scenario];
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        if (completed && round) CHECK(spent.work > previous.work && spent.requested_call_bytes > previous.requested_call_bytes);
        previous = spent;
    }
    if (!completed && status != fault)
        fprintf(stderr, "terminal fault scenario=%s site=%zu status=%u expected=%u\n", terminal_fault_names[scenario], runtime_fail_at, status, fault);
    CHECK(completed || status == fault);
    bool exhausted = instance && instance->budget.exhausted;
    CHECK(!exhausted || fault == XR_XIR_CALL_LIMIT);
    /* A failed replacement can exhaust the shared budget before it is polled. */
    XrXirCallStatus expected_release = driven_fault || exhausted ? fault : XR_XIR_CALL_READY;
    XrXirCallStatus released = xr_xir_instance_free(instance);
    if (released != expected_release)
        fprintf(stderr, "terminal retirement scenario=%s site=%zu completed=%u prior=%u driven=%u released=%u caps=%llu/%llu/%llu spent=%llu/%llu/%llu\n",
            terminal_fault_names[scenario], runtime_fail_at, (unsigned)completed, status, (unsigned)driven_fault, released,
            (unsigned long long)config.requested_value_limit, (unsigned long long)config.requested_call_limit,
            (unsigned long long)config.work_limit, (unsigned long long)previous.requested_bytes,
            (unsigned long long)previous.requested_call_bytes, (unsigned long long)previous.work);
    CHECK(released == expected_release);
    for (unsigned round = 0; round < 2; ++round) {
        if (!xr_xir_call_result_empty(&retained[round])) {
            terminal_fault_payload(scenario, &retained[round]); xr_xir_call_result_drop(&retained[round]);
        }
        if (functions[round].type) {
            const XrXirFunctionBinding *binding = xr_xir_function_binding(&functions[round]);
            CHECK(binding && binding->capture_count == 1); terminal_text(&binding->captures[0], "captured");
            xr_xir_value_drop(&functions[round]);
        }
    }
    if (domain) {
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domain).live_bytes == sizeof(*domain));
        if (cost) *cost = spent;
        xr_xir_domain_drop(domain);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes);
    return completed;
}
static void terminal_fault_cases(TerminalBackend backend, bool fi) {
    XrXirProgram *program = terminal_program(backend); size_t total = 0;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        XrXirDomainBudgetStats cost = {0}; runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
        CHECK(terminal_fault_operation(program, scenario, NULL, &cost)); size_t sites = runtime_attempts;
        const uint64_t exact[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            CHECK(exact[axis]);
            if (!axis) config.requested_value_limit = exact[axis] - minus;
            else if (axis == 1) config.requested_call_limit = exact[axis] - minus;
            else config.work_limit = exact[axis] - minus;
            CHECK(terminal_fault_operation(program, scenario, &config, NULL) == !minus);
        }
        if (fi) for (size_t site = 0; site < sites; ++site) {
            runtime_attempts = 0; runtime_fail_at = site;
            CHECK(!terminal_fault_operation(program, scenario, NULL, NULL) && runtime_attempts > site);
        }
        runtime_fail_at = SIZE_MAX; total += sites;
        printf("TERMINAL_OWNER backend=%s scenario=%s mode=%s sites=%zu value/call/work=%llu/%llu/%llu twoRounds sameDomain exact/minus1 escaped physical=0/0\n",
            terminal_backend_names[backend], terminal_fault_names[scenario], fi ? "fi" : "axes", sites,
            (unsigned long long)exact[0], (unsigned long long)exact[1], (unsigned long long)exact[2]);
    }
    xr_xir_compile_program_drop(program);
    CHECK(terminal_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    printf("TERMINAL_OWNER_TOTAL backend=%s mode=%s scenarios=8 sites=%zu codeReleases=1 compilerRuntimePhysical=0/0\n",
        terminal_backend_names[backend], fi ? "fi" : "axes", total);
}
#endif // XIR_MIXED_TERMINAL_FAULTS_H
