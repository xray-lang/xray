/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_signature_shape_scratch.c - Fresh signature shape and owned storage
 *
 * KEY CONCEPT:
 *   Reused bytes grant no facts; every scope and edge is checked again.
 */
#include "xir/xxir_type_scratch_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical physical[64];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *shape_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true;return NULL; }
    void *pointer=xr_malloc(bytes);if (!pointer)return NULL;
    CHECK(live<64 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void shape_free(void *pointer) {
    if (!pointer)return;
    size_t i=0;while (i<live && physical[i].pointer!=pointer)++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) shape_malloc(bytes)
#define xr_free(pointer) shape_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
/* Exercise the real private signature stage, not a replica of its loop. */
#include "xir/xxir_verify.c"
typedef struct ShapeFixture {
    XrXirTypeNode nodes[33];
    XrXirTypes types;
    XrXirType roots[5];
    XrXirFunction functions[5];
    XrXirBlock block;
    XrXirInstruction returned;
    XrXirModule module;
} ShapeFixture;
static XrXirType constructed(uint32_t index) { return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+index); }
static void fixture(ShapeFixture *f,bool complete) {
    static const char *const names[5]={"a","b","c","d","e"};
    static const uint32_t roots[5]={0,8,32,8,0};
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    for(uint32_t i=1;i<33;++i)f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(i-1)};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=33};
    f->block=(XrXirBlock){.count=1};
    f->returned=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    for(uint32_t i=0;i<5;++i) {
        f->roots[i]=constructed(roots[i]);
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=1,.result=f->roots[i],
            .blocks=&f->block,.block_count=1,.instructions=&f->returned,.instruction_count=1};
        if(complete){f->functions[i].parameters=&f->roots[i];f->functions[i].parameter_count=1;}
    }
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
static XrCompileResourceLimits caps(void) { return (XrCompileResourceLimits){2097152,1048576,2000000}; }
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes==live_bytes);return result;
}
static XrXirCompileContext owner_new(XrCompileResourceLimits limits) {
    CHECK(!live && !live_bytes);XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void owner_free(XrXirCompileContext *context,uint64_t baseline) {
    CHECK(stats(context).live_bytes==baseline && live==1);
    xr_compile_resources_release(context->resources);*context=(XrXirCompileContext){0};
    CHECK(!live && !live_bytes);
}
static XrCompileResourceStats run(XrCompileResourceLimits limits,XrXirStatus expected) {
    ShapeFixture f;fixture(&f,false);XrXirCompileContext context=owner_new(limits);
    uint64_t baseline=stats(&context).live_bytes;
    CHECK(verify_signature_structure(&f.module,&context)==expected);
    XrCompileResourceStats result=stats(&context);CHECK(live==1 && result.live_bytes==baseline);
    owner_free(&context,baseline);return result;
}
static void faults(void) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        ShapeFixture f;fixture(&f,false);XrXirCompileContext context=owner_new(caps());
        uint64_t baseline=stats(&context).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=verify_signature_structure(&f.module,&context);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites==3);}
        else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&context,baseline);
    }
    puts("signature shape: three real growth OOM points, stage physical baseline restored");
}
static void limits(void) {
    /* Five signature visits and ledger creation cost six. Dense roots contain
     * 53 nodes: five pop units, one child edge and two child marks per node.
     * Root seeding adds five after subtracting absent scalar child marks;
     * bitmap clears total 1+2+5+2+1=11, and three growth attempts add three. */
    XrCompileResourceStats measured=run(caps(),XR_XIR_OK);CHECK(measured.work==449);
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,449};
    XrCompileResourceStats again=run(exact,XR_XIR_OK);
    CHECK(again.work==449 && again.allocated_bytes==measured.allocated_bytes && again.peak_bytes==measured.peak_bytes);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)run(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)run(less,XR_XIR_BUDGET);
    for(uint64_t work=1;work<449;++work){less=exact;less.work=work;(void)run(less,XR_XIR_BUDGET);}
}
static void scopes(void) {
    ShapeFixture f;fixture(&f,false);XrXirCompileContext context=owner_new(caps());
    uint64_t baseline=stats(&context).live_bytes;XirTypeScratch scratch={context.resources,NULL,0};
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(0),0,&scratch)==XR_XIR_OK);
    unsigned char *saved=scratch.bytes;unsigned char byte=saved[0];uint32_t capacity=scratch.capacity;
    attempts=0;injected=false;fail_at=0;
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(8),0,&scratch)==XR_XIR_OUT_OF_MEMORY);
    fail_at=SIZE_MAX;CHECK(injected && attempts==1 && scratch.bytes==saved && scratch.capacity==capacity && saved[0]==byte);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(0),0,&scratch)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),0,&scratch)==XR_XIR_OK);
    uint64_t allocations=stats(&context).allocation_count;
    f.nodes[7].element=constructed(7);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),0,&scratch)==XR_XIR_BAD_TYPE);
    f.nodes[7].element=constructed(6);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),0,&scratch)==XR_XIR_OK);
    ShapeFixture other;fixture(&other,false);other.nodes[0].element=constructed(0);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&other.types,constructed(32),0,&scratch)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),0,&scratch)==XR_XIR_OK);
    f.nodes[0].element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    for(uint32_t i=0;i<33;++i)f.nodes[i].parameter_span=1;
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),1,&scratch)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),0,&scratch)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&f.types,constructed(32),1,&scratch)==XR_XIR_OK);
    CHECK(stats(&context).allocation_count==allocations);
    XrXirCompileContext invalid={0};CHECK(xr_xir_compile_type_expression_shape_scratch(&invalid,&f.types,f.roots[0],1,&scratch)==XR_XIR_BAD_STRUCTURE);
    XrCompileResources *foreign=NULL;XrCompileResourceLimits cap=caps();
    CHECK(xr_compile_resources_new(&cap,&foreign)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext wrong={.resources=foreign,.limits=context.limits};
    CHECK(xr_xir_compile_type_expression_shape_scratch(&wrong,&f.types,f.roots[0],1,&scratch)==XR_XIR_BAD_STRUCTURE);
    xr_compile_resources_release(foreign);xir_type_scratch_free(&scratch);owner_free(&context,baseline);
}
static void bad_signatures(void) {
    ShapeFixture f;fixture(&f,true);XrXirCompileContext context=owner_new(caps());
    uint64_t baseline=stats(&context).live_bytes;
    f.functions[1].parameters=NULL;
    CHECK(verify_signature_structure(&f.module,&context)==XR_XIR_BAD_STRUCTURE);
    CHECK(stats(&context).live_bytes==baseline && live==1);
    const XrXirType unit=XR_XIR_UNIT;f.functions[1].parameters=&unit;
    CHECK(verify_signature_structure(&f.module,&context)==XR_XIR_BAD_TYPE);
    CHECK(stats(&context).live_bytes==baseline && live==1);
    f.functions[1].parameters=&f.roots[1];context.limits.parameters=0;
    CHECK(verify_signature_structure(&f.module,&context)==XR_XIR_BUDGET);
    CHECK(stats(&context).live_bytes==baseline && live==1);
    owner_free(&context,baseline);
}
static void public_and_output(void) {
    ShapeFixture f;fixture(&f,true);XrXirCompileContext context=owner_new(caps());
    uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_type_expression_shape(&context,&f.types,constructed(32),0)==XR_XIR_OK);
    CHECK(stats(&context).live_bytes==baseline && live==1);
    XrXirArtifact *held=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_check(&context,&f.module,&held,&diagnostic)==XR_XIR_OK && held);
    const XrXirModule *snapshot=xr_xir_compile_artifact_module(held);
    CHECK(snapshot && snapshot->types && snapshot->types->nodes[0].element==XR_XIR_I64);
    uint64_t pinned=stats(&context).live_bytes;
    f.nodes[0].element=constructed(0);XrXirArtifact *output=held;
    CHECK(xr_xir_compile_check(&context,&f.module,&output,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(output==held && diagnostic.status==XR_XIR_BAD_TYPE && stats(&context).live_bytes==pinned);
    CHECK(snapshot->types->nodes[0].element==XR_XIR_I64);
    xr_xir_compile_artifact_free(held);owner_free(&context,baseline);
}

/* This pool carries both closed and declaration-local Record arguments. */
typedef struct SignatureProofFixture {
    XrXirType arguments[2], parameters[2];
    XrXirTypeNode nodes[3];
    XrXirConstraint required, bound;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypes types;
    XrXirGeneric generics[4];
    XrXirFunction functions[4];
    XrXirBlock blocks[4];
    XrXirInstruction returned, body[2];
    XrXirFunctionIdentity identities[4];
    XrXirSourceModule modules[2];
    uint32_t dependency;
    XrXirDeclarations declarations;
    XrXirModule module;
} SignatureProofFixture;
static void signature_fixture(SignatureProofFixture *f) {
    static const char *const names[4]={"a","b","secretInit","callerInit"};
    static const uint32_t lengths[4]={1,1,10,10};
    memset(f,0,sizeof(*f));
    f->arguments[0]=XR_XIR_I64;f->arguments[1]=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->required.markers=XR_XIR_CONSTRAINT_SENDABLE;f->bound=f->required;
    f->nominal=(XrXirNominalDeclaration){.module={"secret",6},.name={"Record",6},
        .exported=1,.constraints=&f->required,.parameter_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    f->nominals=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    for(uint32_t n=0;n<2;++n) {
        f->nodes[n]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.parameter_span=n,
            .nominal={.declaration=0,.arguments=&f->arguments[n],.argument_count=1}};
        f->parameters[n]=constructed(n);
    }
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(1),.parameter_span=1};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=3,.nominals=&f->nominals};
    f->generics[1]=(XrXirGeneric){.constraints=&f->bound,.parameter_count=1};
    f->returned=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    for(uint32_t n=0;n<4;++n) {
        f->blocks[n]=(XrXirBlock){.count=1};
        f->functions[n]=(XrXirFunction){.name=names[n],.name_length=lengths[n],.result=XR_XIR_UNIT,
            .blocks=&f->blocks[n],.block_count=1,.instructions=&f->returned,.instruction_count=1};
        f->identities[n].module=n==2?0:1;
        if(n<2){f->functions[n].parameters=&f->parameters[n];f->functions[n].parameter_count=1;}
    }
    f->dependency=0;
    f->modules[0]=(XrXirSourceModule){.name="secret",.name_length=6,.initializer=2};
    f->modules[1]=(XrXirSourceModule){.name="caller",.name_length=6,.dependencies=&f->dependency,
        .dependency_count=1,.initializer=3};
    f->declarations=(XrXirDeclarations){.modules=f->modules,.module_count=2,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.generics=f->generics,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
static void signature_expect(XrXirCompileContext *context,SignatureProofFixture *f,
    XrXirStatus expected,uint32_t function) {
    uint64_t baseline=stats(context).live_bytes;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_verify(context,&f->module,&diagnostic)==expected);
    CHECK(diagnostic.status==expected);
    if(expected!=XR_XIR_OK)CHECK(diagnostic.function==function);
    CHECK(stats(context).live_bytes==baseline && live==1);
}
static void signature_priority_and_freshness(void) {
    SignatureProofFixture f;signature_fixture(&f);XrXirCompileContext context=owner_new(caps());
    uint64_t baseline=stats(&context).live_bytes;
    /* The later declaration obligation precedes the earlier naming failure. */
    f.nominal.exported=0;f.bound.markers=0;
    signature_expect(&context,&f,XR_XIR_BAD_TYPE,UINT32_MAX);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;
    signature_expect(&context,&f,XR_XIR_BAD_TYPE,0);
    f.nominal.exported=1;signature_expect(&context,&f,XR_XIR_OK,3);
    f.bound.markers=0;signature_expect(&context,&f,XR_XIR_BAD_TYPE,UINT32_MAX);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;signature_expect(&context,&f,XR_XIR_OK,3);
    const char bad_name=0;f.functions[0].name=&bad_name;f.bound.markers=0;
    signature_expect(&context,&f,XR_XIR_BAD_TYPE,UINT32_MAX);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;signature_expect(&context,&f,XR_XIR_BAD_STRUCTURE,0);
    f.functions[0].name="a";
    /* Same numeric IDs in a new pool carry no proof from the previous scope. */
    SignatureProofFixture other;signature_fixture(&other);other.bound.markers=0;
    other.arguments[1]=XR_XIR_STRING;other.nodes[1].parameter_span=0;other.nodes[2].parameter_span=0;
    signature_expect(&context,&other,XR_XIR_OK,3);
    f.bound.markers=0;signature_expect(&context,&f,XR_XIR_BAD_TYPE,UINT32_MAX);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;signature_expect(&context,&f,XR_XIR_OK,3);
    XrXirProofContext proof={&f.module,{XR_XIR_CONTEXT_FUNCTION,1,0}};
    f.bound.markers=0;
    CHECK(xr_xir_compile_type_use_verify(&context,&proof,constructed(1))==XR_XIR_BAD_TYPE);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_type_use_verify(&context,&proof,constructed(1))==XR_XIR_OK);
    CHECK(stats(&context).live_bytes==baseline && live==1);
    owner_free(&context,baseline);
}
static void signature_instruction_proof(void) {
    SignatureProofFixture f;signature_fixture(&f);f.parameters[1]=XR_XIR_I64;
    f.body[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=constructed(2)};
    f.body[1]=f.returned;f.blocks[1].count=2;
    f.functions[1].instructions=f.body;f.functions[1].instruction_count=2;
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    f.bound.markers=0;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_verify(&context,&f.module,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(diagnostic.function==1 && diagnostic.block==0 && diagnostic.instruction==0);
    CHECK(stats(&context).live_bytes==baseline && live==1);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;signature_expect(&context,&f,XR_XIR_OK,3);
    owner_free(&context,baseline);
}
static void signature_scalar_proof(void) {
    const XrXirType parameter=XR_XIR_I64;
    const XrXirInstruction returned={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    const XrXirBlock block={.count=1};
    const XrXirFunction function={.name="f",.name_length=1,.result=XR_XIR_UNIT,.parameters=&parameter,
        .parameter_count=1,.blocks=&block,.block_count=1,.instructions=&returned,.instruction_count=1};
    const XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.linkage_kind=XR_XIR_PROGRAM};
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_verify(&context,&module,NULL)==XR_XIR_OK);
    VerifyContext state={.remaining=context,.module=&module,.scratch={context.resources,NULL},.pending={context.resources,NULL,0}};
    uint64_t before=stats(&context).work;
    CHECK(signature_type_use_context(&state,0,XR_XIR_I64,false)==XR_XIR_OK);
    /* One new flag read and the original scalar expression-edge proof remain. */
    CHECK(stats(&context).work-before==2);
    xir_type_scratch_free(&state.pending);xr_xir_constraint_scratch_free(&state.scratch);
    owner_free(&context,baseline);
    XrCompileResourceLimits limited=caps();limited.work=2;context=owner_new(limited);baseline=stats(&context).live_bytes;
    state=(VerifyContext){.remaining=context,.module=&module,.scratch={context.resources,NULL},.pending={context.resources,NULL,0}};
    CHECK(signature_type_use_context(&state,0,XR_XIR_I64,false)==XR_XIR_BUDGET);
    CHECK(stats(&context).work==2 && live==1);
    xir_type_scratch_free(&state.pending);xr_xir_constraint_scratch_free(&state.scratch);owner_free(&context,baseline);
}
static void signature_owned_output(void) {
    SignatureProofFixture f;signature_fixture(&f);XrXirCompileContext context=owner_new(caps());
    XrXirArtifact *held=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_check(&context,&f.module,&held,&diagnostic)==XR_XIR_OK && held);
    const XrXirModule *snapshot=xr_xir_compile_artifact_module(held);
    CHECK(snapshot && snapshot->generics && snapshot->generics[1].constraints[0].markers==XR_XIR_CONSTRAINT_SENDABLE);
    uint64_t pinned=stats(&context).live_bytes;f.bound.markers=0;XrXirArtifact *output=held;
    CHECK(xr_xir_compile_check(&context,&f.module,&output,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(output==held && diagnostic.function==UINT32_MAX && stats(&context).live_bytes==pinned);
    CHECK(snapshot->generics[1].constraints[0].markers==XR_XIR_CONSTRAINT_SENDABLE);
    CHECK(xr_xir_compile_artifact_verify(held,&diagnostic)==XR_XIR_OK);
    /* Owned metadata keeps its ledger alive after the caller releases its reference. */
    xr_compile_resources_release(context.resources);context=(XrXirCompileContext){0};
    CHECK(live>1 && live_bytes==pinned);
    CHECK(xr_xir_compile_artifact_verify(held,&diagnostic)==XR_XIR_OK);
    CHECK(stats(xr_xir_compile_artifact_context(held)).live_bytes==pinned);
    xr_xir_compile_artifact_free(held);CHECK(!live && !live_bytes);
}
static XrCompileResourceStats signature_run(XrCompileResourceLimits limits,XrXirStatus expected) {
    SignatureProofFixture f;signature_fixture(&f);XrXirCompileContext context=owner_new(limits);
    uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_verify(&context,&f.module,NULL)==expected);
    XrCompileResourceStats result=stats(&context);owner_free(&context,baseline);return result;
}
static void signature_resources(void) {
    XrCompileResourceStats measured=signature_run(caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
    XrCompileResourceStats again=signature_run(exact,XR_XIR_OK);
    CHECK(again.work==measured.work && again.allocated_bytes==measured.allocated_bytes && again.peak_bytes==measured.peak_bytes);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)signature_run(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)signature_run(less,XR_XIR_BUDGET);
    for(uint64_t work=1;work<exact.work;++work){less=exact;less.work=work;(void)signature_run(less,XR_XIR_BUDGET);}
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        SignatureProofFixture f;signature_fixture(&f);XrXirCompileContext context=owner_new(caps());
        uint64_t baseline=stats(&context).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=xr_xir_compile_verify(&context,&f.module,NULL);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<20000);}
        else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&context,baseline);
    }
    printf("signature declaration proof: full verify sites=%zu, work=%llu, physical 0/0\n",sites,(unsigned long long)measured.work);
}

static void bitmap_atomic_outputs(void) {
    for (uint64_t cut=1;cut<=6;++cut) {
        XrCompileResourceLimits limits=caps();limits.work=cut;
        XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
        unsigned char pending=0x82;uint32_t ceiling=8,index=99;bool found=false;
        XrXirStatus status=xir_type_pending_next(&context,&pending,&ceiling,&index,&found);
        CHECK(status==(cut<6?XR_XIR_BUDGET:XR_XIR_OK));
        uint64_t expected=1+(cut>=2?1:0)+(cut>=5?3:0)+(cut>=6?1:0);
        CHECK(stats(&context).work==expected);
        if(cut<6) CHECK(pending==0x82 && ceiling==8 && index==99 && !found);
        else CHECK(pending==2 && ceiling==7 && index==7 && found);
        owner_free(&context,baseline);
    }
    for (uint64_t cut=1;cut<=9;++cut) {
        XrCompileResourceLimits limits=caps();limits.work=cut;
        XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
        unsigned char pending[4]={1,0,0,0};uint32_t ceiling=32,index=99;bool found=false;
        XrXirStatus status=xir_type_pending_next(&context,pending,&ceiling,&index,&found);
        CHECK(status==(cut<9?XR_XIR_BUDGET:XR_XIR_OK));
        uint64_t expected=(cut<5?cut:5)+(cut>=8?3:0)+(cut>=9?1:0);
        CHECK(stats(&context).work==expected);
        if(cut<9)CHECK(pending[0]==1 && ceiling==32 && index==99 && !found);
        else CHECK(!pending[0] && !ceiling && !index && found);
        CHECK(!pending[1] && !pending[2] && !pending[3]);owner_free(&context,baseline);
    }
    for (uint64_t cut=1;cut<=5;++cut) {
        XrCompileResourceLimits limits=caps();limits.work=cut;
        XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
        unsigned char pending[4]={0};uint32_t ceiling=32,index=99;bool found=true;
        CHECK(xir_type_pending_next(&context,pending,&ceiling,&index,&found)==(cut<5?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(stats(&context).work==cut && index==99);
        if(cut<5)CHECK(ceiling==32 && found);else CHECK(!ceiling && !found);
        owner_free(&context,baseline);
    }
    for (uint64_t cut=1;cut<=3;++cut) {
        XrCompileResourceLimits limits=caps();limits.work=cut;
        XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
        unsigned char pending=0x80;
        CHECK(xir_type_pending_mark(&context,&pending,1)==(cut<3?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(pending==(cut<3?0x80:0x82) && stats(&context).work==(cut<3?1:3));
        owner_free(&context,baseline);
    }
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    XirTypeScratch scratch={context.resources,NULL,0};XrXirStatus status=XR_XIR_OK;
    CHECK(!xir_type_scratch_pending(&context,&scratch,0,&status) && status==XR_XIR_BAD_STRUCTURE);
    CHECK(!scratch.bytes && !scratch.capacity && stats(&context).work==1);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,NULL,XR_XIR_UNIT,0,&scratch)==XR_XIR_OK);
    CHECK(!scratch.bytes && !scratch.capacity && stats(&context).work==2);
    xir_type_scratch_free(&scratch);owner_free(&context,baseline);
}
static void bitmap_same_byte_and_order(void) {
    XrXirTypeNode nodes[10]={0};XrXirTypes types={.nodes=nodes,.count=10};
    XrXirCallableParameter parameters[2]={{constructed(0),0},{constructed(0),0}};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(0)};
    nodes[9]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=constructed(7),.parameters=parameters,.parameter_count=2};
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    XirTypeScratch scratch={context.resources,NULL,0};
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&types,constructed(9),0,&scratch)==XR_XIR_OK);
    /* Node seven seeds node zero after its byte was read; duplicate roots do not skip its check. */
    nodes[0].element=constructed(0);
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&types,constructed(9),0,&scratch)==XR_XIR_BAD_TYPE);
    nodes[0].element=XR_XIR_I64;
    parameters[0].type=constructed(6);parameters[1].type=constructed(6);
    nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.parameter_count=1};
    nodes[7].kind=0;
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&types,constructed(9),0,&scratch)==XR_XIR_BAD_TYPE);
    nodes[7]=nodes[6];nodes[6].kind=0;
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&types,constructed(9),0,&scratch)==XR_XIR_BAD_STRUCTURE);
    nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
    nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
    CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&types,constructed(9),0,&scratch)==XR_XIR_OK);
    xir_type_scratch_free(&scratch);owner_free(&context,baseline);
}
static XrCompileResourceStats bitmap_large_run(XrCompileResourceLimits limits,bool dense,XrXirStatus expected) {
    enum { COUNT=XR_XIR_CONSTRUCTED_TYPE_LIMIT-XR_XIR_CONSTRUCTED_TYPE_BASE };
    static XrXirTypeNode nodes[COUNT];
    memset(nodes,0,sizeof(nodes));nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    if(dense) for(uint32_t i=1;i<COUNT;++i)
        nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(i-1)};
    else nodes[COUNT-1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(0)};
    XrXirTypes types={.nodes=nodes,.count=COUNT};
    XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_type_expression_shape(&context,&types,constructed(COUNT-1),0)==expected);
    XrCompileResourceStats result=stats(&context);owner_free(&context,baseline);return result;
}
static void bitmap_large_boundaries(void) {
    const uint64_t count=XR_XIR_CONSTRUCTED_TYPE_LIMIT-XR_XIR_CONSTRUCTED_TYPE_BASE;
    const uint64_t bytes=count/8;
    /* Sparse: two pops cost ten, zero-byte reads cost bytes-1, edges/marks seven,
     * zeroing costs bytes, and allocation plus ledger creation cost two. */
    const uint64_t sparse_work=2*bytes+18;
    XrCompileResourceStats sparse=bitmap_large_run(caps(),false,XR_XIR_OK);
    CHECK(sparse.work==sparse_work && sparse.allocation_count==2);
    const uint64_t storage=sizeof(XrCompileResources)+sizeof(CompileAllocation)+bytes;
    CHECK(sparse.allocated_bytes==storage && sparse.peak_bytes==storage);
    XrCompileResourceStats dense=bitmap_large_run(caps(),true,XR_XIR_OK);
    CHECK(dense.work==8*count+bytes+3 && dense.allocation_count==2);
    for(unsigned axis=0;axis<3;++axis) for(unsigned below=0;below<2;++below) {
        XrCompileResourceLimits limits=caps();
        if(!axis) limits.work=sparse_work-below;
        else if(axis==1) limits.allocated_bytes=storage-below;
        else limits.live_bytes=storage-below;
        (void)bitmap_large_run(limits,false,below?XR_XIR_BUDGET:XR_XIR_OK);
    }
    for(size_t pass=0;pass<2;++pass) {
        XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
        XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
        XrXirTypes types={.nodes=&node,.count=1};XirTypeScratch scratch={context.resources,NULL,0};
        attempts=0;injected=false;fail_at=pass?0:SIZE_MAX;
        CHECK(xr_xir_compile_type_expression_shape_scratch(&context,&types,constructed(0),0,&scratch)==(pass?XR_XIR_OUT_OF_MEMORY:XR_XIR_OK));
        fail_at=SIZE_MAX;CHECK(attempts==1 && injected==(pass!=0));
        if(pass)CHECK(!scratch.bytes && !scratch.capacity);
        xir_type_scratch_free(&scratch);owner_free(&context,baseline);
    }
}

int main(void){bitmap_atomic_outputs();bitmap_same_byte_and_order();bitmap_large_boundaries();faults();limits();scopes();bad_signatures();public_and_output();
    signature_priority_and_freshness();signature_instruction_proof();signature_scalar_proof();signature_owned_output();signature_resources();
    puts("signature shape fresh scopes/edges, finite axes and physical 0/0");return 0;}
