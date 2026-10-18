/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_lifecycle_native_backend.h - Observed native and bidirectional mixed frames
 */
#ifndef XIR_CELL_LIFECYCLE_NATIVE_BACKEND_H
#define XIR_CELL_LIFECYCLE_NATIVE_BACKEND_H
#if XR_CELL_LIFE_BACKEND < 1 || XR_CELL_LIFE_BACKEND > 3
#error "A lifecycle consumer requires a native or bidirectional mixed backend"
#endif
#include "cell_lifecycle_generated.h"

typedef struct CellLifeMixedBinding {
    XrXirVmBinding vm;
    XrXirCallEntry actual;
    bool native;
} CellLifeMixedBinding;
typedef struct CellLifeNativeOwner {
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    CellLifeMixedBinding *mixed;
    uint32_t count;
    uint64_t steps[2], cross_steps;
} CellLifeNativeOwner;
static CellLifeNativeOwner cell_life_native_owner;

static const CellLifeMixedBinding *cell_life_binding(const void *environment) {
    uintptr_t first = (uintptr_t)cell_life_native_owner.mixed, at = (uintptr_t)environment;
    size_t bytes = (size_t)cell_life_native_owner.count * sizeof(*cell_life_native_owner.mixed);
    CHECK(first && at >= first && at - first < bytes && (at - first) % sizeof(*cell_life_native_owner.mixed) == 0);
    return environment;
}
static XrXirAction cell_life_backend_resume(XrXirCallView *view) {
    const CellLifeMixedBinding *binding = cell_life_binding(view->environment);
    CHECK(view->activation && view->activation->top);
    ++cell_life_native_owner.steps[binding->native];
    const CallFrame *parent = view->activation->top->parent;
    if (parent) {
        uintptr_t first = (uintptr_t)cell_life_native_owner.mixed, at = (uintptr_t)parent->entry->environment;
        size_t bytes = (size_t)cell_life_native_owner.count * sizeof(*cell_life_native_owner.mixed);
        if (at >= first && at - first < bytes && (at - first) % sizeof(*cell_life_native_owner.mixed) == 0)
            cell_life_native_owner.cross_steps += binding->native != cell_life_binding(parent->entry->environment)->native;
        else CHECK(parent->entry->resume == initialization_resume);
    }
    XrXirAction action = binding->actual.resume(view);
    CHECK(view->environment == binding); return action;
}
static void cell_life_backend_release(XrXirCallView *view, XrXirCallStatus reason) {
    const CellLifeMixedBinding *binding = cell_life_binding(view->environment);
    binding->actual.release(view, reason); CHECK(view->environment == binding);
}
static XrXirProgram *cell_life_program(const XrXirCompileContext *context, const char *fixture, uint32_t *entries) {
    CHECK(!cell_life_native_owner.entries && !cell_life_native_owner.lowered);
    unsigned index = 0;
    while (index < 4 && strcmp(fixture, cell_life_fixtures[index])) ++index;
    CHECK(index < 4);
    const CellLifeGeneratedFixture *generated = &cell_life_generated_fixtures[index];
    CHECK(generated->program && generated->count == generated->program->entry_count);
    memcpy(entries, generated->entries, CELL_LIFE_ENTRIES * sizeof(*entries));
    XrXirProgramSpec spec = *generated->program;
#if XR_CELL_LIFE_BACKEND != 1
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(context, spec.proof.bytes, spec.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &spec.target, &cell_life_native_owner.lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(cell_life_native_owner.lowered);
    CHECK(module && module->function_count == generated->count);
    for (unsigned e = 0; e < CELL_LIFE_ENTRIES; ++e)
        CHECK(cell_source_entry(module, cell_life_entry_names[e], true) == entries[e]);
    CHECK(xr_xir_compile_vm_bind_table(cell_life_native_owner.lowered, &cell_life_native_owner.bindings,
        &cell_life_native_owner.entries) == XR_XIR_OK);
    spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = xr_xir_compile_program_proof(cell_life_native_owner.lowered);
#else
    (void)cell_life_entry_names;
    void *entry_memory = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, generated->count, sizeof(XrXirCallEntry),
        &entry_memory) == XR_COMPILE_RESOURCE_OK);
    cell_life_native_owner.entries = entry_memory;
#endif
    void *memory = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, generated->count, sizeof(CellLifeMixedBinding),
        &memory) == XR_COMPILE_RESOURCE_OK);
    cell_life_native_owner.mixed = memory; cell_life_native_owner.count = generated->count;
    for (uint32_t f = 0; f < generated->count; ++f) {
        CellLifeMixedBinding *binding = &cell_life_native_owner.mixed[f];
#if XR_CELL_LIFE_BACKEND == 1
        binding->native = true;
#else
        binding->native = XR_CELL_LIFE_BACKEND == 2 ? generated->leaves[f] != 0 : generated->leaves[f] == 0;
#endif
        if (binding->native) {
            binding->actual = generated->program->entries[f]; CHECK(!binding->actual.environment);
        } else {
            CHECK(cell_life_native_owner.bindings[f].artifact == cell_life_native_owner.lowered &&
                cell_life_native_owner.bindings[f].function == f);
            binding->vm = cell_life_native_owner.bindings[f]; binding->actual = cell_life_native_owner.entries[f];
            CHECK(binding->actual.environment == &cell_life_native_owner.bindings[f]);
            binding->actual.environment = &binding->vm;
        }
        cell_life_native_owner.entries[f] = binding->actual;
        cell_life_native_owner.entries[f].environment = binding;
        cell_life_native_owner.entries[f].resume = cell_life_backend_resume;
        cell_life_native_owner.entries[f].release = cell_life_backend_release;
    }
    spec.entries = cell_life_native_owner.entries;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK && program);
    return program;
}
static void cell_life_program_release(void) {
    CHECK(cell_life_native_owner.steps[1]);
#if XR_CELL_LIFE_BACKEND != 1
    CHECK(cell_life_native_owner.steps[0] && cell_life_native_owner.cross_steps);
#else
    CHECK(!cell_life_native_owner.steps[0] && !cell_life_native_owner.cross_steps);
#endif
    printf("CELL_LIFE_BACKEND backend=%u VMsteps=%llu nativeSteps=%llu crossFrameSteps=%llu\n",
        XR_CELL_LIFE_BACKEND, (unsigned long long)cell_life_native_owner.steps[0],
        (unsigned long long)cell_life_native_owner.steps[1], (unsigned long long)cell_life_native_owner.cross_steps);
    xr_compile_resources_free(cell_life_native_owner.entries);
    xr_compile_resources_free(cell_life_native_owner.bindings);
    xr_compile_resources_free(cell_life_native_owner.mixed);
    xr_xir_compile_artifact_free(cell_life_native_owner.lowered);
    cell_life_native_owner = (CellLifeNativeOwner){0};
}
#endif // XIR_CELL_LIFECYCLE_NATIVE_BACKEND_H
