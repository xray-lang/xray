/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_recursion.c - Logical recursion grows frames without growing the C stack
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_call_observer_internal.h"
#if defined(_MSC_VER)
#include <intrin.h>
static uintptr_t deep_stack_position(void) { return (uintptr_t)_AddressOfReturnAddress(); }
#elif defined(__GNUC__) || defined(__clang__)
static uintptr_t deep_stack_position(void) { return (uintptr_t)__builtin_frame_address(0); }
#else
#error A real native stack position probe is required for this test
#endif
XR_DATA const XrXirProgramSpec worker_deep_fixture_program;
XR_DATA const uint32_t worker_deep_entry, worker_deep_descend;
#define XR_WORKER_CONTROL_CUSTOM_CASE {"deep",&worker_deep_fixture_program,&worker_deep_entry}
#define XR_WORKER_CONTROL_RESOURCE_MAIN
#include "test_xir_mixed_worker_controls.c"
enum { DEEP_RECURSION = 2048, DEEP_STACK_BYTES = 65536, DEEP_LIMIT = 64 };
typedef struct DeepRecord {
    XrXirInstance *instance;
    XrXirCallObserver observer;
    XrXirValue task;
    uintptr_t stack_low, stack_high;
    uint32_t depth, mode, entered, left, live, peak_depth, walks, walk_leaves, yields, vm, native;
    uint64_t before_yield_stack;
} DeepRecord;
typedef struct DeepCode { WorkerControlCode base; DeepRecord record; } DeepCode;
typedef struct DeepMetrics { uint64_t stack, before_yield_stack, pool, work; uint32_t peak, walks; } DeepMetrics;
static DeepCode *deep_code;
static void deep_stack_sample(DeepRecord *record) {
    uintptr_t position = deep_stack_position(); CHECK(position);
    if (!record->stack_low || position < record->stack_low) record->stack_low = position;
    if (position > record->stack_high) record->stack_high = position;
    CHECK(record->stack_high-record->stack_low <= DEEP_STACK_BYTES);
}
static void deep_event(void *owner, const XrXirCallEvent *event) {
    DeepRecord *record = owner; WorkerControlCode *code = &deep_code->base;
    CHECK(event && event->instance == record->instance && event->entry <= code->functions);
    CHECK(event->binding.owner && event->binding.activation && event->binding.generation && event->binding.ticket && event->frame);
    deep_stack_sample(record);
    if (event->kind == XR_XIR_CALL_FRAME_PRESENT || event->kind == XR_XIR_CALL_FRAME_ENTER) {
        ++record->entered; ++record->live;
        if (event->depth > record->peak_depth) record->peak_depth = event->depth;
        if (event->entry == worker_deep_descend) { ++record->walks; CHECK(record->walks <= record->depth+1); }
    }
    if (event->kind != XR_XIR_CALL_FRAME_LEAVE) return;
    CHECK(record->live); --record->live; ++record->left;
    if (event->entry == worker_deep_descend) ++record->walk_leaves;
    if (event->entry == worker_deep_descend || event->entry == worker_deep_entry ||
        (event->entry < code->functions && code->workers[event->entry])) {
        XrXirCallStatus expected = record->mode == 5 ? XR_XIR_CALL_LIMIT :
            record->mode == 3 || record->mode == 4 || (record->mode == 2 && event->entry == worker_deep_entry) ?
            XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED;
        CHECK(event->status == expected);
    }
    if (event->entry >= code->functions) return;
    if (event->code == XR_XIR_CALL_CODE_UNKNOWN) { CHECK(record->mode == 5); return; }
    CHECK(event->code == (code->native[event->entry] ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
    if (event->code == XR_XIR_CALL_CODE_NATIVE) ++record->native; else ++record->vm;
}
static XrXirAction deep_resume(XrXirCallView *view) {
    DeepRecord *record = &deep_code->record;
    CHECK(view->instance == record->instance);
    CHECK(xr_xir_call_observer_bind(view, &record->observer) == XR_XIR_CALL_READY); deep_stack_sample(record);
    XrXirAction action = worker_control_resume(view);
    if (action.kind == XR_XIR_ACTION_AWAIT_TASK && !record->task.type)
        CHECK(xr_xir_value_copy(&action.value, &record->task) == XR_XIR_VALUE_OK);
    if (action.kind == XR_XIR_ACTION_SUSPEND) {
        CHECK(xr_xir_call_current_entry(view->activation) == worker_deep_descend && !record->yields++);
        CHECK(record->walks == record->depth+1 && !record->walk_leaves && record->peak_depth == record->depth+2);
        CHECK(record->task.type && ((XirTask *)(uintptr_t)record->task.payload)->accounting.depth == record->depth+2);
        record->before_yield_stack = record->stack_high-record->stack_low;
    }
    return action;
}
static void deep_code_free(void *pointer) {
    DeepCode *code = pointer; CHECK(code == deep_code); deep_code = NULL; worker_control_free(&code->base);
}
static XrXirProgram *deep_program(unsigned backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, worker_deep_fixture_program.proof.bytes,
        worker_deep_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &worker_deep_fixture_program.target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    DeepCode *owner = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    WorkerControlCode *code = &owner->base;
    CHECK(module->function_count == worker_deep_fixture_program.entry_count && module->function_count <= WORKER_FUNCTIONS);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count, sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    code->functions = module->function_count; code->root = worker_deep_entry; worker_control_targets(code, module);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    for (uint32_t f = 0; f < code->functions; ++f) {
        code->native[f] = backend == 3 || (backend < 2 && (f == code->root) == (backend == 1));
        CHECK(worker_deep_fixture_program.entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = code->native[f] ? worker_deep_fixture_program.entries[f] : code->vm->entries[f];
        code->delegates[f] = code->entries[f].resume; code->entries[f].resume = deep_resume;
    }
    XrXirProgramSpec spec = worker_deep_fixture_program;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){owner, deep_code_free};
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK);
    CHECK(!deep_code && !worker_control_code); deep_code = owner; worker_control_code = code; return program;
}
static XrXirCallStatus deep_poll(XrXirInstance *instance, bool first_yield) {
    unsigned polls = 0; XrXirCallStatus status;
    do { status = xr_xir_instance_poll_bounded(instance, 1).outcome.status; CHECK(++polls < 262144); }
    while (status == XR_XIR_CALL_READY && (!first_yield || !deep_code->record.yields));
    return status;
}
static DeepMetrics deep_operation(XrXirProgram *program, unsigned mode, XrXirValue *retained) {
    DeepRecord *record = &deep_code->record; memset(record, 0, sizeof(*record));
    record->depth = mode ? DEEP_RECURSION : 16; record->mode = mode;
    record->observer = (XrXirCallObserver){record, deep_event};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (mode == 5) config.depth_limit = DEEP_LIMIT;
    CHECK(xr_xir_instance_new(program, &config, &record->instance) == XR_XIR_CALL_READY);
    XrXirInstance *instance = record->instance; XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
    XrXirValue argument = {XR_XIR_I64, 0, record->depth};
    CHECK(xr_xir_instance_start(instance, worker_deep_entry, &argument, 1) == XR_XIR_CALL_READY);
    XrXirCallStatus status = deep_poll(instance, mode != 5); XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
    if (mode != 5) {
        CHECK(status == XR_XIR_CALL_READY && record->yields == 1);
        if (mode == 2) CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        if (mode == 3) CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        if (mode == 4) { CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); instance = NULL; }
        else status = deep_poll(instance, false);
    }
    XrXirCallStatus expected = mode < 2 ? XR_XIR_CALL_RETURNED : mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
    if (instance) {
        CHECK(status == expected);
        XrXirValue value = {0};
        if (mode < 2) {
            CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
            CHECK(value.type == XR_XIR_I64 && value.payload == (int64_t)record->depth+7); xr_xir_value_drop(&value);
        } else CHECK(xr_xir_instance_take_result(instance, &value) == (mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE) && !value.type && !value.payload);
        CHECK(xr_xir_instance_free(instance) == (mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
    }
    CHECK(!record->live && record->entered == record->left && record->walks == record->walk_leaves);
    CHECK(record->walks == (mode == 5 ? DEEP_LIMIT-1 : record->depth+1));
    CHECK(record->peak_depth == (mode == 5 ? DEEP_LIMIT : record->depth+2) && record->yields == (mode == 5 ? 0u : 1u));
    CHECK(record->task.type && xr_xir_value_valid(&record->task)); XirTask *task = (XirTask *)(uintptr_t)record->task.payload;
    CHECK(!task->executor && !task->call && atomic_load_explicit(&task->state, memory_order_acquire) == XIR_TASK_TERMINAL);
    CHECK(task->accounting.peak_depth == record->peak_depth && task->accounting.peak_bytes && task->accounting.peak_bytes <= config.call_limit);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
    CHECK(after.work >= before.work && after.requested_bytes >= before.requested_bytes && after.requested_call_bytes >= before.requested_call_bytes);
    CHECK(!after.call_live && !after.metadata_live && after.call_allocations == after.call_frees && after.metadata_allocations == after.metadata_frees);
    DeepMetrics metrics = {record->stack_high-record->stack_low, record->before_yield_stack, task->accounting.peak_bytes,
        after.work, record->peak_depth, record->walks};
    *retained = record->task; record->task = (XrXirValue){0}; xr_xir_domain_drop(domain); CHECK(!worker_code_releases); return metrics;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); (void)worker_control_main; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend;
    CHECK(backend < 4); XrXirProgram *program = deep_program(backend); XrXirValue retained[6] = {{0}}; DeepMetrics metrics[6];
    for (unsigned mode = 0; mode < 6; ++mode) {
        metrics[mode] = deep_operation(program, mode, &retained[mode]); DeepRecord *record = &deep_code->record;
        if (backend == 2) CHECK(record->vm && !record->native);
        else if (backend == 3) CHECK(record->native && !record->vm);
        else CHECK(record->native && record->vm);
        printf("Deep recursion backend=%s mode=%u walks=%u peak=%u beforeYieldStack=%llu cStack=%llu pool=%llu work=%llu, authenticFrames closed depthLimit64 noHostYield\n",
            worker_backends[backend], mode, metrics[mode].walks, metrics[mode].peak,
            (unsigned long long)metrics[mode].before_yield_stack, (unsigned long long)metrics[mode].stack,
            (unsigned long long)metrics[mode].pool, (unsigned long long)metrics[mode].work);
    }
    CHECK(metrics[1].pool > metrics[0].pool && metrics[1].peak > metrics[0].peak && metrics[1].walks > metrics[0].walks);
    CHECK(metrics[1].before_yield_stack <= metrics[0].before_yield_stack+4096 && metrics[1].stack <= metrics[0].stack+4096);
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !worker_control_code && !deep_code);
    for (unsigned mode = 0; mode < 6; ++mode) {
        XrXirCallResult out = {0}; XrXirCallStatus expected = mode < 3 ? XR_XIR_CALL_RETURNED : mode == 5 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_CANCELLED;
        CHECK(xr_xir_task_copy_outcome(&retained[mode], &out) == expected && xr_xir_panic_empty(&out.panic));
        if (mode < 3) CHECK(out.value.type == XR_XIR_I64 && out.value.payload == (int64_t)(mode ? DEEP_RECURSION : 16u)+7);
        else CHECK(!out.value.type && !out.value.payload);
        xr_xir_call_result_drop(&out); xr_xir_value_drop(&retained[mode]);
    }
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free(); return 0;
}
