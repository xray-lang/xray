/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_cell_cycles.c - Source closure cycles retain real roots until domain retirement
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Cell cycle %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec p20_cell_cycle_fixture_program;
XR_DATA const uint32_t p20_cell_cycle_entries[2];

typedef struct CellCycleFixture {
    XrCompileResources *resources;
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    uint32_t count;
    bool root[64];
    unsigned backend, releases;
} CellCycleFixture;
typedef struct CellCycleWitness { unsigned root, worker, yields; bool captured_call; } CellCycleWitness;

static bool cycle_native(unsigned backend, bool root) {
    return backend == 3 || (backend < 2 && root == (backend == 1));
}
static void cycle_code_drop(void *owner) {
    CellCycleFixture *fixture = owner;
    CHECK(!fixture->releases++ && fixture->vm && fixture->entries);
    xr_xir_compile_program_drop(fixture->vm); fixture->vm = NULL;
    xr_compile_resources_free(fixture->entries); fixture->entries = NULL;
}
static XrXirProgram *cycle_program(CellCycleFixture *fixture, unsigned backend) {
    CHECK(!effects_compile_live && !runtime_live);
    XrCompileResourceLimits caps = {67108864,8388608,128000000};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    fixture->resources = context.resources; fixture->backend = backend;
    const XrXirProgramSpec *native = &p20_cell_cycle_fixture_program;
    void *packet = NULL;
    CHECK(xr_compile_resources_alloc(context.resources, native->proof.length, &packet) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context.resources, native->proof.length) == XR_COMPILE_RESOURCE_OK);
    memcpy(packet, native->proof.bytes, native->proof.length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet, native->proof.length, &checked, NULL) == XR_XIR_OK);
    memset(packet, 0xA5, native->proof.length); xr_compile_resources_free(packet);
    CHECK(xr_xir_compile_lower(checked, &native->target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    CHECK(module->declarations->slot_count == 0 && module->declarations->module_count > 1);
    fixture->count = module->function_count;
    CHECK(xr_compile_resources_calloc(context.resources, fixture->count, sizeof(*fixture->entries),
        (void **)&fixture->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    CHECK(xr_xir_compile_vm_program_take(&lowered, &fixture->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = cycle_native(backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = use_native ? native->entries[f] : fixture->vm->entries[f];
        if (use_native) ++c; else ++vm;
    }
    CHECK(backend < 2 ? vm && c : backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    CHECK(fixture->root[p20_cell_cycle_entries[0]] && !fixture->root[p20_cell_cycle_entries[1]]);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){fixture,cycle_code_drop};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    xr_compile_resources_release(context.resources); return program;
}
/* Inspect only the stopped real frame; execution providers and captures are never replaced. */
static void cycle_observe(const CellCycleFixture *fixture, XrXirInstance *instance,
    CellCycleWitness *witness, uint64_t capture, uint32_t invoked) {
    XrXirCall *call = instance->call;
    if (!call) return;
    CHECK(!instance->driving && !call->driving && call->config.instance == instance);
    CHECK(call->budget == &instance->budget && call->config.admission.domain == instance->domain);
    unsigned frames = 0;
    for (const CallFrame *frame = call->top; frame; frame = frame->parent) {
        CHECK(++frames < 64);
        uintptr_t base = (uintptr_t)call->config.entries, pointer = (uintptr_t)frame->entry;
        CHECK(pointer >= base && (pointer-base) % sizeof(*frame->entry) == 0);
        uint64_t index = (pointer-base) / sizeof(*frame->entry);
        CHECK(index < call->config.entry_count);
        if (index >= fixture->count || !frame->entered) continue;
        uint32_t f = (uint32_t)index; bool native = cycle_native(fixture->backend, fixture->root[f]);
        const XrXirCallEntry *expected = native ? &p20_cell_cycle_fixture_program.entries[f] : &fixture->vm->entries[f];
        CHECK(frame->entry->resume == expected->resume && frame->entry->environment == expected->environment);
        CHECK(frame->code_kind == (native ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM));
        unsigned bit = native ? 2u : 1u;
        if (f == p20_cell_cycle_entries[0]) witness->root |= bit;
        if (!fixture->root[f]) witness->worker |= bit;
        if (capture && f == invoked) {
            CHECK(frame->arguments && frame->arguments[0].payload == (int64_t)capture);
            CHECK(xr_xir_cell_in_domain(&frame->arguments[0], instance->domain)); witness->captured_call = true;
        }
    }
}
static XrXirInstanceResult cycle_tick(const CellCycleFixture *fixture, XrXirInstance **instances,
    unsigned index, CellCycleWitness *witness, uint64_t capture, uint32_t invoked) {
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instances[1-index]->domain);
    XrXirInstanceResult event = xr_xir_instance_poll_bounded(instances[index], 1);
    if (event.outcome.status != XR_XIR_CALL_READY && event.outcome.status != XR_XIR_CALL_RETURNED &&
        event.outcome.status != XR_XIR_CALL_SUSPENDED)
        fprintf(stderr,"Cell cycle runtime backend=%u index=%u status=%u\n",fixture->backend,index,event.outcome.status);
    CHECK(event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == XR_XIR_CALL_RETURNED ||
        event.outcome.status == XR_XIR_CALL_SUSPENDED);
    cycle_observe(fixture, instances[index], witness, capture, invoked);
    if (event.outcome.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(event.epoch && event.outcome.wake && !witness->yields++);
        XrXirWaitRequest request = {0};
        CHECK(xr_xir_instance_wait_request(instances[index], event.epoch, event.outcome.wake, &request) == XR_XIR_CALL_READY);
        CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
            !request.subject && !request.generation && !request.ticket);
        CHECK(xr_xir_instance_resume(instances[index], event.epoch+1, event.outcome.wake) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_resume(instances[index], event.epoch, event.outcome.wake+1) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_resume(instances[index], event.epoch, event.outcome.wake) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_resume(instances[index], event.epoch, event.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    }
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instances[1-index]->domain);
    CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes && before.work == after.work);
    return event;
}
static bool cycle_physical(uintptr_t address) {
    for (size_t i = 0; i < runtime_owned_capacity; ++i) if (runtime_owned[i].pointer) {
        uintptr_t base = (uintptr_t)runtime_owned[i].pointer;
        if (address >= base && address-base < runtime_owned[i].bytes) return true;
    }
    return false;
}
static void cycle_edges(const CellCycleFixture *fixture, const XrXirValue *function,
    XrXirDomain *domain, unsigned mode, uintptr_t addresses[3]) {
    CHECK(xr_xir_value_valid(function) && xr_xir_value_arena(function)->context.resources == fixture->resources);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(function);
    CHECK(binding && binding->capture_count == 1 && binding->captures && binding->entry < fixture->count);
    CHECK(binding->release == function_gate_drop);
    const FunctionGate *gate = binding->owner;
    CHECK(gate && gate->domain == domain && gate->program);
    if (domain->closed) CHECK(!gate->instance);
    CHECK(!fixture->root[binding->entry] && object_pointer(function)->kind == XIR_OBJECT_FUNCTION && object_pointer(function)->domain == domain);
    const XrXirValue *capture = &binding->captures[0];
    CHECK(xr_xir_cell_in_domain(capture, domain));
    const XirCell *cell = (const XirCell *)object_pointer(capture);
    CHECK(cell->initialized && !cell->module_storage && !cell->module_owner && !cell->projection_count);
    XrXirValue contents = {0}; CHECK(xr_xir_cell_read(capture, &contents) == XR_XIR_VALUE_OK);
    CHECK(contents.type == function->type && xr_xir_value_valid(&contents));
    if (!mode) {
        const XrXirFunctionBinding *inside = xr_xir_function_binding(&contents);
        CHECK(inside && inside->entry == binding->entry && inside->capture_count == 1 && inside->captures);
        CHECK(inside->captures[0].type == capture->type && inside->captures[0].payload == capture->payload);
        CHECK(xr_xir_cell_in_domain(&inside->captures[0], domain) && cell->value.payload == contents.payload);
    } else {
        CHECK(contents.payload != function->payload);
        const XrXirFunctionBinding *base = xr_xir_function_binding(&contents);
        CHECK(base && !base->capture_count && base->entry < fixture->count && !fixture->root[base->entry]);
    }
    uintptr_t found[] = {(uintptr_t)function->payload,(uintptr_t)capture->payload,(uintptr_t)contents.payload};
    xr_xir_value_drop(&contents);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(cycle_physical(found[i])); if (addresses[i]) CHECK(addresses[i] == found[i]); else addresses[i] = found[i];
    }
}
static void cycle_invoke(const CellCycleFixture *fixture, XrXirInstance **instances, unsigned index,
    CellCycleWitness *witness, const XrXirValue *function, int64_t expected) {
    const XrXirFunctionBinding *binding = xr_xir_function_binding(function);
    CHECK(binding && binding->capture_count <= 1);
    uint64_t capture = binding->capture_count ? (uint64_t)binding->captures[0].payload : 0;
    XrXirValue command = {XR_XIR_I64,0,1}; uint32_t entry = binding->entry;
    CHECK(xr_xir_instance_start_function(instances[index], function, &command, 1) == XR_XIR_CALL_READY);
    XrXirInstanceResult event = {0}; unsigned calls = 0;
    do { event = cycle_tick(fixture, instances, index, witness, capture, entry); CHECK(++calls < 4096); }
    while (event.outcome.status == XR_XIR_CALL_READY || event.outcome.status == XR_XIR_CALL_SUSPENDED);
    XrXirValue result = {0}; CHECK(xr_xir_instance_take_result(instances[index], &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_I64 && result.payload == expected); xr_xir_value_drop(&result);
}
static void cycle_graph(unsigned backend, unsigned order) {
    CellCycleFixture fixture = {0}; XrXirProgram *program = cycle_program(&fixture, backend);
    XrXirInstance *instances[2] = {0}; XrXirDomain *domains[2] = {0}; CellCycleWitness witness[2] = {{0}};
    XrXirValue functions[2] = {{0}}, aliases[2] = {{0}}; uintptr_t addresses[2][3] = {{0}}, instance_addresses[2] = {0};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        domains[i] = instances[i]->domain;
        instance_addresses[i] = (uintptr_t)instances[i]; CHECK(cycle_physical(instance_addresses[i]));
    }
    CHECK(domains[0] != domains[1]); xr_xir_compile_program_drop(program); CHECK(!fixture.releases);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirValue mode = {XR_XIR_I64,0,i};
        CHECK(xr_xir_instance_start(instances[i], p20_cell_cycle_entries[0], &mode, 1) == XR_XIR_CALL_READY);
    }
    bool done[2] = {0}; unsigned polls = 0;
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) if (!done[i]) {
        XrXirInstanceResult event = cycle_tick(&fixture, instances, i, &witness[i], 0, UINT32_MAX); CHECK(++polls < 4096);
        if (event.outcome.status != XR_XIR_CALL_RETURNED) continue;
        CHECK(xr_xir_instance_take_result(instances[i], &functions[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_value_copy(&functions[i], &aliases[i]) == XR_XIR_VALUE_OK);
        cycle_edges(&fixture, &functions[i], domains[i], i, addresses[i]); done[i] = true;
    }
    CHECK(addresses[0][0] != addresses[1][0] && addresses[0][1] != addresses[1][1]);
    for (unsigned i = 0; i < 2; ++i) {
        cycle_invoke(&fixture, instances, i, &witness[i], &functions[i], 42);
        CHECK(witness[i].captured_call && witness[i].yields == 1 && witness[i].root == (cycle_native(backend,true) ? 2u : 1u) &&
            witness[i].worker == (cycle_native(backend,false) ? 2u : 1u));
    }
    const XrXirFunctionBinding *broken = xr_xir_function_binding(&functions[1]); XrXirValue base = {0};
    CHECK(xr_xir_cell_read(&broken->captures[0], &base) == XR_XIR_VALUE_OK);
    cycle_invoke(&fixture, instances, 1, &witness[1], &base, 201); xr_xir_value_drop(&base);
    for (unsigned i = 0; i < 2; ++i) {
        unsigned at = i ? 1-order : order;
        CHECK(xr_xir_instance_free(instances[at]) == XR_XIR_CALL_READY); instances[at] = NULL;
        CHECK(!fixture.releases && !cycle_physical(instance_addresses[at]));
    }
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(domains[i]->closed); cycle_edges(&fixture, &functions[i], domains[i], i, addresses[i]);
        XrXirDomainBudgetStats cost = xr_xir_domain_budget_stats(domains[i]);
        CHECK(!cost.call_live && cost.call_allocations == cost.call_frees && cost.metadata_live == sizeof(FunctionGate));
    }
    size_t attempts = runtime_attempts;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned at = i ? 1-order : order;
        xr_xir_value_drop(&functions[at]); CHECK(cycle_physical(addresses[at][0]) && cycle_physical(addresses[at][1]));
        cycle_edges(&fixture, &aliases[at], domains[at], at, addresses[at]); CHECK(!fixture.releases);
        xr_xir_value_drop(&aliases[at]);
        CHECK(!cycle_physical(addresses[at][0]) && !cycle_physical(addresses[at][1]) && !cycle_physical(addresses[at][2]));
        if (!i) CHECK(!fixture.releases);
    }
    CHECK(attempts == runtime_attempts && fixture.releases == 1 && !fixture.vm && !fixture.entries);
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    printf("P20 cell cycles backend=%u order=%u actualClosureCellBackedge1 sourceBreak1 externalRoots4 actualCapturedCalls2 base201 codeLease1 noAllocateDrop physical0/0\n",backend,order);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); const char *names[] = {"0","1","vm","native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1],names[backend])) ++backend; CHECK(backend < 4);
    for (unsigned order = 0; order < 2; ++order) cycle_graph(backend,order);
    return 0;
}
