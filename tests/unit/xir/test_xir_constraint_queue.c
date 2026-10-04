/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_constraint_queue.c - Independent proof queue and ownership oracles
 *
 * KEY CONCEPT:
 *   Fresh queries test queue boundaries, authentic scopes, finite resource
 *   limits and rollback of every actual observed allocation failure.
 */
#include "xir/xxir_constraint_proof_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical physical[1024];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *queue_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void queue_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && physical[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) queue_malloc(bytes)
#define xr_free(pointer) queue_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
typedef struct QueueFixture {
    XrXirTypeNode nodes[257];
    XrXirCallableParameter parameters[18];
    XrXirTypes types;
    XrXirModule module;
    XrXirFunction functions[2];
    XrXirGeneric generics[2];
    XrXirConstraint bound;
    XrXirType root;
} QueueFixture;
static XrXirType constructed(uint32_t i) { return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i); }
static void deep_fixture(QueueFixture *f,uint32_t depth) {
    CHECK(depth && depth<=256);memset(f,0,sizeof(*f));
    for (uint32_t i=0;i<depth;++i) f->nodes[i]=(XrXirTypeNode){
        .kind=XR_XIR_TYPE_NULLABLE,.element=i?constructed(i-1):XR_XIR_I64};
    f->types=(XrXirTypes){f->nodes,depth,NULL,NULL};f->root=constructed(depth-1);
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.linkage_kind=XR_XIR_LIBRARY,.types=&f->types};
}
static void wide_fixture(QueueFixture *f) {
    deep_fixture(f,17);
    /* Descending direct children queue leaf zero last; one repeated child must
     * neither add an obligation nor erase the earlier authentic one. */
    for (uint32_t i=0;i<17;++i) f->parameters[i]=(XrXirCallableParameter){constructed(16-i),0};
    f->parameters[17]=(XrXirCallableParameter){constructed(3),0};
    f->nodes[17]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_UNIT,
        .parameters=f->parameters,.parameter_count=18,.result=XR_XIR_I64};
    f->types.count=18;f->root=constructed(17);
}
static XrCompileResourceLimits caps(void) {
    return (XrCompileResourceLimits){UINT64_C(2)*1024*1024,UINT64_C(1)*1024*1024,UINT64_C(2000000)};
}
static XrXirCompileContext owner_new(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};CHECK(!live && !live_bytes);
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static void owner_free(XrXirCompileContext *context,uint64_t baseline) {
    CHECK(stats(context).live_bytes==baseline);xr_compile_resources_release(context->resources);
    *context=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static XrXirStatus public_proof(const XrXirCompileContext *context,QueueFixture *f) {
    XrXirProofContext proof={&f->module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    return xr_xir_compile_type_use_verify(context,&proof,f->root);
}
static XrXirStatus growing_scratch(const XrXirCompileContext *context,QueueFixture *large) {
    QueueFixture small;deep_fixture(&small,7);
    XrXirStatus status=xr_xir_compile_types_structure_verify(context,&small.types);
    XirConstraintScratch scratch={context->resources,NULL};
    XrXirProofContext a={&small.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XrXirProofContext b={&large->module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    if (status==XR_XIR_OK) status=xr_xir_compile_type_use_verify_scratch(context,&a,small.root,&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_use_verify_scratch(context,&b,large->root,&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_use_verify_scratch(context,&a,small.root,&scratch);
    xr_xir_constraint_scratch_free(&scratch);return status;
}
static XrCompileResourceStats measured_run(QueueFixture *f,XrCompileResourceLimits limits,XrXirStatus expected,bool growth) {
    XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
    XrXirStatus status=xr_xir_compile_types_structure_verify(&context,&f->types);
    if (status==XR_XIR_OK) status=growth?growing_scratch(&context,f):public_proof(&context,f);
    CHECK(status==expected);XrCompileResourceStats result=stats(&context);
    owner_free(&context,baseline);return result;
}
static void boundaries(void) {
    static const uint32_t depths[]={7,8,9,16,17,32,256};
    for (size_t n=0;n<sizeof(depths)/sizeof(depths[0]);++n) {
        QueueFixture f;deep_fixture(&f,depths[n]);XrXirCompileContext context=owner_new(caps());
        uint64_t baseline=stats(&context).live_bytes;
        CHECK(xr_xir_compile_types_structure_verify(&context,&f.types)==XR_XIR_OK);
        attempts=0;CHECK(public_proof(&context,&f)==XR_XIR_OK);
        CHECK(attempts==(depths[n]+7)/8);
        f.nodes[depths[n]-1].element=f.root;
        CHECK(public_proof(&context,&f)==XR_XIR_BAD_TYPE);
        owner_free(&context,baseline);
    }
    QueueFixture wide;wide_fixture(&wide);XrXirCompileContext context=owner_new(caps());
    uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&context,&wide.types)==XR_XIR_OK);
    attempts=0;CHECK(public_proof(&context,&wide)==XR_XIR_OK);CHECK(attempts==3);
    wide.nodes[0].parameter_span=1;CHECK(public_proof(&context,&wide)==XR_XIR_BAD_TYPE);
    owner_free(&context,baseline);
}
static void allocation_faults(QueueFixture *f,bool growth) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
        CHECK(xr_xir_compile_types_structure_verify(&context,&f->types)==XR_XIR_OK);
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=growth?growing_scratch(&context,f):public_proof(&context,f);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<=34); }
        else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&context,baseline);
    }
    printf("queue fresh OOM sites=%zu physical=0/0\n",sites);
}
static void exact_limits(QueueFixture *f,bool growth) {
    XrCompileResourceStats measured=measured_run(f,caps(),XR_XIR_OK,growth);
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
    XrCompileResourceStats again=measured_run(f,exact,XR_XIR_OK,growth);
    CHECK(again.allocated_bytes==measured.allocated_bytes && again.peak_bytes==measured.peak_bytes && again.work==measured.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measured_run(f,less,XR_XIR_BUDGET,growth);
    less=exact;--less.live_bytes;(void)measured_run(f,less,XR_XIR_BUDGET,growth);
    less=exact;--less.work;(void)measured_run(f,less,XR_XIR_BUDGET,growth);
}
static void scoped_reuse(void) {
    QueueFixture f;deep_fixture(&f,17);
    for (uint32_t i=0;i<17;++i) f.nodes[i].parameter_span=1;
    f.nodes[0].element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f.functions[0]=(XrXirFunction){.name="allow",.name_length=5,.result=XR_XIR_I64};
    f.functions[1]=(XrXirFunction){.name="deny",.name_length=4,.result=XR_XIR_I64};
    f.generics[0]=(XrXirGeneric){.constraints=&f.bound,.parameter_count=1};
    f.module.functions=f.functions;f.module.function_count=2;f.module.generics=f.generics;
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&context,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_generics_structure_verify(&context,&f.module)==XR_XIR_OK);
    XirConstraintScratch scratch={context.resources,NULL};
    XrXirProofContext allow={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0}},deny={&f.module,{XR_XIR_CONTEXT_FUNCTION,1,0}};
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&allow,f.root,&scratch)==XR_XIR_OK);
    uint64_t allocated=stats(&context).allocation_count;
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&deny,f.root,&scratch)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&allow,f.root,&scratch)==XR_XIR_OK);
    CHECK(stats(&context).allocation_count==allocated);
    f.nodes[16].element=f.root;
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&allow,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.nodes[16].element=constructed(15);
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&allow,f.root,&scratch)==XR_XIR_OK);
    CHECK(stats(&context).allocation_count==allocated);
    XrXirCompileContext invalid={0};CHECK(xr_xir_compile_type_use_verify_scratch(&invalid,&allow,f.root,&scratch)==XR_XIR_BAD_STRUCTURE);
    xr_xir_constraint_scratch_free(&scratch);owner_free(&context,baseline);
}

/* Eight or fewer descending nodes fit one queue block. The independent work
 * formula retains N traversal units, k zero writes and one prefix update;
 * node visits, enqueue/run and the scalar leaf contribute 3k+2 more units. */
static uint64_t prefix_work(uint32_t pool, uint32_t depth) {
    CHECK(depth && depth <= 8 && depth <= pool);
    return (uint64_t)pool + 4 * (uint64_t)depth + 3;
}
static void prefix_reuse(void) {
    QueueFixture f, other;deep_fixture(&f,17);deep_fixture(&other,9);
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&context,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&context,&other.types)==XR_XIR_OK);
    XirConstraintScratch scratch={context.resources,NULL};
    XrXirProofContext proof={&f.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    static const uint32_t roots[]={7,0,6,1,7,0};
    uint64_t allocations=0;
    for (size_t n=0;n<sizeof(roots)/sizeof(roots[0]);++n) {
        XrCompileResourceStats before=stats(&context);
        CHECK(xr_xir_compile_type_use_verify_scratch(&context,&proof,constructed(roots[n]),&scratch)==XR_XIR_OK);
        XrCompileResourceStats after=stats(&context);
        CHECK(after.work-before.work==prefix_work(17,roots[n]+1));
        if (!n) allocations=after.allocation_count;
        CHECK(after.allocation_count==allocations);
    }
    proof.module=&other.module;
    XrCompileResourceStats before=stats(&context);
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&proof,constructed(1),&scratch)==XR_XIR_OK);
    CHECK(stats(&context).work-before.work==prefix_work(9,2));
    CHECK(stats(&context).allocation_count==allocations);
    other.nodes[1].element=constructed(1);
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&proof,constructed(1),&scratch)==XR_XIR_BAD_TYPE);
    other.nodes[1].element=constructed(0);
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&proof,constructed(1),&scratch)==XR_XIR_OK);
    XirConstraintScratch wrong={NULL,NULL};
    CHECK(xr_xir_compile_type_use_verify_scratch(&context,&proof,constructed(1),&wrong)==XR_XIR_BAD_STRUCTURE);
    CHECK(!wrong.memory);
    xr_xir_constraint_scratch_free(&scratch);owner_free(&context,baseline);
}
static XrXirStatus prefix_multiple(const XrXirCompileContext *context, unsigned order, unsigned bad) {
    QueueFixture f;deep_fixture(&f,17);
    XrXirConstraint requirements[3]={{0},{0},{0}};
    f.functions[0]=(XrXirFunction){.name="formal",.name_length=6,.result=XR_XIR_UNIT};
    f.generics[0]=(XrXirGeneric){.constraints=requirements,.parameter_count=3};
    f.module.functions=f.functions;f.module.function_count=1;f.module.generics=f.generics;
    XrXirType arguments[3]={constructed(order?6:1),constructed(order?1:6),constructed(order?6:1)};
    if (bad) {
        f.nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.parameter_count=1};
        f.nodes[6].element=constructed(6);
    }
    XrXirProofContext proof={&f.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XrXirConstraintUse use={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},0,arguments,3};
    return xr_xir_compile_constraints_prove(context,&proof,&use);
}
/* Seven distinct nodes, nine constructed visits, eight enqueues/runs and one
 * empty requirement. Ascending roots perform two prefix updates, descending
 * roots one; the unchanged traversal charge is the complete 17-node pool. */
static uint64_t prefix_multiple_work(unsigned order) {
    return 17 + 7 + (order?1u:2u) + 1 + 9 + 8 + 8 + 1 + 1;
}
static void prefix_multiple_faults(void) {
    for (unsigned order=0;order<2;++order) {
        size_t sites=0;
        for (size_t pass=0;pass<=sites;++pass) {
            XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
            uint64_t work=stats(&context).work;attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=prefix_multiple(&context,order,0);size_t actual=attempts;fail_at=SIZE_MAX;
            if (!pass) {
                CHECK(status==XR_XIR_OK && stats(&context).work-work==prefix_multiple_work(order));
                sites=actual;CHECK(sites==1);
            } else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
            owner_free(&context,baseline);
        }
        XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
        CHECK(prefix_multiple(&context,order,1)==(order?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE));
        CHECK(prefix_multiple(&context,order,0)==XR_XIR_OK);
        owner_free(&context,baseline);
    }
}
static void prefix_multiple_priors(void) {
    QueueFixture f;deep_fixture(&f,17);XrXirConstraint requirements[3]={{0},{0},{0}};
    f.functions[0]=(XrXirFunction){.name="formal",.name_length=6,.result=XR_XIR_UNIT};
    f.generics[0]=(XrXirGeneric){.constraints=requirements,.parameter_count=3};
    f.module.functions=f.functions;f.module.function_count=1;f.module.generics=f.generics;
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&context,&f.types)==XR_XIR_OK);
    CHECK(xr_xir_compile_generics_structure_verify(&context,&f.module)==XR_XIR_OK);
    owner_free(&context,baseline);
}
static void prefix_multiple_limits(void) {
    XrXirCompileContext baseline_owner=owner_new(caps());uint64_t baseline=stats(&baseline_owner).live_bytes;
    CHECK(prefix_multiple(&baseline_owner,0,0)==XR_XIR_OK);
    XrCompileResourceStats measured=stats(&baseline_owner);owner_free(&baseline_owner,baseline);
    CHECK(measured.work==1+prefix_multiple_work(0));
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
    for (unsigned metric=0;metric<4;++metric) {
        XrCompileResourceLimits limit=exact;
        if (metric==1) --limit.allocated_bytes;
        if (metric==2) --limit.live_bytes;
        if (metric==3) --limit.work;
        XrXirCompileContext context=owner_new(limit);baseline=stats(&context).live_bytes;
        CHECK(prefix_multiple(&context,0,0)==(metric?XR_XIR_BUDGET:XR_XIR_OK));
        owner_free(&context,baseline);
    }
}
static void prefix_work_cuts(void) {
    QueueFixture f;deep_fixture(&f,17);f.root=constructed(6);
    XrXirCompileContext prior=owner_new(caps());uint64_t baseline=stats(&prior).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&prior,&f.types)==XR_XIR_OK);owner_free(&prior,baseline);
    for (unsigned kind=0;kind<3;++kind) {
        uint64_t expected=kind?prefix_multiple_work(kind-1):prefix_work(17,7);
        for (uint64_t cut=0;cut<=expected;++cut) {
            XrCompileResourceLimits limits=caps();limits.work=1+cut;
            XrXirCompileContext context=owner_new(limits);baseline=stats(&context).live_bytes;
            XrXirStatus status=kind?prefix_multiple(&context,kind-1,0):public_proof(&context,&f);
            CHECK(status==(cut==expected?XR_XIR_OK:XR_XIR_BUDGET));
            CHECK(stats(&context).work<=1+cut);
            owner_free(&context,baseline);
        }
    }
}

int main(void) {
    prefix_multiple_priors();prefix_reuse();prefix_multiple_faults();prefix_multiple_limits();prefix_work_cuts();
    boundaries();QueueFixture deep,wide;deep_fixture(&deep,256);wide_fixture(&wide);
    allocation_faults(&deep,false);allocation_faults(&wide,false);allocation_faults(&deep,true);
    exact_limits(&deep,false);exact_limits(&wide,false);exact_limits(&deep,true);scoped_reuse();
    puts("constraint queue deep/wide boundaries, fresh OOM, exact/minus1 and current-scope scratch PASS");return 0;
}
