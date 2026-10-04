/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_type_match_scratch.c - Fresh substitution and active stack leases
 *
 * KEY CONCEPT:
 *   Independent pool and child oracles exercise the real compiler allocator.
 */
#include "xir/xxir_type_match_internal.h"
#include "xir/xxir_generic.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
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

static XrCompileResourceLimits caps(void) { return (XrCompileResourceLimits){2097152,1048576,2000000}; }
static XrXirCompileContext owner_new(XrCompileResourceLimits limits) {
    CHECK(!live && !live_bytes);XrXirCompileContext context={0};
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
static XrXirType constructed(uint32_t i) { return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i); }
static XrXirType parameter(void) { return (XrXirType)XR_XIR_TYPE_PARAMETER_BASE; }
typedef struct MatchFixture {
    XrXirTypeNode small_source[7], small_target[7], large_source[160], large_target[160];
    XrXirTypes small_from, small_to, large_from, large_to;
    XrXirTypeNode nested_source[2], nested_target[3];
    XrXirCallableParameter source_parameters[2], target_parameters[2];
    XrXirTypes nested_from, nested_to;
} MatchFixture;
static void chain(XrXirTypeNode *nodes,uint32_t count,XrXirType leaf) {
    for (uint32_t i=0;i<count;++i) nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,
        .element=i?constructed(i-1):leaf,.parameter_span=leaf==parameter()?1:0};
}
static void fixture(MatchFixture *f) {
    memset(f,0,sizeof(*f));
    chain(f->small_source,7,parameter());chain(f->small_target,7,XR_XIR_STRING);
    chain(f->large_source,160,parameter());chain(f->large_target,160,XR_XIR_STRING);
    f->small_from=(XrXirTypes){f->small_source,7,NULL,NULL};f->small_to=(XrXirTypes){f->small_target,7,NULL,NULL};
    f->large_from=(XrXirTypes){f->large_source,160,NULL,NULL};f->large_to=(XrXirTypes){f->large_target,160,NULL,NULL};
    f->source_parameters[0]=(XrXirCallableParameter){XR_XIR_I64,0};
    f->source_parameters[1]=(XrXirCallableParameter){XR_XIR_STRING,0};
    memcpy(f->target_parameters,f->source_parameters,sizeof(f->target_parameters));
    f->nested_source[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=parameter(),.parameter_span=1};
    f->nested_source[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=f->source_parameters,
        .parameter_count=2,.result=parameter(),.parameter_span=1};
    f->nested_target[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING};
    f->nested_target[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nested_target[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=f->target_parameters,
        .parameter_count=2,.result=constructed(1)};
    f->nested_from=(XrXirTypes){f->nested_source,2,NULL,NULL};f->nested_to=(XrXirTypes){f->nested_target,3,NULL,NULL};
}
static XrXirStatus nested(const XrXirCompileContext *c,MatchFixture *f,XrXirTypeMatchScratch *scratch) {
    XrXirType argument=constructed(0);
    return xr_xir_compile_type_substitution_matches_between_scratch(c,&f->nested_from,&f->nested_to,
        &argument,1,constructed(1),constructed(2),scratch);
}
static XrXirStatus sequence(const XrXirCompileContext *c,MatchFixture *f) {
    XrXirType argument=XR_XIR_STRING;XrXirTypeMatchScratch scratch={c->resources,NULL};
    /* The canonical nested pool has distinct Array<String>/Array<I64> roots.
     * A negative nested proof still needs two active stacks before its leaf. */
    XrXirStatus status=nested(c,f,&scratch);
    if (status==XR_XIR_BAD_TYPE) status=XR_XIR_OK;
    else if (status==XR_XIR_OK) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xr_xir_compile_type_substitution_matches_between_scratch(c,&f->small_from,&f->small_to,
        &argument,1,constructed(6),constructed(6),&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_substitution_matches_between_scratch(c,&f->large_from,&f->large_to,
        &argument,1,constructed(159),constructed(159),&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_substitution_matches_between_scratch(c,&f->small_from,&f->small_to,
        &argument,1,constructed(6),constructed(6),&scratch);
    if (status==XR_XIR_OK) {
        status=nested(c,f,&scratch);
        if (status==XR_XIR_BAD_TYPE) status=XR_XIR_OK;
        else if (status==XR_XIR_OK) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_xir_type_match_scratch_free(&scratch);return status;
}
static void cross_pool(void) {
    XrXirTypeNode source[]={{.kind=XR_XIR_TYPE_ARRAY,.element=parameter(),.parameter_span=1},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
    XrXirTypeNode destination[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64}};
    XrXirTypes from={source,2,NULL,NULL},to={destination,3,NULL,NULL};XrXirType argument=XR_XIR_STRING;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&c,&from)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&c,&to)==XR_XIR_OK);
    XrXirTypeMatchScratch scratch={c.resources,NULL};
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&from,&to,&argument,1,constructed(0),constructed(1),&scratch)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&from,&to,&argument,1,constructed(0),constructed(0),&scratch)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&from,&to,NULL,0,constructed(1),constructed(2),&scratch)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&from,&to,NULL,0,constructed(1),constructed(1),&scratch)==XR_XIR_BAD_TYPE);
    uint64_t allocations=stats(&c).allocation_count;
    destination[1].element=XR_XIR_I64;
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&from,&to,&argument,1,constructed(0),constructed(1),&scratch)==XR_XIR_BAD_TYPE);
    CHECK(stats(&c).allocation_count==allocations);
    destination[1].element=XR_XIR_STRING;
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&from,&to,&argument,1,constructed(0),constructed(1),&scratch)==XR_XIR_OK);
    XrXirCompileContext foreign={0};XrCompileResourceLimits limits=caps();
    CHECK(xr_compile_resources_new(&limits,&foreign.resources)==XR_COMPILE_RESOURCE_OK);foreign.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats before=stats(&c),other=stats(&foreign);
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&foreign,&from,&to,&argument,1,constructed(0),constructed(1),&scratch)==XR_XIR_BAD_STRUCTURE);
    CHECK(stats(&c).work==before.work && stats(&foreign).work==other.work);
    xr_compile_resources_release(foreign.resources);
    xr_xir_type_match_scratch_free(&scratch);owner_free(&c,baseline);
}
static void nested_freshness(void) {
    MatchFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.nested_from)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.nested_to)==XR_XIR_OK);
    XrXirTypeMatchScratch scratch={c.resources,NULL};uint64_t before=stats(&c).allocation_count;
    CHECK(nested(&c,&f,&scratch)==XR_XIR_BAD_TYPE);
    CHECK(stats(&c).allocation_count==before+2); /* Canonical negative: two live stacks. */
    before=stats(&c).allocation_count;
    /* Bare-walker mechanical stress only: duplicate destination nodes are not
     * an admitted pool. This proves inner success cannot corrupt outer children
     * without claiming that duplicate IDs may enter a Checked artifact. */
    f.nested_target[1].element=XR_XIR_STRING;
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.nested_to)==XR_XIR_BAD_STRUCTURE);
    CHECK(nested(&c,&f,&scratch)==XR_XIR_OK);
    /* First-fit now gives outer the previous capacity-three inner block.
     * The remaining capacity-two block cannot serve the new inner traversal,
     * so a third block is required. Both capacity-three blocks then reuse. */
    CHECK(stats(&c).allocation_count==before+1);
    before=stats(&c).allocation_count;
    f.target_parameters[1].type=XR_XIR_BOOL;
    CHECK(nested(&c,&f,&scratch)==XR_XIR_BAD_TYPE); /* Child after inner return is still compared. */
    f.target_parameters[1].type=XR_XIR_STRING;
    CHECK(nested(&c,&f,&scratch)==XR_XIR_OK);
    f.target_parameters[1].mode=1;
    CHECK(nested(&c,&f,&scratch)==XR_XIR_BAD_TYPE);
    f.target_parameters[1].mode=0;
    f.nested_target[2].flags=XR_XIR_CALLABLE_NO_SUSPEND;
    CHECK(nested(&c,&f,&scratch)==XR_XIR_BAD_TYPE);
    f.nested_target[2].flags=0;
    CHECK(nested(&c,&f,&scratch)==XR_XIR_OK && stats(&c).allocation_count==before);
    xr_xir_type_match_scratch_free(&scratch);owner_free(&c,baseline);
}
static void growth_reuse(void) {
    MatchFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.small_from)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.small_to)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.large_from)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&c,&f.large_to)==XR_XIR_OK);
    XrXirType argument=XR_XIR_STRING;XrXirTypeMatchScratch scratch={c.resources,NULL};
    uint64_t before=stats(&c).allocation_count;
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&f.small_from,&f.small_to,&argument,1,constructed(6),constructed(6),&scratch)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==before+1);
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&f.large_from,&f.large_to,&argument,1,constructed(159),constructed(159),&scratch)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==before+2);
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&f.small_from,&f.small_to,&argument,1,constructed(6),constructed(6),&scratch)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==before+2);
    f.large_target[0].element=XR_XIR_I64;
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&f.large_from,&f.large_to,&argument,1,constructed(159),constructed(159),&scratch)==XR_XIR_BAD_TYPE);
    f.large_target[0].element=XR_XIR_STRING;
    CHECK(xr_xir_compile_type_substitution_matches_between_scratch(&c,&f.large_from,&f.large_to,&argument,1,constructed(159),constructed(159),&scratch)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==before+2);
    xr_xir_type_match_scratch_free(&scratch);CHECK(stats(&c).live_bytes==baseline);
    before=stats(&c).allocation_count;
    for (uint32_t i=0;i<2;++i) {
        CHECK(xr_xir_compile_type_substitution_matches_between(&c,&f.small_from,&f.small_to,&argument,1,constructed(6),constructed(6))==XR_XIR_OK);
        CHECK(stats(&c).live_bytes==baseline && stats(&c).allocation_count==before+i+1);
    }
    owner_free(&c,baseline);
}
static XrCompileResourceStats measured(XrCompileResourceLimits limits,XrXirStatus expected) {
    MatchFixture f;fixture(&f);XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    CHECK(sequence(&c,&f)==expected);XrCompileResourceStats result=stats(&c);owner_free(&c,baseline);return result;
}
static void faults(void) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        MatchFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=sequence(&c,&f);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<32); }
        else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&c,baseline);
    }
    printf("type match real stack OOM sites=%zu physical=0/0\n",sites);
}
static void limits(void) {
    XrCompileResourceStats base=measured(caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={base.allocated_bytes,base.peak_bytes,base.work};
    XrCompileResourceStats actual=measured(exact,XR_XIR_OK);
    CHECK(actual.allocated_bytes==base.allocated_bytes && actual.peak_bytes==base.peak_bytes && actual.work==base.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)measured(less,XR_XIR_BUDGET);
}
static void deep_body(void) {
    enum { DEPTH=160 };
    XrXirTypeNode nodes[DEPTH];chain(nodes,DEPTH,parameter());XrXirTypes types={nodes,DEPTH,NULL,NULL};
    const XrXirType t=parameter();const XrXirConstraint sendable={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    const XrXirType parameters[]={XR_XIR_I64,XR_XIR_STRING},arguments[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_STRING};
    const uint32_t operands[]={0,1,3};
    const XrXirInstruction caller[]={
        {XR_XIR_CALL,XR_XIR_I64,{0,1},{0},1,{0,1}},
        {XR_XIR_CALL,XR_XIR_STRING,{1,1},{0},1,{1,1}},
        {XR_XIR_CALL,XR_XIR_STRING,{2,1},{0},1,{2,1}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}}};
    const XrXirInstruction body[]={
        {XR_XIR_ARRAY_NEW,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+DEPTH-1),{0},{0},0,{0}},
        {XR_XIR_COPY,t,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    const XrXirBlock blocks[]={{0,4,0,0},{0,3,0,0}};
    const XrXirFunction functions[]={
        {"caller",6,parameters,2,XR_XIR_STRING,blocks,1,caller,4,operands,3},
        {"id",2,&t,1,t,blocks+1,1,body,3,NULL,0}};
    const XrXirGeneric generics[]={{NULL,0,arguments,3,NULL},{&sendable,1,NULL,0,NULL}};
    const XrXirModule built={XR_XIR_BUILT,functions,2,NULL,generics,&types,NULL,XR_XIR_PROGRAM,NULL};
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128)*1024*1024};
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_check(&c,&built,&checked,NULL)==XR_XIR_OK);
    memset(nodes,0xcc,sizeof(nodes));
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);
    CHECK(module->function_count==3 && module->types->count==DEPTH*2);
    for (uint32_t f=1;f<3;++f) {
        XrXirType type=module->functions[f].instructions[0].type;
        for (uint32_t i=0;i<DEPTH;++i) {
            CHECK(xr_xir_type_is_array(module->types,type) && !xr_xir_type_span(module->types,type));
            type=xr_xir_array_element(module->types,type);
        }
        CHECK(type==(f==1?XR_XIR_I64:XR_XIR_STRING));
    }
    CHECK(module->functions[0].instructions[1].immediate==module->functions[0].instructions[2].immediate);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);owner_free(&c,baseline);
}
int main(void) {
    cross_pool();nested_freshness();growth_reuse();faults();limits();deep_body();
    puts("fresh type substitution, claimed stacks, owned deep body, finite axes and physical release PASS");return 0;
}
