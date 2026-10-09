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
static void pc_compact_classification(void) {
    static const XrXirType scalars[]={XR_XIR_UNIT,XR_XIR_BOOL,XR_XIR_I64,XR_XIR_STRING,
        XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,
        XR_XIR_U64,XR_XIR_F32,XR_XIR_F64,XR_XIR_PANIC_INFO,XR_XIR_RUNE};
    static const XrXirType retained[]={XR_XIR_ERROR,(XrXirType)4,(XrXirType)EE_ENUM,
        (XrXirType)EE_CELL,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,(XrXirType)UINT32_MAX};
    XrXirInstruction op={.op=XR_XIR_COPY};
    XrXirFunction function={.instructions=&op,.instruction_count=1};ErrorFlow flow={0};flow.function=&function;
    bool active=true;
    for (size_t i=0;i<sizeof(scalars)/sizeof(*scalars);++i) {
        op.type=scalars[i];CHECK(error_row_active(&flow,0,&active)==XR_XIR_OK && !active);
        function.parameters=&scalars[i];function.parameter_count=1;CHECK(error_row_active(&flow,0,&active)==XR_XIR_OK && !active);
        function.parameter_count=0;
    }
    for (size_t i=0;i<sizeof(retained)/sizeof(*retained);++i) {
        op.type=retained[i];CHECK(error_row_active(&flow,0,&active)==XR_XIR_OK && active);
    }
    /* Conservative membership does not admit these malformed Unit producers. */
    op.type=XR_XIR_UNIT;op.op=XR_XIR_GO;CHECK(error_row_active(&flow,0,&active)==XR_XIR_OK && active);
    op.op=XR_XIR_INVOKE_ERROR;CHECK(error_row_active(&flow,0,&active)==XR_XIR_OK && active);
}
static void pc_compact_bulk(void) {
    EdgeFixture f;pc_fixture(&f,PC_REVERSE,false);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    pc_private_begin(&c,&f,&e,&terms,&flow);uint64_t before=pc_stats(&c).work;
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==4);
    CHECK(pc_stats(&c).work-before<UINT64_C(25000));
    CHECK(flow.values==545 && flow.active_count==1 && flow.active[0]==512);
    CHECK(flow.stride==1 && e.words==1 && flow.storage_capacity==4684);
    CHECK(flow.zero[0]==0 && flow.zero!=flow.work && flow.zero!=flow.snapshot);
    for (uint32_t v=0;v<545;++v) {
        CHECK(flow.slots[v]==(v==512 ? 0u : UINT32_MAX));
        CHECK(error_read(&flow,flow.work,v)[0]==(v==512 ? UINT64_C(4) : 0));
        CHECK((error_value(&flow,flow.work,v)!=NULL)==(v==512));
    }
    size_t allocations=attempts;
    CHECK(error_instruction(&flow,0)==XR_XIR_OK && flow.zero[0]==0);
    CHECK(error_value(&flow,flow.work,512)[0]==4 && attempts==allocations);
    pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
}
static void pc_compact_zero_private(void) {
    for (unsigned branch=0;branch<2;++branch) {
        EdgeFixture f;pc_compact_zero_fixture(&f,branch!=0);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);e.errors[2]=8;
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[1]==8);
        CHECK(!flow.active_count && flow.stride==1 && e.words==1 && flow.zero[0]==0);
        CHECK(flow.storage_capacity==(branch ? 99u : 57u));
        CHECK(flow.zero!=flow.snapshot && flow.zero!=flow.work && flow.snapshot[0]==8);
        CHECK(flow.work[0]==0 && flow.states[0]==0);
        for (uint32_t b=0;b<f.functions[1].block_count;++b) CHECK(flow.reachable[b]==1);
        for (uint32_t v=0;v<flow.values;++v) {
            CHECK(flow.slots[v]==UINT32_MAX && !error_value(&flow,flow.work,v));
            CHECK(error_read(&flow,flow.work,v)==flow.zero);
        }
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
}
static void pc_compact_two_words(void) {
    for (unsigned enough=0;enough<2;++enough) {
        EdgeFixture f;XrXirNominalVariant variants[70];char names[70][4];
        pc_compact_wide_fixture(&f,variants,names);
        XrCompileResourceLimits caps=pc_caps();XrXirCompileContext c=pc_owner(caps);
        uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.atom_count==70 && e.words==2);
        CHECK(e.errors[2]==0 && e.errors[3]==1 && flow.active_count==1 && flow.active[0]==512);
        CHECK(flow.stride==2 && flow.storage_capacity==4972);
        CHECK(flow.zero[0]==0 && flow.zero[1]==0 && error_read(&flow,flow.work,0)==flow.zero);
        uint64_t *dest=flow.states+(size_t)30*flow.stride;
        memset(dest,0,2*sizeof(uint64_t));flow.reachable[30]=0;flow.changed=false;
        memset(flow.edge,0xa5,2*sizeof(uint64_t));memset(flow.snapshot,0x5a,2*sizeof(uint64_t));
        uint64_t remaining=enough ? 77u : 76u,used=pc_stats(&c).work;
        CHECK(used<=caps.work-remaining);
        CHECK(xr_compile_resources_work(c.resources,caps.work-used-remaining)==XR_COMPILE_RESOURCE_OK);
        uint64_t before=pc_stats(&c).work;size_t allocations=attempts;
        CHECK(error_edge(&flow,31,30,NULL,true)==(enough ? XR_XIR_OK : XR_XIR_BUDGET));
        CHECK(pc_stats(&c).work-before==(enough ? 77u : 6u) && attempts==allocations);
        CHECK(dest[0]==0 && dest[1]==(enough ? 1u : 0u));
        CHECK(flow.reachable[30]==(enough ? 3u : 0u) && flow.changed==(enough!=0));
        CHECK(flow.work[0]==0 && flow.work[1]==1 && flow.zero[0]==0 && flow.zero[1]==0);
        pc_pattern(flow.edge,2*sizeof(uint64_t),0xa5);pc_pattern(flow.snapshot,2*sizeof(uint64_t),0x5a);
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
}
static void pc_compact_reuse(void) {
    EdgeFixture f;XrXirNominalVariant variants[70];char names[70][4];
    pc_compact_wide_fixture(&f,variants,names);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    pc_private_begin(&c,&f,&e,&terms,&flow);
    CHECK(error_function(&flow,1)==XR_XIR_OK && flow.storage_capacity==4972);
    uint8_t *storage=flow.storage;size_t allocations=attempts;
    pc_compact_zero_fixture(&f,false);pc_compact_variants(&f,variants,names);
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    allocations=attempts;memset(storage,0xff,4972);e.errors[2]=e.errors[3]=0;e.errors[4]=8;e.errors[5]=0;
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[2]==8 && e.errors[3]==0);
    CHECK(flow.storage==storage && flow.storage_capacity==4972 && !flow.active_count && flow.stride==2);
    CHECK(flow.zero[0]==0 && flow.zero[1]==0 && flow.snapshot[0]==8 && flow.snapshot[1]==0);
    CHECK(attempts==allocations && !error_value(&flow,flow.work,0));
    pc_compact_wide_fixture(&f,variants,names);
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    allocations=attempts;memset(storage,0xff,4972);e.errors[2]=e.errors[3]=0;
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.errors[2]==0 && e.errors[3]==1);
    CHECK(flow.storage==storage && flow.storage_capacity==4972 && flow.active_count==1 && flow.slots[512]==0);
    CHECK(flow.zero[0]==0 && flow.zero[1]==0 && attempts==allocations);
    pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
}
static XrXirStatus pc_compact_whole(const XrXirCompileContext *c,unsigned mode) {
    EdgeFixture f;XrXirNominalVariant variants[70];char names[70][4];
    if (mode==1) pc_compact_wide_fixture(&f,variants,names);
    else pc_compact_zero_fixture(&f,mode==2);
    f.module.stage=XR_XIR_BUILT;XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&e);
    else CHECK(!checked);
    if (status==XR_XIR_OK) {
        CHECK(e && e->count==3 && e->functions[1].throws==XR_XIR_EFFECT_MAY);
        CHECK(!xr_xir_effects_error_unknown(e,1) && !xr_xir_effects_error_unidentified(e,1));
        if (mode==1) {
            CHECK(e->words==2 && e->atom_count==70 && e->errors[2]==0 && e->errors[3]==1);
            CHECK(e->errors[4]==8 && e->errors[5]==0 && xr_xir_effects_error(e,1,(XrXirType)EE_ENUM,62));
        } else CHECK(e->words==1 && e->atom_count==2 && e->errors[1]==8 && e->errors[2]==8);
        for (uint32_t i=0;i<3;++i) {
            CHECK(e->root[i].requires_root==(i==0) && !e->root[i].unresolved && e->task_errors[i]==XR_XIR_OK);
            CHECK(e->functions[i].suspend==XR_XIR_EFFECT_NONE);
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
    pc_compact_classification();pc_compact_bulk();pc_compact_zero_private();pc_compact_two_words();
    pc_compact_reuse();pc_compact_bad_types();
    for (unsigned mode=0;mode<3;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(pc_compact_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    for (unsigned mode=0;mode<2;++mode) { pc_compact_faults(mode);pc_compact_limits(mode); }
    puts("compact literals: 4684/57/99/4972 bytes, zero-active CALL, bit64, immutable zero, exact77/76");
}
#endif // XIR_ERROR_COMPACT_ROWS_CONTROLS_H
