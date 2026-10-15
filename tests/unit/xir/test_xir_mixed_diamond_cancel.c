/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_diamond_cancel.c - Every unfinished Source initialization prefix retires once
 */
#define XR_DIAMOND_RESOURCE_MAIN
#include "test_xir_mixed_diamond.c"
typedef struct DiamondCancelTrace {
    unsigned begins[4], ready[4], published[6], released[6], publications, releases;
} DiamondCancelTrace;
typedef struct DiamondCancelCoverage { unsigned cursors, publications; } DiamondCancelCoverage;
static void diamond_cancel_trace(void *owner, XrXirLifecycleEvent event, uint32_t index) {
    DiamondCancelTrace *trace = owner;
    if (event == XR_XIR_MODULE_BEGIN || event == XR_XIR_MODULE_READY) {
        CHECK(index < 4); unsigned *count = event == XR_XIR_MODULE_BEGIN ? &trace->begins[index] : &trace->ready[index];
        CHECK(++*count == 1);
    } else {
        CHECK(index < 6);
        if (event == XR_XIR_SLOT_PUBLISHED) { CHECK(trace->publications < 6); trace->published[trace->publications++] = index; }
        else { CHECK(event == XR_XIR_SLOT_RELEASED && trace->releases < 6); trace->released[trace->releases++] = index; }
    }
}
static XrXirInstance *diamond_cancel_new(XrXirProgram *program, unsigned index, DiamondCancelTrace *trace) {
    CHECK(index < 2); DiamondRecord *record = &diamond_code->records[index];
    memset(record, 0, sizeof(*record)); record->code = diamond_code;
    record->observer = (XrXirCallObserver){record, diamond_event};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.trace = diamond_cancel_trace; config.trace_context = trace;
    CHECK(xr_xir_instance_new(program, &config, &record->instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(record->instance, diamond_entry, NULL, 0) == XR_XIR_CALL_READY);
    return record->instance;
}
static void diamond_cancel_finish(XrXirInstance *instance, XrXirCallStatus expected) {
    XrXirInstanceResult event = {0}; unsigned polls = 0;
    do { event = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 4096); }
    while (event.outcome.status == XR_XIR_CALL_READY);
    CHECK(event.outcome.status == expected);
    if (expected == XR_XIR_CALL_RETURNED) {
        XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        diamond_value(&value); xr_xir_value_drop(&value);
    } else CHECK(expected == XR_XIR_CALL_CANCELLED && !event.outcome.value.type && !event.outcome.value.payload);
}
static void diamond_cancel_reverse(const DiamondCancelTrace *trace, const DiamondCancelTrace *before) {
    CHECK(trace->publications == before->publications && trace->releases == before->publications);
    CHECK(!memcmp(trace->begins, before->begins, sizeof(trace->begins)) &&
        !memcmp(trace->ready, before->ready, sizeof(trace->ready)));
    for (unsigned i = 0; i < trace->releases; ++i) CHECK(trace->released[i] == before->published[before->publications-1-i]);
}
static void diamond_cancel_balance(XrXirDomain *domain, XrXirDomainBudgetStats before) {
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
    CHECK(after.work >= before.work && after.requested_bytes >= before.requested_bytes && after.requested_call_bytes >= before.requested_call_bytes);
    CHECK(!after.call_live && !after.metadata_live && after.call_allocations == after.call_frees && after.metadata_allocations == after.metadata_frees);
    CHECK(xr_xir_domain_stats(domain).live_bytes == sizeof(*domain)); xr_xir_domain_drop(domain);
}
static unsigned diamond_cancel_prefix_count(XrXirProgram *program, unsigned backend) {
    const size_t blocks = runtime_live, bytes = runtime_bytes; DiamondCancelTrace trace = {0};
    XrXirInstance *instance = diamond_cancel_new(program, 0, &trace); unsigned ticks = 0;
    while (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_INITIALIZING) {
        CHECK(!diamond_code->records[0].roots);
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY && ++ticks < 4096);
    }
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && trace.publications == 6 && !trace.releases);
    diamond_cancel_finish(instance, XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance, diamond_entry, NULL, 0) == XR_XIR_CALL_READY);
    diamond_cancel_finish(instance, XR_XIR_CALL_RETURNED); diamond_verify(&diamond_code->records[0], backend);
    CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); diamond_code->records[0].instance = NULL;
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !diamond_code_releases); return ticks;
}
static void diamond_cancel_sticky(XrXirInstance *instance, const DiamondCancelTrace *trace) {
    const uint64_t epoch = instance->epoch; DiamondCancelTrace unchanged = *trace;
    CHECK(instance->state == XR_XIR_INSTANCE_FAILED && !instance->publication_count);
    XrXirValue occupied = {XR_XIR_I64,0,73}, before = occupied;
    CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_STATE && !memcmp(&occupied, &before, sizeof(before)));
    XrXirCallResult held = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,73}}, original = held;
    CHECK(xr_xir_instance_copy_failure(instance, &held) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&held, &original, sizeof(original)));
    for (unsigned repeat = 0; repeat < 24; ++repeat) {
        CHECK(xr_xir_instance_start(instance, diamond_entry, NULL, 0) == XR_XIR_CALL_CANCELLED);
        XrXirInstanceResult event = xr_xir_instance_poll_bounded(instance, 1);
        CHECK(event.outcome.status == XR_XIR_CALL_CANCELLED && event.epoch == epoch);
        XrXirCallResult copied = {0}; CHECK(xr_xir_instance_copy_failure(instance, &copied) == XR_XIR_CALL_CANCELLED);
        CHECK(!copied.value.type && !copied.value.payload && xr_xir_panic_empty(&copied.panic)); xr_xir_call_result_drop(&copied);
    }
    CHECK(!memcmp(trace, &unchanged, sizeof(unchanged)) && !diamond_code->records[0].roots);
}
static void diamond_cancel_prefix(XrXirProgram *program, unsigned backend, unsigned prefix,
    unsigned mode, DiamondCancelCoverage *coverage) {
    const size_t blocks = runtime_live, bytes = runtime_bytes; DiamondCancelTrace trace[2] = {{0}};
    XrXirInstance *victim = diamond_cancel_new(program, 0, &trace[0]), *peer = diamond_cancel_new(program, 1, &trace[1]);
    XrXirDomain *domain = victim->domain, *peer_domain = peer->domain;
    CHECK(domain != peer_domain && xr_xir_domain_retain(domain) && xr_xir_domain_retain(peer_domain));
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain), peer_before = xr_xir_domain_budget_stats(peer_domain);
    DiamondRecord peer_record = diamond_code->records[1]; DiamondCancelTrace peer_trace = trace[1];
    for (unsigned tick = 0; tick < prefix; ++tick) CHECK(xr_xir_instance_poll_bounded(victim, 1).outcome.status == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_state(victim) == XR_XIR_INSTANCE_INITIALIZING && !diamond_code->records[0].roots && victim->cursor < 4);
    coverage->cursors |= 1u << victim->cursor; coverage->publications |= 1u << trace[0].publications;
    DiamondCancelTrace published = trace[0]; diamond_expected_fault = XR_XIR_CALL_CANCELLED;
    if (mode == 2) { CHECK(xr_xir_instance_free(victim) == XR_XIR_CALL_READY); victim = NULL; }
    else {
        if (mode == 1) CHECK(xr_xir_instance_stop(victim) == XR_XIR_CALL_READY);
        else { CHECK(!mode); CHECK(xr_xir_instance_cancel_current(victim) == XR_XIR_CALL_CANCEL_REQUESTED); }
        diamond_cancel_finish(victim, XR_XIR_CALL_CANCELLED); diamond_cancel_sticky(victim, &trace[0]);
        CHECK(xr_xir_instance_free(victim) == XR_XIR_CALL_READY); victim = NULL;
    }
    diamond_code->records[0].instance = NULL; diamond_expected_fault = XR_XIR_CALL_READY;
    CHECK(!diamond_code->records[0].roots); diamond_cancel_reverse(&trace[0], &published);
    for (unsigned i = 0; i < 4; ++i) CHECK(diamond_code->records[0].initializers[i] == diamond_code->records[0].initializer_leaves[i]);
    XrXirDomainBudgetStats unchanged = xr_xir_domain_budget_stats(peer_domain);
    CHECK(peer_before.work == unchanged.work && peer_before.requested_bytes == unchanged.requested_bytes && peer_before.requested_call_bytes == unchanged.requested_call_bytes);
    CHECK(!memcmp(&peer_record, &diamond_code->records[1], sizeof(peer_record)) && !memcmp(&peer_trace, &trace[1], sizeof(peer_trace)));
    diamond_cancel_finish(peer, XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(peer, diamond_entry, NULL, 0) == XR_XIR_CALL_READY);
    diamond_cancel_finish(peer, XR_XIR_CALL_RETURNED); diamond_verify(&diamond_code->records[1], backend);
    DiamondCancelTrace peer_published = trace[1]; CHECK(peer_published.publications == 6);
    CHECK(xr_xir_instance_free(peer) == XR_XIR_CALL_READY); diamond_code->records[1].instance = NULL;
    diamond_cancel_reverse(&trace[1], &peer_published);
    diamond_cancel_balance(domain, before); diamond_cancel_balance(peer_domain, peer_before);
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !diamond_code_releases);
}
static int diamond_cancel_main(int argc, char **argv) {
    CHECK(argc == 2); (void)diamond_normal_main; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], diamond_backends[backend])) ++backend;
    CHECK(backend < 4); XrXirProgram *program = diamond_program(backend); DiamondCancelCoverage coverage = {0};
    unsigned prefixes = diamond_cancel_prefix_count(program, backend); CHECK(prefixes);
    for (unsigned prefix = 0; prefix < prefixes; ++prefix)
        for (unsigned mode = 0; mode < 3; ++mode) diamond_cancel_prefix(program, backend, prefix, mode, &coverage);
    CHECK(coverage.cursors == 15 && coverage.publications == 127);
    xr_xir_compile_program_drop(program); CHECK(diamond_code_releases == 1 && !diamond_code && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    printf("Diamond initializer cancel backend=%s actual prefixes=%u cancel+stop+free=%u, cursorMask15/publishedMask127, root never enters, exact reverse published slots, actual init frames closed,24sticky copies/start/poll, peerSLRT twice, code lease1, physical0/0\n",
        diamond_backends[backend], prefixes, 3*prefixes); return 0;
}
int main(int argc, char **argv) { return diamond_cancel_main(argc, argv); }
