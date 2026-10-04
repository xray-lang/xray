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
int main(void){scopes();requirements();faults();limits();puts("declaration scratch fresh scope, marker, signature, actual OOM and finite axes PASS");return 0;}
