/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_result_resources.c - Late compound results keep the original finite Domain
 */
#define XR_TASK_RESULT_RESOURCE_MAIN
#include "test_xir_mixed_task_result_lifetime.c"

typedef struct TaskResultRun {
    TaskLateFixture fixture;
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirValue tasks[2];
    TaskLateWitness witness;
    XrXirDomainBudgetStats previous;
    XrXirCallStatus fault;
    bool complete, driven;
} TaskResultRun;

static void task_result_cost(TaskResultRun *run, bool second) {
    XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(run->domain);
    CHECK(cost.requested_bytes >= run->previous.requested_bytes &&
        cost.requested_call_bytes >= run->previous.requested_call_bytes && cost.work >= run->previous.work);
    if (second && run->complete) CHECK(cost.requested_call_bytes > run->previous.requested_call_bytes &&
        cost.work > run->previous.work && cost.requested_bytes > run->previous.requested_bytes);
    run->previous = cost;
}
static void task_result_round(TaskResultRun *run, unsigned round) {
    XrXirCallStatus status = xr_xir_instance_start(run->instance, p20_task_result_entries[0], NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == run->fault); run->complete = false; return; }
    unsigned polls = 0;
    do {
        status = xr_xir_instance_poll_bounded(run->instance, 1).outcome.status; CHECK(++polls < 4096);
        if (status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED)
            task_late_observe(&run->fixture, run->instance, &run->witness);
    } while (status == XR_XIR_CALL_READY);
    if (status != XR_XIR_CALL_RETURNED) {
        CHECK(status == run->fault); run->complete = false; run->driven = true; return;
    }
    CHECK(xr_xir_instance_take_result(run->instance, &run->tasks[round]) == XR_XIR_CALL_RETURNED);
    XrXirValue empty = {0};
    CHECK(xr_xir_instance_take_result(run->instance, &empty) == XR_XIR_CALL_BAD_STATE && !empty.type && !empty.payload);
}
static bool task_result_value_status(TaskResultRun *run, XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return true;
    CHECK(status == (run->fault == XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT));
    run->complete = false; return false;
}
static bool task_result_read(TaskResultRun *run, const XrXirValue *value, const char *first) {
    XrXirValueAdmission admission = {xr_xir_value_arena(value), NULL, NULL, NULL, 10000, 65536};
    int64_t length = -1; XrXirValueStatus status = xr_xir_array_len(value, &admission, &length);
    if (!task_result_value_status(run, status)) { CHECK(length == -1); return false; }
    CHECK(length == 2); const char *expected[] = {first, "birch"};
    for (int64_t at = 0; at < 2; ++at) {
        XrXirValue text = {0}; XrXirFaultDetail fault = {0};
        status = xr_xir_array_get(value, at, &admission, &text, &fault);
        if (!task_result_value_status(run, status)) { CHECK(!text.type && !text.reserved && !text.payload); return false; }
        task_late_text(&text, expected[at]); xr_xir_value_drop(&text);
    }
    return true;
}
static void task_result_late_cow(TaskResultRun *run, XrXirValue *first, const XrXirValue *second) {
    if (!task_result_read(run, first, "cedar") || !task_result_read(run, second, "cedar")) return;
    XrXirValue replacement = {0};
    XrXirValueStatus status = xr_xir_string_new(run->domain, "elm", 3, &replacement);
    if (!task_result_value_status(run, status)) { CHECK(!replacement.type && !replacement.payload); return; }
    const XrXirTypeArena *arena = xr_xir_value_arena(first);
    XrXirValueAdmission admission = {arena, run->domain, NULL, NULL, 10000, 65536};
    XrXirValue saved = *first;
    XrXirValuePlace place = {(XrXirType)first->type, &first->payload}; XrXirFaultDetail fault = {0};
    status = xr_xir_array_set(&place, 0, &replacement, &admission, &fault);
    xr_xir_value_drop(&replacement);
    if (!task_result_value_status(run, status)) {
        CHECK(!memcmp(first, &saved, sizeof(saved))); (void)task_result_read(run, first, "cedar");
    } else (void)task_result_read(run, first, "elm");
    (void)task_result_read(run, second, "cedar");
    task_late_metadata_only(&run->fixture, arena);
}
static void task_result_copy(TaskResultRun *run, XrXirValue *task) {
    if (!task->type) return;
    const XirTask *owned = (const XirTask *)(uintptr_t)task->payload;
    CHECK(xr_xir_task_storage_valid(owned) && owned->state == XIR_TASK_TERMINAL && !owned->call && !owned->executor);
    CHECK(owned->outcome.status == XR_XIR_CALL_RETURNED && owned->object.domain == run->domain);
    task_late_metadata_only(&run->fixture, xr_xir_value_arena(task));
    XrXirCallResult copies[2] = {{0}}; bool copied[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirCallStatus status = xr_xir_task_copy_outcome(task, &copies[i]);
        copied[i] = status == XR_XIR_CALL_RETURNED;
        if (!copied[i]) {
            CHECK(status == run->fault && xr_xir_call_result_empty(&copies[i]) && xr_xir_value_valid(task));
            run->complete = false;
        } else CHECK(!copies[i].wake && xr_xir_panic_empty(&copies[i].panic));
    }
    xr_xir_value_drop(task); CHECK(!task->type && !task->reserved && !task->payload);
    if (copied[0] && copied[1]) task_result_late_cow(run, &copies[0].value, &copies[1].value);
    else for (unsigned i = 0; i < 2; ++i) if (copied[i]) (void)task_result_read(run, &copies[i].value, "cedar");
    for (unsigned i = 0; i < 2; ++i) { xr_xir_call_result_drop(&copies[i]); CHECK(xr_xir_call_result_empty(&copies[i])); }
}
/* Preparation is outside runtime injection. Each transaction nevertheless owns
 * its Program, drops the caller before execution, and retires code before late copies. */
static bool task_result_operation(unsigned backend, size_t fail_at,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost, size_t *attempts) {
    TaskResultRun run = {.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM, .complete=true};
    runtime_fail_at = SIZE_MAX;
    XrXirProgram *program = task_late_program(&run.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    runtime_attempts = 0; runtime_fail_at = fail_at;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.instance);
    run.complete = status == XR_XIR_CALL_READY; CHECK(run.complete || (status == run.fault && !run.instance));
    if (run.instance) { run.domain = run.instance->domain; CHECK(xr_xir_domain_retain(run.domain)); }
    xr_xir_compile_program_drop(program);
    CHECK(run.instance ? !run.fixture.releases : run.fixture.releases == 1);
    for (unsigned round = 0; run.complete && round < 2; ++round) {
        CHECK(run.instance->domain == run.domain); task_result_round(&run, round); task_result_cost(&run, round != 0);
    }
    if (run.complete) {
        CHECK(run.witness.root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(run.witness.worker_providers == (task_late_native(backend, false) ? 2u : 1u) && run.witness.yielded_count == 2);
    }
    if (run.instance) {
        XrXirCallStatus expected = run.driven || run.instance->budget.exhausted ? run.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.instance); run.instance = NULL;
        if (status != expected) fprintf(stderr, "Task resource retirement backend=%u site=%zu actual=%u expected=%u\n", backend, fail_at, status, expected);
        CHECK(status == expected);
    }
    CHECK(run.fixture.releases == 1 && !run.fixture.vm && !run.fixture.entries);
    for (unsigned round = 0; round < 2; ++round) task_result_copy(&run, &run.tasks[round]);
    if (run.domain) {
        task_result_cost(&run, false);
        XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(run.domain);
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees &&
            final.metadata_allocations == final.metadata_frees && xr_xir_domain_stats(run.domain).live_bytes == sizeof(*run.domain));
        CHECK(final.requested_bytes <= config.requested_value_limit && final.requested_call_bytes <= config.requested_call_limit &&
            final.work <= config.work_limit);
        if (cost) *cost = final;
        xr_xir_domain_drop(run.domain);
    }
    if (attempts) *attempts = runtime_attempts;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    runtime_fail_at = SIZE_MAX; return run.complete;
}
static size_t task_result_axes(unsigned backend) {
    XrXirDomainBudgetStats cost = {0}; size_t sites = 0;
    CHECK(task_result_operation(backend, SIZE_MAX, NULL, &cost, &sites) && sites && sites < 20000);
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis] - minus;
        else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
        else config.work_limit = axes[axis] - minus;
        XrXirDomainBudgetStats spent = {0};
        bool complete = task_result_operation(backend, SIZE_MAX, &config, &spent, NULL);
        CHECK(complete == !minus);
        printf("Task result axis backend=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend, axis, minus, (unsigned)complete, (unsigned long long)spent.requested_bytes,
            (unsigned long long)spent.requested_call_bytes, (unsigned long long)spent.work);
    }
    printf("Task result resources backend=%u sites=%zu value=%llu call=%llu work=%llu sameInstanceDomainTwoRounds codeDeadBeforeLateCopy exact/minus1 physical0/0\n",
        backend, sites, (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
    return sites;
}
#ifndef XR_TASK_PENDING_RESOURCE_MAIN
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4); (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4); size_t sites = task_result_axes(backend);
    if (!strcmp(argv[2], "fi")) {
        char *end = NULL; CHECK(argc == 4);
        unsigned long expected = strtoul(argv[3], &end, 10);
        CHECK(argv[3][0] && end && !*end && expected == sites && expected < 20000);
        for (size_t site = 0; site < sites; ++site) {
            size_t attempts = 0;
            CHECK(!task_result_operation(backend, site, NULL, NULL, &attempts) && attempts > site);
        }
        printf("Task result complete runtime FI backend=%u sites=%zu everyOrdinalHit typedOOM finalCodeDead lateCopies physical0/0\n", backend, sites);
    } else CHECK(argc == 3 && !strcmp(argv[2], "axes"));
    return 0;
}
#endif
