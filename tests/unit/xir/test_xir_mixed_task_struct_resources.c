/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_struct_resources.c - Struct outcomes spend one original finite Domain
 */
#define XR_TASK_CANCEL_PREFIX_MAIN
#include "test_xir_mixed_task_pending_resources.c"
#include "xir/xxir_struct.h"
#include "xir/xxir_atomic.h"

typedef struct TaskStructResourceRun {
    TaskResultRun base;
    XrXirValue strings[2];
    uintptr_t atoms[2];
} TaskStructResourceRun;

static bool task_struct_resource_value(TaskStructResourceRun *run, XrXirValueStatus status) {
    return task_result_value_status(&run->base, status);
}
static void task_struct_resource_owner(const TaskStructResourceRun *run, const XrXirValue *value) {
    CHECK(xr_xir_value_valid(value) && object_pointer(value)->domain == run->base.domain);
}
static bool task_struct_resource_get(TaskStructResourceRun *run, const XrXirValue *value,
    unsigned field, XrXirValue *output) {
    task_struct_resource_owner(run, value);
    XrXirValueAdmission admission = {xr_xir_value_arena(value), run->base.domain, NULL, NULL, 10000, 65536};
    XrXirValueStatus status = xr_xir_struct_get(value, field, &admission, output);
    if (!task_struct_resource_value(run, status)) {
        CHECK(!output->type && !output->reserved && !output->payload && xr_xir_value_valid(value)); return false;
    }
    task_struct_resource_owner(run, output); return true;
}
static bool task_struct_resource_array(TaskStructResourceRun *run, const XrXirValue *value, const char *first) {
    task_struct_resource_owner(run, value);
    XrXirValueAdmission admission = {xr_xir_value_arena(value), run->base.domain, NULL, NULL, 10000, 65536};
    int64_t length = -1; XrXirValueStatus status = xr_xir_array_len(value, &admission, &length);
    if (!task_struct_resource_value(run, status)) { CHECK(length == -1); return false; }
    CHECK(length == 2); const char *expected[] = {first, "birch"};
    for (int64_t index = 0; index < 2; ++index) {
        XrXirValue text = {0}; XrXirFaultDetail fault = {0};
        status = xr_xir_array_get(value, index, &admission, &text, &fault);
        if (!task_struct_resource_value(run, status)) {
            CHECK(!text.type && !text.reserved && !text.payload); return false;
        }
        task_late_text(&text, expected[index]); task_struct_resource_owner(run, &text); xr_xir_value_drop(&text);
    }
    return true;
}
static bool task_struct_resource_atomic(TaskStructResourceRun *run, const XrXirValue *value, bool add, int64_t expected) {
    task_struct_resource_owner(run, value);
    XrXirValue operand = {XR_XIR_I64,0,7}, output = {0};
    XrXirValueAdmission admission = {xr_xir_value_arena(value), run->base.domain, NULL, NULL, 10000, 65536};
    XrXirAtomicRequest request = {value, add ? &operand : NULL, NULL, add ? 1u : 0u,
        add ? XR_XIR_ATOMIC_OPERATION_FETCH_ADD : XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_I64};
    XrXirAtomicProgress progress = {0};
    XrXirAtomicOutcome result = xr_xir_atomic_start(&request, &admission, NULL, &progress, &output);
    bool complete = result.status == XR_XIR_RUN_OK && result.permission == XR_XIR_CALL_READY;
    if (!complete) {
        CHECK(!output.type && !output.reserved && !output.payload);
        if (result.permission != XR_XIR_CALL_READY) CHECK(result.permission == run->base.fault);
        else if (run->base.fault == XR_XIR_CALL_OOM) CHECK(result.status == XR_XIR_RUN_OUT_OF_MEMORY);
        else CHECK(result.status == XR_XIR_RUN_STEP_LIMIT || result.status == XR_XIR_RUN_FRAME_LIMIT);
        run->base.complete = false;
    } else CHECK(!result.continuing && output.type == XR_XIR_I64 && !output.reserved && output.payload == expected);
    xr_xir_atomic_progress_clear(&progress); xr_xir_value_drop(&output); return complete;
}
static void task_struct_resource_pair(TaskStructResourceRun *run, XrXirValue *first,
    const XrXirValue *second, unsigned round) {
    XrXirValue array = {0}, other = {0}, atomic = {0}, alias = {0}, replacement = {0};
    const XrXirTypeArena *arena = xr_xir_value_arena(first);
    if (!task_struct_resource_get(run, first, 0, &array) || !task_struct_resource_get(run, second, 0, &other)) goto done;
    if (!task_struct_resource_array(run, &array, "cedar") || !task_struct_resource_array(run, &other, "cedar")) goto done;
    if (!task_struct_resource_get(run, first, 1, &atomic) || !task_struct_resource_get(run, second, 1, &alias)) goto done;
    CHECK(atomic.type == alias.type && atomic.payload == alias.payload); run->atoms[round] = (uintptr_t)atomic.payload;
    if (!task_struct_resource_atomic(run, &atomic, false, 42) || !task_struct_resource_atomic(run, &atomic, true, 42) ||
        !task_struct_resource_atomic(run, &atomic, false, 49) || !task_struct_resource_atomic(run, &alias, false, 49)) goto done;
    XrXirValueStatus status = xr_xir_string_new(run->base.domain, "elm", 3, &replacement);
    if (!task_struct_resource_value(run, status)) { CHECK(!replacement.type && !replacement.reserved && !replacement.payload); goto done; }
    XrXirValueAdmission admission = {arena, run->base.domain, NULL, NULL, 10000, 65536};
    XrXirValue saved_array = array, saved_struct = *first; XrXirFaultDetail fault = {0};
    XrXirValuePlace element = {(XrXirType)array.type, &array.payload};
    status = xr_xir_array_set(&element, 0, &replacement, &admission, &fault);
    if (!task_struct_resource_value(run, status)) {
        CHECK(!memcmp(&array, &saved_array, sizeof(array)) && !memcmp(first, &saved_struct, sizeof(*first))); goto done;
    }
    XrXirValuePlace field = {(XrXirType)first->type, &first->payload};
    status = xr_xir_struct_set(&field, 0, &array, &admission);
    if (!task_struct_resource_value(run, status)) { CHECK(!memcmp(first, &saved_struct, sizeof(*first))); goto done; }
    CHECK(first->payload != second->payload && second->payload == saved_struct.payload);
    xr_xir_value_drop(&array); xr_xir_value_drop(&other); xr_xir_value_drop(&replacement);
    if (!task_struct_resource_get(run, first, 0, &array) || !task_struct_resource_get(run, second, 0, &other)) goto done;
    if (!task_struct_resource_array(run, &array, "elm") || !task_struct_resource_array(run, &other, "cedar")) goto done;
    xr_xir_value_drop(&atomic); xr_xir_value_drop(&alias);
    if (!task_struct_resource_get(run, first, 1, &atomic) || !task_struct_resource_get(run, second, 1, &alias)) goto done;
    CHECK((uintptr_t)atomic.payload == run->atoms[round] && atomic.payload == alias.payload);
    if (!task_struct_resource_atomic(run, &atomic, false, 49) || !task_struct_resource_atomic(run, &alias, false, 49)) goto done;
    status = xr_xir_array_get(&other, 1, &admission, &run->strings[round], &fault);
    if (!task_struct_resource_value(run, status)) CHECK(!run->strings[round].type && !run->strings[round].reserved && !run->strings[round].payload);
    else task_struct_resource_owner(run, &run->strings[round]);
done:
    xr_xir_value_drop(&array); xr_xir_value_drop(&other); xr_xir_value_drop(&atomic);
    xr_xir_value_drop(&alias); xr_xir_value_drop(&replacement);
    task_late_metadata_only(&run->base.fixture, arena);
}
static void task_struct_resource_read(TaskStructResourceRun *run, const XrXirValue *value) {
    XrXirValue array = {0}, atomic = {0};
    if (task_struct_resource_get(run, value, 0, &array)) (void)task_struct_resource_array(run, &array, "cedar");
    if (task_struct_resource_get(run, value, 1, &atomic)) (void)task_struct_resource_atomic(run, &atomic, false, 42);
    xr_xir_value_drop(&array); xr_xir_value_drop(&atomic);
}
static bool task_struct_resource_physical(uintptr_t address) {
    for (size_t i = 0; i < runtime_owned_capacity; ++i) if (runtime_owned[i].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[i].pointer;
        if (address >= base && address-base < runtime_owned[i].bytes) return true;
    }
    return false;
}
/* Fix expected execution before late-copy faults. A later LIMIT/OOM must not
 * weaken a previously completed Task's original terminal state. */
static void task_struct_resource_late(TaskStructResourceRun *run, unsigned mode,
    const XrXirCallStatus before[2], bool execution_complete) {
    XrXirCallResult copies[2][2] = {{{0}}}; bool copied[2][2] = {{0}};
    XrXirCallStatus stored[2] = {0}; uintptr_t addresses[2] = {0};
    for (unsigned round = 0; round < 2; ++round) if (run->base.tasks[round].type) {
        XrXirValue *value = &run->base.tasks[round]; const XirTask *task = (const XirTask *)(uintptr_t)value->payload;
        CHECK(xr_xir_task_storage_valid(task) && task->state == XIR_TASK_TERMINAL && !task->call && !task->executor);
        task_struct_resource_owner(run, value); stored[round] = task->outcome.status;
        if (before[round] != XR_XIR_CALL_READY) CHECK(stored[round] == before[round]);
        if (execution_complete) CHECK(stored[round] == (round && mode >= 2 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
        else CHECK(stored[round] == XR_XIR_CALL_RETURNED || stored[round] == XR_XIR_CALL_CANCELLED || stored[round] == run->base.fault);
        const XrXirTypeArena *arena = xr_xir_value_arena(value); task_late_metadata_only(&run->base.fixture, arena);
        const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
        const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)value->type);
        CHECK(node && node->kind == XR_XIR_TYPE_TASK && !node->parameter_span && xr_xir_type_is_struct(types, node->element));
        addresses[round] = (uintptr_t)value->payload; CHECK(task_struct_resource_physical(addresses[round]));
        XrXirCallResult occupied = {.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,193}}, saved;
        memcpy(&saved, &occupied, sizeof(saved));
        XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(run->base.domain); size_t attempts = runtime_attempts;
        CHECK(xr_xir_task_copy_outcome(value, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&saved, &occupied, sizeof(saved)));
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(run->base.domain);
        CHECK(cost.work == after.work && cost.requested_bytes == after.requested_bytes &&
            cost.requested_call_bytes == after.requested_call_bytes && attempts == runtime_attempts);
        for (unsigned i = 0; i < 2; ++i) {
            XrXirCallStatus status = xr_xir_task_copy_outcome(value, &copies[round][i]);
            copied[round][i] = !xr_xir_call_result_empty(&copies[round][i]);
            if (!copied[round][i]) {
                CHECK(status == run->base.fault && xr_xir_value_valid(value)); run->base.complete = false;
            } else {
                CHECK(status == stored[round] && !copies[round][i].wake && xr_xir_panic_empty(&copies[round][i].panic));
                if (status != XR_XIR_CALL_RETURNED) CHECK(!copies[round][i].value.type && !copies[round][i].value.reserved && !copies[round][i].value.payload);
            }
        }
    }
    size_t attempts = runtime_attempts;
    for (unsigned round = 0; round < 2; ++round) {
        xr_xir_value_drop(&run->base.tasks[round]);
        CHECK(!run->base.tasks[round].type && !run->base.tasks[round].reserved && !run->base.tasks[round].payload);
    }
    CHECK(attempts == runtime_attempts);
    for (unsigned round = 0; round < 2; ++round) if (addresses[round]) CHECK(!task_struct_resource_physical(addresses[round]));
    for (unsigned round = 0; round < 2; ++round) if (stored[round] == XR_XIR_CALL_RETURNED) {
        if (copied[round][0] && copied[round][1]) task_struct_resource_pair(run, &copies[round][0].value, &copies[round][1].value, round);
        else for (unsigned i = 0; i < 2; ++i) if (copied[round][i]) task_struct_resource_read(run, &copies[round][i].value);
    }
    if (run->atoms[0] && run->atoms[1]) CHECK(run->atoms[0] != run->atoms[1]);
    for (unsigned round = 0; round < 2; ++round) for (unsigned i = 0; i < 2; ++i) {
        xr_xir_call_result_drop(&copies[round][i]); CHECK(xr_xir_call_result_empty(&copies[round][i]));
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 2; ++i) if (run->strings[i].type) {
        task_late_text(&run->strings[i], "birch"); xr_xir_value_drop(&run->strings[i]);
    }
}
static void task_struct_resource_cold(TaskStructResourceRun *run) {
    XrXirCallStatus status = xr_xir_instance_start(run->base.instance, p20_task_result_entries[1], NULL, 0);
    if (status != XR_XIR_CALL_READY) { CHECK(status == run->base.fault); run->base.complete = false; return; }
    unsigned polls = 0;
    do { status = task_pending_resource_tick(&run->base); CHECK(++polls < 4096); } while (status == XR_XIR_CALL_READY);
    if (!run->base.complete) return;
    CHECK(status == XR_XIR_CALL_RETURNED); XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(run->base.instance, &value) == XR_XIR_CALL_RETURNED);
    task_late_text(&value, "cold"); task_struct_resource_owner(run, &value); xr_xir_value_drop(&value);
}
/* Each attempt owns a Program. Only compiler preparation precedes runtime FI;
 * constructors, real user code, control, retirement and late access share one sequence. */
static bool task_struct_resource_operation(unsigned backend, unsigned mode, size_t failure,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost, size_t *sites) {
    CHECK(mode < 4 && !runtime_live && !runtime_bytes); runtime_fail_at = SIZE_MAX;
    TaskStructResourceRun run = {.base={.fault=limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM,.complete=true}};
    XrXirProgram *program = task_late_program(&run.base.fixture, backend);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    runtime_attempts = 0; runtime_fail_at = failure;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &run.base.instance);
    run.base.complete = status == XR_XIR_CALL_READY;
    CHECK(run.base.complete || (status == run.base.fault && !run.base.instance));
    if (run.base.instance) { run.base.domain = run.base.instance->domain; CHECK(xr_xir_domain_retain(run.base.domain)); }
    xr_xir_compile_program_drop(program); CHECK(run.base.instance ? !run.base.fixture.releases : run.base.fixture.releases == 1);
    if (run.base.complete) task_struct_resource_cold(&run);
    if (run.base.complete) { task_result_round(&run.base, 0); task_result_cost(&run.base, false); }
    XrXirCallStatus before[2] = {XR_XIR_CALL_READY,XR_XIR_CALL_READY};
    if (run.base.tasks[0].type) {
        const XirTask *warm = (const XirTask *)(uintptr_t)run.base.tasks[0].payload;
        CHECK(warm->state == XIR_TASK_TERMINAL && warm->outcome.status == XR_XIR_CALL_RETURNED); before[0] = XR_XIR_CALL_RETURNED;
    }
    if (run.base.complete) {
        CHECK(run.base.instance->domain == run.base.domain);
        task_pending_resource_control(&run.base, mode); task_result_cost(&run.base, true);
    }
    if (run.base.complete) {
        CHECK(run.base.witness.root_providers == (task_late_native(backend, true) ? 2u : 1u));
        CHECK(run.base.witness.worker_providers == (task_late_native(backend, false) ? 2u : 1u) && run.base.witness.yielded_count == 2);
    }
    for (unsigned round = 0; round < 2; ++round) if (run.base.tasks[round].type) {
        const XirTask *task = (const XirTask *)(uintptr_t)run.base.tasks[round].payload;
        if (before[round] != XR_XIR_CALL_READY) CHECK(task->state == XIR_TASK_TERMINAL && task->outcome.status == before[round]);
        else if (task->state == XIR_TASK_TERMINAL) before[round] = task->outcome.status;
    }
    if (run.base.instance) {
        XrXirCallStatus expected = run.base.driven || run.base.instance->budget.exhausted ? run.base.fault : XR_XIR_CALL_READY;
        status = xr_xir_instance_free(run.base.instance); run.base.instance = NULL;
        if (status != expected) fprintf(stderr,"Struct resource retirement backend=%u mode=%u site=%zu status=%u expected=%u\n",backend,mode,failure,status,expected);
        CHECK(status == expected);
    }
    CHECK(run.base.fixture.releases == 1 && !run.base.fixture.vm && !run.base.fixture.entries);
    const bool execution_complete = run.base.complete;
    task_struct_resource_late(&run, mode, before, execution_complete);
    if (run.base.domain) {
        task_result_cost(&run.base, false); XrXirDomainBudgetStats final = xr_xir_domain_budget_stats(run.base.domain);
        CHECK(!final.call_live && !final.metadata_live && final.call_allocations == final.call_frees &&
            final.metadata_allocations == final.metadata_frees && xr_xir_domain_stats(run.base.domain).live_bytes == sizeof(*run.base.domain));
        CHECK(final.requested_bytes <= config.requested_value_limit && final.requested_call_bytes <= config.requested_call_limit && final.work <= config.work_limit);
        if (cost) *cost = final;
        xr_xir_domain_drop(run.base.domain);
    }
    if (sites) *sites = runtime_attempts;
    runtime_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return run.base.complete;
}
static size_t task_struct_resource_axes(unsigned backend, unsigned mode) {
    XrXirDomainBudgetStats cost = {0}; size_t sites = 0;
    CHECK(task_struct_resource_operation(backend, mode, SIZE_MAX, NULL, &cost, &sites) && sites && sites < 20000);
    const uint64_t axes[] = {cost.requested_bytes,cost.requested_call_bytes,cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis]-minus;
        else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
        else config.work_limit = axes[axis]-minus;
        XrXirDomainBudgetStats spent = {0};
        bool complete = task_struct_resource_operation(backend, mode, SIZE_MAX, &config, &spent, NULL);
        CHECK(complete == !minus);
        printf("Struct Task resource axis backend=%u mode=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend,mode,axis,minus,(unsigned)complete,(unsigned long long)spent.requested_bytes,
            (unsigned long long)spent.requested_call_bytes,(unsigned long long)spent.work);
    }
    printf("Struct Task resources backend=%u mode=%u sites=%zu value=%llu call=%llu work=%llu originalDomain coldWarmPendingControl codeDead allTasksDead lateStructArrayCOW realAtomic metadataBeforeStrings physical0/0\n",
        backend,mode,sites,(unsigned long long)axes[0],(unsigned long long)axes[1],(unsigned long long)axes[2]); return sites;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 5); (void)task_late_graph; (void)task_result_axes; (void)task_pending_resource_axes;
    static const char *const names[] = {"0","1","vm","native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1],names[backend])) ++backend;
    CHECK(backend < 4);
    if (!strcmp(argv[2],"axes")) {
        CHECK(argc == 3); for (unsigned mode = 0; mode < 4; ++mode) (void)task_struct_resource_axes(backend,mode); return 0;
    }
    CHECK(argc == 5 && !strcmp(argv[2],"fi"));
    unsigned mode = (unsigned)task_pending_resource_number(argv[3]); size_t expected = task_pending_resource_number(argv[4]); CHECK(mode < 4);
    size_t sites = task_struct_resource_axes(backend,mode); CHECK(sites == expected);
    for (size_t site = 0; site < sites; ++site) {
        size_t attempts = 0; CHECK(!task_struct_resource_operation(backend,mode,site,NULL,NULL,&attempts) && attempts > site);
    }
    printf("Struct Task complete runtime FI backend=%u mode=%u sites=%zu everyOriginalOrdinalHit typedOOM sameOriginalDomain codeLease1 allTasksDead physical0/0\n",backend,mode,sites);
    return 0;
}
