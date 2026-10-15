/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_fault_resources.c - Typed Task faults keep one finite Domain through late reads
 */
#define XR_TASK_FAULT_RESOURCE_MAIN
#include "test_xir_mixed_task_fault_lifetime.c"

typedef struct TaskFaultRun {
    TaskLateFixture fixture;
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirValue tasks[2];
    TaskLateWitness witness;
    XrXirCallStatus fault;
    unsigned captured;
    bool complete, driven;
} TaskFaultRun;

static bool task_fault_resource_value(TaskFaultRun *run, XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return true;
    CHECK(status == (run->fault == XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT));
    run->complete = false; return false;
}
static void task_fault_resource_owner(const TaskFaultRun *run, const XrXirValue *value) {
    CHECK(xr_xir_value_valid(value) && value->payload && object_pointer(value)->domain == run->domain);
}
static XrXirCallStatus task_fault_resource_tick(TaskFaultRun *run) {
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(run->instance, 1);
    if (event.outcome.status == run->fault) {
        CHECK(!event.outcome.value.type && !event.outcome.value.reserved && !event.outcome.value.payload &&
            !event.outcome.wake && xr_xir_panic_empty(&event.outcome.panic));
        run->complete = false; run->driven = true; return event.outcome.status;
    }
    CHECK(event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == XR_XIR_CALL_RETURNED);
    task_late_observe(&run->fixture, run->instance, &run->witness); return event.outcome.status;
}
static void task_fault_resource_capture(TaskFaultRun *run) {
    XrXirCall *call = run->instance->call; XrXirTaskWaitToken token = {0};
    if (!call || !call->wait_linked || !call->waiting_task.type || !xr_xir_call_task_wait_token(call, &token)) return;
    const XirTask *task = (const XirTask *)(uintptr_t)call->waiting_task.payload;
    CHECK(xr_xir_task_storage_valid(task) && task->object.domain == run->domain);
    CHECK(token.binding.owner == run->instance->executor && token.binding.activation == &run->instance->executor->root);
    CHECK(token.binding.generation == run->instance->executor->generation && token.binding.ticket == run->instance->executor->root.ticket);
    CHECK(token.request.kind == XR_XIR_WAIT_TASK && token.request.subject == task);
    for (unsigned i = 0; i < run->captured; ++i) if (run->tasks[i].payload == call->waiting_task.payload) return;
    if (task->state == XIR_TASK_TERMINAL || !task->call->next_wake) return;
    CHECK(run->captured < 2 && task->call->next_wake == 1 && task->executor == run->instance->executor);
    XrXirValue *output = &run->tasks[run->captured];
    if (!task_fault_resource_value(run, xr_xir_value_copy(&call->waiting_task, output))) {
        CHECK(!output->type && !output->reserved && !output->payload); return;
    }
    ++run->captured;
}
static void task_fault_resource_call(TaskFaultRun *run, unsigned entry, const char *text) {
    XrXirCallStatus status = xr_xir_instance_start(run->instance, p20_task_result_entries[entry], NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; return; }
    unsigned polls = 0;
    do {
        status = task_fault_resource_tick(run); CHECK(++polls < 4096);
        if (!run->complete) return;
        if (!entry) { task_fault_resource_capture(run); if (!run->complete) return; }
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == XR_XIR_CALL_RETURNED); XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(run->instance, &result) == XR_XIR_CALL_RETURNED);
    if (entry) { task_late_text(&result, text); task_fault_resource_owner(run, &result); }
    else CHECK(run->captured == 2 && result.type == run->tasks[1].type && result.payload == run->tasks[1].payload);
    xr_xir_value_drop(&result); XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(run->instance, &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.reserved && !empty.payload);
}
static void task_fault_resource_read(TaskFaultRun *run, const XrXirCallResult *outcome, unsigned kind) {
    CHECK(!outcome->wake);
    if (outcome->status == XR_XIR_CALL_THROWN) {
        CHECK(!kind && xr_xir_panic_empty(&outcome->panic));
        XrXirEnumBorrow borrowed = {0}; CHECK(xr_xir_enum_borrow(&outcome->value, &borrowed) == XR_XIR_VALUE_OK);
        task_fault_resource_owner(run, &outcome->value);
        CHECK(borrowed.name.length == 9 && !memcmp(borrowed.name.bytes, "TaskFault", 9) &&
            borrowed.member.length == 6 && !memcmp(borrowed.member.bytes, "Failed", 6) && borrowed.field_count == 1);
        XrXirValueAdmission admission = {xr_xir_value_arena(&outcome->value), run->domain, NULL, NULL, 10000, 65536};
        XrXirValue message = {0};
        if (task_fault_resource_value(run, xr_xir_enum_get(&outcome->value, 0, 0, &admission, &message))) {
            task_late_text(&message, "task-throw"); task_fault_resource_owner(run, &message);
        }
        else CHECK(!message.type && !message.reserved && !message.payload && xr_xir_call_result_valid(outcome));
        xr_xir_value_drop(&message);
    } else if (outcome->status == XR_XIR_CALL_ASSERTION) {
        CHECK(kind && !outcome->value.type && !outcome->value.reserved && !outcome->value.payload && outcome->panic.detail.code == 445);
        task_late_text(&outcome->panic.message, "task-panic"); task_fault_resource_owner(run, &outcome->panic.message);
    } else CHECK((outcome->status == run->fault || outcome->status == XR_XIR_CALL_CANCELLED) &&
        !outcome->value.type && !outcome->value.reserved && !outcome->value.payload && xr_xir_panic_empty(&outcome->panic));
}
static void task_fault_resource_late(TaskFaultRun *run, const XrXirCallStatus before[2], bool execution_complete) {
    XrXirCallResult copies[2][2] = {{{0}}}; XrXirValue info = {0}; uintptr_t addresses[2] = {0};
    for (unsigned kind = 0; kind < 2; ++kind) if (run->tasks[kind].type) {
        const XirTask *task = (const XirTask *)(uintptr_t)run->tasks[kind].payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
        CHECK(task->object.domain == run->domain); XrXirCallStatus stored = task->outcome.status;
        if (before[kind] != XR_XIR_CALL_READY) CHECK(stored == before[kind]);
        if (execution_complete) CHECK(stored == (kind ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_THROWN));
        task_late_metadata_only(&run->fixture, xr_xir_value_arena(&run->tasks[kind]));
        for (unsigned n = 0; n < 2; ++n) {
            XrXirCallStatus status = xr_xir_task_copy_outcome(&run->tasks[kind], &copies[kind][n]);
            if (xr_xir_call_result_empty(&copies[kind][n])) {
                CHECK(status == run->fault && xr_xir_value_valid(&run->tasks[kind])); run->complete = false;
            } else { CHECK(status == stored); task_fault_resource_read(run, &copies[kind][n], kind); }
        }
        if (kind && copies[kind][0].status == XR_XIR_CALL_ASSERTION) {
            if (!task_fault_resource_value(run, xr_xir_panic_info_new(run->domain, &copies[kind][0].panic, &info)))
                CHECK(!info.type && !info.reserved && !info.payload && xr_xir_call_result_valid(&copies[kind][0]));
            else task_fault_resource_owner(run, &info);
        }
        addresses[kind] = (uintptr_t)run->tasks[kind].payload;
    }
    size_t attempts = runtime_attempts;
    for (unsigned kind = 0; kind < 2; ++kind) {
        xr_xir_value_drop(&run->tasks[kind]); CHECK(!run->tasks[kind].type && !run->tasks[kind].reserved && !run->tasks[kind].payload);
    }
    CHECK(attempts == runtime_attempts);
    for (size_t slot = 0; slot < runtime_owned_capacity; ++slot) if (runtime_owned[slot].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[slot].pointer;
        for (unsigned kind = 0; kind < 2; ++kind) if (addresses[kind])
            CHECK(addresses[kind] < base || addresses[kind]-base >= runtime_owned[slot].bytes);
    }
    for (unsigned kind = 0; kind < 2; ++kind) for (unsigned n = 0; n < 2; ++n) {
        if (!xr_xir_call_result_empty(&copies[kind][n])) task_fault_resource_read(run, &copies[kind][n], kind);
        xr_xir_call_result_drop(&copies[kind][n]); CHECK(xr_xir_call_result_empty(&copies[kind][n]));
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    if (info.type) {
        XrXirFaultDetail detail = {0}; XrXirValue message = {0};
        CHECK(xr_xir_panic_info_detail(&info, &detail) && detail.code == 445);
        bool copied = task_fault_resource_value(run, xr_xir_panic_info_message(&info, &message));
        if (!copied) CHECK(!message.type && !message.reserved && !message.payload && xr_xir_value_valid(&info));
        else task_fault_resource_owner(run, &message);
        xr_xir_value_drop(&info); if (copied) task_late_text(&message, "task-panic"); xr_xir_value_drop(&message);
    }
}
/* Program preparation precedes runtime injection. Keep one caller reference
 * for statistics while execution, retirement and late access spend the same Domain. */
static bool task_fault_resource_operation(unsigned backend, size_t failure, const XrXirInstanceConfig *limits,
    XrXirDomainBudgetStats *cost, size_t *sites) {
    CHECK(!runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    TaskFaultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .complete=true};
    XrXirProgram *program = task_late_program(&run.fixture, backend); task_fault_layout(program);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    runtime_attempts = 0; runtime_fail_at = failure;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY; CHECK(run.complete || (status == run.fault && !run.instance));
    if (run.instance) { run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain)); }
    xr_xir_compile_program_drop(program); CHECK(run.instance ? !run.fixture.releases : run.fixture.releases == 1);
    if (run.complete) task_fault_resource_call(&run, 1, "cold");
    if (run.complete) task_fault_resource_call(&run, 0, NULL);
    if (run.complete) task_fault_resource_call(&run, 1, "thrown:task-panic:inner:outer");
    if (run.complete) CHECK(run.captured == 2 && run.witness.yielded_count == 2 &&
        run.witness.root_providers == (task_late_native(backend, true) ? 2u : 1u) &&
        run.witness.worker_providers == (task_late_native(backend, false) ? 2u : 1u));
    /* A later retirement or copy fault cannot rewrite a terminal child. */
    XrXirCallStatus before[2] = {0};
    for (unsigned kind = 0; kind < 2; ++kind) if (run.tasks[kind].type) {
        const XirTask *task = (const XirTask *)(uintptr_t)run.tasks[kind].payload;
        if (task->state == XIR_TASK_TERMINAL) before[kind] = task->outcome.status;
    }
    if (run.instance) {
        bool failed = run.driven || run.instance->budget.exhausted;
        status = xr_xir_instance_free(run.instance); run.instance = NULL;
        if (failed) CHECK(status == run.fault); else CHECK(status == XR_XIR_CALL_READY || status == run.fault);
        if (status == run.fault) run.complete = false;
    }
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    const bool execution_complete = run.complete;
    task_fault_resource_late(&run, before, execution_complete);
    if (run.domain) {
        XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(run.domain);
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees &&
            final.metadata_allocations == final.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        CHECK(final.requested_bytes <= config.requested_value_limit && final.requested_call_bytes <= config.requested_call_limit && final.work <= config.work_limit);
        if (cost) *cost = final; xr_xir_domain_drop(run.domain);
    }
    if (sites) *sites = runtime_attempts; runtime_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return run.complete;
}
static size_t task_fault_resource_axes(unsigned backend) {
    XrXirDomainBudgetStats cost = {0}; size_t sites = 0;
    CHECK(task_fault_resource_operation(backend, SIZE_MAX, NULL, &cost, &sites) && sites && sites < 20000);
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis]-minus;
        else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
        else config.work_limit = axes[axis]-minus;
        XrXirDomainBudgetStats spent = {0};
        bool complete = task_fault_resource_operation(backend, SIZE_MAX, &config, &spent, NULL);
        CHECK(complete == !minus);
        printf("Task fault resource axis backend=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend, axis, minus, (unsigned)complete, (unsigned long long)spent.requested_bytes,
            (unsigned long long)spent.requested_call_bytes, (unsigned long long)spent.work);
    }
    printf("Task fault resources backend=%u sites=%zu value=%llu call=%llu work=%llu sameOriginalDomain codeLease1 allTasksDeadBeforeLateReads metadataDeadBeforeInfoString physical0/0\n",
        backend, sites, (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]); return sites;
}
#ifndef XR_TASK_FAULT_PREFIX_RESOURCE_MAIN
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4); (void)task_fault_graph; (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); size_t sites = task_fault_resource_axes(backend);
    if (!strcmp(argv[2], "fi")) {
        CHECK(argc == 4); char *end = NULL; unsigned long expected = strtoul(argv[3], &end, 10);
        CHECK(argv[3][0] && end && !*end && expected == sites && expected < 20000);
        for (size_t site = 0; site < sites; ++site) {
            size_t attempts = 0; CHECK(!task_fault_resource_operation(backend, site, NULL, NULL, &attempts) && attempts > site);
        }
        printf("Task fault complete runtime FI backend=%u sites=%zu everyOrdinalHit typedOOM sameOriginalDomain codeLease1 physical0/0\n", backend, sites);
    } else CHECK(argc == 3 && !strcmp(argv[2], "axes"));
    return 0;
}
#endif
