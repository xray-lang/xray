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
#include "test_xir_error_cfg_pending.c"
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
        f->blocks[physical]=(XrXirBlock){.first=first,.count=at-first};
    }
    f->base.functions[1].parameters=&f->parameter;f->base.functions[1].parameter_count=1;
    f->base.functions[1].blocks=f->blocks;f->base.functions[1].block_count=graph->count;
    f->base.functions[1].instructions=f->instructions;f->base.functions[1].instruction_count=at;
    f->base.functions[0].error_type=XR_XIR_NEVER;
    f->base.functions[1].error_type=(XrXirType)EE_ENUM;
    f->base.functions[2].error_type=(XrXirType)EE_ENUM;
    f->base.module.stage=XR_XIR_BUILT;
}
static XrXirStatus sg_whole(const XrXirCompileContext *context,SgFixture *f) {
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&f->base.module,&checked,&diagnostic);
    if(status==XR_XIR_OK)
        status=xr_xir_compile_effects_infer_verified(context,xr_xir_compile_artifact_module(checked),&effects);
    if(status==XR_XIR_OK) CHECK(effects && xr_xir_effects_read(effects,1).status==XR_XIR_OK);
    if(status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"graph status%u diagnostic%u f%u b%u i%u\n",(unsigned)status,(unsigned)diagnostic.reason,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);return status;
}
static void sg_case(uint32_t blocks,unsigned kind,unsigned order,uint32_t width) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,blocks,kind);sg_fixture(&fixture,&graph,order,width);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    CHECK(sg_whole(&context,&fixture)==XR_XIR_OK);pc_release(&context,baseline);
}
static void sg_rejection(void) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,6,0);sg_fixture(&fixture,&graph,0,2);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
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
    pc_release(&context,baseline);puts("target/type rejection f1/b0/i2 and f1/b0/i0, same-owner recovery PASS");
}

static void sg_resources(void) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,16,1);sg_fixture(&fixture,&graph,2,130);
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=sg_whole(&context,&fixture);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites);}
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&context).live_bytes==baseline);
        CHECK(sg_whole(&context,&fixture)==XR_XIR_OK);pc_release(&context,baseline);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    if(argc==2) {sg_resources();CHECK(!live && !live_bytes);return 0;}
    static const uint32_t sizes[]={16,32,64,128,256};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i)
        for(unsigned kind=0;kind<2;++kind)for(unsigned order=0;order<3;++order)sg_case(sizes[i],kind,order,2);
    for(unsigned kind=0;kind<3;++kind)for(unsigned order=0;order<3;++order)sg_case(6,kind,order,2);
    static const uint32_t widths[]={61,62,63,64,65,127,128,129,130};
    for(unsigned i=0;i<sizeof(widths)/sizeof(*widths);++i)sg_case(6,2,2,widths[i]);
    sg_rejection();CHECK(!live && !live_bytes);puts("common CFG topology, permutations and owned allocation rollback passed");return 0;
}
