/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_pure_worker.c - Library state does not taint a pure GO entry
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_call_observer_internal.h"
XR_DATA const XrXirProgramSpec library_counter_fixture_program;
XR_DATA const uint32_t library_counter_entry;
#define XR_WORKER_CONTROL_CUSTOM_CASE {"catalog-pure", &library_counter_fixture_program, &library_counter_entry}
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"

typedef struct LibraryPureRecord {
    XrXirInstance *instance;
    XrXirCallObserver observer;
    XrXirValue tasks[2];
    unsigned round, entered, left, live, roots, ticks, pure, yields, vm, native;
} LibraryPureRecord;
typedef struct LibraryPureCode {
    WorkerControlCode base;
    LibraryPureRecord records[2];
    uint32_t tick, pure;
} LibraryPureCode;
static LibraryPureCode *library_pure_code;

static void library_pure_event(void *owner, const XrXirCallEvent *event) {
    LibraryPureRecord *record = owner; LibraryPureCode *code = library_pure_code;
    CHECK(event && code && event->instance == record->instance && event->entry <= code->base.functions);
    CHECK(event->binding.owner && event->binding.activation && event->binding.generation && event->binding.ticket && event->frame);
    if (event->kind == XR_XIR_CALL_FRAME_PRESENT || event->kind == XR_XIR_CALL_FRAME_ENTER) { ++record->entered; ++record->live; }
    if (event->kind != XR_XIR_CALL_FRAME_LEAVE) return;
    CHECK(record->live && event->status == XR_XIR_CALL_RETURNED); --record->live; ++record->left;
    if (event->entry >= code->base.functions) return;
    CHECK(event->code == (code->base.native[event->entry] ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
    if (event->code == XR_XIR_CALL_CODE_VM) ++record->vm; else ++record->native;
    if (event->entry == code->base.root) ++record->roots;
    if (event->entry == code->tick) ++record->ticks;
    if (event->entry == code->pure) ++record->pure;
}
static XrXirAction library_pure_resume(XrXirCallView *view) {
    LibraryPureCode *code = library_pure_code; CHECK(code);
    LibraryPureRecord *record = view->instance == code->records[0].instance ? &code->records[0] : &code->records[1];
    CHECK(view->instance && view->instance == record->instance && record->round < 2);
    CHECK(xr_xir_call_observer_bind(view, &record->observer) == XR_XIR_CALL_READY);
    XrXirAction action = worker_control_resume(view);
    if (action.kind == XR_XIR_ACTION_AWAIT_TASK && !record->tasks[record->round].type) {
        CHECK(xr_xir_call_current_entry(view->activation) == code->base.root);
        CHECK(xr_xir_value_copy(&action.value, &record->tasks[record->round]) == XR_XIR_VALUE_OK);
    }
    if (action.kind == XR_XIR_ACTION_SUSPEND) {
        CHECK(xr_xir_call_current_entry(view->activation) == code->pure); ++record->yields;
    }
    return action;
}
static void library_pure_free(void *pointer) {
    LibraryPureCode *code = pointer; CHECK(code == library_pure_code); library_pure_code = NULL;
    for (unsigned i = 0; i < 2; ++i) CHECK(!code->records[i].live && !code->records[i].tasks[0].type && !code->records[i].tasks[1].type);
    worker_control_free(&code->base);
}
static uint32_t library_pure_entry(const XrXirModule *module, const char *name) {
    uint32_t result = UINT32_MAX; size_t length = strlen(name);
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == length && !memcmp(module->functions[f].name, name, length)) { CHECK(result == UINT32_MAX); result = f; }
    CHECK(result != UINT32_MAX); return result;
}
static XrXirProgram *library_pure_program(unsigned backend) {
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, library_counter_fixture_program.proof.bytes,
        library_counter_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &library_counter_fixture_program.target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == library_counter_fixture_program.entry_count && module->function_count <= WORKER_FUNCTIONS);
    CHECK(module->declarations->module_count == 2 && module->declarations->slot_count == 1 && module->declarations->slots[0].mutable);
    LibraryPureCode *owner = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    WorkerControlCode *code = &owner->base; code->functions = module->function_count; code->root = library_counter_entry;
    owner->tick = library_pure_entry(module, "tick"); owner->pure = library_pure_entry(module, "pure"); worker_control_targets(code, module);
    CHECK(module->declarations->functions[owner->tick].module == module->declarations->functions[owner->pure].module);
    CHECK(module->declarations->functions[owner->pure].module != module->declarations->root_module && code->workers[owner->pure] && !code->workers[owner->tick]);
    CHECK(xr_compile_resources_calloc(context.resources, code->functions, sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    for (uint32_t f = 0; f < code->functions; ++f) {
        code->native[f] = backend == 3 || (backend < 2 && (f == code->root) == (backend == 1));
        CHECK(library_counter_fixture_program.entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = code->native[f] ? library_counter_fixture_program.entries[f] : code->vm->entries[f];
        code->delegates[f] = code->entries[f].resume; code->entries[f].resume = library_pure_resume;
    }
    XrXirProgramSpec spec = library_counter_fixture_program;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){owner, library_pure_free};
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK);
    CHECK(program->permissions->entries[owner->tick].requires_root && program->permissions->entries[owner->tick].worker != XR_XIR_OK);
    CHECK(!program->permissions->entries[owner->pure].requires_root && !program->permissions->entries[owner->pure].unresolved &&
        program->permissions->entries[owner->pure].worker == XR_XIR_OK);
    CHECK(!library_pure_code && !worker_control_code); library_pure_code = owner; worker_control_code = code;
    XrCompileResourceStats stats = {0}; CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    printf("Catalog pure preparation backend=%s sites=%zu allocated=%llu peak=%llu work=%llu, tickRoot pureNone workerAdmitted sameLibrarySlot1\n",
        worker_backends[backend], effects_compile_attempts, (unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    xr_compile_resources_release(context.resources); return program;
}
static void library_pure_late(XrXirValue *tasks) {
    CHECK(effects_compile_live == 2 && effects_compile_bytes && !effects_source_owner_count);
    CHECK(xr_xir_value_valid(&tasks[0])); const XrXirTypeArena *arena = xr_xir_value_arena(&tasks[0]); CHECK(arena);
    const CompileAllocation *allocation = (const CompileAllocation *)(const void *)arena-1;
    XrCompileResources *resources = allocation->info.resources;
    CHECK(resources && resources->references == 1 && effects_compile_bytes == sizeof(*resources)+sizeof(*allocation)+allocation->info.bytes);
    for (size_t slot = 0; slot < 32768; ++slot) {
        EffectsCompileAllocation live = effects_compile_allocations[slot];
        if (live.pointer == resources) CHECK(live.bytes == sizeof(*resources));
        else if (live.pointer == allocation) CHECK(live.bytes == sizeof(*allocation)+allocation->info.bytes);
        else CHECK(!live.pointer);
    }
    printf("Catalog pure late compilerBlocks=%zu compilerBytes=%zu ledgerReferences=%llu, exactTaskTypeArenaAndLedgerOnly\n",
        effects_compile_live, effects_compile_bytes, (unsigned long long)resources->references);
    XrXirCallResult outcomes[4] = {{0}};
    for (unsigned i = 0; i < 4; ++i) {
        XirTask *task = (XirTask *)(uintptr_t)tasks[i].payload;
        CHECK(xr_xir_value_valid(&tasks[i]) && !task->executor && !task->call && atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL);
        CHECK(xr_xir_value_arena(&tasks[i]) == arena);
        CHECK(xr_xir_task_copy_outcome(&tasks[i], &outcomes[i]) == XR_XIR_CALL_RETURNED && outcomes[i].status == XR_XIR_CALL_RETURNED);
        CHECK(!xr_xir_call_result_empty(&outcomes[i]) && xr_xir_panic_empty(&outcomes[i].panic));
        xr_xir_value_drop(&tasks[i]);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(outcomes[i].value.type == XR_XIR_I64 && outcomes[i].value.payload == 7); xr_xir_call_result_drop(&outcomes[i]);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)worker_control_main; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend; CHECK(backend < 4);
    XrXirProgram *program = library_pure_program(backend); XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        LibraryPureRecord *record = &library_pure_code->records[i]; record->observer = (XrXirCallObserver){record, library_pure_event};
        CHECK(xr_xir_instance_new(program, &config, &record->instance) == XR_XIR_CALL_READY);
    }
    CHECK(library_pure_code->records[0].instance->domain != library_pure_code->records[1].instance->domain);
    xr_xir_compile_program_drop(program); XrXirValue results[4] = {{0}}, tasks[4] = {{0}};
    for (unsigned round = 0; round < 2; ++round) {
        bool done[2] = {0}; unsigned polls = 0;
        for (unsigned i = 0; i < 2; ++i) {
            LibraryPureRecord *record = &library_pure_code->records[i]; record->round = round;
            CHECK(xr_xir_instance_start(record->instance, library_counter_entry, NULL, 0) == XR_XIR_CALL_READY);
        }
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            LibraryPureRecord *record = &library_pure_code->records[i]; XrXirDomain *peer = library_pure_code->records[1-i].instance->domain;
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(peer);
            XrXirCallStatus status = xr_xir_instance_poll_bounded(record->instance, 1).outcome.status;
            CHECK(++polls < 32768 && (status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED)); done[i] = status == XR_XIR_CALL_RETURNED;
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(peer);
            CHECK(before.work == after.work && before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes);
        }
        for (unsigned i = 0; i < 2; ++i) {
            LibraryPureRecord *record = &library_pure_code->records[i]; unsigned index = 2*round+i;
            CHECK(!record->live && record->entered == record->left && record->roots == round+1 && record->ticks == 2*(round+1) && record->pure == round+1 && record->yields == round+1);
            if (backend == 2) CHECK(record->vm && !record->native); else if (backend == 3) CHECK(record->native && !record->vm); else CHECK(record->vm && record->native);
            CHECK(xr_xir_instance_take_result(record->instance, &results[index]) == XR_XIR_CALL_RETURNED);
            CHECK(results[index].type == XR_XIR_I64 && results[index].payload == (round ? 374 : 172));
            printf("Catalog pure step backend=%s instance=%u round=%u result=%lld roots=%u ticks=%u pure=%u yields=%u frames=%u/%u vm=%u native=%u\n",
                worker_backends[backend], i, round, (long long)results[index].payload, record->roots, record->ticks,
                record->pure, record->yields, record->entered, record->left, record->vm, record->native);
            CHECK(record->tasks[round].type); tasks[index] = record->tasks[round]; record->tasks[round] = (XrXirValue){0};
        }
    }
    CHECK(xr_xir_instance_free(library_pure_code->records[0].instance) == XR_XIR_CALL_READY && !worker_code_releases);
    library_pure_code->records[0].instance = NULL;
    CHECK(xr_xir_instance_free(library_pure_code->records[1].instance) == XR_XIR_CALL_READY && worker_code_releases == 1 && !library_pure_code && !worker_control_code);
    library_pure_late(tasks);
    for (unsigned i = 0; i < 4; ++i) { CHECK(results[i].type == XR_XIR_I64 && results[i].payload == (i < 2 ? 172 : 374)); xr_xir_value_drop(&results[i]); }
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    printf("Catalog pure worker backend=%s twoInstances172/374 privateStateIsolated pureGOwithStatefulSibling authenticFrames childYield4 lateTask7 codeLease1 callerCompilerDead physical0/0\n", worker_backends[backend]);
    return 0;
}
