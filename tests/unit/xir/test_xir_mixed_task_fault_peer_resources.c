/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_fault_peer_resources.c - Fault a real Task owner while an independent peer waits
 */
static void task_fault_peer_before_free(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) task_fault_peer_before_free(pointer)
#define main task_fault_prefix_resource_original_main
#include "test_xir_mixed_task_fault_prefix_resources.c"
#undef main
#undef XIR_RUNTIME_BEFORE_FREE

typedef struct TaskFaultPeerScope {
    size_t sites, failure, begin, armed;
    bool hit;
} TaskFaultPeerScope;
typedef struct TaskFaultPeerResult {
    XrXirDomainBudgetStats cost;
    size_t sites, peer_sites;
    unsigned polls, phase, captured, terminal;
    bool complete, hit;
} TaskFaultPeerResult;
static XrXirDomain *task_fault_peer_borrowed;
static XrXirDomainBudgetStats task_fault_peer_final_cost;
static unsigned task_fault_peer_disposals;
/* Observe the actual allocation immediately before free; no reference or
 * substitute Domain keeps the ledger alive after its real owners disappear. */
static void task_fault_peer_before_free(void *pointer) {
    if (!pointer || pointer != task_fault_peer_borrowed) return;
    XrXirDomain *domain = pointer;
    CHECK(!atomic_load_explicit(&domain->references, memory_order_relaxed) && !domain->objects);
    CHECK(domain->stats.live_bytes == sizeof(*domain) && domain->stats.allocations == domain->stats.frees+1);
    task_fault_peer_final_cost = domain->budget;
    CHECK(!domain->budget.call_live && !domain->budget.metadata_live &&
        domain->budget.call_allocations == domain->budget.call_frees &&
        domain->budget.metadata_allocations == domain->budget.metadata_frees);
    CHECK(!task_fault_peer_disposals++); task_fault_peer_borrowed = NULL;
}
/* Only victim phases receive fault ordinals. Peer setup and recovery remain
 * physically observed but cannot consume or move a victim fault index. */
static void task_fault_peer_scope_begin(TaskFaultPeerScope *scope) {
    scope->begin = runtime_attempts;
    scope->armed = scope->failure >= scope->sites && scope->failure != SIZE_MAX ?
        runtime_attempts + scope->failure-scope->sites : SIZE_MAX;
    runtime_fail_at = scope->armed;
}
static void task_fault_peer_scope_end(TaskFaultPeerScope *scope) {
    CHECK(runtime_attempts >= scope->begin);
    if (scope->armed != SIZE_MAX && runtime_attempts > scope->armed) scope->hit = true;
    scope->sites += runtime_attempts-scope->begin; runtime_fail_at = SIZE_MAX;
}
static void task_fault_peer_cost(XrXirDomainBudgetStats before, XrXirDomainBudgetStats after) {
    CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes &&
        before.work == after.work);
}
static XrXirTaskWaitToken task_fault_peer_pause(TaskFaultRun *peer) {
    CHECK(xr_xir_instance_start(peer->instance, p20_task_result_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    unsigned polls = 0;
    do {
        CHECK(task_fault_resource_tick(peer) == XR_XIR_CALL_READY && ++polls < 4096);
        task_fault_resource_capture(peer);
    } while (!peer->captured || !peer->witness.yielded_count);
    CHECK(peer->captured == 1 && peer->witness.yielded_count == 1);
    XrXirTaskWaitToken token = {0}; CHECK(xr_xir_call_task_wait_token(peer->instance->call, &token));
    const XirTask *task = (const XirTask *)(uintptr_t)peer->tasks[0].payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state != XIR_TASK_TERMINAL && task->call->next_wake == 1);
    CHECK(token.request.subject == task); return token;
}
static void task_fault_peer_same_wait(TaskFaultRun *peer, const XrXirTaskWaitToken *before, uintptr_t slot) {
    CHECK((uintptr_t)peer->instance->slots[0].payload == slot && xr_xir_value_valid(&peer->instance->slots[0]));
    const XirCell *cell = (const XirCell *)object_pointer(&peer->instance->slots[0]);
    CHECK(cell && cell->object.kind == XIR_OBJECT_CELL && cell->object.domain == peer->domain);
    task_late_text(&cell->value, "cold");
    XrXirTaskWaitToken after = {0}; CHECK(xr_xir_call_task_wait_token(peer->instance->call, &after));
    CHECK(after.binding.owner == before->binding.owner && after.binding.activation == before->binding.activation &&
        after.binding.generation == before->binding.generation && after.binding.ticket == before->binding.ticket);
    CHECK(after.wake == before->wake && after.request.kind == before->request.kind && after.request.reserved == before->request.reserved &&
        after.request.after_ms == before->request.after_ms && after.request.subject == before->request.subject &&
        after.request.generation == before->request.generation && after.request.ticket == before->request.ticket);
    XrXirCallResult empty = {0}; size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_copy_outcome(&peer->tasks[0], &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    CHECK(attempts == runtime_attempts);
}
static void task_fault_peer_retire(TaskFaultRun *run, XrXirCallStatus retirement[2]) {
    for (unsigned kind = 0; kind < run->captured; ++kind) {
        const XirTask *task = (const XirTask *)(uintptr_t)run->tasks[kind].payload;
        if (task->state == XIR_TASK_TERMINAL) retirement[kind] = task->outcome.status;
    }
    if (!run->instance) return;
    bool failed = run->driven || run->instance->budget.exhausted;
    XrXirCallStatus status = xr_xir_instance_free(run->instance); run->instance = NULL;
    if (failed) CHECK(status == run->fault); else CHECK(status == XR_XIR_CALL_READY || status == run->fault);
    if (status == run->fault) run->complete = false;
}
static void task_fault_peer_finish(TaskFaultRun *peer, unsigned backend) {
    task_fault_prefix_resource_finish(peer, XR_XIR_CALL_RETURNED, NULL);
    CHECK(peer->complete && peer->captured == 2 && peer->witness.yielded_count == 2);
    task_fault_resource_call(peer, 1, "thrown:task-panic:inner:outer"); CHECK(peer->complete);
    CHECK(peer->witness.root_providers == (task_late_native(backend, true) ? 2u : 1u) &&
        peer->witness.worker_providers == (task_late_native(backend, false) ? 2u : 1u));
    for (unsigned kind = 0; kind < 2; ++kind) {
        XrXirCallResult copy = {0};
        CHECK(xr_xir_task_copy_outcome(&peer->tasks[kind], &copy) == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN));
        task_fault_resource_read(peer, &copy, kind); xr_xir_call_result_drop(&copy);
    }
    CHECK(xr_xir_instance_free(peer->instance) == XR_XIR_CALL_READY); peer->instance = NULL;
    for (unsigned kind = 0; kind < 2; ++kind) xr_xir_value_drop(&peer->tasks[kind]);
}
static TaskFaultPeerResult task_fault_peer_operation(unsigned backend, unsigned prefix, unsigned mode,
    unsigned order, size_t failure, const XrXirInstanceConfig *limits) {
    CHECK(mode < 4 && order < 2 && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    CHECK(!task_fault_peer_borrowed); task_fault_peer_final_cost = (XrXirDomainBudgetStats){0}; task_fault_peer_disposals = 0;
    TaskFaultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .complete=true};
    XrXirProgram *program = task_late_program(&run.fixture, backend); task_fault_layout(program);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    TaskFaultRun peer = {.fixture=run.fixture, .fault=XR_XIR_CALL_OOM, .complete=true};
    CHECK(xr_xir_instance_new(program, &config, &peer.instance) == XR_XIR_CALL_READY); peer.domain = peer.instance->domain;
    xr_xir_compile_program_drop(program); CHECK(!run.fixture.releases);
    task_fault_resource_call(&peer, 1, "cold"); XrXirTaskWaitToken peer_wait = task_fault_peer_pause(&peer);
    XrXirDomainBudgetStats peer_cost = xr_xir_domain_budget_stats(peer.domain);
    uintptr_t peer_slot = (uintptr_t)peer.instance->slots[0].payload;
    if (limits) config = *limits;
    TaskFaultPeerScope scope = {.failure=failure}; runtime_attempts = 0; task_fault_peer_scope_begin(&scope);
    XrXirCallStatus status = xr_xir_instance_new(peer.instance->program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY; CHECK(run.complete || (status == run.fault && !run.instance));
    const bool had_domain = run.instance != NULL;
    if (run.instance) { run.domain = run.instance->domain; task_fault_peer_borrowed = run.domain; CHECK(run.domain != peer.domain); }
    TaskFaultPrefixResourceResult driven = {0}; XrXirCallStatus control[2] = {0}, retirement[2] = {0};
    if (run.complete) task_fault_resource_call(&run, 1, "cold");
    if (run.complete) task_fault_prefix_resource_drive(&run, prefix, mode, &driven, control);
    if (!order) task_fault_peer_retire(&run, retirement);
    task_fault_peer_scope_end(&scope);
    task_fault_peer_cost(peer_cost, xr_xir_domain_budget_stats(peer.domain)); task_fault_peer_same_wait(&peer, &peer_wait, peer_slot);
    task_fault_peer_cost(peer_cost, xr_xir_domain_budget_stats(peer.domain));
    XrXirDomainBudgetStats victim_before = {0};
    if (task_fault_peer_borrowed) victim_before = xr_xir_domain_budget_stats(task_fault_peer_borrowed);
    size_t peer_begin = runtime_attempts; task_fault_peer_finish(&peer, backend); size_t peer_sites = runtime_attempts-peer_begin;
    if (task_fault_peer_borrowed) task_fault_peer_cost(victim_before, xr_xir_domain_budget_stats(task_fault_peer_borrowed));
    CHECK(run.fixture.releases == (order && run.instance ? 0u : 1u));
    task_fault_peer_scope_begin(&scope);
    if (order) task_fault_peer_retire(&run, retirement);
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    const bool execution_complete = run.complete;
    task_fault_prefix_resource_late(&run, mode, control, retirement, execution_complete);
    task_fault_peer_scope_end(&scope);
    CHECK(!task_fault_peer_borrowed && task_fault_peer_disposals == (had_domain ? 1u : 0u));
    CHECK(task_fault_peer_final_cost.requested_bytes <= config.requested_value_limit &&
        task_fault_peer_final_cost.requested_call_bytes <= config.requested_call_limit && task_fault_peer_final_cost.work <= config.work_limit);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    return (TaskFaultPeerResult){task_fault_peer_final_cost, scope.sites, peer_sites, driven.polls,
        driven.phase, driven.captured, driven.terminal, run.complete, scope.hit};
}
static size_t task_fault_peer_axes(unsigned backend, unsigned prefix, unsigned mode, unsigned order) {
    TaskFaultPeerResult normal = task_fault_peer_operation(backend, prefix, mode, order, SIZE_MAX, NULL);
    CHECK(normal.complete && !normal.hit && normal.polls == prefix && normal.sites && normal.sites < 20000);
    unsigned phase = prefix < 4 ? 0u : prefix < 6 ? 1u : prefix < 12 ? 2u : prefix < 26 ? 3u : prefix < 33 ? 4u : 5u;
    CHECK(normal.phase == phase && normal.captured == (phase < 2 ? 0u : phase < 4 ? 1u : 2u) &&
        normal.terminal == (phase < 3 ? 0u : phase < 5 ? 1u : 2u));
    const uint64_t axes[] = {normal.cost.requested_bytes, normal.cost.requested_call_bytes, normal.cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis]-minus;
        else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
        else config.work_limit = axes[axis]-minus;
        TaskFaultPeerResult result = task_fault_peer_operation(backend, prefix, mode, order, SIZE_MAX, &config);
        CHECK(result.complete == !minus && !result.hit);
        if (!minus) CHECK(result.phase == normal.phase && result.captured == normal.captured && result.terminal == normal.terminal);
        printf("Task fault peer axis backend=%u order=%u prefix=%u mode=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend, order, prefix, mode, axis, minus, (unsigned)result.complete, (unsigned long long)result.cost.requested_bytes,
            (unsigned long long)result.cost.requested_call_bytes, (unsigned long long)result.cost.work);
    }
    printf("Task fault peer resources backend=%u order=%u prefix=%u mode=%u phase=%u captured=%u terminal=%u sites=%zu peerExcludedSites=%zu value=%llu call=%llu work=%llu noCallerDomainRetain exactDisposalSnapshot unchangedPeerWait codeLease1 physical0/0\n",
        backend, order, prefix, mode, normal.phase, normal.captured, normal.terminal, normal.sites, normal.peer_sites,
        (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]); return normal.sites;
}
int main(int argc, char **argv) {
    CHECK(argc == 4 || argc == 7);
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); unsigned order = (unsigned)task_fault_prefix_resource_number(argv[2]); CHECK(order < 2);
    unsigned mode = (unsigned)task_fault_prefix_resource_number(argv[argc == 4 ? 3 : 4]); CHECK(mode >= 1 && mode <= 3);
    TaskFaultPeerResult baseline = task_fault_peer_operation(backend, 0, 0, order, SIZE_MAX, NULL);
    CHECK(baseline.complete && baseline.polls == 66);
    if (argc == 7) {
        CHECK(!strcmp(argv[3], "fi")); unsigned prefix = (unsigned)task_fault_prefix_resource_number(argv[5]); CHECK(prefix < baseline.polls);
        size_t sites = task_fault_peer_axes(backend, prefix, mode, order); CHECK(sites == task_fault_prefix_resource_number(argv[6]));
        for (size_t site = 0; site < sites; ++site) {
            TaskFaultPeerResult result = task_fault_peer_operation(backend, prefix, mode, order, site, NULL);
            CHECK(!result.complete && result.hit && result.sites > site);
        }
        printf("Task fault peer complete victim FI backend=%u order=%u prefix=%u mode=%u sites=%zu everyVictimOrdinalHit typedOOM noCallerDomainRetain peerRecoveryUninjected codeLease1 physical0/0\n",
            backend, order, prefix, mode, sites); return 0;
    }
    size_t sites = 0;
    for (unsigned prefix = 0; prefix < baseline.polls; ++prefix) sites += task_fault_peer_axes(backend, prefix, mode, order);
    printf("Task fault peer census backend=%u order=%u mode=%u actualPrefixes=%u normalAllocationSum=%zu exactMinus1=%u physical0/0\n",
        backend, order, mode, baseline.polls, sites, baseline.polls*6); return 0;
}
