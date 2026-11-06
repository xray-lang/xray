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
static XrCompileResourceLimits ee_caps(void) {
    return (XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,UINT64_C(4194304),true};
}
static XrXirCompileContext ee_owner(XrCompileResourceLimits caps) {
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();return c;
}

static XrCompileResourceStats ee_stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}

static void ee_release(XrXirCompileContext *c, uint64_t baseline) {
    CHECK(ee_stats(c).held_bytes==baseline);xr_compile_resources_release(c->resources);
    *c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}

static void ee_golden(const XrXirEffects *effects,EdgeCase mode,bool snapshot) {
    (void)snapshot;
    const EffectFinalOutcomes *facts=effect_final_outcomes(effects);
    CHECK(facts && effects->count==(mode==EE_CLEANUP || mode==EE_PANIC?4u:3u));
    CHECK(facts->functions[0].throws==XR_XIR_EFFECT_NONE && facts->functions[1].throws==XR_XIR_EFFECT_MAY);
    CHECK(facts->functions[2].throws==(mode==EE_TEMPORARY?XR_XIR_EFFECT_NONE:XR_XIR_EFFECT_MAY));
    CHECK(facts->functions[1].suspend==XR_XIR_EFFECT_NONE && facts->root[0].requires_root &&
        !facts->root[0].unresolved && effect_outcomes_empty(&effects->outcomes_work));
}
static XrXirStatus ee_whole(const XrXirCompileContext *c, EdgeCase mode, bool snapshot) {
    EdgeFixture f;ee_fixture(&f,mode,snapshot);XrXirDiagnostic d={0};XrXirEffects *e=NULL;
    f.module.stage=XR_XIR_BUILT;XrXirArtifact *checked=NULL;
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_MEMORY_LIMIT && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"edge fixture case=%u snapshot=%u status=%u function=%u block=%u instruction=%u\n",
            (unsigned)mode,(unsigned)snapshot,status,d.function,d.block,d.instruction);
    if (status==XR_XIR_OK) {
        CHECK(checked && xr_xir_compile_artifact_module(checked)->stage==XR_XIR_CHECKED);
        status=xr_xir_compile_effects_analyze(checked,&e);
    } else CHECK(!checked);
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&f,0xCE,sizeof(f));
    if (status==XR_XIR_OK) ee_golden(e,mode,snapshot);else CHECK(!e);
    xr_xir_compile_effects_free(e);return status;
}
static void ee_cases(void) {
    for (unsigned mode=EE_JUMP;mode<=EE_PHI;++mode) for (unsigned snapshot=0;snapshot<2;++snapshot) {
        XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).held_bytes;
        CHECK(ee_whole(&c,(EdgeCase)mode,snapshot!=0)==XR_XIR_OK);ee_release(&c,baseline);
    }
    puts("edge literals: jump/branch/filter/invoke/cleanup/panic/temporary/PHI loop PASS");
}
static void ee_negative(void) {
    EdgeFixture f;ee_fixture(&f,EE_JUMP,false);XrXirCompileContext c=ee_owner(ee_caps());uint64_t baseline=ee_stats(&c).held_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    f.subject[4].targets[0]=UINT32_MAX;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_STRUCTURE);
    f.subject[4].targets[0]=1;f.parameters[0]=(XrXirType)300;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_TYPE);
    f.parameters[0]=(XrXirType)EE_CELL;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    ee_release(&c,baseline);
}
#include "xir_error_edge_snapshot_resources.h"
int main(void) {
    ee_cases();ee_negative();
    ee_fault_case(EE_PHI);ee_fault_case(EE_PANIC);ee_fault_case(EE_INVOKE);ee_fault_case(EE_TEMPORARY);
    ee_limits();ee_occupied();
    puts("typed edge inputs, declared bounds, owned snapshots, real OOM and held-memory recovery PASS");return 0;
}
