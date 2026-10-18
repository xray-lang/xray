/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_constructed_control.c - Root cancellation and stop preserve Task ownership
 */
#define main constructed_reference_main
#include "test_xir_task_constructed_results.c"
#undef main

typedef struct ConstructedControl {
    ConstructedFixture base;
    XrXirCallEntry selected[CONSTRUCTED_FUNCTIONS];
    XrXirInstance *instances[2];
    unsigned worker_steps[2];
    bool worker[CONSTRUCTED_FUNCTIONS];
} ConstructedControl;
static ConstructedControl *constructed_control;
static XrXirAction constructed_control_resume(XrXirCallView *view) {
    CHECK(constructed_control && xr_xir_call_admission(view));
    uint32_t entry = xr_xir_call_current_entry(view->activation); CHECK(entry < CONSTRUCTED_FUNCTIONS);
    unsigned instance = 2;
    for (unsigned i = 0; i < 2; ++i) if (constructed_control->instances[i] == view->instance) instance = i;
    CHECK(instance < 2);
    bool child = false;
    CHECK(xr_xir_task_executor_view_member(constructed_control->instances[instance]->executor, view, &child));
    if (constructed_control->worker[entry]) { CHECK(child); ++constructed_control->worker_steps[instance]; }
    return constructed_control->selected[entry].resume(view);
}
static void constructed_control_build(ConstructedControl *control, const char *backend) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    ConstructedFixture *fixture = &control->base; constructed_control = control;
    fixture->lowered = constructed_lower(context, fixture->entry, false);
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->lowered);
    const XrXirProgramSpec *native = &task_constructed_native_program;
    CHECK(module->function_count <= CONSTRUCTED_FUNCTIONS && module->function_count == native->entry_count);
    XrXirProgramPacket packet = xir_program_packet_borrow(fixture->lowered);
    CHECK(packet.length == native->packet.length && !memcmp(packet.bytes, native->packet.bytes, packet.length));
    bool vm = !strcmp(backend, "vm"), all_native = !strcmp(backend, "native"), root_native = !strcmp(backend, "native-vm");
    CHECK(vm || all_native || root_native || !strcmp(backend, "vm-native"));
    for (uint32_t f = 0; f < module->function_count; ++f) {
        control->worker[f] = module->functions[f].name_length >= 7 && !memcmp(module->functions[f].name, "worker_", 7);
        bool use_native = all_native || (!vm && (control->worker[f] != root_native));
        XrXirCallEntry entry = {0};
        CHECK(xr_xir_compile_vm_bind(fixture->lowered, f, &fixture->bindings[f], &entry) == XR_XIR_OK);
        control->selected[f] = use_native ? native->entries[f] : entry;
        fixture->entries[f] = control->selected[f]; fixture->entries[f].resume = constructed_control_resume;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, native->target, fixture->entries, module->function_count,
        module->declarations, {fixture, constructed_code_drop}, module->types, packet};
    CHECK(xr_xir_compile_program_seal(context, &spec, &fixture->program) == XR_XIR_OK);
}
static XirTask *constructed_control_task(ConstructedControl *control,unsigned instance,unsigned family) {
    const XrXirModule *module=xr_xir_compile_artifact_module(control->base.lowered);
    for (XirTask *task=control->instances[instance]->executor->active;task;task=task->active_next) {
        uint32_t entry=task->activation.entry;CHECK(entry<module->function_count);
        const XrXirFunction *function=&module->functions[entry];
        if (function->name_length==7+strlen(constructed_names[family]) &&
            !memcmp(function->name,"worker_",7) &&
            !memcmp(function->name+7,constructed_names[family],strlen(constructed_names[family]))) return task;
    }
    return NULL;
}
static bool constructed_control_frontier(ConstructedControl *control,unsigned instance,unsigned family,unsigned frontier) {
    if (!frontier) return true;
    XirTask *task=constructed_control_task(control,instance,family);
    return task && (frontier == 1 ? !control->worker_steps[instance] : control->worker_steps[instance] == 1);
}
static void constructed_control_pair(ConstructedControl *control, unsigned family, unsigned frontier, bool stop) {
    XrXirDomain *domains[2] = {0}; XrXirValue tasks[2] = {{0}};
    memset(control->worker_steps, 0, sizeof(control->worker_steps));
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(control->base.program, &config, &control->instances[i]) == XR_XIR_CALL_READY);
        domains[i] = control->instances[i]->domain; CHECK(xr_xir_domain_retain(domains[i]));
        CHECK(xr_xir_instance_start(control->instances[i], control->base.entry[family][0], NULL, 0) == XR_XIR_CALL_READY);
    }
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = control->instances[i]; unsigned polls = 0;
        while (!constructed_control_frontier(control, i, family, frontier)) {
            CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY && ++polls < 4096);
        }
        if (frontier) {
            XirTask *task=constructed_control_task(control,i,family);CHECK(task);
            XrXirValue handle=task_core_task_value(task);
            CHECK(xr_xir_value_copy(&handle, &tasks[i]) == XR_XIR_VALUE_OK);
            XrXirCallResult pending = {0}; const size_t attempts = runtime_attempts;
            CHECK(xr_xir_task_copy_outcome(&tasks[i], &pending) == XR_XIR_CALL_BAD_STATE);
            CHECK(xr_xir_call_result_empty(&pending) && runtime_attempts == attempts);
        }
        CHECK((stop ? xr_xir_instance_stop(instance) : xr_xir_instance_cancel_current(instance)) ==
            (stop ? XR_XIR_CALL_READY : XR_XIR_CALL_CANCEL_REQUESTED));
        XrXirInstanceResult result = {0};
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 4096); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); control->instances[i] = NULL;
        if (tasks[i].type) {
            const XirTask *task = (const XirTask *)(uintptr_t)tasks[i].payload;
            CHECK(task->state == XIR_TASK_TERMINAL && !task->executor && !task->call);
            XrXirCallResult first = {0}, second = {0};
            XrXirCallStatus expected = stop ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED;
            CHECK(xr_xir_task_copy_outcome(&tasks[i], &first) == expected);
            CHECK(xr_xir_task_copy_outcome(&tasks[i], &second) == expected);
            if (!stop) { CHECK(constructed_value(&first.value, family)); CHECK(constructed_value(&second.value, family)); }
            else CHECK(!first.value.type && !second.value.type && xr_xir_panic_empty(&first.panic) && xr_xir_panic_empty(&second.panic));
            /* A different constructed tag cannot become a valid terminal Task result. */
            XirTask malformed = *task;
            malformed.outcome.status = XR_XIR_CALL_RETURNED;
            malformed.outcome.value = (XrXirValue){XR_XIR_BOOL, 0, 1};
            CHECK(!xr_xir_task_storage_valid(&malformed));
            xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second); xr_xir_value_drop(&tasks[i]);
        }
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes == sizeof(*domains[i]));
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 3); unsigned family = CONSTRUCTED_CASES;
    for (unsigned i = 0; i < CONSTRUCTED_CASES; ++i) if (!strcmp(argv[1], constructed_names[i])) family = i;
    CHECK(family < CONSTRUCTED_CASES); ConstructedControl control = {0}; constructed_control_build(&control, argv[2]);
    for (unsigned frontier = 0; frontier < 3; ++frontier) for (unsigned stop = 0; stop < 2; ++stop)
        constructed_control_pair(&control, family, frontier, stop != 0);
    xr_xir_compile_program_drop(control.base.program);
    CHECK(control.base.code_releases == 1 && !control.base.lowered && !runtime_live && !runtime_bytes);
    constructed_control = NULL; effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_CONSTRUCTED_CONTROL %s %s rootCancel childReturned stop childCancelled frontiers=3 twoInstances wrongTagDenied physical=0/0\n",
        argv[1], argv[2]); return 0;
}
