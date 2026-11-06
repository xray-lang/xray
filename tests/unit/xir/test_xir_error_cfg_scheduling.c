/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_cfg_scheduling.c - Independent graph expectations and real error costs
 *
 * KEY CONCEPT:
 *   Reachable throw sites determine literal variant sets without consulting
 *   the production lattice. Canonical block storage is independently permuted.
 */
#define main pending_aggregate_entry
#include "test_xir_root_conditional_helpers.c"
#undef main
#include <time.h>

enum { SG_BLOCKS=256, SG_ATOMS=130 };
#define SG_NONE UINT32_MAX
typedef struct SgGraph {
    uint32_t count,edges[SG_BLOCKS][2],throws[SG_BLOCKS];
} SgGraph;
typedef struct SgFixture {
    EdgeFixture base;
    XrXirBlock blocks[SG_BLOCKS];
    XrXirInstruction instructions[SG_BLOCKS+2];
    XrXirNominalVariant variants[SG_ATOMS];
    char names[SG_ATOMS][12];
    uint32_t map[SG_BLOCKS];
    XrXirType parameter;
} SgFixture;
static void sg_graph(SgGraph *graph,uint32_t count,unsigned kind) {
    CHECK(count>=6 && count<=SG_BLOCKS);
    memset(graph,0xff,sizeof(*graph));graph->count=count;
    for(uint32_t b=0;b<count-1;++b)graph->edges[b][0]=b+1;
    graph->throws[count-1]=0;
    if(kind==1)graph->edges[count-2][1]=1;
    if(kind==2) {
        graph->edges[0][0]=1;graph->edges[0][1]=2;
        graph->edges[1][0]=3;graph->edges[2][0]=3;
        graph->edges[3][0]=4;graph->edges[3][1]=5;
        graph->edges[4][0]=graph->edges[5][0]=SG_NONE;
        graph->throws[4]=0;graph->throws[5]=1;
        for(uint32_t b=6;b<count;++b)graph->edges[b][0]=3;
    }
}
static void sg_fixture(SgFixture *f,const SgGraph *graph,unsigned order,uint32_t width) {
    CHECK(width>=2 && width<=SG_ATOMS && order<3);
    memset(f,0,sizeof(*f));ee_fixture(&f->base,EE_JUMP,false);
    for(uint32_t a=0;a<width;++a) {
        int length=snprintf(f->names[a],sizeof(f->names[a]),"Variant%u",a);
        CHECK(length>0 && (size_t)length<sizeof(f->names[a]));
        f->variants[a]=(XrXirNominalVariant){{f->names[a],(uint32_t)length},0,0};
    }
    f->base.nominal.variants=f->variants;f->base.nominal.variant_count=width;
    f->parameter=XR_XIR_BOOL;
    for(uint32_t b=0;b<graph->count;++b)f->map[b]=b;
    if(order==1)for(uint32_t b=1;b<graph->count;++b)f->map[b]=graph->count-b;
    if(order==2) {
        uint32_t state=32504;
        for(uint32_t b=graph->count-1;b>1;--b) {
            state=state*UINT32_C(1664525)+UINT32_C(1013904223);
            uint32_t other=1+state%b,tmp=f->map[b];f->map[b]=f->map[other];f->map[other]=tmp;
        }
    }
    uint32_t inverse[SG_BLOCKS],at=0;
    for(uint32_t b=0;b<graph->count;++b)inverse[f->map[b]]=b;
    for(uint32_t physical=0;physical<graph->count;++physical) {
        uint32_t logical=inverse[physical],first=at;
        if(!physical) {
            f->instructions[at++]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
            f->instructions[at++]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=width-1};
        }
        if(graph->throws[logical]!=SG_NONE)
            f->instructions[at++]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1+graph->throws[logical]}};
        else if(graph->edges[logical][1]!=SG_NONE)
            f->instructions[at++]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},
                .targets={f->map[graph->edges[logical][0]],f->map[graph->edges[logical][1]]}};
        else {
            CHECK(graph->edges[logical][0]!=SG_NONE);
            f->instructions[at++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={f->map[graph->edges[logical][0]]}};
        }
        f->blocks[physical]=(XrXirBlock){.first=first,.count=at-first,
            .frontier=physical?f->blocks[0].count:0};
    }
    f->base.functions[1].parameters=&f->parameter;f->base.functions[1].parameter_count=1;
    f->base.functions[1].blocks=f->blocks;f->base.functions[1].block_count=graph->count;
    f->base.functions[1].instructions=f->instructions;f->base.functions[1].instruction_count=at;
    f->base.module.stage=XR_XIR_BUILT;
}
static XrXirStatus sg_whole(const XrXirCompileContext *context,SgFixture *fixture) {
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture->base.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK) {
        const EffectFinalOutcomes *final=effect_final_outcomes(effects);
        CHECK(final && final->functions[1].throws==XR_XIR_EFFECT_MAY && !final->functions[1].suspend);
    }
    xr_xir_compile_effects_free(effects);return status;
}
static void sg_rejection(void) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,6,0);sg_fixture(&fixture,&graph,0,2);
    XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();uint64_t baseline=rp_stats(&context).held_bytes;
    CHECK(sg_whole(&context,&fixture)==XR_XIR_OK);
    uint32_t target=fixture.instructions[2].targets[0];fixture.instructions[2].targets[0]=UINT32_MAX;
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic)==XR_XIR_BAD_STRUCTURE && !checked);
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE && diagnostic.function==1 &&
        diagnostic.block==0 && diagnostic.instruction==2);
    fixture.instructions[2].targets[0]=target;
    CHECK(sg_whole(&context,&fixture)==XR_XIR_OK);
    fixture.instructions[0].type=XR_XIR_I64;diagnostic=(XrXirDiagnostic){0};
    CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic)==XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.status==XR_XIR_BAD_TYPE && diagnostic.function==1 &&
        diagnostic.block==0 && diagnostic.instruction==0);
    sg_fixture(&fixture,&graph,0,2);CHECK(sg_whole(&context,&fixture)==XR_XIR_OK);
    rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);puts("target/type rejection f1/b0/i2 and f1/b0/i0, same-owner recovery PASS");
}
static XrCompileResourceStats sg_complete(XrCompileResourceLimits caps,XrXirStatus expected) {
    XrXirCompileContext context=rp_owner(caps);context.limits=xr_xir_compile_default_limits();context.limits=xr_xir_compile_default_limits();
    uint64_t baseline=rp_stats(&context).held_bytes;
    SgFixture fixture;SgGraph graph;sg_graph(&graph,32,1);sg_fixture(&fixture,&graph,1,65);
    CHECK(sg_whole(&context,&fixture)==expected);
    XrCompileResourceStats stats=rp_stats(&context);rp_owner_free(&context,baseline);
    CHECK(!rp_live && !rp_live_bytes);return stats;
}
static void sg_resources(void) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();context.limits=xr_xir_compile_default_limits();
        uint64_t baseline=rp_stats(&context).held_bytes;
        SgFixture fixture;SgGraph graph;sg_graph(&graph,32,1);sg_fixture(&fixture,&graph,1,65);
        rp_attempts=0;rp_injected=false;rp_fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=sg_whole(&context,&fixture);rp_fail_at=SIZE_MAX;
        if(!pass) { CHECK(status==XR_XIR_OK && rp_attempts);sites=rp_attempts; }
        else CHECK(rp_injected && rp_attempts>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(rp_stats(&context).held_bytes==baseline && sg_whole(&context,&fixture)==XR_XIR_OK);
        rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
    }
    XrCompileResourceStats full=sg_complete(rp_caps(),XR_XIR_OK);
    CHECK(full.peak_bytes>1);
    XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,full.peak_bytes,true};
    (void)sg_complete(exact,XR_XIR_OK);--exact.memory_limit;(void)sg_complete(exact,XR_XIR_MEMORY_LIMIT);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--resources")) { sg_resources();return 0; }
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--scale")));
    for(unsigned kind=0;kind<3;++kind) for(unsigned order=0;order<3;++order) {
        SgFixture fixture;SgGraph graph;sg_graph(&graph,argc==2?256:32,kind);
        sg_fixture(&fixture,&graph,order,argc==2?130:2);
        XrXirCompileContext context=rp_owner(rp_caps());context.limits=xr_xir_compile_default_limits();context.limits=xr_xir_compile_default_limits();
        uint64_t baseline=rp_stats(&context).held_bytes;CHECK(sg_whole(&context,&fixture)==XR_XIR_OK);
        rp_owner_free(&context,baseline);CHECK(!rp_live && !rp_live_bytes);
    }
    sg_rejection();return 0;
}
