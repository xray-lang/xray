/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_callee_first_controls.h - Independent dependency ordering facts
 *
 * KEY CONCEPT:
 *   Ordering preserves multiedges and fair cyclic summary propagation.
 *   Owned scratch failures preserve summaries and permit same-owner retry.
 */
#ifndef XIR_ERROR_CALLEE_FIRST_CONTROLS_H
#define XIR_ERROR_CALLEE_FIRST_CONTROLS_H
#include "xir_construction_fixture.h"
#include "xir_error_dependency_fixture.h"
typedef enum CalleeWholeMode { CF_CHAIN,CF_DUPLICATE,CF_CYCLE,CF_GO,CF_UNKNOWN } CalleeWholeMode;
typedef struct CalleeWholeFixture { ErrorDependencyFixture dependency;uint32_t duplicate[2]; } CalleeWholeFixture;
static void cf_whole_fixture(CalleeWholeFixture *whole,CalleeWholeMode mode,bool reverse) {
    memset(whole,0,sizeof(*whole));ErrorDependencyFixture *f=&whole->dependency;
    ed_fixture(f,mode==CF_GO ? ED_GO : mode==CF_UNKNOWN ? ED_UNKNOWN : ED_DIRECT,false);
    f->module.stage=XR_XIR_BUILT;
    uint32_t leaf=ED_COUNT-1;
    if (mode==CF_CHAIN && reverse) {
        for (uint32_t i=2;i<ED_COUNT;++i) {
            f->ops[i][0]=(XrXirInstruction){.op=XR_XIR_CALL,.args={0,1},.immediate=i-1};
            f->ops[i][1]=f->init;f->blocks[i].count=2;f->functions[i].instruction_count=2;
            f->functions[i].operands=&f->operand;f->functions[i].operand_count=1;
        }
        leaf=2;
    }
    f->ops[leaf][0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)ED_ENUM};
    f->ops[leaf][1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1}};
    f->functions[leaf].operands=NULL;f->functions[leaf].operand_count=0;
    f->blocks[leaf].count=2;f->functions[leaf].instruction_count=2;
    if (mode==CF_CHAIN && reverse) {
        f->blocks[ED_COUNT-1].count=2;f->functions[ED_COUNT-1].instruction_count=2;
    } else if (mode==CF_DUPLICATE || mode==CF_CYCLE) {
        uint32_t caller=mode==CF_DUPLICATE ? 2u : ED_COUNT-2;
        f->ops[caller][1]=f->ops[caller][0];f->ops[caller][1].args[0]=1;
        if (mode==CF_CYCLE) f->ops[caller][0].immediate=2;
        f->ops[caller][2]=f->init;f->blocks[caller].count=3;
        f->functions[caller].instruction_count=3;
        f->functions[caller].operands=whole->duplicate;f->functions[caller].operand_count=2;
    }
}
static void cf_whole_golden(const XrXirEffects *e,CalleeWholeMode mode) {
    CHECK(e && e->count==ED_COUNT);
    for (uint32_t f=0;f<ED_COUNT;++f) {
        CHECK(effect_final_outcomes(e)->functions[f].throws==(f<2 ? XR_XIR_EFFECT_NONE : XR_XIR_EFFECT_MAY));
        CHECK(effect_roots_read(e)->root[f].requires_root==(f==0) && effect_roots_read(e)->root[f].unresolved==(mode==CF_UNKNOWN && f==2));
        CHECK(effect_final_outcomes(e)->functions[f].suspend==(f==2 && mode==CF_GO ? XR_XIR_EFFECT_MAY :
            f==2 && mode==CF_UNKNOWN ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE));
    }
}
static XrXirStatus cf_whole(const XrXirCompileContext *c,CalleeWholeMode mode,bool reverse) {
    CalleeWholeFixture whole;cf_whole_fixture(&whole,mode,reverse);
    XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(c, &whole.dependency.module, &checked, &d);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&e);
    if (status==XR_XIR_OK) {
        cf_whole_golden(e,mode);
    } else {
        CHECK(!e);
        if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr,"callee mode%u reverse%u status%u f%u b%u i%u\n",
                (unsigned)mode,(unsigned)reverse,(unsigned)status,d.function,d.block,d.instruction);
    }
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);return status;
}
static void cf_whole_cases(void) {
    for (unsigned mode=CF_CHAIN;mode<=CF_UNKNOWN;++mode) for (unsigned reverse=0;reverse<2;++reverse) {
        if (reverse && mode!=CF_CHAIN) continue;
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(cf_whole(&c,(CalleeWholeMode)mode,reverse!=0)==XR_XIR_OK);pc_release(&c,baseline);
    }
    puts("callee-first full32 chain both orders/real duplicate/cyclic external source/GO/unknown literals");
}
static void cf_whole_faults(void) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirStatus status=cf_whole(&c,CF_CYCLE,false);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline && cf_whole(&c,CF_CYCLE,false)==XR_XIR_OK);
        pc_release(&c,baseline);
    }
    printf("callee-first public Check/effects actual OOM sites%zu sameownerretry physical0\n",sites);
}
static XrCompileResourceStats cf_measured(XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(cf_whole(&c,CF_CYCLE,false)==expected);XrCompileResourceStats s=pc_stats(&c);
    pc_release(&c,baseline);return s;
}
static void cf_whole_limits(void) {
    XrCompileResourceStats a=cf_measured(pc_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=cf_measured(exact,XR_XIR_OK);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)cf_measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)cf_measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)cf_measured(less,XR_XIR_BUDGET);
}
static void cf_occupied(void) {
    CalleeWholeFixture f;cf_whole_fixture(&f,CF_CHAIN,false);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&c, &f.dependency.module, &checked, NULL)==XR_XIR_OK);
    XrXirEffects sentinel={0};memset(&sentinel,0x5a,sizeof(sentinel));XrXirEffects copy=sentinel,*e=&sentinel;
    XrCompileResourceStats before=pc_stats(&c);
    CHECK(xr_xir_compile_effects_analyze(checked,&e)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=pc_stats(&c);
    CHECK(e==&sentinel && !memcmp(&sentinel,&copy,sizeof(sentinel)));
    CHECK(before.work==after.work && before.live_bytes==after.live_bytes && before.allocated_bytes==after.allocated_bytes);
    xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
}
static void pc_callee_first_controls(void) {
    cf_whole_cases();
    cf_whole_faults();cf_whole_limits();cf_occupied();
}
#endif // XIR_ERROR_CALLEE_FIRST_CONTROLS_H
