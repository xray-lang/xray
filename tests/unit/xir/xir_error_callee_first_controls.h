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
typedef struct CalleeOrderFixture {
    uint32_t heads[5],error_heads[5],queue[5];
    uint8_t queued[5];
    EffectEdge edges[4],error_edges[2];
    uint64_t summary[10];
    XrXirModule module;
    XrXirEffects effects;
    EffectGraph graph;
} CalleeOrderFixture;
static void cf_edge(CalleeOrderFixture *f,uint32_t callee,uint32_t caller,bool go) {
    uint32_t *head=go ? f->error_heads : f->heads;
    EffectEdge *edges=go ? f->error_edges : f->edges;
    uint32_t *count=go ? &f->graph.error_edge_count : &f->graph.edge_count;
    CHECK(*count<(go ? 2u : 4u));
    edges[*count]=(EffectEdge){caller,head[callee],0,false};head[callee]=(*count)++;
}
static void cf_order_fixture(CalleeOrderFixture *f,unsigned mode) {
    memset(f,0,sizeof(*f));uint32_t n=mode==3 ? 5u : mode>=2 ? 3u : 4u;
    f->module.function_count=n;f->effects.count=n;f->effects.words=2;
    for (uint32_t i=0;i<10;++i) f->summary[i]=UINT64_C(0x9182736455aa6600)+i;
    f->effects.errors=f->summary;
    for (uint32_t i=0;i<5;++i) f->heads[i]=f->error_heads[i]=UINT32_MAX;
    f->graph=(EffectGraph){.heads=f->heads,.queue=f->queue,.queued=f->queued,.edges=f->edges};
    if (mode<2) {
        cf_edge(f,2,0,false);cf_edge(f,3,2,false);
        if (mode) cf_edge(f,3,2,false);
    } else if (mode==2) {
        cf_edge(f,2,1,false);cf_edge(f,1,0,true);
    } else if (mode==3) {
        cf_edge(f,2,0,false);cf_edge(f,3,2,false);cf_edge(f,2,3,false);cf_edge(f,4,2,false);
    } else if (mode==4) {
        cf_edge(f,2,0,false);cf_edge(f,2,0,true);
    } else {
        CHECK(mode==5);cf_edge(f,0,0,false);cf_edge(f,0,1,false);
    }
    if (f->graph.error_edge_count) {
        f->graph.error_heads=f->error_heads;f->graph.error_edges=f->error_edges;
    }
    memset(f->queue,0xa5,sizeof(f->queue));memset(f->queued,0xa5,sizeof(f->queued));
}
static void cf_order_golden(const CalleeOrderFixture *f,unsigned mode) {
    static const uint32_t orders[6][5]={{1,3,2,0,0},{1,3,2,0,0},{2,1,0,0,0},
        {1,4,0,2,3},{1,2,0,0,0},{2,0,1,0,0}};
    for (uint32_t i=0;i<f->effects.count;++i)
        CHECK(f->graph.queue[i]==orders[mode][i] && f->graph.queued[i]==1);
}
static ErrorFlow cf_flow(CalleeOrderFixture *f,XrXirCompileContext *c,bool reuse) {
    ErrorFlow flow={0};flow.module=&f->module;flow.effects=&f->effects;flow.remaining=c;
    flow.escaping=f->summary;flow.function=(const XrXirFunction *)f;
    flow.stride=77;flow.values=99;flow.cell_count=3;flow.active_count=5;
    flow.changed=true;flow.summary_changed=true;flow.restart=true;
    if (reuse) {
        XrXirStatus status=XR_XIR_OK;
        flow.storage=xir_compile_calloc(c,64,1,&status);CHECK(status==XR_XIR_OK && flow.storage);
        flow.storage_capacity=64;memset(flow.storage,0x7b,64);
        flow.states=flow.work=flow.edge=flow.snapshot=(uint64_t *)flow.storage;
        flow.reachable=flow.storage;
        flow.roots=flow.cells=flow.slots=flow.active=(uint32_t *)flow.storage;
        flow.zero=(const uint64_t *)flow.storage;
    }
    return flow;
}
static void cf_preserved(const CalleeOrderFixture *f,const ErrorFlow *flow,bool reuse) {
    for (uint32_t i=0;i<10;++i) CHECK(f->summary[i]==UINT64_C(0x9182736455aa6600)+i);
    CHECK(flow->escaping==f->summary && flow->function==(const XrXirFunction *)f);
    CHECK(flow->stride==77 && flow->values==99 && flow->cell_count==3 && flow->active_count==5);
    CHECK(flow->changed && flow->summary_changed && flow->restart);
    if (reuse) {
        CHECK(!flow->states && !flow->work && !flow->edge && !flow->snapshot && !flow->reachable);
        CHECK(!flow->roots && !flow->cells && !flow->slots && !flow->active && !flow->zero);
        CHECK(flow->storage_capacity==64);
        pc_pattern(flow->storage+(size_t)f->effects.count*4,64-(size_t)f->effects.count*4,0x7b);
    } else CHECK(!flow->storage && !flow->storage_capacity);
}
static void cf_order_cases(void) {
    static const uint64_t fresh[6]={57,61,51,65,51,37},reuse_work[6]={66,70,60,74,60,46};
    for (unsigned mode=0;mode<6;++mode) for (unsigned reuse=0;reuse<2;++reuse) {
        CalleeOrderFixture f;cf_order_fixture(&f,mode);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        ErrorFlow flow=cf_flow(&f,&c,reuse!=0);uint64_t before=pc_stats(&c).work;size_t count=attempts;
        CHECK(error_queue_prepare(&f.graph,&flow)==XR_XIR_OK);cf_order_golden(&f,mode);
        CHECK(pc_stats(&c).work-before==(reuse ? reuse_work[mode] : fresh[mode]));
        CHECK(attempts-count==(reuse ? 0u : 1u));cf_preserved(&f,&flow,reuse!=0);
        error_storage_free(&flow);pc_release(&c,baseline);
    }
    puts("callee-first DAG/duplicate/GO/cycle/cross-list/self-cycle literal order and actual fees");
}
static void cf_order_budget(void) {
    for (unsigned reuse=0;reuse<2;++reuse) for (unsigned enough=0;enough<2;++enough) {
        CalleeOrderFixture f;cf_order_fixture(&f,0);
        XrCompileResourceLimits caps=pc_caps();XrXirCompileContext c=pc_owner(caps);
        uint64_t baseline=pc_stats(&c).live_bytes;ErrorFlow flow=cf_flow(&f,&c,reuse!=0);
        uint64_t available=(reuse ? 66u : 57u)-(enough ? 0u : 1u),used=pc_stats(&c).work;
        CHECK(xr_compile_resources_work(c.resources,caps.work-used-available)==XR_COMPILE_RESOURCE_OK);
        uint64_t before=pc_stats(&c).work;size_t count=attempts;
        CHECK(error_queue_prepare(&f.graph,&flow)==(enough ? XR_XIR_OK : XR_XIR_BUDGET));
        CHECK(pc_stats(&c).work-before==available && attempts-count==(reuse ? 0u : 1u));
        cf_preserved(&f,&flow,reuse!=0);if (enough) cf_order_golden(&f,0);
        error_storage_free(&flow);pc_release(&c,baseline);
    }
    CalleeOrderFixture f;cf_order_fixture(&f,0);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    ErrorFlow flow=cf_flow(&f,&c,false);uint64_t before=pc_stats(&c).work;
    attempts=0;injected=false;fail_at=0;
    CHECK(error_queue_prepare(&f.graph,&flow)==XR_XIR_OUT_OF_MEMORY && injected && attempts==1);
    fail_at=SIZE_MAX;CHECK(pc_stats(&c).work-before==1 && pc_stats(&c).live_bytes==baseline);
    cf_preserved(&f,&flow,false);pc_pattern(f.queue,sizeof(f.queue),0xa5);
    CHECK(error_queue_prepare(&f.graph,&flow)==XR_XIR_OK && pc_stats(&c).work-before==58);
    cf_order_golden(&f,0);cf_preserved(&f,&flow,false);pc_release(&c,baseline);
}
static void cf_order_invalid(void) {
    for (unsigned mode=0;mode<5;++mode) {
        CalleeOrderFixture f;cf_order_fixture(&f,0);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        ErrorFlow flow=cf_flow(&f,&c,false);
        if (mode==0) f.edges[0].caller=4;
        else if (mode==1) f.heads[2]=2;
        else if (mode==2) f.edges[0].next=0;
        else if (mode==3) f.graph.edge_count=3;
        else f.graph.edges=NULL;
        CHECK(error_queue_prepare(&f.graph,&flow)==XR_XIR_BAD_STRUCTURE);
        cf_preserved(&f,&flow,false);pc_release(&c,baseline);
    }
}
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
    CHECK(e && e->count==ED_COUNT && e->atom_count==2 && e->words==1);
    for (uint32_t f=0;f<ED_COUNT;++f) {
        uint64_t bits=f<2 ? 0u : mode==CF_UNKNOWN && f==2 ? 1u : 4u;
        CHECK(e->errors[f]==bits);
        CHECK(e->functions[f].throws==(bits==1 ? XR_XIR_EFFECT_UNKNOWN : bits ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE));
        CHECK(e->task_errors[f]==(bits==1 ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        CHECK(e->root[f].requires_root==(f==0) && e->root[f].unresolved==(mode==CF_UNKNOWN && f==2));
        CHECK(e->functions[f].suspend==(f==2 && mode==CF_GO ? XR_XIR_EFFECT_MAY :
            f==2 && mode==CF_UNKNOWN ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE));
    }
}
static XrXirStatus cf_whole(const XrXirCompileContext *c,CalleeWholeMode mode,bool reverse) {
    CalleeWholeFixture whole;cf_whole_fixture(&whole,mode,reverse);
    XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(c, &whole.dependency.module, &checked, &d);
    uint64_t before=pc_stats(c).work;
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&e);
    if (status==XR_XIR_OK) {
        cf_whole_golden(e,mode);CHECK(pc_stats(c).work-before<UINT64_C(400000));
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
    cf_order_cases();cf_order_budget();cf_order_invalid();cf_whole_cases();
    cf_whole_faults();cf_whole_limits();cf_occupied();
}
#endif // XIR_ERROR_CALLEE_FIRST_CONTROLS_H
