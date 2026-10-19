/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_vm_bind_table.c - Atomic finite-ledger VM binding tables
 *
 * KEY CONCEPT:
 *   One complete artifact verification precedes all existing per-function
 *   binders; every failure preserves outputs and releases temporary owners.
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
#include "xir_construction_fixture.h"

typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical physical[1024];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *table_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void table_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && physical[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) table_malloc(bytes)
#define xr_free(pointer) table_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static XrCompileResourceLimits caps(void) {
    return (XrCompileResourceLimits){UINT64_C(32)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(64000000)};
}
static XrCompileResourceStats stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static XrXirCompileContext owner_new(XrCompileResourceLimits limits) {
    CHECK(!live && !live_bytes && fail_at==SIZE_MAX);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();return c;
}
static void owner_free(XrXirCompileContext *c,uint64_t baseline) {
    CHECK(stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);
    *c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static XrXirArtifact *fixture(const XrXirCompileContext *c,bool lowered) {
    const XrXirInstruction unit[]={{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT}};
    const XrXirInstruction value[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    const XrXirInstruction identity[]={{.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    const XrXirBlock one={.first=0,.count=1},two={.first=0,.count=2};
    const XrXirType parameter=XR_XIR_I64;
    const XrXirFunction functions[]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&one,.block_count=1,.instructions=unit,.instruction_count=1},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=value,.instruction_count=2},
        {.name="cleanup",.name_length=7,.result=XR_XIR_UNIT,.blocks=&one,.block_count=1,.instructions=unit,.instruction_count=1},
        {.name="identity",.name_length=8,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
            .blocks=&one,.block_count=1,.instructions=identity,.instruction_count=1}};
    const XrXirSourceModule source={"table",5,NULL,0,0};
    XrXirFunctionIdentity identities[4]={{0}};identities[2].cleanup_owner=2;
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.root_module=0,.entry_function=1};
    const XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=4,
        .declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*result=NULL;
    CHECK(xir_fixture_check(c,&built,&checked,NULL)==XR_XIR_OK);
    if (!lowered) return checked;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&result,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);return result;
}
static void output_rejections(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    XrXirArtifact *a=fixture(&c,true),*checked=fixture(&c,false);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;uint64_t work=stats(&c).work;
    CHECK(xr_xir_compile_vm_bind_table(a,NULL,&entries)==XR_XIR_BAD_STRUCTURE && !entries);
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,NULL)==XR_XIR_BAD_STRUCTURE && !bindings);
    union { XrXirVmBinding *bindings; XrXirCallEntry *entries; } alias={0};
    CHECK(xr_xir_compile_vm_bind_table(a,&alias.bindings,&alias.entries)==XR_XIR_BAD_STRUCTURE && !alias.bindings);
    bindings=(XrXirVmBinding *)(uintptr_t)1;
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,&entries)==XR_XIR_BAD_STRUCTURE && bindings==(XrXirVmBinding *)(uintptr_t)1 && !entries);
    bindings=NULL;entries=(XrXirCallEntry *)(uintptr_t)2;
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,&entries)==XR_XIR_BAD_STRUCTURE && !bindings && entries==(XrXirCallEntry *)(uintptr_t)2);
    entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(checked,&bindings,&entries)==XR_XIR_BAD_STAGE && !bindings && !entries);
    CHECK(xr_xir_compile_vm_bind_table(NULL,&bindings,&entries)==XR_XIR_BAD_STAGE && !bindings && !entries);
    CHECK(stats(&c).work==work);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(a);owner_free(&c,baseline);
}
static void field_oracles(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    XrXirArtifact *a=fixture(&c,true);XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,&entries)==XR_XIR_OK && bindings && entries);
    for(uint32_t i=0;i<4;++i) {
        const XrXirFunctionLayout *layout=xr_xir_compile_artifact_layout(a,i);
        XrXirVmBinding one={0};XrXirCallEntry e={0};
        CHECK(xr_xir_compile_vm_bind(a,i,&one,&e)==XR_XIR_OK);
        CHECK(bindings[i].artifact==a && bindings[i].function==i && one.artifact==a && one.function==i);
        CHECK(entries[i].abi_version==XR_XIR_CALL_ABI_VERSION && entries[i].abi_version==e.abi_version);
        CHECK(entries[i].parameter_count==(i==3?1u:0u) && entries[i].parameter_count==e.parameter_count);
        CHECK(entries[i].parameters==e.parameters);
        if(i==3) CHECK(entries[i].parameters && entries[i].parameters[0]==XR_XIR_I64 && e.parameters[0]==XR_XIR_I64);
        CHECK(entries[i].result==(i==0 || i==2?XR_XIR_UNIT:XR_XIR_I64) && entries[i].result==e.result);
        CHECK(layout && entries[i].state_bytes>=layout->frame_bytes && entries[i].state_bytes==e.state_bytes);
        CHECK(entries[i].resume && entries[i].resume==e.resume && entries[i].release && entries[i].release==e.release);
        CHECK(entries[i].environment==&bindings[i] && e.environment==&one);
        CHECK(entries[i].flags==(i==1?XR_XIR_ENTRY_EXIT:0u) && entries[i].flags==e.flags);
        CHECK(entries[i].cleanup_owner==(i==2?2u:0u) && entries[i].cleanup_owner==e.cleanup_owner);
    }
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
    xr_xir_compile_artifact_free(a);owner_free(&c,baseline);
}
static void damaged_layout(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    XrXirArtifact *a=fixture(&c,true);XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    uint32_t bytes=a->layouts[1].frame_bytes;CHECK(bytes<UINT32_MAX);
    a->layouts[1].frame_bytes=bytes+1;
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,&entries)==XR_XIR_BAD_LAYOUT && !bindings && !entries);
    a->layouts[1].frame_bytes=bytes;
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,&entries)==XR_XIR_OK && bindings && entries);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
    xr_xir_compile_artifact_free(a);owner_free(&c,baseline);
}
static void all_allocation_faults(void) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
        XrXirArtifact *a=fixture(&c,true);XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
        size_t blocks=live,bytes=live_bytes;attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=xr_xir_compile_vm_bind_table(a,&bindings,&entries);
        size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass) { CHECK(status==XR_XIR_OK && bindings && entries);sites=actual;CHECK(sites>=2); }
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && actual==pass && !bindings && !entries);
        xr_compile_resources_free(entries);xr_compile_resources_free(bindings);CHECK(live==blocks && live_bytes==bytes);
        xr_xir_compile_artifact_free(a);owner_free(&c,baseline);
    }
    printf("VM binding table actual allocation failures=%zu; each physical=0/0\n",sites);
}
static uint64_t work_probe(uint64_t limit,bool exact,uint64_t expected_final) {
    XrCompileResourceLimits limits=caps();limits.work=limit;
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    XrXirArtifact *a=fixture(&c,true);XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    size_t blocks=live,bytes=live_bytes;
    CHECK(xr_xir_compile_vm_bind_table(a,&bindings,&entries)==(exact?XR_XIR_OK:XR_XIR_BUDGET));
    CHECK(exact?(bindings!=NULL && entries!=NULL):(!bindings && !entries));
    uint64_t measured=stats(&c).work;if(expected_final) CHECK(measured==expected_final);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);CHECK(live==blocks && live_bytes==bytes);
    xr_xir_compile_artifact_free(a);owner_free(&c,baseline);return measured;
}
static void work_limits(void) {
    uint64_t exact=work_probe(caps().work,true,0);
    CHECK(work_probe(exact,true,exact)==exact);
    uint64_t publication=sizeof(XrXirVmBinding *)+sizeof(XrXirCallEntry *);
    CHECK(exact>publication+1);(void)work_probe(exact-1,false,exact-publication);
    printf("VM binding table whole-graph exact work=%llu, minus1 rejects publication; physical=0/0\n",(unsigned long long)exact);
}
int main(void) {
    output_rejections();field_oracles();damaged_layout();all_allocation_faults();work_limits();
    puts("VM table stage/output/type/layout/environment/cleanup/fault/work owners PASS");return 0;
}
