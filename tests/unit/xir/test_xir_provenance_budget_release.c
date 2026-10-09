/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_provenance_budget_release.c - Release the owned reachability queue
 *
 * KEY CONCEPT:
 *   A fresh ledger rejects the second work charge after queue allocation.
 *   The interior seen view never becomes a separately releasable owner.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical provenance_blocks[1024];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *provenance_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    provenance_blocks[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void provenance_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && provenance_blocks[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=provenance_blocks[i].bytes);
    live_bytes-=provenance_blocks[i].bytes;provenance_blocks[i]=provenance_blocks[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) provenance_malloc(bytes)
#define xr_free(pointer) provenance_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

/* Canonical production definitions give this TU the private reachability entry. */
#include "xir/xxir.c"
typedef struct ProvenanceFixture {
    XrXirInstruction instruction;
    XrXirBlock block;
    XrXirFunction function;
    XrXirSourceModule source;
    XrXirFunctionIdentity identity;
    XrXirDeclarations declarations;
    XrXirModule module;
    XrXirOrigin origin;
} ProvenanceFixture;
static void fixture(ProvenanceFixture *f) {
    memset(f,0,sizeof(*f));f->instruction=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->block=(XrXirBlock){.first=0,.count=1};
    f->function=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&f->block,.block_count=1,.instructions=&f->instruction,.instruction_count=1};
    f->source=(XrXirSourceModule){"owner",5,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=&f->identity,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=&f->function,.function_count=1,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
    f->origin=(XrXirOrigin){.function=0,.argument_count=0,.arguments=NULL};
}
static XrCompileResourceStats stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static XrXirCompileContext owner_new(uint64_t work) {
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    const XrCompileResourceLimits limits={65536,32768,work};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();CHECK(stats(&c).work==1);return c;
}
static void owner_free(XrXirCompileContext *c,size_t physical,uint64_t bytes) {
    CHECK(live==physical && live_bytes==bytes && stats(c).live_bytes==bytes);
    xr_compile_resources_release(c->resources);*c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static void ample_control(ProvenanceFixture *f) {
    XrXirCompileContext c=owner_new(100000);size_t physical=live;uint64_t bytes=live_bytes;
    CHECK(xir_fixture_verify(&c, &f->module, NULL)==XR_XIR_OK);
    ProvenanceMatch match={.source=&f->module,.destination=&f->module,.origins=&f->origin,.count=1,.remaining=&c};
    attempts=0;CHECK(provenance_reachable(&match)==XR_XIR_OK && attempts==1);
    owner_free(&c,physical,bytes);
}
static void precise_rejection(ProvenanceFixture *f) {
    /* New ledger initialization uses one unit. Three remaining units allow
     * count+source_count=2, then allocation=1, but reject the next two units. */
    XrXirCompileContext c=owner_new(4);size_t physical=live;uint64_t bytes=live_bytes;
    ProvenanceMatch match={.source=&f->module,.destination=&f->module,.origins=&f->origin,.count=1,.remaining=&c};
    attempts=0;CHECK(provenance_reachable(&match)==XR_XIR_BUDGET && attempts==1);
    CHECK(stats(&c).work==4);owner_free(&c,physical,bytes);
}
static void public_work_cuts(ProvenanceFixture *f) {
    /* Public correspondence currently expects an ordinary Program entry. */
    XrXirInstruction entry_ops[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    XrXirBlock entry_block={.first=0,.count=2};
    XrXirFunction functions[2]={f->function,{.name="entry",.name_length=5,.result=XR_XIR_I64,
        .blocks=&entry_block,.block_count=1,.instructions=entry_ops,.instruction_count=2}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirDeclarations declarations=f->declarations;declarations.functions=identities;
    declarations.root_module=0;declarations.entry_function=1;
    XrXirModule module=f->module;module.functions=functions;module.function_count=2;
    module.declarations=&declarations;module.linkage_kind=XR_XIR_PROGRAM;
    XrXirOrigin origins[2]={{.function=0},{.function=1}};
    /* This borrowed metadata wrapper owns no storage; the ordinary graph is
     * independently verified below before its complete correspondence proof. */
    XrXirArtifact source={.module=module};
    XrXirProvenance instance={.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=&source,.origins=origins,.count=2};
    XrXirCompileContext c=owner_new(100000);size_t physical=live;uint64_t bytes=live_bytes;
    CHECK(xir_fixture_verify(&c, &module, NULL)==XR_XIR_OK);owner_free(&c,physical,bytes);
    c=owner_new(100000);physical=live;bytes=live_bytes;source.context=c;
    CHECK(xr_xir_compile_provenance_functions_match(&c,&module,&module,&instance,NULL)==XR_XIR_OK);
    XrCompileResourceStats required=stats(&c);
    uint64_t exact=required.work;CHECK(exact>4);owner_free(&c,physical,bytes);
    for(uint64_t work=1;work<=exact;++work) {
        c=owner_new(work);physical=live;bytes=live_bytes;source.context=c;
        XrXirStatus status=xr_xir_compile_provenance_functions_match(&c,&module,&module,&instance,NULL);
        CHECK(status==(work==exact?XR_XIR_OK:XR_XIR_BUDGET));owner_free(&c,physical,bytes);
    }
    printf("public provenance census: allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
        (unsigned long long)required.work);
    printf("public provenance complete work cuts=%llu each physical=0/0\n",(unsigned long long)exact);
}
int main(void) {
    ProvenanceFixture f;fixture(&f);ample_control(&f);precise_rejection(&f);public_work_cuts(&f);
    puts("provenance queue owner released on second work rejection; physical=0/0 PASS");return 0;
}
