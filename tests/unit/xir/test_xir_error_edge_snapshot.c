/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_edge_snapshot.c - Typed CFG facts and necessary PHI snapshots
 *
 * KEY CONCEPT:
 *   Every PHI reads the same predecessor row. Edges without PHIs do not copy
 *   bytes into a temporary that no transfer consumes.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct EdgePhysical { void *pointer;size_t bytes; } EdgePhysical;
static EdgePhysical physical_allocations[2048];
static size_t live,live_bytes,attempts,fail_at=SIZE_MAX;
static bool injected;
static void *ee_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true;return NULL; }
    void *p=xr_malloc(bytes);if (!p) return NULL;
    CHECK(live<2048 && bytes<=SIZE_MAX-live_bytes);
    physical_allocations[live++]=(EdgePhysical){p,bytes};live_bytes+=bytes;return p;
}
static void ee_free(void *p) {
    if (!p) return;
    size_t i=0;while (i<live && physical_allocations[i].pointer!=p) ++i;
    CHECK(i<live && live_bytes>=physical_allocations[i].bytes);
    live_bytes-=physical_allocations[i].bytes;physical_allocations[i]=physical_allocations[--live];xr_free(p);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) ee_malloc(bytes)
#define xr_free(pointer) ee_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "xir/xxir_effects.c"
#include "xir_error_edge_snapshot_fixture.h"
static XrCompileResourceLimits ee_caps(void) { return (XrCompileResourceLimits){8388608,4194304,8000000}; }
static XrXirCompileContext ee_owner(XrCompileResourceLimits caps) {
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats ee_stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void ee_release(XrXirCompileContext *c, uint64_t baseline) {
    CHECK(ee_stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);
    *c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static void ee_golden(const XrXirEffects *e, EdgeCase mode, bool snapshot) {
    uint64_t bits=ee_expected(mode,snapshot);
    CHECK(e && e->count>=3 && e->atom_count==2 && e->words==1 && e->errors[1]==bits);
    CHECK(!xr_xir_effects_error_unknown(e,1) && !xr_xir_effects_error_unidentified(e,1));
    CHECK(xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,0)==((bits&4)!=0));
    CHECK(xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,1)==((bits&8)!=0));
    CHECK(xr_xir_effects_function(e,1)->throws==XR_XIR_EFFECT_MAY);
}
static XrXirStatus ee_whole(const XrXirCompileContext *c, EdgeCase mode, bool snapshot) {
    EdgeFixture f;ee_fixture(&f,mode,snapshot);XrXirDiagnostic d={0};XrXirEffects *e=NULL;
    f.module.stage=XR_XIR_BUILT;XrXirArtifact *checked=NULL;
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"edge fixture case=%u snapshot=%u status=%u function=%u block=%u instruction=%u\n",
            (unsigned)mode,(unsigned)snapshot,status,d.function,d.block,d.instruction);
    if (status==XR_XIR_OK) {
        CHECK(checked && xr_xir_compile_artifact_module(checked)->stage==XR_XIR_CHECKED);
        status=xr_xir_compile_effects_analyze(checked,&e);
    } else CHECK(!checked);
    if (status==XR_XIR_OK) ee_golden(e,mode,snapshot);else CHECK(!e);
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);return status;
}
static void ee_cases(void) {
    for (unsigned mode=EE_JUMP;mode<=EE_PHI;++mode) for (unsigned snapshot=0;snapshot<2;++snapshot) {
        XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
        CHECK(ee_whole(&c,(EdgeCase)mode,snapshot!=0)==XR_XIR_OK);ee_release(&c,baseline);
    }
    puts("edge literals: jump/branch/filter/invoke/cleanup/panic/temporary/PHI loop PASS");
}
static void ee_private_begin(const XrXirCompileContext *c, EdgeFixture *f,
                             XrXirEffects *e, EffectTerms *terms, ErrorFlow *flow) {
    CHECK(xir_fixture_verify(c, &f->module, NULL)==XR_XIR_OK);
    *e=(XrXirEffects){0};e->count=f->module.function_count;
    XrXirCompileContext context=*c;
    CHECK(effect_errors_seed(&f->module,e,&context)==XR_XIR_OK);
    *terms=(EffectTerms){0};terms->remaining=flow->remaining;terms->types=f->types;
    flow->module=&f->module;flow->effects=e;flow->terms=terms;
}
static void ee_private_free(ErrorFlow *flow, EffectTerms *terms, XrXirEffects *e) {
    error_storage_free(flow);effect_terms_free(terms);
    xr_compile_resources_free(e->errors);xr_compile_resources_free(e->atoms);
    CHECK(!flow->storage && !flow->storage_capacity && !flow->states && !flow->roots && !flow->cells);
}
static void ee_phi_step(void) {
    EdgeFixture f;ee_fixture(&f,EE_PHI,false);XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    ee_private_begin(&c,&f,&e,&terms,&flow);
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==12);
    memset(flow.work,0,flow.stride*sizeof(uint64_t));
    error_value(&flow,flow.work,5)[0]=4;error_value(&flow,flow.work,6)[0]=8;
    CHECK(error_edge(&flow,2,1,NULL,true)==XR_XIR_OK);
    CHECK(error_value(&flow,flow.edge,5)[0]==8 && error_value(&flow,flow.edge,6)[0]==4);
    CHECK(error_value(&flow,flow.snapshot,5)[0]==4 && error_value(&flow,flow.snapshot,6)[0]==8);
    ee_private_free(&flow,&terms,&e);ee_release(&c,baseline);
    puts("two PHIs crossed on one backedge: independent predecessor literals 8/4 PASS");
}
static void ee_freshness(void) {
    EdgeFixture f;ee_fixture(&f,EE_PHI,false);XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    ee_private_begin(&c,&f,&e,&terms,&flow);
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==12);
    for (unsigned snapshot=0;snapshot<2;++snapshot) {
        ee_fixture(&f,EE_TEMPORARY,snapshot!=0);CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
        e.errors[1]=0;memset(flow.storage,0xff,flow.storage_capacity);
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==(snapshot?4u:8u));
    }
    ee_fixture(&f,EE_FILTER,false);CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    e.errors[1]=0;memset(flow.storage,0xff,flow.storage_capacity);
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==4);
    ee_fixture(&f,EE_PHI,false);CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    e.errors[1]=0;memset(flow.storage,0xff,flow.storage_capacity);
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==12);
    ee_private_free(&flow,&terms,&e);ee_release(&c,baseline);
}
static void ee_bulk(void) {
    EdgeFixture f;ee_fixture(&f,EE_BULK,false);XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    ee_private_begin(&c,&f,&e,&terms,&flow);
    CHECK(f.functions[1].instruction_count==545 && f.functions[1].block_count==32);
    uint64_t before=ee_stats(&c).work;
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==4);
    uint64_t work=ee_stats(&c).work-before;CHECK(work<UINT64_C(1000000));
    _Static_assert(UINT64_C(2)*31*545*22+UINT64_C(2)*32*545*9+UINT64_C(35)*545*8>UINT64_C(1000000),
        "old full snapshots exceed the independent error-function work ceiling");
    printf("edge no-PHI bulk: 512 scalar definitions, 32 blocks, work=%llu below literal1000000\n",(unsigned long long)work);
    ee_private_free(&flow,&terms,&e);ee_release(&c,baseline);
}
static void ee_negative(void) {
    EdgeFixture f;ee_fixture(&f,EE_JUMP,false);XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).live_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    f.subject[4].targets[0]=UINT32_MAX;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_STRUCTURE);
    f.subject[4].targets[0]=1;f.parameters[0]=(XrXirType)300;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_TYPE);
    f.parameters[0]=(XrXirType)EE_CELL;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    ee_release(&c,baseline);
}
#include "xir_error_edge_snapshot_resources.h"
int main(void) {
    ee_cases();ee_phi_step();ee_freshness();ee_bulk();ee_negative();
    ee_fault_case(EE_PHI);ee_fault_case(EE_PANIC);ee_fault_case(EE_INVOKE);ee_fault_case(EE_TEMPORARY);
    ee_limits();ee_occupied();
    puts("necessary PHI snapshots, exact typed errors, full checks and finite physical0 PASS");return 0;
}
