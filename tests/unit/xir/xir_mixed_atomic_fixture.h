/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_atomic_fixture.h - Authentic Atomic code routes and original owners
 */
#ifndef XIR_MIXED_ATOMIC_FIXTURE_H
#define XIR_MIXED_ATOMIC_FIXTURE_H
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"Atomic runtime %d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_execution_owner.h"
static void atomic_runtime_before_free(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) atomic_runtime_before_free(pointer)
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_atomic.h"
#include "xir_mixed_atomic_source.h"
#include "base/xsha256.h"
XR_DATA const XrXirProgramSpec p20_atomic_runtime_fixture_program;
XR_DATA const uint32_t p20_atomic_runtime_entries[ATOMIC_RUNTIME_ENTRIES];
XR_DATA const unsigned char p20_atomic_runtime_c_sha256[32];
XR_DATA const size_t p20_atomic_runtime_c_bytes;
static XrXirDomain *atomic_domains[2];
static XrXirDomainBudgetStats atomic_final_cost[2],atomic_previous_cost[2];
static unsigned atomic_domain_disposals[2];
static void atomic_runtime_monotone(unsigned owner,XrXirDomainBudgetStats cost) {
    CHECK(owner<2 && cost.bound && cost.requested_limit==67108864 && cost.requested_call_limit==67108864 && cost.work_limit==128000000);
    CHECK(cost.requested_bytes>=atomic_previous_cost[owner].requested_bytes && cost.requested_call_bytes>=atomic_previous_cost[owner].requested_call_bytes && cost.work>=atomic_previous_cost[owner].work);
    CHECK(cost.requested_bytes<=cost.requested_limit && cost.requested_call_bytes<=cost.requested_call_limit && cost.work<=cost.work_limit);
    atomic_previous_cost[owner]=cost;
}
static void atomic_runtime_cost(unsigned owner) {
    if(atomic_domains[owner])atomic_runtime_monotone(owner,xr_xir_domain_budget_stats(atomic_domains[owner]));
}
/* Observe the original domain at reference zero without retaining it. */
static void atomic_runtime_before_free(void *pointer) {
    for(unsigned i=0;pointer && i<2;++i)if(pointer==atomic_domains[i]) {
        XrXirDomain *domain=pointer;CHECK(!atomic_load_explicit(&domain->references,memory_order_relaxed) && !domain->objects);
        CHECK(domain->stats.live_bytes==sizeof(*domain) && domain->stats.allocations==domain->stats.frees+1);
        CHECK(!domain->budget.call_live && !domain->budget.metadata_live);
        CHECK(domain->budget.call_allocations==domain->budget.call_frees && domain->budget.metadata_allocations==domain->budget.metadata_frees);
        atomic_runtime_monotone(i,domain->budget);atomic_final_cost[i]=domain->budget;CHECK(!atomic_domain_disposals[i]++);atomic_domains[i]=NULL;
    }
}
typedef struct AtomicRuntimeFixture {
    XrCompileResources *resources;
    XrXirProgram *vm;
    XrXirCallEntry *entries;
    uint32_t count;
    bool root[64];
    bool c_body_matched,vm_prepared;
    unsigned backend,releases,stage;
    XrXirStatus status;
} AtomicRuntimeFixture;
static bool atomic_runtime_native(unsigned backend,bool root) {
    return backend==3 || (backend<2 && root==(backend==1));
}
static void atomic_runtime_code_drop(void *owner) {
    AtomicRuntimeFixture *f=owner;CHECK(!f->releases++ && f->vm && f->entries);
    xr_xir_compile_program_drop(f->vm);f->vm=NULL;xr_compile_resources_free(f->entries);f->entries=NULL;
}
static XrXirStatus atomic_runtime_resource_status(XrCompileResourceStatus status) {
    if(status==XR_COMPILE_RESOURCE_OK)return XR_XIR_OK;
    CHECK(status==XR_COMPILE_RESOURCE_BUDGET || status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
}
static bool atomic_runtime_images(AtomicSourcePrepare *s,AtomicRuntimeFixture *f,XrXirProgram **program) {
    const XrXirProgramSpec *native=&p20_atomic_runtime_fixture_program;
    const XrXirModule *m=xr_xir_compile_artifact_module(s->lowered);
    CHECK(m->function_count==native->entry_count && m->function_count<64);
    CHECK(!memcmp(s->entries,p20_atomic_runtime_entries,sizeof(s->entries)));
    XrXirProgramProof proof=xr_xir_compile_program_proof(s->lowered);
    CHECK(proof.length==native->proof.length && !memcmp(proof.bytes,native->proof.bytes,proof.length));f->count=m->function_count;
    XrCompileResourceStatus allocated=xr_compile_resources_calloc(s->context->resources,f->count,sizeof(*f->entries),(void **)&f->entries);
    if(!atomic_source_stage(s,9,atomic_runtime_resource_status(allocated))){CHECK(!f->entries);return false;}
    XrXirArtifact *original=s->lowered;
    if(!atomic_source_stage(s,10,xr_xir_compile_vm_program_take(&s->lowered,&f->vm))){CHECK(!f->vm && s->lowered==original);return false;}
    CHECK(!s->lowered && f->vm);f->vm_prepared=true;unsigned vm=0,c=0;
    for(uint32_t entry=0;entry<f->count;++entry) {
        f->root[entry]=m->declarations->functions[entry].module==m->declarations->root_module;
        bool use_native=atomic_runtime_native(f->backend,f->root[entry]);
        CHECK(native->entries[entry].resume!=f->vm->entries[entry].resume);
        f->entries[entry]=use_native?native->entries[entry]:f->vm->entries[entry];if(use_native)++c;else ++vm;
    }
    CHECK(f->backend<2?vm && c:f->backend==2?vm==f->count && !c:c==f->count && !vm);
    XrXirProgramSpec spec=*native;spec.entries=f->entries;spec.types=m->types;spec.declarations=m->declarations;
    spec.proof=proof;spec.code=(XrXirCodeLease){f,atomic_runtime_code_drop};
    if(!atomic_source_stage(s,11,xr_xir_compile_program_seal(s->context,&spec,program))){CHECK(!*program && !f->releases);return false;}
    CHECK(*program && !f->releases);return true;
}
static bool atomic_runtime_prepare(AtomicRuntimeFixture *f,unsigned backend,bool source_chain,XrXirProgram **program) {
    CHECK(backend<4 && !effects_compile_live && !runtime_live);f->backend=backend;
    AtomicSourcePrepare s={.context=effects_source_owner(67108864,128000000)};f->resources=s.context->resources;
    bool complete=false;
    if(source_chain) {
        if(!atomic_source_prepare(&s))goto done;
        uint8_t digest[32];xr_sha256((const uint8_t *)s.generated.text,s.generated.length,digest);
        CHECK(s.generated.length==p20_atomic_runtime_c_bytes && !memcmp(digest,p20_atomic_runtime_c_sha256,sizeof(digest)));
        f->c_body_matched=true;
    }else {
        const XrXirProgramSpec *native=&p20_atomic_runtime_fixture_program;void *packet=NULL;
        CHECK(xr_compile_resources_alloc(s.context->resources,native->proof.length,&packet)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_work(s.context->resources,native->proof.length)==XR_COMPILE_RESOURCE_OK);
        memcpy(packet,native->proof.bytes,native->proof.length);
        CHECK(xr_xir_compile_checked_read(s.context,packet,native->proof.length,&s.checked,NULL)==XR_XIR_OK);
        memset(packet,0xA5,native->proof.length);xr_compile_resources_free(packet);
        CHECK(xr_xir_compile_lower(s.checked,&native->target,&s.lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(s.checked);s.checked=NULL;atomic_source_entries(&s);
    }
    complete=atomic_runtime_images(&s,f,program);
done:
    f->status=s.status;f->stage=s.stage;atomic_source_cleanup(&s);
    if(!*program){xr_xir_compile_program_drop(f->vm);f->vm=NULL;xr_compile_resources_free(f->entries);f->entries=NULL;}
    return complete;
}

#endif
