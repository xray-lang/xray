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
/* This full scan has no production transfer, queue, rank, or summary reads. */
static uint32_t sg_expected(const SgGraph *graph) {
    bool reachable[SG_BLOCKS]={false};reachable[0]=true;
    bool changed;
    do {
        changed=false;
        for(uint32_t b=0;b<graph->count;++b)if(reachable[b])
            for(unsigned e=0;e<2;++e) {
                uint32_t to=graph->edges[b][e];
                if(to==SG_NONE)continue;
                CHECK(to<graph->count);
                if(!reachable[to]){reachable[to]=true;changed=true;}
            }
    } while(changed);
    uint32_t result=0;
    for(uint32_t b=0;b<graph->count;++b)
        if(reachable[b] && graph->throws[b]!=SG_NONE) {
            CHECK(graph->throws[b]<2);result|=1u<<graph->throws[b];
        }
    return result;
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
    f->base.module.stage=XR_XIR_BUILT;
}
static XrXirStatus sg_whole(const XrXirCompileContext *context,SgFixture *f,uint32_t expected,uint32_t width) {
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&f->base.module,&checked,&diagnostic);
    if(status==XR_XIR_OK)
        status=xr_xir_compile_effects_infer_verified(context,xr_xir_compile_artifact_module(checked),&effects);
    if(status==XR_XIR_OK) {
        CHECK(effects && !xr_xir_effects_error_unknown(effects,1) && !xr_xir_effects_error_unidentified(effects,1));
        for(uint32_t a=0;a<width;++a) {
            bool present=(a==0 && (expected&1)) || (a==width-1 && (expected&2));
            CHECK(xr_xir_effects_error(effects,1,(XrXirType)EE_ENUM,a)==present);
        }
    }
    if(status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"graph status%u diagnostic%u f%u b%u i%u\n",(unsigned)status,(unsigned)diagnostic.reason,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);return status;
}
static uint64_t sg_run(uint32_t blocks,unsigned kind,unsigned order,uint32_t width) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,blocks,kind);
    uint32_t expected=sg_expected(&graph);sg_fixture(&fixture,&graph,order,width);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    clock_t start=clock();CHECK(sg_whole(&context,&fixture,expected,width)==XR_XIR_OK);
    double seconds=(double)(clock()-start)/CLOCKS_PER_SEC;
    XrCompileResourceStats stats=pc_stats(&context);
    printf("{\"blocks\":%u,\"kind\":%u,\"order\":%u,\"atoms\":%u,\"expected\":%u,"
        "\"work\":%llu,\"allocated\":%llu,\"peak\":%llu,\"seconds\":%.6f}\n",
        blocks,kind,order,width,expected,(unsigned long long)stats.work,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,seconds);
    CHECK(pc_stats(&context).live_bytes==baseline);pc_release(&context,baseline);return stats.work;
}
static void sg_rejection(void) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,6,0);sg_fixture(&fixture,&graph,0,2);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    CHECK(sg_whole(&context,&fixture,1,2)==XR_XIR_OK);
    uint32_t target=fixture.instructions[2].targets[0];fixture.instructions[2].targets[0]=UINT32_MAX;
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic)==XR_XIR_BAD_STRUCTURE && !checked);
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE && diagnostic.function==1 &&
        diagnostic.block==0 && diagnostic.instruction==2);
    fixture.instructions[2].targets[0]=target;
    CHECK(sg_whole(&context,&fixture,1,2)==XR_XIR_OK);
    fixture.instructions[0].type=XR_XIR_I64;diagnostic=(XrXirDiagnostic){0};
    CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic)==XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.status==XR_XIR_BAD_TYPE && diagnostic.function==1 &&
        diagnostic.block==0 && diagnostic.instruction==0);
    sg_fixture(&fixture,&graph,0,2);CHECK(sg_whole(&context,&fixture,1,2)==XR_XIR_OK);
    pc_release(&context,baseline);puts("target/type rejection f1/b0/i2 and f1/b0/i0, same-owner recovery PASS");
}
static void sg_singleton(void) {
    for(unsigned cycle=0;cycle<2;++cycle) {
        SgGraph graph;memset(&graph,0xff,sizeof(graph));graph.count=1;
        if(cycle){graph.edges[0][0]=0;graph.edges[0][1]=0;}
        else graph.throws[0]=1;
        SgFixture fixture;sg_fixture(&fixture,&graph,0,65);
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        if(cycle) {
            XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
            CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic)==XR_XIR_BAD_STRUCTURE && !checked);
            CHECK(diagnostic.function==1 && diagnostic.block==0 && diagnostic.instruction==2);
            graph.edges[0][0]=graph.edges[0][1]=SG_NONE;graph.throws[0]=1;
            sg_fixture(&fixture,&graph,0,65);
        }
        CHECK(sg_whole(&context,&fixture,sg_expected(&graph),65)==XR_XIR_OK);
        fixture.base.module.stage=XR_XIR_CHECKED;
        ErrorFlow flow={0};flow.remaining=&context;
        XrXirEffects effects={0};effects.count=fixture.base.module.function_count;
        EffectTerms terms={0};terms.remaining=&context;terms.types=fixture.base.types;
        flow.body=effect_body_raw_request(&context,&fixture.base.module);flow.effects=&effects;flow.terms=&terms;
        CHECK(effect_errors_seed(&flow.body.domain,&effects,&context)==XR_XIR_OK);
        CHECK(error_function(&flow,1)==XR_XIR_OK && !flow.queue && !flow.pending && !flow.scheduling);
        flow.scheduling=true;flow.reachable[0]=ERROR_BLOCK_REACHABLE;
        CHECK(error_cfg_enqueue(&flow,0)==XR_XIR_OK && (flow.reachable[0]&ERROR_BLOCK_PENDING));
        CHECK(error_cfg_enqueue(&flow,0)==XR_XIR_OK && !flow.pending && !flow.queue);
        flow.reachable[0]&=(uint8_t)~ERROR_BLOCK_PENDING;
        CHECK(error_cfg_enqueue(&flow,0)==XR_XIR_OK && (flow.reachable[0]&ERROR_BLOCK_PENDING));
        flow.scheduling=false;
        error_storage_free(&flow);effect_terms_free(&terms);
        xr_compile_resources_free(effects.outcomes_work.errors);xr_compile_resources_free(effects.outcomes_work.atoms);
        pc_release(&context,baseline);
    }
    puts("singleton throw/no queue, duplicate/reschedule and precise entry-backedge rejection PASS");
}
static void sg_fresh_widths(void) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,6,2);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    ErrorFlow flow={0};flow.remaining=&context;
    static const uint32_t widths[]={2,61,62,63,64,65,127,128,129,130,2};
    for(unsigned i=0;i<sizeof(widths)/sizeof(*widths);++i) {
        uint32_t width=widths[i];sg_fixture(&fixture,&graph,i%3,width);fixture.base.module.stage=XR_XIR_CHECKED;
        CHECK(xir_fixture_verify(&context,&fixture.base.module,NULL)==XR_XIR_OK);
        XrXirEffects effects={0};effects.count=fixture.base.module.function_count;
        effects.outcomes_work.source_type_count=fixture.base.types.count;
        EffectTerms terms={0};terms.remaining=&context;terms.types=fixture.base.types;
        flow.body=effect_body_raw_request(&context,&fixture.base.module);
        flow.effects=&effects;flow.terms=&terms;
        CHECK(effect_errors_seed(&flow.body.domain,&effects,&context)==XR_XIR_OK);
        if(flow.storage)memset(flow.storage,0xa5,flow.storage_capacity);
        CHECK(error_function(&flow,1)==XR_XIR_OK && !flow.scheduling && !flow.pending);
        CHECK(((uintptr_t)flow.queue%sizeof(uint32_t))==0);
        /* Construction probes inspect private rows; public reads require a published Final. */
        const EffectFinalOutcomes *facts=&effects.outcomes_work;
        CHECK(!effect_final_outcomes(&effects) && facts->errors && facts->atoms &&
            facts->atom_count==width && facts->error_rows==effects.count &&
            facts->words==((uint64_t)width+65)/64);
        const uint64_t *row=facts->errors+facts->words;
        CHECK(!error_bit(row,0) && !error_bit(row,1));
        bool seen[130]={false};CHECK(width<=sizeof(seen)/sizeof(seen[0]));
        for(uint32_t a=0;a<facts->atom_count;++a){
            EffectErrorAtom atom=facts->atoms[a];
            CHECK(atom.type==(XrXirType)EE_ENUM && atom.variant<width && !seen[atom.variant]);
            seen[atom.variant]=true;
            CHECK(error_bit(row,a+2)==(atom.variant==0 || atom.variant==width-1));
        }
        CHECK(!xr_xir_effects_error(&effects,1,(XrXirType)EE_ENUM,0));
        effect_terms_free(&terms);xr_compile_resources_free(effects.outcomes_work.errors);xr_compile_resources_free(effects.outcomes_work.atoms);
    }
    error_storage_free(&flow);CHECK(!flow.queue && !flow.scheduling && !flow.pending);
    pc_release(&context,baseline);puts("real atom widths grow/shrink on poisoned same-owner scratch PASS");
}
static XrCompileResourceStats sg_resource_run(XrCompileResourceLimits caps,XrXirStatus expected) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,16,1);sg_fixture(&fixture,&graph,2,130);
    XrXirCompileContext context=pc_owner(caps);uint64_t baseline=pc_stats(&context).live_bytes;
    CHECK(sg_whole(&context,&fixture,sg_expected(&graph),130)==expected);
    XrCompileResourceStats stats=pc_stats(&context);pc_release(&context,baseline);return stats;
}
static void sg_resources(void) {
    SgFixture fixture;SgGraph graph;sg_graph(&graph,16,1);sg_fixture(&fixture,&graph,2,130);
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=sg_whole(&context,&fixture,sg_expected(&graph),130);
        size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<4096);}
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&context).live_bytes==baseline);
        CHECK(sg_whole(&context,&fixture,sg_expected(&graph),130)==XR_XIR_OK);
        pc_release(&context,baseline);
    }
    XrCompileResourceStats first=sg_resource_run(pc_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={first.allocated_bytes,first.peak_bytes,first.work};
    XrCompileResourceStats repeat=sg_resource_run(exact,XR_XIR_OK);
    CHECK(first.allocated_bytes==repeat.allocated_bytes && first.peak_bytes==repeat.peak_bytes && first.work==repeat.work);
    CHECK(exact.allocated_bytes && exact.live_bytes && exact.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)sg_resource_run(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)sg_resource_run(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)sg_resource_run(less,XR_XIR_BUDGET);
    printf("independent loop/130 atoms: %zu complete OOM sites, exact/minus-one axes, same-owner retries physical0\n",sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--scale") || !strcmp(argv[1],"--resources"))));
    if(argc==2 && !strcmp(argv[1],"--resources")){sg_resources();CHECK(!live && !live_bytes);return 0;}
    if(argc==2) {
        static const uint32_t sizes[]={16,32,64,128,256};
        uint64_t previous[2][3]={{0}};
        for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
            for(unsigned kind=0;kind<2;++kind) {
                uint64_t minimum=UINT64_MAX,maximum=0;
                for(unsigned order=0;order<3;++order) {
                    uint64_t work=sg_run(sizes[i],kind,order,2);
                    /* These fixed nonempty fixtures include complete check
                     * and inference on one owner. Bound physical-order cost
                     * and doubling growth without resetting their ledger. */
                    if(previous[kind][order]) CHECK(work<3*previous[kind][order]);
                    previous[kind][order]=work;
                    if(work<minimum) minimum=work;
                    if(work>maximum) maximum=work;
                }
                CHECK(maximum<2*minimum);
            }
        }
    } else {
        for(unsigned kind=0;kind<3;++kind)for(unsigned order=0;order<3;++order)sg_run(6,kind,order,2);
        static const uint32_t widths[]={61,62,63,64,65,127,128,129,130};
        for(unsigned i=0;i<sizeof(widths)/sizeof(*widths);++i)sg_run(6,2,2,widths[i]);
        sg_rejection();sg_singleton();sg_fresh_widths();
    }
    CHECK(!live && !live_bytes);puts("independent reachable throw sets and canonical permutations PASS");return 0;
}
