/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_cancel.c - Cancellation at every actual worker activation prefix
 */
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"
static unsigned worker_cancel_finish(XrXirInstance *instance, XrXirCallStatus expected) {
    unsigned ticks = 0; XrXirInstanceResult event = {0};
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++ticks < 32768); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == expected);
    if (expected == XR_XIR_CALL_RETURNED) {
        XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
        CHECK(result.type == XR_XIR_I64 && result.payload == 7); xr_xir_value_drop(&result);
    } else CHECK(!event.outcome.value.type && !event.outcome.value.payload);
    return ticks;
}
static XrXirInstance *worker_cancel_new(XrXirProgram *program, const WorkerControlCase *oracle) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    worker_cancel_finish(instance, XR_XIR_CALL_RETURNED);
    return instance;
}
static unsigned worker_cancel_count(XrXirProgram *program, const WorkerControlCase *oracle) {
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    XrXirInstance *instance = worker_cancel_new(program, oracle);
    CHECK(xr_xir_instance_start(instance, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    unsigned ticks = worker_cancel_finish(instance, XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !worker_code_releases);
    return ticks;
}
static void worker_cancel_balance(XrXirDomain *domain, XrXirDomainBudgetStats before) {
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
    CHECK(after.work >= before.work && after.requested_bytes >= before.requested_bytes &&
        after.requested_call_bytes >= before.requested_call_bytes);
    CHECK(!after.call_live && !after.metadata_live && after.call_allocations == after.call_frees &&
        after.metadata_allocations == after.metadata_frees && xr_xir_domain_stats(domain).live_bytes == sizeof(*domain));
    xr_xir_domain_drop(domain);
}
static void worker_cancel_prefix(XrXirProgram *program, const WorkerControlCase *oracle, unsigned prefix, unsigned mode) {
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    XrXirInstance *victim = worker_cancel_new(program, oracle), *peer = worker_cancel_new(program, oracle);
    XrXirDomain *domain = victim->domain, *peer_domain = peer->domain;
    CHECK(domain != peer_domain && xr_xir_domain_retain(domain) && xr_xir_domain_retain(peer_domain));
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain), peer_before = xr_xir_domain_budget_stats(peer_domain);
    CHECK(xr_xir_instance_start(victim, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(peer, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirDomainBudgetStats waiting = xr_xir_domain_budget_stats(peer_domain);
    XrXirInstanceResult event = {0};
    for (unsigned tick = 0; tick < prefix; ++tick) {
        event = xr_xir_instance_poll_bounded(victim, 1); CHECK(event.outcome.status == XR_XIR_CALL_READY);
    }
    if (mode == 2) {
        CHECK(xr_xir_instance_free(victim) == XR_XIR_CALL_READY); victim = NULL;
    } else {
        if (mode == 1) CHECK(xr_xir_instance_stop(victim) == XR_XIR_CALL_READY);
        else { CHECK(!mode); CHECK(xr_xir_instance_cancel_current(victim) == XR_XIR_CALL_CANCEL_REQUESTED); }
        worker_cancel_finish(victim, XR_XIR_CALL_CANCELLED);
        XrXirValue output = {0}; CHECK(xr_xir_instance_take_result(victim, &output) == XR_XIR_CALL_BAD_STATE && !output.type && !output.payload);
        CHECK(xr_xir_instance_resume(victim, event.epoch, 1) == XR_XIR_CALL_BAD_STATE);
        if (mode == 1) CHECK(xr_xir_instance_start(victim, *oracle->entry, NULL, 0) == XR_XIR_CALL_BAD_STATE);
        else {
            CHECK(xr_xir_instance_start(victim, *oracle->entry, NULL, 0) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_resume(victim, event.epoch, 1) == XR_XIR_CALL_BAD_STATE);
            worker_cancel_finish(victim, XR_XIR_CALL_RETURNED);
        }
    }
    XrXirDomainBudgetStats unchanged = xr_xir_domain_budget_stats(peer_domain);
    CHECK(waiting.work == unchanged.work && waiting.requested_bytes == unchanged.requested_bytes &&
        waiting.requested_call_bytes == unchanged.requested_call_bytes);
    worker_cancel_finish(peer, XR_XIR_CALL_RETURNED);
    if (victim) CHECK(xr_xir_instance_free(victim) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(peer) == XR_XIR_CALL_READY);
    worker_cancel_balance(domain, before); worker_cancel_balance(peer_domain, peer_before);
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !worker_code_releases);
}
static int worker_cancel_main(int argc, char **argv) {
    CHECK(argc == 3); (void)worker_control_main;
    const WorkerControlCase *oracle = NULL; unsigned backend = 0;
    for (unsigned i = 0; i < 7; ++i) if (!strcmp(argv[1], worker_control_cases[i].name)) oracle = &worker_control_cases[i];
    while (backend < 4 && strcmp(argv[2], worker_backends[backend])) ++backend;
    CHECK(oracle && backend < 4); XrXirProgram *program = worker_control_program(oracle, backend);
    unsigned prefixes = worker_cancel_count(program, oracle); CHECK(prefixes);
    for (unsigned prefix = 0; prefix < prefixes; ++prefix) {
        for (unsigned mode = 0; mode < 3; ++mode) worker_cancel_prefix(program, oracle, prefix, mode);
    }
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    printf("Worker cancel case=%s backend=%s actual prefixes=%u cancel+stop+free=%u, sameDomain restart only cancel, stale reject, peer7, code lease1, physical0/0\n",
        oracle->name, worker_backends[backend], prefixes, 3*prefixes);
    return 0;
}
#ifndef XR_WORKER_CANCEL_RESOURCE_MAIN
int main(int argc, char **argv) { return worker_cancel_main(argc, argv); }
#endif
