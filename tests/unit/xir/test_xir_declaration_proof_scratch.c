/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_declaration_proof_scratch.c - Fresh declaration facts on owned bytes
 *
 * KEY CONCEPT:
 *   Every type-use query authenticates its current scope and initializes tasks
 *   anew. Module-owned storage is released on every final status.
 */
#include "xir/xxir_constraint_proof_internal.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_implementation_verify.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical physical[1024];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *error_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void error_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && physical[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) error_malloc(bytes)
#define xr_free(pointer) error_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

/* The real production implementation exposes private operation assertions only
 * in this TU; all proof bodies come from the canonical source. */
#include "xir/xxir_constraint_proof.c"
typedef struct DeclarationFixture {
    XrXirTypeNode nodes[21];
    XrXirCallableParameter callable_parameter;
    XrXirNominalField field;
    XrXirType projected_field, parameters[3];
    XrXirNominalDeclaration nominals[2];
    XrXirNominalTable nominal_table;
    XrXirInterfaceMethod method;
    XrXirInterfaceDeclaration interface;
    XrXirInterfaceTable interfaces;
    XrXirTypes types;
    XrXirConstraint bound;
    XrXirGeneric generics[4];
    XrXirFunction functions[4];
    XrXirInstruction returns[4];
    XrXirBlock blocks[4];
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[4];
    XrXirSlot slot;
    XrXirDeclarations declarations;
    XrXirModule module;
} DeclarationFixture;
static XrXirType constructed(uint32_t n) { return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n); }
static void fixture(DeclarationFixture *f) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
    for(uint32_t n=1;n<=17;++n) f->nodes[n]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,
        .element=n==1?(XrXirType)XR_XIR_TYPE_PARAMETER_BASE:constructed(n-1),.parameter_span=1};
    f->field=(XrXirNominalField){.name={"value",5},.type=constructed(0)};
    f->projected_field=constructed(0);
    f->nominals[0]=(XrXirNominalDeclaration){.module={"proofs",6},.name={"Record",6},
        .fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    f->nominals[1]=f->nominals[0];f->nominals[1].name=(XrXirLiteral){"Object",6};
    f->nominals[1].kind=XR_XIR_NOMINAL_CLASS;
    f->nominal_table=(XrXirNominalTable){f->nominals,2,NULL};
    f->nodes[18]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=0,.fields=&f->projected_field,.field_count=1}};
    f->callable_parameter=(XrXirCallableParameter){constructed(0),0};
    f->nodes[19]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->callable_parameter,
        .parameter_count=1,.result=XR_XIR_UNIT};
    f->nodes[20]=f->nodes[18];f->nodes[20].nominal.declaration=1;
    f->method=(XrXirInterfaceMethod){.name={"inspect",7},.signature=constructed(19)};
    f->interface=(XrXirInterfaceDeclaration){.module={"proofs",6},.name={"Inspect",7},
        .methods=&f->method,.method_count=1};
    f->interfaces=(XrXirInterfaceTable){&f->interface,1};
    f->types=(XrXirTypes){f->nodes,21,&f->nominal_table,&f->interfaces};
    f->parameters[0]=constructed(17);f->parameters[1]=constructed(1);f->parameters[2]=constructed(17);
    static const char *names[]={"init","small","large","closed"};
    static const uint32_t lengths[]={4,5,5,6};
    for(uint32_t n=0;n<4;++n){
        f->returns[n]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
        f->blocks[n]=(XrXirBlock){.first=0,.count=1};
        f->functions[n]=(XrXirFunction){.name=names[n],.name_length=lengths[n],.result=XR_XIR_UNIT,
            .blocks=&f->blocks[n],.block_count=1,.instructions=&f->returns[n],.instruction_count=1};
    }
    f->functions[1].parameters=&f->parameters[1];f->functions[1].parameter_count=1;
    f->functions[2].parameters=f->parameters;f->functions[2].parameter_count=3;
    f->functions[3].parameters=&f->projected_field;f->functions[3].parameter_count=1;
    f->generics[1]=(XrXirGeneric){.constraints=&f->bound,.parameter_count=1};
    f->generics[2]=f->generics[1];
    f->source=(XrXirSourceModule){"proofs",6,NULL,0,0};
    f->slot=(XrXirSlot){.module=0,.type=constructed(0)};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.types=&f->types,.generics=f->generics,.linkage_kind=XR_XIR_LIBRARY};
}
static XrCompileResourceLimits caps(void){return (XrCompileResourceLimits){2097152,1048576,2000000};}
static XrXirCompileContext owner_new(XrCompileResourceLimits limits){
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats stats(const XrXirCompileContext *c){
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void owner_free(XrXirCompileContext *c,uint64_t baseline){
    CHECK(stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);*c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static XrXirStatus whole(const XrXirCompileContext *c){
    DeclarationFixture f;fixture(&f);XrXirStatus status=xr_xir_compile_verify(c,&f.module,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_declaration_constraints_verify(c,&f.module);
    if(status==XR_XIR_OK)status=xr_xir_compile_module_constraints_verify(c,&f.module);
    return status;
}
static XrCompileResourceStats measured(XrCompileResourceLimits limits,XrXirStatus expected){
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    CHECK(whole(&c)==expected);XrCompileResourceStats s=stats(&c);owner_free(&c,baseline);return s;
}
static void faults(void){
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;XrXirStatus status=whole(&c);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<1024);}else CHECK(injected && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&c,baseline);
    }
    printf("declaration full verifier+public operations fresh OOM sites=%zu physical=0/0\n",sites);
}
static void limits(void){
    XrCompileResourceStats a=measured(caps(),XR_XIR_OK);XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=measured(exact,XR_XIR_OK);CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)measured(less,XR_XIR_BUDGET);less=exact;--less.work;(void)measured(less,XR_XIR_BUDGET);
}
static void scopes(void){
    DeclarationFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_OK);
    XirConstraintScratch scratch={c.resources,NULL};
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_OK);
    uint64_t small=stats(&c).allocation_count;
    CHECK(proof_function_declaration(&f.module,2,&c,&scratch)==XR_XIR_OK);
    uint64_t large=stats(&c).allocation_count;CHECK(large>small);
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_OK);
    f.functions[3].parameters=&f.parameters[0];
    CHECK(proof_function_declaration(&f.module,3,&c,&scratch)==XR_XIR_BAD_TYPE);
    f.functions[3].parameters=&f.projected_field;
    CHECK(proof_function_declaration(&f.module,3,&c,&scratch)==XR_XIR_OK);
    f.bound.markers=UINT32_MAX;
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_BAD_TYPE);
    f.bound.markers=0;CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_OK);
    f.nodes[17].element=constructed(17);
    CHECK(proof_function_declaration(&f.module,2,&c,&scratch)==XR_XIR_BAD_TYPE);
    f.nodes[17].element=constructed(16);
    CHECK(proof_function_declaration(&f.module,2,&c,&scratch)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==large);
    xr_xir_constraint_scratch_free(&scratch);CHECK(!scratch.memory && !scratch.resources);
    CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_OK);
    owner_free(&c,baseline);
}
static void requirements(void){
    DeclarationFixture f;fixture(&f);
    XrXirConstraint required={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f.nominals[0].constraints=&required;f.nominals[0].parameter_count=1;
    f.nodes[18].nominal.arguments=&parameter;f.nodes[18].nominal.argument_count=1;f.nodes[18].parameter_span=1;
    XrXirType subject=constructed(18);f.functions[1].parameters=&subject;
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_OK);
    XirConstraintScratch scratch={c.resources,NULL};
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_OK);
    uint64_t allocations=stats(&c).allocation_count;
    f.bound.markers=0;CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_BAD_TYPE);
    f.bound.markers=XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_OK);
    required.markers=XR_XIR_CONSTRAINT_EQUAL;
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_BAD_TYPE);
    required.markers=XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(proof_function_declaration(&f.module,1,&c,&scratch)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==allocations);
    xr_xir_constraint_scratch_free(&scratch);
    /* A cross-module class slot still fails the unchanged marker proof. */
    fixture(&f);f.slot.type=constructed(20);
    CHECK(xr_xir_compile_module_constraints_verify(&c,&f.module)==XR_XIR_BAD_TYPE);
    f.slot.type=constructed(0);CHECK(xr_xir_compile_module_constraints_verify(&c,&f.module)==XR_XIR_OK);
    owner_free(&c,baseline);
}
/* Whole vectors use the same production proof core as public single uses.
 * Expected marker entailment is independent of queue/storage implementation. */
typedef struct BatchFixture {
    XrXirTypeNode nodes[3];
    XrXirType arguments[2], receiver;
    XrXirConstraint nominal_bound, interface_bound, own_bound, goals[2], empty;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirInterfaceMethod methods[2];
    XrXirInterfaceDeclaration interfaces[2];
    XrXirInterfaceTable interface_table;
    XrXirTypes types;
    XrXirGeneric generic;
    XrXirFunction function;
    XrXirInstruction instruction;
    XrXirBlock block;
    XrXirSourceModule source;
    XrXirFunctionIdentity identity;
    XrXirImplementationBinding binding;
    XrXirImplementation implementation;
    XrXirImplementationTable implementations;
    XrXirDeclarations declarations;
    XrXirModule module;
} BatchFixture;
static void batch_fixture(BatchFixture *f) {
    memset(f,0,sizeof(*f));
    f->arguments[0]=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->arguments[1]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+1);
    f->nominal_bound.markers=XR_XIR_CONSTRAINT_EQUAL;
    f->interface_bound=f->nominal_bound;f->own_bound.markers=XR_XIR_CONSTRAINT_SENDABLE;
    f->goals[0]=f->nominal_bound;f->goals[1]=f->own_bound;
    f->nominal=(XrXirNominalDeclaration){.module={"batch",5},.name={"Record",6},.exported=1,
        .constraints=&f->nominal_bound,.parameter_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.parameter_span=1,
        .nominal={.declaration=0,.arguments=f->arguments,.argument_count=1}};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->methods[0]=(XrXirInterfaceMethod){.name={"inspect",7},.signature=constructed(0),
        .own_parameter_count=1,.constraints=&f->own_bound};
    f->methods[1]=f->methods[0];f->methods[1].constraints=&f->empty;
    for(uint32_t n=0;n<2;++n) f->interfaces[n]=(XrXirInterfaceDeclaration){.module={"batch",5},
        .name={n?"Other":"Inspect",n?5u:7u},.exported=1,.constraints=&f->interface_bound,
        .parameter_count=1,.methods=&f->methods[n],.method_count=1};
    f->interface_table=(XrXirInterfaceTable){f->interfaces,2};
    f->types=(XrXirTypes){f->nodes,3,&f->nominals,&f->interface_table};
    f->receiver=constructed(1);
    f->instruction=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->block=(XrXirBlock){.first=0,.count=1};
    f->function=(XrXirFunction){.name="inspect",.name_length=7,.parameters=&f->receiver,.parameter_count=1,
        .result=XR_XIR_UNIT,.blocks=&f->block,.block_count=1,.instructions=&f->instruction,.instruction_count=1};
    f->generic=(XrXirGeneric){.constraints=f->goals,.parameter_count=2};
    f->source=(XrXirSourceModule){"batch",5,NULL,0,0};
    f->identity=(XrXirFunctionIdentity){.exported=1,.nominal_owner=1,
        .method_kind=XR_XIR_READ_METHOD,.member_access=XR_XIR_MEMBER_PUBLIC};
    f->binding=(XrXirImplementationBinding){.requirement={0,f->arguments,1},.member=0,.function=0};
    f->implementation=(XrXirImplementation){.nominal_declaration=0,.interface={0,f->arguments,1},
        .bindings=&f->binding,.binding_count=1};
    f->implementations=(XrXirImplementationTable){&f->implementation,1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=&f->identity,
        .implementations=&f->implementations,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=&f->function,.function_count=1,
        .generics=&f->generic,.types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static XrXirStatus batch_use(const XrXirCompileContext *c,BatchFixture *f) {
    XrXirProofContext proof={&f->module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,0,0}};
    XirConstraintArguments use={&f->module,{XR_XIR_CONTEXT_FUNCTION,0,0},f->arguments,2};
    return xr_xir_compile_constraint_arguments_prove(c,&proof,&use);
}
static XrXirStatus batch_original_queries(const XrXirCompileContext *c,BatchFixture *f) {
    XrXirProofContext proof={&f->module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,0,0}};
    for(uint32_t a=0;a<2;++a) {
        XrXirConstraintUse use={&f->module,{XR_XIR_CONTEXT_FUNCTION,0,0},a,f->arguments,2};
        XrXirStatus status=xr_xir_compile_constraints_prove(c,&proof,&use);
        if(status!=XR_XIR_OK)return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus batch_sequence(const XrXirCompileContext *c) {
    BatchFixture f;batch_fixture(&f);
    XrXirStatus status=xr_xir_compile_types_structure_verify(c,&f.types);
    if(status==XR_XIR_OK)status=xr_xir_compile_interfaces_verify_structure(c,&f.interface_table,&f.types);
    if(status==XR_XIR_OK)status=xr_xir_compile_method_signature_verify(c,&f.module,0);
    if(status==XR_XIR_OK)status=xr_xir_compile_implementations_verify(c,&f.module);
    if(status==XR_XIR_OK)status=batch_use(c,&f);
    if(status==XR_XIR_OK)status=batch_original_queries(c,&f);
    return status;
}
static void batch_authority(void) {
    BatchFixture f;batch_fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(batch_sequence(&c)==XR_XIR_OK);
    CHECK(batch_use(&c,&f)==XR_XIR_OK && batch_original_queries(&c,&f)==XR_XIR_OK);
    f.goals[1].markers|=XR_XIR_CONSTRAINT_ERROR;
    CHECK(batch_use(&c,&f)==XR_XIR_BAD_TYPE && batch_original_queries(&c,&f)==XR_XIR_BAD_TYPE);
    f.goals[1]=f.own_bound;f.own_bound.markers=0;
    CHECK(batch_use(&c,&f)==XR_XIR_BAD_TYPE); /* Goals cannot seed themselves. */
    f.own_bound.markers=XR_XIR_CONSTRAINT_SENDABLE;
    f.goals[0].markers|=XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(batch_use(&c,&f)==XR_XIR_BAD_TYPE); /* Own facts cannot leak to parent. */
    f.goals[0]=f.nominal_bound;CHECK(batch_use(&c,&f)==XR_XIR_OK);
    f.binding.requirement.declaration=1;
    CHECK(batch_use(&c,&f)==XR_XIR_BAD_TYPE); /* Different origin supplies no own bound. */
    CHECK(xr_xir_compile_implementations_verify(&c,&f.module)==XR_XIR_BAD_TYPE);
    f.binding.requirement.declaration=0;
    XrXirProofContext wrong={&f.module,{XR_XIR_CONTEXT_CONFORMANCE_METHOD,0,1}};
    XirConstraintArguments use={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},f.arguments,2};
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,&wrong,&use)==XR_XIR_BAD_STRUCTURE);
    f.identity.member_access=XR_XIR_MEMBER_PRIVATE;
    CHECK(batch_use(&c,&f)==XR_XIR_BAD_TYPE);
    f.identity.member_access=XR_XIR_MEMBER_PUBLIC;CHECK(batch_use(&c,&f)==XR_XIR_OK);
    owner_free(&c,baseline);
}
static void batch_result_and_empty(void) {
    BatchFixture f;batch_fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    XrXirProofContext closed={&f.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    uint32_t kinds[2]={XR_XIR_BINDER_TYPE,XR_XIR_BINDER_RESULT_VARIABLE};
    XrXirType arguments[2]={XR_XIR_I64,XR_XIR_UNIT};
    f.goals[0].markers=XR_XIR_CONSTRAINT_EQUAL;f.goals[1]=(XrXirConstraint){0};f.generic.parameter_kinds=kinds;
    XirConstraintArguments use={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},arguments,2};
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,&closed,&use)==XR_XIR_OK);
    XrXirConstraintUse single={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},1,arguments,2};
    CHECK(xr_xir_compile_constraints_prove(&c,&closed,&single)==XR_XIR_OK);
    f.goals[1].markers=XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,&closed,&use)==XR_XIR_BAD_TYPE);
    f.goals[1].markers=0;kinds[1]=XR_XIR_BINDER_TYPE;
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,&closed,&use)==XR_XIR_BAD_TYPE);
    kinds[1]=XR_XIR_BINDER_RESULT_VARIABLE;arguments[0]=XR_XIR_UNIT;
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,&closed,&use)==XR_XIR_BAD_TYPE);
    f.generic=(XrXirGeneric){0};use.arguments=NULL;use.argument_count=0;
    uint64_t allocations=stats(&c).allocation_count,work=stats(&c).work;
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,NULL,&use)==XR_XIR_OK);
    CHECK(stats(&c).allocation_count==allocations && stats(&c).work==work);
    use.arguments=arguments;CHECK(xr_xir_compile_constraint_arguments_prove(&c,NULL,&use)==XR_XIR_BAD_STRUCTURE);
    use.arguments=NULL;f.generic.parameter_count=1;f.generic.constraints=f.goals;
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,NULL,&use)==XR_XIR_BAD_STRUCTURE);
    f.generic=(XrXirGeneric){0};single.arguments=NULL;single.argument_count=0;single.parameter=0;
    CHECK(xr_xir_compile_constraints_prove(&c,&closed,&single)==XR_XIR_BAD_STRUCTURE);
    owner_free(&c,baseline);
}
static XrXirStatus batch_constructed(const XrXirCompileContext *c,unsigned bad) {
    BatchFixture f;batch_fixture(&f);XrXirConstraint goals[9]={{0}};XrXirType arguments[9];
    for(uint32_t a=0;a<9;++a) {
        arguments[a]=constructed(2);goals[a].markers=a?XR_XIR_CONSTRAINT_SENDABLE:XR_XIR_CONSTRAINT_EQUAL;
    }
    f.generic.constraints=goals;f.generic.parameter_count=bad?2u:9u;
    f.module.declarations=NULL;f.function.parameters=NULL;f.function.parameter_count=0;
    XrXirProofContext closed={&f.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XirConstraintArguments use={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},arguments,f.generic.parameter_count};
    if(bad) {
        /* Explicit malformed-query negative: TYPE(root) appends its bad child
         * after valid R0, and the child must fail before R1 is enqueued. */
        f.nodes[2].element=constructed(0);f.nodes[0].parameter_count=1;
        goals[0].markers=0;XrXirInterfaceApplication missing={0,arguments,1};
        goals[1]=(XrXirConstraint){.interfaces=&missing,.interface_count=1};
        XrXirConstraintUse original={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},0,arguments,2};
        XrXirStatus status=xr_xir_compile_constraints_prove(c,&closed,&original);
        if(status!=XR_XIR_BAD_STRUCTURE)return status;
    }
    return xr_xir_compile_constraint_arguments_prove(c,&closed,&use);
}
static void batch_first_failure(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(batch_constructed(&c,0)==XR_XIR_OK);CHECK(batch_constructed(&c,1)==XR_XIR_BAD_STRUCTURE);
    /* Nine roots fit the formal vector. An unsatisfied first goal must not
     * allocate queue space for any later binder and hide BAD_TYPE behind OOM. */
    BatchFixture f;batch_fixture(&f);XrXirConstraint goals[9]={{0}};
    XrXirType arguments[9];for(uint32_t a=0;a<9;++a)arguments[a]=XR_XIR_I64;
    goals[0].markers=XR_XIR_CONSTRAINT_ERROR;f.generic.constraints=goals;f.generic.parameter_count=9;
    f.module.declarations=NULL;f.function.parameters=NULL;f.function.parameter_count=0;
    XrXirProofContext closed={&f.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XirConstraintArguments use={&f.module,{XR_XIR_CONTEXT_FUNCTION,0,0},arguments,9};
    attempts=0;injected=false;fail_at=1;
    CHECK(xr_xir_compile_constraint_arguments_prove(&c,&closed,&use)==XR_XIR_BAD_TYPE);
    CHECK(!injected && attempts==1);fail_at=SIZE_MAX;
    owner_free(&c,baseline);
}
static XrXirStatus batch_resource_operation(const XrXirCompileContext *c,unsigned mode) {
    if(mode)return batch_constructed(c,0);
    BatchFixture f;batch_fixture(&f);return batch_use(c,&f);
}
static XrCompileResourceStats batch_measured(XrCompileResourceLimits limits,XrXirStatus expected,unsigned mode) {
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    CHECK(batch_resource_operation(&c,mode)==expected);XrCompileResourceStats s=stats(&c);owner_free(&c,baseline);return s;
}
static void batch_resources(void) {
    for(unsigned mode=0;mode<2;++mode) {
        size_t sites=0;
        for(size_t pass=0;pass<=sites;++pass) {
            XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
            attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=batch_resource_operation(&c,mode);size_t actual=attempts;fail_at=SIZE_MAX;
            if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<1024);}
            else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
            owner_free(&c,baseline);
        }
        XrCompileResourceStats a=batch_measured(caps(),XR_XIR_OK,mode);
        XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
        XrCompileResourceStats b=batch_measured(exact,XR_XIR_OK,mode);
        CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
        for(unsigned axis=0;axis<3;++axis) {
            XrCompileResourceLimits less=exact;
            if(!axis)--less.allocated_bytes;else if(axis==1)--less.live_bytes;else --less.work;
            (void)batch_measured(less,XR_XIR_BUDGET,mode);
        }
        for(uint64_t cut=1;cut<a.work;++cut) {
            XrCompileResourceLimits limited=caps();limited.work=cut;
            XrCompileResourceStats result=batch_measured(limited,XR_XIR_BUDGET,mode);CHECK(result.work<=cut);
        }
        printf("declaration whole arguments mode=%u actual OOM sites=%zu work=%llu physical=0/0\n",
            mode,sites,(unsigned long long)a.work);
    }
}
static void batch_tests(void) {
    batch_authority();batch_result_and_empty();batch_first_failure();batch_resources();
}

int main(void){scopes();requirements();faults();limits();batch_tests();puts("declaration scratch fresh scope, marker, signature, actual OOM and finite axes PASS");return 0;}
