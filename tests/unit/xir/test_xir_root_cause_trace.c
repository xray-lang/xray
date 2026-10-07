/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_cause_trace.c - Detached dual cause chains and physical resource gates
 */
#include "base/xmalloc.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_defaults_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_trace_owner.h"
#pragma push_macro("xr_compile_resources_work")
#define xr_compile_resources_work(resources,units) trace_charged_work(resources,units)
#include "xir/xxir_effects.c"
#pragma pop_macro("xr_compile_resources_work")
#include "xir_root_trace_fixture.h"

static const XrXirRootCauseStep trace_init[]={{0,1,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_INITIALIZER}};
static const XrXirRootCauseStep trace_read[]={{2,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_default[]={
    {5,0,2,UINT32_MAX,1,XR_XIR_ROOT_CAUSE_CALL},
    {2,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_indirect[]={{6,0,UINT32_MAX,UINT32_MAX,0,XR_XIR_ROOT_CAUSE_INDIRECT}};
static const XrXirRootCauseStep trace_mixed_root[]={{7,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_mixed_unknown[]={{7,1,UINT32_MAX,UINT32_MAX,0,XR_XIR_ROOT_CAUSE_INDIRECT}};
static const XrXirRootCauseStep trace_relay_root[]={
    {8,0,7,UINT32_MAX,1,XR_XIR_ROOT_CAUSE_CALL},
    {7,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_relay_unknown[]={
    {8,0,7,UINT32_MAX,1,XR_XIR_ROOT_CAUSE_CALL},
    {7,1,UINT32_MAX,UINT32_MAX,0,XR_XIR_ROOT_CAUSE_INDIRECT}};
static const XrXirRootCauseStep trace_scc_a[]={{9,1,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_scc_b[]={
    {10,0,9,UINT32_MAX,1,XR_XIR_ROOT_CAUSE_CALL},
    {9,1,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_tie[]={
    {11,0,10,UINT32_MAX,2,XR_XIR_ROOT_CAUSE_CALL},
    {10,0,9,UINT32_MAX,1,XR_XIR_ROOT_CAUSE_CALL},
    {9,1,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_cleanup[]={
    {12,0,13,UINT32_MAX,1,XR_XIR_ROOT_CAUSE_CLEANUP},
    {13,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
static const XrXirRootCauseStep trace_cleanup_body[]={{13,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_MUTABLE_SLOT}};
typedef struct TraceExpected {
    const XrXirRootCauseStep *root, *unknown;
    uint32_t root_count, unknown_count;
} TraceExpected;
static const TraceExpected trace_expected[]={
    {trace_init,NULL,1,0},{NULL,NULL,0,0},{trace_read,NULL,1,0},{NULL,NULL,0,0},
    {NULL,NULL,0,0},{trace_default,NULL,2,0},{NULL,trace_indirect,0,1},
    {trace_mixed_root,trace_mixed_unknown,1,1},{trace_relay_root,trace_relay_unknown,2,2},
    {trace_scc_a,NULL,1,0},{trace_scc_b,NULL,2,0},{trace_tie,NULL,3,0},
    {trace_cleanup,NULL,2,0},{trace_cleanup_body,NULL,1,0},{NULL,NULL,0,0},{NULL,NULL,0,0}};
_Static_assert(sizeof(trace_expected)/sizeof(*trace_expected)==16,"Every fixture function needs a literal oracle");

static void trace_chain_equal(const XrXirRootCauseStep *actual, const XrXirRootCauseStep *expected, uint32_t count) {
    CHECK(count ? actual!=NULL && expected!=NULL : actual==NULL && expected==NULL);
    for(uint32_t i=0;i<count;++i) {
        CHECK(actual[i].function==expected[i].function && actual[i].instruction==expected[i].instruction &&
            actual[i].callee==expected[i].callee && actual[i].slot==expected[i].slot &&
            actual[i].distance==expected[i].distance && actual[i].cause==expected[i].cause);
    }
}
static void trace_oracle(const XrXirRootCauseTrace *trace, uint32_t function) {
    CHECK(function<16); TraceExpected expected=trace_expected[function];
    const XrXirRootEffects *facts=xr_xir_root_cause_trace_facts(trace);
    CHECK(facts && facts->requires_root==(expected.root_count!=0) && facts->unresolved==(expected.unknown_count!=0));
    uint32_t count=UINT32_MAX;
    const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,false,&count);
    CHECK(count==expected.root_count); trace_chain_equal(steps,expected.root,count);
    steps=xr_xir_root_cause_trace_steps(trace,true,&count);
    CHECK(count==expected.unknown_count); trace_chain_equal(steps,expected.unknown,count);
}
static void trace_literals(void) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps());
    uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
    for(uint32_t f=0;f<16;++f) {
        XrXirRootCauseTrace *trace=NULL; XrCompileResourceStats before=trace_stats(&context);
        trace_attempts=0;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,f,&trace)==XR_XIR_OK && trace);
        XrCompileResourceStats after=trace_stats(&context);
        uint64_t steps=(uint64_t)trace_expected[f].root_count+trace_expected[f].unknown_count;
        uint64_t bytes=sizeof(XrXirRootCauseTrace)+steps*sizeof(XrXirRootCauseStep);
        CHECK(trace_attempts==1 && after.allocation_count==before.allocation_count+1);
        CHECK(after.allocated_bytes-before.allocated_bytes==bytes+sizeof(CompileAllocation));
        CHECK(after.work-before.work==bytes+2*steps+3);
        trace_oracle(trace,f); xr_xir_compile_root_cause_trace_free(trace);
        CHECK(trace_stats(&context).live_bytes==before.live_bytes);
    }
    xr_xir_compile_effects_free(effects); trace_owner_free(&context,baseline); trace_balanced(mark);
}
static void trace_detached(void) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps());
    XrXirArtifact *producer=trace_fixture(&context); XrXirEffects *effects=NULL; XrXirRootCauseTrace *trace=NULL;
    CHECK(xr_xir_compile_effects_analyze(producer,&effects)==XR_XIR_OK);
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&trace)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(producer);
    xr_compile_resources_release(context.resources); context=(XrXirCompileContext){0};
    CHECK(trace_live==mark.blocks+2); trace_oracle(trace,8);
    uint32_t count=99; const XrXirRootCauseStep *before=xr_xir_root_cause_trace_steps(trace,false,&count);
    CHECK(count==2); CHECK(xr_xir_root_cause_trace_steps(trace,false,NULL)==NULL);
    CHECK(xr_xir_root_cause_trace_steps(trace,false,&count)==before && count==2);
    xr_xir_compile_root_cause_trace_free(trace); trace_balanced(mark);
}
static void trace_invalid_inputs(void) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps()),invalid={0};
    uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
    XrXirCompileContext other=trace_owner(trace_caps()); uint64_t other_baseline=trace_stats(&other).live_bytes;
    XrCompileResourceStats before=trace_stats(&context),other_before=trace_stats(&other);
    TraceMark retained=trace_mark(); trace_attempts=0; XrXirRootCauseTrace *output=NULL;
    CHECK(xr_xir_compile_root_cause_trace_copy(NULL,effects,8,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_root_cause_trace_copy(&invalid,effects,8,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_root_cause_trace_copy(&other,effects,8,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,NULL,8,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,16,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,UINT32_MAX,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,NULL)==XR_XIR_BAD_STRUCTURE);
    output=(XrXirRootCauseTrace *)(uintptr_t)1;
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==(XrXirRootCauseTrace *)(uintptr_t)1 && !trace_attempts);
    uint32_t count=77;
    CHECK(xr_xir_root_cause_trace_steps(NULL,false,&count)==NULL && count==77);
    CHECK(xr_xir_root_cause_trace_steps(NULL,true,&count)==NULL && count==77);
    CHECK(xr_xir_root_cause_trace_facts(NULL)==NULL); xr_xir_compile_root_cause_trace_free(NULL);
    trace_stats_equal(trace_stats(&context),before); trace_stats_equal(trace_stats(&other),other_before);
    trace_balanced(retained);
    output=NULL; CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_OK);
    XrXirRootCauseTrace *occupied=output; uint8_t bytes[256];
    size_t size=sizeof(*occupied)+4*sizeof(XrXirRootCauseStep);
    _Static_assert(sizeof(XrXirRootCauseTrace)+4*sizeof(XrXirRootCauseStep)<=256,"Occupied trace snapshot must fit");
    memcpy(bytes,occupied,size); before=trace_stats(&context); retained=trace_mark(); trace_attempts=0;
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==occupied && !trace_attempts && !memcmp(bytes,occupied,size));
    CHECK(xr_xir_root_cause_trace_steps(occupied,false,NULL)==NULL);
    trace_stats_equal(trace_stats(&context),before); trace_balanced(retained); trace_oracle(output,8);
    xr_xir_compile_root_cause_trace_free(output); xr_xir_compile_effects_free(effects);
    trace_owner_free(&other,other_baseline); trace_owner_free(&context,baseline); trace_balanced(mark);
}
static void trace_bad_forests(void) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps());
    uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
    XrXirRootEffectWitness known[16],unknown[16]; XrXirRootEffects facts[16];
    memcpy(known,effects->root_witnesses,sizeof(known)); memcpy(unknown,effects->unresolved_witnesses,sizeof(unknown));
    memcpy(facts,effects->root,sizeof(facts));
    for(unsigned attack=0;attack<14;++attack) {
        switch(attack) {
        case 0: effects->root_witnesses[8].cause=XR_XIR_ROOT_CAUSE_NONE; break;
        case 1: effects->root_witnesses[8].distance=16; break;
        case 2: effects->root_witnesses[8].callee=16; break;
        case 3: effects->root_witnesses[8].slot=0; break;
        case 4: effects->root_witnesses[8].distance=3; break;
        case 5: effects->root_witnesses[8].cause=XR_XIR_ROOT_CAUSE_MUTABLE_SLOT; break;
        case 6: effects->root[7].requires_root=false; break;
        case 7: effects->root_witnesses[7].callee=7; break;
        case 8: effects->root_witnesses[7].cause=XR_XIR_ROOT_CAUSE_INDIRECT; break;
        case 9: effects->root[8].requires_root=false; break;
        case 10: effects->unresolved_witnesses[8].callee=8; break;
        case 11: effects->unresolved_witnesses[7].cause=XR_XIR_ROOT_CAUSE_MUTABLE_SLOT; break;
        case 12: effects->root[8].unresolved=false; break;
        case 13: effects->unresolved_witnesses[7].distance=UINT32_MAX; break;
        }
        TraceMark retained=trace_mark(); XrCompileResourceStats before=trace_stats(&context);
        XrXirRootCauseTrace *output=NULL; trace_attempts=0;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,8,&output)==XR_XIR_BAD_STRUCTURE);
        CHECK(!output && !trace_attempts && trace_stats(&context).allocated_bytes==before.allocated_bytes);
        trace_balanced(retained);
        memcpy(effects->root_witnesses,known,sizeof(known)); memcpy(effects->unresolved_witnesses,unknown,sizeof(unknown));
        memcpy(effects->root,facts,sizeof(facts));
    }
    xr_xir_compile_effects_free(effects); trace_owner_free(&context,baseline); trace_balanced(mark);
}
/* These synthetic enum substitutions test trace terminal validation alone;
 * they make no claim about type-marker or requirement inference. */
static void trace_terminal_values(void) {
    TraceMark mark=trace_mark(); XrXirCompileContext context=trace_owner(trace_caps());
    uint64_t baseline=trace_stats(&context).live_bytes; XrXirEffects *effects=trace_summary(&context);
    effects->root_witnesses[2].cause=XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST;
    effects->unresolved_witnesses[6].cause=XR_XIR_ROOT_CAUSE_REQUIREMENT;
    const XrXirRootCauseStep known={2,0,UINT32_MAX,0,0,XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST};
    const XrXirRootCauseStep unknown={6,0,UINT32_MAX,UINT32_MAX,0,XR_XIR_ROOT_CAUSE_REQUIREMENT};
    for(unsigned kind=0;kind<2;++kind) {
        XrXirRootCauseTrace *trace=NULL;
        CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,kind?6u:2u,&trace)==XR_XIR_OK);
        uint32_t count=99; const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,kind!=0,&count);
        CHECK(count==1); trace_chain_equal(steps,kind?&unknown:&known,count);
        xr_xir_compile_root_cause_trace_free(trace);
    }
    xr_xir_compile_effects_free(effects); trace_owner_free(&context,baseline); trace_balanced(mark);
}

#include "xir_root_trace_resources.h"
int main(void) {
    CHECK(!trace_live && !trace_live_bytes);
    trace_literals(); trace_detached(); trace_invalid_inputs(); trace_bad_forests(); trace_terminal_values();
    trace_oom_sites(); trace_budget_boundaries(); trace_post_admission_cuts();
    CHECK(!trace_live && !trace_live_bytes);
    puts("Owned root cause traces: literal dual chains, detached lifetime, failure atomicity and physical zero passed");
    return 0;
}
