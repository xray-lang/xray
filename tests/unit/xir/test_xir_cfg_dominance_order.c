/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cfg_dominance_order.c - Independent normal and fault dominance
 *
 * KEY CONCEPT:
 *   A Boolean full-scan oracle reads literal edges, never the production
 *   queue, rank or bitset. Current dirty scratch and refused work must not
 *   retain facts or permissions for the next graph on the same owner.
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_compile_memory.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)

static size_t od_work_calls, od_refuse = SIZE_MAX;
static bool od_refused;
static bool od_work(const XrXirCompileContext *context,uint64_t work) {
    if (od_work_calls++==od_refuse) { od_refused=true;return false; }
    return xir_compile_work(context,work);
}
#define xir_compile_work(context,work) od_work(context,work)
#include "xir/xxir_verify.c"
#undef xir_compile_work

enum { OD_MAX=129, OD_EDGES=3*OD_MAX };
typedef struct OdEdge { uint32_t from,to;bool fault; } OdEdge;
typedef struct OdFixture {
    uint32_t count,edge_count,map[OD_MAX];
    OdEdge edges[OD_EDGES];
    XrXirBlock blocks[OD_MAX];
    XrXirInstruction instructions[OD_MAX];
    XrXirFunction function;
} OdFixture;

static void od_edge(OdFixture *f,uint32_t from,uint32_t to,bool fault) {
    CHECK(f->edge_count<OD_EDGES && from<f->count && to<f->count);
    f->edges[f->edge_count++]=(OdEdge){from,to,fault};
}
static void od_fixture(OdFixture *f,uint32_t count,unsigned kind,unsigned order) {
    CHECK(count && count<=OD_MAX && (count>=6 || !kind));
    memset(f,0,sizeof(*f));f->count=count;
    for (uint32_t b=0;b<count;++b) f->map[b]=b;
    if (order==1) for (uint32_t b=1;b<count;++b) f->map[b]=count-b;
    if (order==2) {
        uint32_t seed=UINT32_C(0x3f2a19b7);
        for (uint32_t b=count-1;b>1;--b) {
            seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
            uint32_t at=1+seed%b,value=f->map[at];f->map[at]=f->map[b];f->map[b]=value;
        }
    }
    for (uint32_t b=0;b+1<count;++b) {
        if (kind==3 && b<4) continue;
        od_edge(f,b,b+1,false);
    }
    if (kind==1) od_edge(f,0,count-2,false);
    if (kind==2) od_edge(f,count-2,1,false);
    if (kind==3) {
        od_edge(f,0,1,false);od_edge(f,0,2,false);od_edge(f,1,3,false);
        od_edge(f,2,3,false);od_edge(f,2,4,false);
        od_edge(f,3,1,false);od_edge(f,3,2,false);
    }
    if (kind==4) { od_edge(f,1,count-1,true);od_edge(f,count/2,count/2,true); }
    if (kind==5) od_edge(f,count/2,count/2,false);
    for (uint32_t b=0;b<count;++b) {
        uint32_t physical=f->map[b],normal=0;
        XrXirInstruction *op=&f->instructions[physical];
        f->blocks[physical]=(XrXirBlock){.first=physical,.count=1};
        for (uint32_t e=0;e<f->edge_count;++e) if (f->edges[e].from==b) {
            OdEdge edge=f->edges[e];
            if (edge.fault) { CHECK(!f->blocks[physical].panic);f->blocks[physical].panic=f->map[edge.to]; }
            else { CHECK(normal<2);op->targets[normal++]=f->map[edge.to]; }
        }
        op->op=normal==2?XR_XIR_BRANCH:normal?XR_XIR_JUMP:XR_XIR_RETURN;
    }
    f->function=(XrXirFunction){.blocks=f->blocks,.block_count=count,
        .instructions=f->instructions,.instruction_count=count};
}

/* The oracle's individual Boolean facts and fixed physical scan deliberately
 * share neither production ordering nor bitset transfer implementation. */
static void od_expected(const OdFixture *f,bool expected[OD_MAX][OD_MAX]) {
    memset(expected,0,OD_MAX*OD_MAX*sizeof(bool));
    expected[0][0]=true;
    for (uint32_t b=1;b<f->count;++b)
        for (uint32_t d=0;d<f->count;++d) expected[b][d]=true;
    bool changed;
    do {
        changed=false;
        for (uint32_t b=1;b<f->count;++b) for (uint32_t d=0;d<f->count;++d) {
            bool value=true,found=false;
            for (uint32_t e=0;e<f->edge_count;++e) {
                OdEdge edge=f->edges[e];if (edge.to!=b) continue;
                found=true;
                if (!expected[edge.from][d] || (edge.fault && edge.from==d)) value=false;
            }
            CHECK(found);if (b==d) value=true;
            if (value!=expected[b][d]) { expected[b][d]=value;changed=true; }
        }
    } while (changed);
}
static void od_compare(const OdFixture *f,const Graph *graph,unsigned kind) {
    bool expected[OD_MAX][OD_MAX];od_expected(f,expected);
    for (uint32_t b=0;b<f->count;++b) for (uint32_t d=0;d<graph->words*64;++d) {
        uint32_t physical=d<f->count?f->map[d]:d;
        uint64_t word=graph->dominators[(size_t)f->map[b]*graph->words+physical/64];
        CHECK(((word>>(physical%64))&1)==(d<f->count && expected[b][d]));
    }
    bool seen[OD_MAX]={false};uint32_t rank[OD_MAX]={0};
    CHECK(!graph->queue[0]);
    for (uint32_t i=0;i<f->count;++i) {
        uint32_t b=graph->queue[i];CHECK(b<f->count && !seen[b]);seen[b]=true;rank[b]=i;
    }
    if (kind<2) for (uint32_t e=0;e<f->edge_count;++e)
        CHECK(rank[f->map[f->edges[e].from]]<rank[f->map[f->edges[e].to]]);
}
static XrCompileResourceLimits od_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(8388608),UINT64_C(4194304),UINT64_C(4000000)};
}
static XrCompileResourceStats od_stats(const VerifyContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->remaining.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats;
}
static VerifyContext od_owner(XrCompileResourceLimits caps) {
    VerifyContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.remaining.resources)==XR_COMPILE_RESOURCE_OK);
    return context;
}
static XrXirStatus od_solve(VerifyContext *context,const OdFixture *f,Graph *graph) {
    XrXirStatus status=graph_allocate(graph,&f->function,context);
    if (status==XR_XIR_OK) status=graph_connect(graph,&f->function,context);
    if (status==XR_XIR_OK) status=graph_dominators(graph,&f->function,context);
    return status;
}
static void od_free(VerifyContext *context,uint64_t baseline) {
    xr_compile_resources_free(context->graph_storage);context->graph_storage=NULL;context->graph_capacity=0;
    CHECK(od_stats(context).live_bytes==baseline);xr_compile_resources_release(context->remaining.resources);
    *context=(VerifyContext){0};
}
static void od_cases(void) {
    static const uint32_t sizes[]={1,2,6,63,64,65,127,128,129,6};
    unsigned cases=0;
    for (unsigned size=0;size<sizeof(sizes)/sizeof(*sizes);++size) {
        VerifyContext context=od_owner(od_caps());uint64_t baseline=od_stats(&context).live_bytes;
        for (unsigned kind=0;kind<(sizes[size]<6?1u:6u);++kind) for (unsigned order=0;order<3;++order) {
            OdFixture f;od_fixture(&f,sizes[size],kind,order);Graph graph={0};
            if (context.graph_storage) memset(context.graph_storage,0xa5,context.graph_capacity);
            CHECK(od_solve(&context,&f,&graph)==XR_XIR_OK);od_compare(&f,&graph,kind);++cases;
        }
        /* A narrower graph must overwrite all current rows in wider dirty storage. */
        OdFixture narrow;od_fixture(&narrow,2,0,0);Graph graph={0};
        memset(context.graph_storage,0xa5,context.graph_capacity);
        CHECK(od_solve(&context,&narrow,&graph)==XR_XIR_OK);od_compare(&narrow,&graph,0);
        od_free(&context,baseline);
    }
    printf("%u literal graph permutations, Boolean normal/fault dominance and dirty wide/narrow reuse PASS\n",cases);
}
static XrCompileResourceStats od_budget(XrCompileResourceLimits caps,XrXirStatus expected) {
    VerifyContext context=od_owner(caps);uint64_t baseline=od_stats(&context).live_bytes;
    OdFixture f;od_fixture(&f,65,3,2);Graph graph={0};
    CHECK(od_solve(&context,&f,&graph)==expected);
    if (expected==XR_XIR_OK) od_compare(&f,&graph,3);
    XrCompileResourceStats stats=od_stats(&context);od_free(&context,baseline);return stats;
}
static void od_boundaries(void) {
    XrCompileResourceStats first=od_budget(od_caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={first.allocated_bytes,first.peak_bytes,first.work};
    XrCompileResourceStats again=od_budget(exact,XR_XIR_OK);
    CHECK(first.allocated_bytes==again.allocated_bytes && first.peak_bytes==again.peak_bytes && first.work==again.work);
    CHECK(exact.allocated_bytes && exact.live_bytes && exact.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)od_budget(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)od_budget(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)od_budget(less,XR_XIR_BUDGET);
    puts("private complete graph exact/minus-one allocation, live and work axes PASS");
}
static void od_refusals(void) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        VerifyContext context=od_owner(od_caps());uint64_t baseline=od_stats(&context).live_bytes;
        OdFixture f;od_fixture(&f,6,3,2);Graph graph={0};
        CHECK(graph_allocate(&graph,&f.function,&context)==XR_XIR_OK);
        CHECK(graph_connect(&graph,&f.function,&context)==XR_XIR_OK);
        uint64_t before=od_stats(&context).work;
        od_work_calls=0;od_refuse=pass?pass-1:SIZE_MAX;od_refused=false;
        XrXirStatus status=graph_dominators(&graph,&f.function,&context);
        size_t calls=od_work_calls;od_refuse=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=calls;CHECK(sites && sites<4096); }
        else CHECK(od_refused && calls==pass && status==XR_XIR_BUDGET);
        uint64_t after=od_stats(&context).work;CHECK(after>=before);
        memset(context.graph_storage,0xa5,context.graph_capacity);graph=(Graph){0};
        CHECK(od_solve(&context,&f,&graph)==XR_XIR_OK);od_compare(&f,&graph,3);
        CHECK(od_stats(&context).work>after);od_free(&context,baseline);
    }
    printf("%zu synthetic work callback refusals, same-owner cumulative rebuild PASS\n",sites);
}
int main(void) {
    od_cases();od_boundaries();od_refusals();return 0;
}
