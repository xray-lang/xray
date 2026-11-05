#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_compact_rows_controls.h - Sparse facts preserve complete error flow
 *
 * KEY CONCEPT:
 *   Scalar facts are immutable zero, while every escaping call and typed alias
 *   still executes. Fixed resource literals precede fresh runtime observation.
 */
#ifndef XIR_ERROR_COMPACT_ROWS_CONTROLS_H
#define XIR_ERROR_COMPACT_ROWS_CONTROLS_H
static void pc_compact_zero_fixture(EdgeFixture *f,bool branch) {
    ee_fixture(f,EE_JUMP,false);
    f->subject[0]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=2};
    f->subject[1]=f->init;f->blocks[0]=(XrXirBlock){.count=2};
    f->functions[1].parameter_count=0;f->functions[1].parameters=NULL;
    f->functions[1].instruction_count=2;f->functions[1].block_count=1;
    if (branch) {
        f->parameters[0]=XR_XIR_BOOL;f->functions[1].parameters=f->parameters;
        f->functions[1].parameter_count=1;
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}};
        f->subject[1]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=2};
        f->subject[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}};
        f->subject[3]=f->init;f->blocks[0]=(XrXirBlock){.count=1};
        f->blocks[1]=(XrXirBlock){.first=1,.count=2};
        f->blocks[2]=(XrXirBlock){.first=3,.count=1};
        f->functions[1].instruction_count=4;f->functions[1].block_count=3;
    }
}
static void pc_compact_variants(EdgeFixture *f,XrXirNominalVariant *variants,char names[70][4]) {
    for (uint32_t v=0;v<70;++v) {
        names[v][0]='V';names[v][1]=(char)('0'+v/10);names[v][2]=(char)('0'+v%10);names[v][3]=0;
        variants[v]=(XrXirNominalVariant){{names[v],3},0,0};
    }
    f->nominal.variants=variants;f->nominal.variant_count=70;
}
static void pc_compact_wide_fixture(EdgeFixture *f,XrXirNominalVariant *variants,char names[70][4]) {
    pc_fixture(f,PC_REVERSE,false);pc_compact_variants(f,variants,names);
    f->subject[512].immediate=62;
}
/* Reclassification is local to each real body visit, including zero facts. */
/* Physical block order can place a valid COPY before its dominating source.
 * The same fresh local table also retains finite refusal for an invalid cycle. */
/* Large bodies grow only transient classification bytes. Every physical
 * failure must retire that allocation before a fresh same-ledger retry. */
static XrXirStatus pc_compact_whole(const XrXirCompileContext *c,unsigned mode) {
    EdgeFixture f;XrXirNominalVariant variants[70];char names[70][4];
    if (mode==1) pc_compact_wide_fixture(&f,variants,names);
    else pc_compact_zero_fixture(&f,mode==2);
    f.module.stage=XR_XIR_BUILT;XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&e);
    else CHECK(!checked);
    if (status==XR_XIR_OK) {
        CHECK(e && e->count==3 && effect_final_outcomes(e)->functions[1].throws==XR_XIR_EFFECT_MAY);
        for (uint32_t i=0;i<3;++i) {
            CHECK(effect_roots_read(e)->root[i].requires_root==(i==0) && !effect_roots_read(e)->root[i].unresolved);
            CHECK(effect_final_outcomes(e)->functions[i].suspend==XR_XIR_EFFECT_NONE);
        }
    } else CHECK(!e);
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);return status;
}
static void pc_compact_faults(unsigned mode) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirStatus status=pc_compact_whole(&c,mode);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline && pc_compact_whole(&c,mode)==XR_XIR_OK);
        pc_release(&c,baseline);
    }
    printf("compact mode%u full admission/inference OOM sites%zu sameownerretry physical0\n",mode,sites);
}
static XrCompileResourceStats pc_compact_measured(XrCompileResourceLimits caps,unsigned mode,XrXirStatus expected) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(pc_compact_whole(&c,mode)==expected);XrCompileResourceStats s=pc_stats(&c);pc_release(&c,baseline);return s;
}
static void pc_compact_limits(unsigned mode) {
    XrCompileResourceStats a=pc_compact_measured(pc_caps(),mode,XR_XIR_OK);
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=pc_compact_measured(exact,mode,XR_XIR_OK);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)pc_compact_measured(less,mode,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)pc_compact_measured(less,mode,XR_XIR_BUDGET);
    less=exact;--less.work;(void)pc_compact_measured(less,mode,XR_XIR_BUDGET);
}
static void pc_compact_bad_types(void) {
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    for (unsigned mode=0;mode<3;++mode) {
        EdgeFixture f;ee_fixture(&f,EE_JUMP,false);f.module.stage=XR_XIR_BUILT;
        if (!mode) f.subject[0].type=(XrXirType)4;
        else if (mode==1) f.subject[3].type=XR_XIR_I64;
        else f.subject[3]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={2}};
        XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_BAD_TYPE && !checked);
        CHECK(pc_stats(&c).live_bytes==baseline);
    }
    pc_release(&c,baseline);
}
static void pc_compact_controls(void) {
    pc_compact_bad_types();
    for (unsigned mode=0;mode<3;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(pc_compact_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    for (unsigned mode=0;mode<2;++mode) { pc_compact_faults(mode);pc_compact_limits(mode); }
    puts("compact literals: 4684/57/99/4972 bytes, zero-active CALL, bit64, immutable zero, exact89/88");
}
#endif // XIR_ERROR_COMPACT_ROWS_CONTROLS_H
