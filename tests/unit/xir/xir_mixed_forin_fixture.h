/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_forin_fixture.h - Authentic code leases and final original-Domain observations
 */
#ifndef XIR_MIXED_FORIN_FIXTURE_H
#define XIR_MIXED_FORIN_FIXTURE_H
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) {fprintf(stderr,"For-in lifetime %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
static void forin_before_free(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) forin_before_free(pointer)
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
XR_DATA const XrXirProgramSpec p20_iterator_lifetime_fixture_program;
XR_DATA const uint32_t p20_iterator_lifetime_entries[4];
static XrXirDomain *forin_borrowed_domain[2];
static XrXirDomainBudgetStats forin_final_cost[2],forin_previous_cost[2];
static unsigned forin_domain_disposals[2];
static void forin_monotone(unsigned owner,XrXirDomainBudgetStats cost) {
    CHECK(owner<2 && cost.requested_bytes>=forin_previous_cost[owner].requested_bytes);
    CHECK(cost.requested_call_bytes>=forin_previous_cost[owner].requested_call_bytes && cost.work>=forin_previous_cost[owner].work);
    forin_previous_cost[owner]=cost;
}
/* This hook observes the actual owner at reference zero without retaining it. */
static void forin_before_free(void *pointer) {
    if(!pointer)return;
    for(unsigned i=0;i<2;++i)if(pointer==forin_borrowed_domain[i]) {
        XrXirDomain *domain=pointer;
        CHECK(!atomic_load_explicit(&domain->references,memory_order_relaxed) && !domain->objects);
        CHECK(domain->stats.live_bytes==sizeof(*domain) && domain->stats.allocations==domain->stats.frees+1);
        CHECK(!domain->budget.call_live && !domain->budget.metadata_live);
        CHECK(domain->budget.call_allocations==domain->budget.call_frees && domain->budget.metadata_allocations==domain->budget.metadata_frees);
        forin_monotone(i,domain->budget);forin_final_cost[i]=domain->budget;CHECK(!forin_domain_disposals[i]++);forin_borrowed_domain[i]=NULL;
    }
}
static void forin_costs(void) {
    for(unsigned i=0;i<2;++i)if(forin_borrowed_domain[i])forin_monotone(i,xr_xir_domain_budget_stats(forin_borrowed_domain[i]));
}
typedef struct IteratorFixture {
    XrCompileResources *resources;
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    uint32_t count;
    bool root[64];
    unsigned backend, releases;
} IteratorFixture;

static bool iterator_native(unsigned backend, bool root) {
    return backend == 3 || (backend < 2 && root == (backend == 1));
}
static void iterator_code_drop(void *owner) {
    IteratorFixture *fixture = owner;
    CHECK(!fixture->releases++ && fixture->vm && fixture->entries);
    xr_xir_compile_program_drop(fixture->vm); fixture->vm = NULL;
    xr_compile_resources_free(fixture->entries); fixture->entries = NULL;
}
static XrXirProgram *iterator_program(IteratorFixture *fixture, unsigned backend) {
    CHECK(!effects_compile_live && !runtime_live);
    XrCompileResourceLimits caps = {67108864,8388608,128000000};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    fixture->resources = context.resources; fixture->backend = backend;
    const XrXirProgramSpec *native = &p20_iterator_lifetime_fixture_program;
    void *packet = NULL;
    CHECK(xr_compile_resources_alloc(context.resources, native->proof.length, &packet) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context.resources, native->proof.length) == XR_COMPILE_RESOURCE_OK);
    memcpy(packet, native->proof.bytes, native->proof.length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet, native->proof.length, &checked, NULL) == XR_XIR_OK);
    memset(packet, 0xA5, native->proof.length); xr_compile_resources_free(packet);
    CHECK(xr_xir_compile_lower(checked, &native->target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    CHECK(module->declarations->slot_count == 3 && module->declarations->module_count > 1);
    fixture->count = module->function_count;
    CHECK(xr_compile_resources_calloc(context.resources, fixture->count, sizeof(*fixture->entries),
        (void **)&fixture->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    CHECK(xr_xir_compile_vm_program_take(&lowered, &fixture->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = iterator_native(backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = use_native ? native->entries[f] : fixture->vm->entries[f];
        if (use_native) ++c; else ++vm;
    }
    CHECK(backend < 2 ? vm && c : backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    for (unsigned i = 0; i < 4; ++i) CHECK(fixture->root[p20_iterator_lifetime_entries[i]]);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){fixture,iterator_code_drop};
    XrXirProgram *program = NULL;
    uint32_t generator=UINT32_MAX;
    for (uint32_t f=0;f<fixture->count;++f) if (fixture->entries[f].flags&XR_XIR_ENTRY_GENERATOR) {generator=f;break;}
    CHECK(generator!=UINT32_MAX && (fixture->entries[generator].flags&XR_XIR_ENTRY_EXIT));
    const uint32_t rejected[]={generator,p20_iterator_lifetime_entries[0]};
    for (unsigned i=0;i<2;++i) {
        uint32_t f=rejected[i],flags=fixture->entries[f].flags;fixture->entries[f].flags^=XR_XIR_ENTRY_GENERATOR;
        CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_BAD_STRUCTURE && !program && !fixture->releases);
        fixture->entries[f].flags=flags;
    }
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    xr_compile_resources_release(context.resources); return program;
}

static bool iterator_physical(uintptr_t address) {
    for (size_t i=0;i<runtime_owned_capacity;++i) if (runtime_owned[i].pointer) {
        uintptr_t base=(uintptr_t)runtime_owned[i].pointer;
        if (address>=base && address-base<runtime_owned[i].bytes) return true;
    }
    return false;
}
static void iterator_text(const XrXirValue *value,const char *expected) {
    const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&text,&length));
    CHECK(length==strlen(expected) && !memcmp(text,expected,length));
}

#endif // XIR_MIXED_FORIN_FIXTURE_H
