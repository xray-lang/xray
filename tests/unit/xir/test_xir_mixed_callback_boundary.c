/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_callback_boundary.c - Real task callbacks reject nested execution on their active driver
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_packet_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Callback boundary %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
XR_DATA const XrXirProgramSpec p20_callback_boundary_fixture_program;
XR_DATA const uint32_t p20_callback_boundary_entries[2];

typedef struct CallbackFixture {
    XrCompileResources *resources;
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    uint32_t count;
    bool root[64];
    unsigned backend, releases;
} CallbackFixture;
typedef struct CallbackWitness { unsigned root, worker, yields; bool captured_call; } CallbackWitness;

static bool callback_native(unsigned backend, bool root) {
    return backend == 3 || (backend < 2 && root == (backend == 1));
}
static void callback_code_drop(void *owner) {
    CallbackFixture *fixture = owner;
    CHECK(!fixture->releases++ && fixture->vm && fixture->entries);
    xr_xir_compile_program_drop(fixture->vm); fixture->vm = NULL;
    xr_compile_resources_free(fixture->entries); fixture->entries = NULL;
}
static XrXirProgram *callback_program(CallbackFixture *fixture, unsigned backend) {
    CHECK(!effects_compile_live && !runtime_live);
    XrCompileResourceLimits caps = {67108864,8388608,128000000};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    fixture->resources = context.resources; fixture->backend = backend;
    const XrXirProgramSpec *native = &p20_callback_boundary_fixture_program;
    void *packet = NULL;
    CHECK(xr_compile_resources_alloc(context.resources, native->packet.length, &packet) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context.resources, native->packet.length) == XR_COMPILE_RESOURCE_OK);
    memcpy(packet, native->packet.bytes, native->packet.length);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet, native->packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet, 0xA5, native->packet.length); xr_compile_resources_free(packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed, &native->target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    CHECK(module->declarations->slot_count == 0 && module->declarations->module_count > 1);
    fixture->count = module->function_count;
    CHECK(xr_compile_resources_calloc(context.resources, fixture->count, sizeof(*fixture->entries),
        (void **)&fixture->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramPacket accepted = xir_program_packet_borrow(lowered);
    CHECK(accepted.length == native->packet.length && !memcmp(accepted.bytes, native->packet.bytes, accepted.length));
    CHECK(xr_xir_compile_vm_program_take(&lowered, &fixture->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = callback_native(backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = use_native ? native->entries[f] : fixture->vm->entries[f];
        if (use_native) ++c; else ++vm;
    }
    CHECK(backend < 2 ? vm && c : backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    CHECK(fixture->root[p20_callback_boundary_entries[0]] && !fixture->root[p20_callback_boundary_entries[1]]);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.packet = accepted; spec.code = (XrXirCodeLease){fixture,callback_code_drop};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    xr_compile_resources_release(context.resources); return program;
}

typedef struct CallbackTrace {
    CallbackFixture *fixture;
    XrXirInstance *instance;
    XrXirValue task;
    unsigned outputs, refusals, pending, root, worker;
    bool inside, cancel;
} CallbackTrace;

static void callback_busy(CallbackTrace *trace, XrXirCallStatus status) {
    if (status != XR_XIR_CALL_BUSY) fprintf(stderr,"Callback refusal output=%u probe=%u actualStatus=%u\n",trace->outputs,trace->refusals,status);
    CHECK(status == XR_XIR_CALL_BUSY); ++trace->refusals;
}
static void callback_frame(CallbackTrace *trace, XrXirCall *call, bool root) {
    CHECK(call && call->config.instance == trace->instance && call->top);
    CHECK(call->budget == &trace->instance->budget && call->config.admission.domain == trace->instance->domain);
    const CallFrame *frame = call->top; CallbackFixture *fixture = trace->fixture;
    uintptr_t base = (uintptr_t)call->config.entries, address = (uintptr_t)frame->entry;
    CHECK(address >= base && (address-base) % sizeof(*frame->entry) == 0);
    uint64_t index = (address-base) / sizeof(*frame->entry);
    CHECK(index < fixture->count && fixture->root[index] == root);
    CHECK(index == p20_callback_boundary_entries[root ? 0 : 1]);
    bool native = callback_native(fixture->backend, root);
    const XrXirCallEntry *entry = native ? &p20_callback_boundary_fixture_program.entries[index] : &fixture->vm->entries[index];
    CHECK(frame->entry->resume == entry->resume && frame->entry->environment == entry->environment);
    CHECK(frame->code_kind == (native ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
    if (root) trace->root |= native ? 2u : 1u; else trace->worker |= native ? 2u : 1u;
}
/* A synchronous provider may reject nested driving, but must never pump the executor. */
static void callback_reject(CallbackTrace *trace, XrXirCall *active) {
    XrXirInstance *instance = trace->instance; uint64_t epoch = instance->epoch;
    XrXirWaitRequest request, unchanged; memset(&request, 0x5A, sizeof(request)); unchanged = request;
    XrXirValue value = {XR_XIR_UNIT,0,0}; XrXirCallResult outcome = {0};
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
    size_t attempts = runtime_attempts, live = runtime_live, bytes = runtime_bytes;
    callback_busy(trace, xr_xir_instance_start(instance, p20_callback_boundary_entries[0], NULL, 0));
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(instance, 1);
    callback_busy(trace, event.outcome.status); CHECK(event.epoch == epoch && !event.outcome.wake);
    CHECK(!event.outcome.value.type && !event.outcome.value.payload);
    callback_busy(trace, xr_xir_instance_resume(instance, epoch, instance->call->result.wake));
    callback_busy(trace, xr_xir_instance_wait_request(instance, epoch, instance->call->result.wake, &request));
    CHECK(!memcmp(&request, &unchanged, sizeof(request)));
    callback_busy(trace, xr_xir_instance_take_result(instance, &value)); CHECK(value.type == XR_XIR_UNIT && !value.payload);
    callback_busy(trace, xr_xir_instance_copy_failure(instance, &outcome)); CHECK(xr_xir_call_result_empty(&outcome));
    callback_busy(trace, xr_xir_instance_cancel_current(instance));
    callback_busy(trace, xr_xir_instance_free(instance));
    callback_busy(trace, xr_xir_call_poll_bounded(active, 1).status);
    callback_busy(trace, xr_xir_call_resume(active, active->result.wake));
    callback_busy(trace, xr_xir_call_free(active));
    callback_busy(trace, xr_xir_call_take_result(active, &value)); CHECK(value.type == XR_XIR_UNIT && !value.payload);
    callback_busy(trace, xr_xir_call_take_outcome(active, &outcome)); CHECK(xr_xir_call_result_empty(&outcome));
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
    CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes && before.work == after.work);
    CHECK(attempts == runtime_attempts && live == runtime_live && bytes == runtime_bytes);
    CHECK(instance->epoch == epoch && instance->driving && !instance->stopping && active->driving && !active->cancel_requested);
}
static XrXirOutputStatus callback_output(void *context, const XrXirOutputGroup *group) {
    CallbackTrace *trace = context; XrXirInstance *instance = trace->instance;
    CHECK(trace && !trace->inside && trace->outputs < 3); trace->inside = true;
    const int64_t expected[] = {17,25,42};
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved && group->values[0].payload == expected[trace->outputs]);
    CHECK(instance->driving && instance->executor->driving && instance->executor->current);
    XrXirCall *active = instance->executor->current->call; CHECK(active && active->driving);
    callback_frame(trace, active, trace->outputs == 2);
    if (trace->outputs < 2) {
        CHECK(active != instance->call && instance->call->waiting_task.type && instance->call->wait.kind == XR_XIR_WAIT_TASK);
        callback_frame(trace, instance->call, true);
        if (!trace->task.type) CHECK(xr_xir_value_copy(&instance->call->waiting_task, &trace->task) == XR_XIR_VALUE_OK);
        CHECK(trace->task.payload == instance->call->waiting_task.payload);
        const XirTask *task = (const XirTask *)(uintptr_t)trace->task.payload;
        CHECK(task->executor == instance->executor && task->call == active && task->activation.call == active);
        XrXirCallResult pending = {0};
        CHECK(xr_xir_task_copy_outcome(&trace->task, &pending) == XR_XIR_CALL_BAD_STATE && xr_xir_call_result_empty(&pending));
        ++trace->pending;
    } else CHECK(active == instance->call && !instance->call->waiting_task.type);
    callback_reject(trace, active);
    CHECK(trace->outputs < 2 ? trace->pending == trace->outputs + 1 : trace->pending == 2);
    if (trace->cancel) {
        CHECK(!trace->outputs && xr_xir_call_request_cancel(active) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(active->cancel_requested);
    }
    ++trace->outputs; trace->inside = false; return XR_XIR_OUTPUT_OK;
}
static void callback_graph(unsigned backend, unsigned order, bool cancel) {
    CallbackFixture fixture = {0}; XrXirProgram *program = callback_program(&fixture, backend);
    CallbackTrace traces[2] = {{0}}; XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        traces[i].fixture = &fixture; traces[i].cancel = cancel;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,callback_output,&traces[i]};
        CHECK(xr_xir_instance_new(program, &config, &traces[i].instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(traces[i].instance, p20_callback_boundary_entries[0], NULL, 0) == XR_XIR_CALL_READY);
    }
    CHECK(traces[0].instance->domain != traces[1].instance->domain);
    xr_xir_compile_program_drop(program); CHECK(!fixture.releases);
    bool done[2] = {0}; unsigned polls = 0;
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) if (!done[i]) {
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(traces[1-i].instance->domain);
        unsigned outputs = traces[1-i].outputs;
        XrXirInstanceResult event = xr_xir_instance_poll_bounded(traces[i].instance, 1); CHECK(++polls < 4096);
        CHECK(event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == (cancel ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(traces[1-i].instance->domain);
        CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes && before.work == after.work);
        CHECK(outputs == traces[1-i].outputs);
        if (event.outcome.status == XR_XIR_CALL_READY) continue;
        XrXirValue task = {0};
        CHECK(xr_xir_instance_take_result(traces[i].instance, &task) == (cancel ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_RETURNED));
        if (cancel) CHECK(!task.type && !task.payload);
        else CHECK(task.type == traces[i].task.type && task.payload == traces[i].task.payload);
        xr_xir_value_drop(&task);
        CHECK(traces[i].outputs == (cancel ? 1u : 3u) && traces[i].refusals == (cancel ? 13u : 39u) && traces[i].pending == (cancel ? 1u : 2u) && !traces[i].inside);
        CHECK(traces[i].root == (callback_native(backend,true) ? 2u : 1u) && traces[i].worker == (callback_native(backend,false) ? 2u : 1u));
        done[i] = true;
    }
    for (unsigned i = 0; i < 2; ++i) {
        unsigned at = i ? 1-order : order;
        CHECK(xr_xir_instance_free(traces[at].instance) == XR_XIR_CALL_READY); traces[at].instance = NULL;
    }
    CHECK(fixture.releases == 1 && !fixture.vm && !fixture.entries);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirCallResult outcome = {0};
        CHECK(xr_xir_task_copy_outcome(&traces[i].task, &outcome) == (cancel ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
        CHECK(outcome.value.type == (uint32_t)(cancel ? XR_XIR_UNIT : XR_XIR_I64) && outcome.value.payload == (cancel ? 0 : 42));
        xr_xir_call_result_drop(&outcome); xr_xir_value_drop(&traces[i].task);
    }
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    printf("P20 callback boundary backend=%u order=%u cancel=%u actualOutputCallbacks=%u busyRefusals=%u pendingReadRejects=%u lateTasks2 codeLease1 physical0/0\n",backend,order,cancel,traces[0].outputs+traces[1].outputs,traces[0].refusals+traces[1].refusals,traces[0].pending+traces[1].pending);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); const char *names[] = {"0","1","vm","native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1],names[backend])) ++backend; CHECK(backend < 4);
    for (unsigned order = 0; order < 2; ++order)
        for (unsigned cancel = 0; cancel < 2; ++cancel) callback_graph(backend,order,cancel != 0);
    return 0;
}
