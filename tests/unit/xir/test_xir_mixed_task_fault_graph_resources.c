/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_fault_graph_resources.c - Bound and fault both real Task owners through physical retirement
 */
static void task_fault_graph_before_free(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) task_fault_graph_before_free(pointer)
#define main task_fault_prefix_resource_original_main
#include "test_xir_mixed_task_fault_prefix_resources.c"
#undef main
#undef XIR_RUNTIME_BEFORE_FREE

typedef struct TaskFaultGraphSpec { unsigned backend, prefix, mode, order, limited; } TaskFaultGraphSpec;
typedef struct TaskFaultGraphResult {
    XrXirDomainBudgetStats cost[2];
    TaskFaultPrefixResourceResult driven;
    size_t sites;
    bool complete[2], hit;
} TaskFaultGraphResult;
static XrXirDomain *task_fault_graph_borrowed[2];
static XrXirDomainBudgetStats task_fault_graph_cost[2];
static unsigned task_fault_graph_disposals[2];
static void task_fault_graph_before_free(void *pointer) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirDomain *domain = task_fault_graph_borrowed[i];
        if (!pointer || pointer != domain) continue;
        CHECK(!atomic_load_explicit(&domain->references, memory_order_relaxed) && !domain->objects);
        CHECK(domain->stats.live_bytes == sizeof(*domain) && domain->stats.allocations == domain->stats.frees+1);
        CHECK(!domain->budget.call_live && !domain->budget.metadata_live &&
            domain->budget.call_allocations == domain->budget.call_frees && domain->budget.metadata_allocations == domain->budget.metadata_frees);
        task_fault_graph_cost[i] = domain->budget; CHECK(!task_fault_graph_disposals[i]++);
        task_fault_graph_borrowed[i] = NULL;
    }
}
static void task_fault_graph_same_cost(XrXirDomainBudgetStats before, XrXirDomainBudgetStats after) {
    CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes && before.work == after.work);
}
static bool task_fault_graph_pause(TaskFaultRun *peer, XrXirTaskWaitToken *token) {
    XrXirCallStatus status = xr_xir_instance_start(peer->instance, p20_task_result_entries[0], NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == peer->fault); peer->complete = false; return false; }
    unsigned polls = 0;
    do {
        status = task_fault_resource_tick(peer); if (!peer->complete) return false;
        CHECK(status == XR_XIR_CALL_READY && ++polls < 4096); task_fault_resource_capture(peer);
        if (!peer->complete) return false;
    } while (!peer->captured || !peer->witness.yielded_count);
    CHECK(peer->captured == 1 && peer->witness.yielded_count == 1);
    CHECK(xr_xir_call_task_wait_token(peer->instance->call, token));
    const XirTask *task = (const XirTask *)(uintptr_t)peer->tasks[0].payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state != XIR_TASK_TERMINAL && task->call->next_wake == 1);
    CHECK(token->request.subject == task); return true;
}
static void task_fault_graph_same_peer(TaskFaultRun *peer, const XrXirTaskWaitToken *before,
    XrXirDomainBudgetStats cost, uintptr_t slot) {
    XrXirTaskWaitToken after = {0}; CHECK(xr_xir_call_task_wait_token(peer->instance->call, &after));
    CHECK(after.binding.owner == before->binding.owner && after.binding.activation == before->binding.activation &&
        after.binding.generation == before->binding.generation && after.binding.ticket == before->binding.ticket);
    CHECK(after.wake == before->wake && after.request.kind == before->request.kind && after.request.reserved == before->request.reserved &&
        after.request.after_ms == before->request.after_ms && after.request.subject == before->request.subject &&
        after.request.generation == before->request.generation && after.request.ticket == before->request.ticket);
    CHECK(xr_xir_value_valid(&peer->instance->slots[0]) && (uintptr_t)peer->instance->slots[0].payload == slot);
    const XirCell *cell = (const XirCell *)object_pointer(&peer->instance->slots[0]);
    CHECK(cell && cell->object.kind == XIR_OBJECT_CELL && cell->object.domain == peer->domain);
    CHECK(cell->initialized && cell->module_storage && cell->module_owner == peer->instance && !cell->module_slot);
    task_late_text(&cell->value, "cold");
    XrXirCallResult empty = {0}; size_t attempts = runtime_attempts;
    CHECK(xr_xir_task_copy_outcome(&peer->tasks[0], &empty) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&empty));
    CHECK(attempts == runtime_attempts); task_fault_graph_same_cost(cost, xr_xir_domain_budget_stats(peer->domain));
}
static void task_fault_graph_retire(TaskFaultRun *run, XrXirCallStatus retirement[2]) {
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
static void task_fault_graph_finish_peer(TaskFaultRun *peer, unsigned backend) {
    if (peer->complete) task_fault_prefix_resource_finish(peer, XR_XIR_CALL_RETURNED, NULL);
    if (peer->complete) task_fault_resource_call(peer, 1, "thrown:task-panic:inner:outer");
    if (peer->complete) CHECK(peer->captured == 2 && peer->witness.yielded_count == 2 &&
        peer->witness.root_providers == (task_late_native(backend, true) ? 2u : 1u) &&
        peer->witness.worker_providers == (task_late_native(backend, false) ? 2u : 1u));
}
/* Fix each owner's execution expectations before either owner's late copy
 * can fail. All four actual Task allocations die before the copies are read again. */
static void task_fault_graph_late(TaskFaultRun runs[2], const TaskFaultGraphSpec *spec,
    const XrXirCallStatus control[2], const XrXirCallStatus retirement[2][2]) {
    XrXirCallResult copies[2][2][2] = {{{{0}}}}; XrXirValue infos[2] = {{0}}; uintptr_t addresses[2][2] = {{0}};
    const bool complete[] = {runs[0].complete, runs[1].complete};
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < runs[i].captured; ++kind) {
        TaskFaultRun *run = &runs[i]; const XirTask *task = (const XirTask *)(uintptr_t)run->tasks[kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor && task->object.domain == run->domain);
        XrXirCallStatus stored = task->outcome.status;
        if (!i && control[kind] != XR_XIR_CALL_READY) CHECK(stored == control[kind]);
        if (retirement[i][kind] != XR_XIR_CALL_READY) CHECK(stored == retirement[i][kind]);
        XrXirCallStatus expected = kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN;
        if (!i && spec->mode >= 2 && control[kind] == XR_XIR_CALL_READY) expected = XR_XIR_CALL_CANCELLED;
        if (complete[i]) CHECK(stored == expected);
        else CHECK(stored == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN) || stored == XR_XIR_CALL_CANCELLED || stored == run->fault);
        task_late_metadata_only(&runs[0].fixture, xr_xir_value_arena(&run->tasks[kind]));
        for (unsigned n = 0; n < 2; ++n) {
            XrXirCallStatus status = xr_xir_task_copy_outcome(&run->tasks[kind], &copies[i][kind][n]);
            if (xr_xir_call_result_empty(&copies[i][kind][n])) {
                CHECK(status == run->fault && xr_xir_value_valid(&run->tasks[kind])); run->complete = false;
            } else { CHECK(status == stored); task_fault_resource_read(run, &copies[i][kind][n], kind); }
        }
        if (kind && copies[i][kind][0].status == XR_XIR_CALL_ASSERTION &&
            !task_fault_resource_value(run, xr_xir_panic_info_new(run->domain, &copies[i][kind][0].panic, &infos[i])))
            CHECK(!infos[i].type && !infos[i].reserved && !infos[i].payload && xr_xir_call_result_valid(&copies[i][kind][0]));
        addresses[i][kind] = (uintptr_t)run->tasks[kind].payload;
    }
    size_t attempts = runtime_attempts;
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind) {
        xr_xir_value_drop(&runs[i].tasks[kind]); CHECK(!runs[i].tasks[kind].type && !runs[i].tasks[kind].reserved && !runs[i].tasks[kind].payload);
    }
    CHECK(attempts == runtime_attempts);
    for (size_t slot = 0; slot < runtime_owned_capacity; ++slot) if (runtime_owned[slot].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[slot].pointer;
        for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind) if (addresses[i][kind])
            CHECK(addresses[i][kind] < base || addresses[i][kind]-base >= runtime_owned[slot].bytes);
    }
    for (unsigned i = 0; i < 2; ++i) for (unsigned kind = 0; kind < 2; ++kind) for (unsigned n = 0; n < 2; ++n) {
        if (!xr_xir_call_result_empty(&copies[i][kind][n])) task_fault_resource_read(&runs[i], &copies[i][kind][n], kind);
        xr_xir_call_result_drop(&copies[i][kind][n]); CHECK(xr_xir_call_result_empty(&copies[i][kind][n]));
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 2; ++i) if (infos[i].type) {
        XrXirValue message = {0}; XrXirFaultDetail detail = {0};
        CHECK(xr_xir_panic_info_detail(&infos[i], &detail) && detail.code == 445);
        bool copied = task_fault_resource_value(&runs[i], xr_xir_panic_info_message(&infos[i], &message));
        if (!copied) CHECK(!message.type && !message.reserved && !message.payload && xr_xir_value_valid(&infos[i]));
        else task_fault_resource_owner(&runs[i], &message);
        xr_xir_value_drop(&infos[i]); if (copied) task_late_text(&message, "task-panic"); xr_xir_value_drop(&message);
    }
}
static TaskFaultGraphResult task_fault_graph_operation(const TaskFaultGraphSpec *spec, size_t failure,
    const XrXirInstanceConfig *limits) {
    CHECK(spec->order < 2 && spec->mode < 4 && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    TaskFaultRun runs[2] = {{0}}; bool had_domain[2] = {false};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!task_fault_graph_borrowed[i]); task_fault_graph_cost[i] = (XrXirDomainBudgetStats){0}; task_fault_graph_disposals[i] = 0;
        runs[i].fault = limits && spec->limited == i ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM; runs[i].complete = true;
    }
    XrXirProgram *program = task_late_program(&runs[0].fixture, spec->backend); task_fault_layout(program);
    runs[1].fixture = runs[0].fixture;
    runtime_attempts = 0; runtime_fail_at = failure;
    for (unsigned at = 0; at < 2; ++at) {
        unsigned i = 1-at; XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        if (limits && spec->limited == i) config = *limits;
        XrXirCallStatus status = xr_xir_instance_new(program, &config, &runs[i].instance);
        runs[i].complete = status == XR_XIR_CALL_READY; CHECK(runs[i].complete || (status == runs[i].fault && !runs[i].instance));
        if (runs[i].instance) { had_domain[i] = true; runs[i].domain = runs[i].instance->domain; task_fault_graph_borrowed[i] = runs[i].domain; }
    }
    if (had_domain[0] && had_domain[1]) CHECK(runs[0].domain != runs[1].domain);
    xr_xir_compile_program_drop(program); CHECK(runs[0].fixture.releases == (!had_domain[0] && !had_domain[1] ? 1u : 0u));
    TaskFaultRun *run = &runs[0], *peer = &runs[1]; XrXirTaskWaitToken token = {0}; bool paused = false;
    if (peer->complete) task_fault_resource_call(peer, 1, "cold");
    if (peer->complete) paused = task_fault_graph_pause(peer, &token);
    XrXirDomainBudgetStats peer_cost = {0}; uintptr_t peer_slot = 0;
    if (paused) { peer_cost = xr_xir_domain_budget_stats(peer->domain); peer_slot = (uintptr_t)peer->instance->slots[0].payload; }
    TaskFaultGraphResult result = {0}; XrXirCallStatus control[2] = {0}, retirement[2][2] = {{0}};
    if (run->complete) task_fault_resource_call(run, 1, "cold");
    if (run->complete) task_fault_prefix_resource_drive(run, spec->prefix, spec->mode, &result.driven, control);
    if (!spec->order) task_fault_graph_retire(run, retirement[0]);
    if (paused) task_fault_graph_same_peer(peer, &token, peer_cost, peer_slot);
    XrXirDomainBudgetStats victim_before = {0};
    if (task_fault_graph_borrowed[0]) victim_before = xr_xir_domain_budget_stats(task_fault_graph_borrowed[0]);
    task_fault_graph_finish_peer(peer, spec->backend); task_fault_graph_retire(peer, retirement[1]);
    if (task_fault_graph_borrowed[0]) task_fault_graph_same_cost(victim_before, xr_xir_domain_budget_stats(task_fault_graph_borrowed[0]));
    if (spec->order) task_fault_graph_retire(run, retirement[0]);
    CHECK(run->fixture.releases == 1 && !run->fixture.vm && !run->fixture.entries);
    task_fault_graph_late(runs, spec, control, retirement);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!task_fault_graph_borrowed[i] && task_fault_graph_disposals[i] == (had_domain[i] ? 1u : 0u));
        result.cost[i] = task_fault_graph_cost[i]; result.complete[i] = runs[i].complete;
        if (limits && spec->limited == i) CHECK(result.cost[i].requested_bytes <= limits->requested_value_limit &&
            result.cost[i].requested_call_bytes <= limits->requested_call_limit && result.cost[i].work <= limits->work_limit);
    }
    result.sites = runtime_attempts; result.hit = failure != SIZE_MAX && runtime_attempts > failure; runtime_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return result;
}
static size_t task_fault_graph_axes(TaskFaultGraphSpec spec) {
    TaskFaultGraphResult normal = task_fault_graph_operation(&spec, SIZE_MAX, NULL);
    CHECK(normal.complete[0] && normal.complete[1] && !normal.hit && normal.driven.polls == spec.prefix && normal.sites && normal.sites < 20000);
    unsigned phase = spec.prefix < 4 ? 0u : spec.prefix < 6 ? 1u : spec.prefix < 12 ? 2u : spec.prefix < 26 ? 3u : spec.prefix < 33 ? 4u : 5u;
    CHECK(normal.driven.phase == phase && normal.driven.captured == (phase < 2 ? 0u : phase < 4 ? 1u : 2u) &&
        normal.driven.terminal == (phase < 3 ? 0u : phase < 5 ? 1u : 2u));
    for (unsigned owner = 0; owner < 2; ++owner) {
        spec.limited = owner; const uint64_t axes[] = {normal.cost[owner].requested_bytes, normal.cost[owner].requested_call_bytes, normal.cost[owner].work};
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
            if (!axis) config.requested_value_limit = axes[axis]-minus;
            else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
            else config.work_limit = axes[axis]-minus;
            TaskFaultGraphResult result = task_fault_graph_operation(&spec, SIZE_MAX, &config);
            CHECK(result.complete[owner] == !minus && result.complete[1-owner] && !result.hit);
            if (!minus) CHECK(result.driven.phase == normal.driven.phase && result.driven.captured == normal.driven.captured && result.driven.terminal == normal.driven.terminal);
            printf("Task fault graph axis backend=%u order=%u prefix=%u mode=%u owner=%u axis=%u minus=%u complete=%u peerComplete=%u value=%llu call=%llu work=%llu\n",
                spec.backend, spec.order, spec.prefix, spec.mode, owner, axis, minus, (unsigned)result.complete[owner], (unsigned)result.complete[1-owner],
                (unsigned long long)result.cost[owner].requested_bytes, (unsigned long long)result.cost[owner].requested_call_bytes, (unsigned long long)result.cost[owner].work);
        }
    }
    printf("Task fault graph resources backend=%u order=%u prefix=%u mode=%u phase=%u captured=%u terminal=%u sites=%zu bothOwnersNoRetain completeRuntimeBoundary peerOwnFailures codeLease1 physical0/0\n",
        spec.backend, spec.order, spec.prefix, spec.mode, normal.driven.phase, normal.driven.captured, normal.driven.terminal, normal.sites); return normal.sites;
}
int main(int argc, char **argv) {
    CHECK(argc == 4 || argc == 9);
    static const char *const names[] = {"0", "1", "vm", "native"}; TaskFaultGraphSpec spec = {.limited=2};
    while (spec.backend < 4 && strcmp(argv[1], names[spec.backend])) ++spec.backend;
    CHECK(spec.backend < 4); spec.order = (unsigned)task_fault_prefix_resource_number(argv[2]); CHECK(spec.order < 2);
    spec.mode = (unsigned)task_fault_prefix_resource_number(argv[argc == 4 ? 3 : 4]); CHECK(spec.mode >= 1 && spec.mode <= 3);
    TaskFaultGraphSpec full = spec; full.mode = 0; TaskFaultGraphResult baseline = task_fault_graph_operation(&full, SIZE_MAX, NULL);
    CHECK(baseline.complete[0] && baseline.complete[1] && baseline.driven.polls == 66);
    if (argc == 9) {
        CHECK(!strcmp(argv[3], "fi")); spec.prefix = (unsigned)task_fault_prefix_resource_number(argv[5]); CHECK(spec.prefix < baseline.driven.polls);
        size_t sites = task_fault_graph_axes(spec), begin = task_fault_prefix_resource_number(argv[6]), end = task_fault_prefix_resource_number(argv[7]);
        CHECK(sites == task_fault_prefix_resource_number(argv[8]) && begin < end && end <= sites);
        for (size_t site = begin; site < end; ++site) {
            TaskFaultGraphResult result = task_fault_graph_operation(&spec, site, NULL);
            CHECK(result.hit && result.sites > site && (!result.complete[0] || !result.complete[1]));
        }
        printf("Task fault graph complete FI backend=%u order=%u prefix=%u mode=%u range=%zu:%zu of%zu bothOwnersEveryOrdinal typedOOM noCounterExclusion noCallerDomainRetain codeLease1 physical0/0\n",
            spec.backend, spec.order, spec.prefix, spec.mode, begin, end, sites); return 0;
    }
    size_t sites = 0;
    for (spec.prefix = 0; spec.prefix < baseline.driven.polls; ++spec.prefix) sites += task_fault_graph_axes(spec);
    printf("Task fault graph census backend=%u order=%u mode=%u actualPrefixes=%u normalAllocationSum=%zu exactMinus1=%u physical0/0\n",
        spec.backend, spec.order, spec.mode, baseline.driven.polls, sites, baseline.driven.polls*12); return 0;
}
