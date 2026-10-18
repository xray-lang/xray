/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_reference_root_operation_cases.h - Fresh lexical admission and real reference masks
 */
#ifndef XIR_REFERENCE_ROOT_OPERATION_CASES_H
#define XIR_REFERENCE_ROOT_OPERATION_CASES_H
#include "xir/xxir_callable_root_obligations.inc.c"
#include "xir_reference_root_scalar_cases.h"
typedef struct RootOperationCase {
    const XrXirModule *module;const XrXirEffects *effects;
    uint32_t repeats,count,masks[4],actual[64];
} RootOperationCase;
static const uint32_t root_operation_functions[]={2,2,5,7};
static const uint32_t root_operation_instructions[]={0,1,1,0};
static XrXirStatus root_operation_read(void *data,XirEffectContextScope *scope) {
    RootOperationCase *c=data;
    for(uint32_t r=0;r<c->repeats;++r)for(uint32_t n=0;n<c->count;++n) {
        uint32_t held=UINT32_MAX,out=held;
        XrXirStatus status=xir_effects_reference_root_scoped(scope,root_operation_functions[n],
            root_operation_instructions[n],&out);
        if(status!=XR_XIR_OK) {CHECK(out==held);return status;}
        CHECK(out==c->masks[n]);c->actual[r*c->count+n]=out;
    }
    return XR_XIR_OK;
}
static XrXirStatus root_operation_ignore_error(void *data,XirEffectContextScope *scope) {
    RootOperationCase *c=data;uint32_t out=UINT32_MAX;
    XrXirStatus status=xir_effects_reference_root_scoped(scope,c->module->function_count,0,&out);
    CHECK(status==XR_XIR_BAD_STRUCTURE && out==UINT32_MAX);
    XrCompileResourceStats before=rp_stats(scope->context);size_t attempts=rp_attempts;
    CHECK(xir_effects_reference_root_scoped(scope,2,0,&out)==status && out==UINT32_MAX);
    XrCompileResourceStats after=rp_stats(scope->context);
    CHECK(!memcmp(&before,&after,sizeof(before)) && rp_attempts==attempts);return XR_XIR_OK;
}
static XrXirStatus root_operation_run(const XrXirCompileContext *context,RootOperationCase *c) {
    return xir_effects_context_operation(context,c->effects,c->module,c->module->types,root_operation_read,c);
}
static void root_operation_case(uint32_t generic,uint32_t repeats,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirTypeNode nodes[2]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
         .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED|XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT}};
    XrXirTypes types={nodes,2,NULL,NULL};
    XrXirInstruction outer[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=1,.targets={1,0}},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction child[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=2,.targets={1,0}},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction nested[3]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=4},{.op=XR_XIR_RETURN}};
    XrXirInstruction pure={.op=XR_XIR_RETURN};
    XrXirInstruction shared[2]={{.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257},{.op=XR_XIR_RETURN}};
    XrXirInstruction entry[4]={{.op=XR_XIR_CALL,.immediate=0},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction init[3]={{.op=XR_XIR_CELL_NEW,.type=(XrXirType)257},
        {.op=XR_XIR_SLOT_INIT},{.op=XR_XIR_RETURN}};
    XrXirBlock split[2]={{.count=1},{.first=1,.count=1,.frontier=1}},one={.count=1},two={.count=2},three={.count=3},four={.count=4};
    XrXirFunction functions[8]={
        {.name="outer",.name_length=5,.blocks=split,.block_count=2,.instructions=outer,.instruction_count=2},
        {.name="child",.name_length=5,.blocks=split,.block_count=2,.instructions=child,.instruction_count=2},
        {.name="nested",.name_length=6,.blocks=&three,.block_count=1,.instructions=nested,.instruction_count=3},
        {.name="pure",.name_length=4,.blocks=&one,.block_count=1,.instructions=&pure,.instruction_count=1},
        {.name="shared",.name_length=6,.blocks=&two,.block_count=1,.instructions=shared,.instruction_count=2},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&four,.block_count=1,
         .instructions=entry,.instruction_count=4},
        {.name="init",.name_length=4,.blocks=&three,.block_count=1,.instructions=init,.instruction_count=3}};
    XrXirFunctionIdentity identities[8]={[1]={.cleanup_owner=1},[2]={.cleanup_owner=2},
        [3]={.promises=XR_XIR_FUNCTION_NO_SUSPEND},[4]={.promises=XR_XIR_FUNCTION_NO_SUSPEND}};
    XrXirSourceModule identity={"nested-reference",16,NULL,0,6};
    XrXirSlot slot={0,(XrXirType)257,1};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,
        .entry_function=5,.slots=&slot,.slot_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=7,
        .types=&types,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};

    XrXirInstruction generic_body[2]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3},{.op=XR_XIR_RETURN}};
    XrXirConstraint constraint={0};XrXirGeneric generics[8]={[7]={.constraints=&constraint,.parameter_count=1}};
    if(generic) {
        functions[7]=(XrXirFunction){.name="generic",.name_length=7,.blocks=&two,.block_count=1,
            .instructions=generic_body,.instruction_count=2};module.function_count=8;module.generics=generics;
    }
    XrXirArtifact *checked=NULL;EffectContextOwner *owner=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_check(&context,&module,&checked,&diagnostic)==XR_XIR_OK);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    CHECK(effect_context_owner_build(&context,owned,NULL,NULL,&owner)==XR_XIR_OK);
    XrXirEffects *root_effects=NULL;CHECK(xr_xir_compile_effects_analyze(checked,&root_effects)==XR_XIR_OK);
    XrXirEffects effects={.resources=context.resources,.count=module.function_count,
        .outcomes_work={.contexts=owner,.root=effect_roots_read(root_effects)->root}};
    RootOperationCase c={.module=owned,.effects=&effects,.repeats=repeats,.count=generic?4u:3u,
        .masks={0,XR_XIR_CALLABLE_ROOT_REQUIRED,0,0}};
    for(uint32_t n=0;n<c.count;++n) {
        uint32_t value=UINT32_MAX;CHECK(root_scalar_reference(&context,&effects,owned,
            root_operation_functions[n],root_operation_instructions[n],&value)==XR_XIR_OK && value==c.masks[n]);
    }
    CHECK(root_scalar_declaration(owned,&effects,&context,&diagnostic)==XR_XIR_OK);
    CHECK(declaration_callable_root_verify(owned,&effects,&context,&diagnostic)==XR_XIR_OK);
    XrCompileResourceStats before=rp_stats(&context);size_t start=rp_attempts;
    CHECK(root_operation_run(&context,&c)==XR_XIR_OK);size_t sites=rp_attempts-start;
    XrCompileResourceStats after=rp_stats(&context);uint64_t required=after.work-before.work;
    fprintf(stdout,"ROOT_OPERATION generic%u repeats%u work%llu sites%zu\n",generic,repeats,(unsigned long long)required,sites);
    for(size_t site=0;site<sites;++site) {
        uint64_t live=rp_stats(&context).live_bytes;size_t blocks=rp_live,bytes=rp_live_bytes;
        rp_injected=false;rp_fail_at=rp_attempts+site;
        CHECK(root_operation_run(&context,&c)==XR_XIR_OUT_OF_MEMORY && rp_injected);
        rp_fail_at=SIZE_MAX;CHECK(rp_stats(&context).live_bytes==live && rp_live==blocks && rp_live_bytes==bytes);
    }
    CHECK(xir_effects_context_operation(&context,&effects,owned,owned->types,root_operation_ignore_error,&c)==XR_XIR_BAD_STRUCTURE);
    XrXirInstruction *changed=(XrXirInstruction *)owned->functions[3].instructions;
    int64_t immediate=changed[0].immediate;changed[0].immediate^=1;
    CHECK(root_operation_run(&context,&c)==XR_XIR_BAD_STRUCTURE);changed[0].immediate=immediate;
    CHECK(root_operation_run(&context,&c)==XR_XIR_OK);
    for(uint32_t n=0;n<c.count;++n) {
        uint32_t value=UINT32_MAX;CHECK(xir_effects_reference_root(&context,&effects,owned,
            root_operation_functions[n],root_operation_instructions[n],&value)==XR_XIR_OK && value==c.masks[n]);
    }
    uint32_t untouched=UINT32_MAX;
    CHECK(xir_effects_reference_root_scoped(NULL,2,0,&untouched)==XR_XIR_BAD_STRUCTURE && untouched==UINT32_MAX);
    XrXirModule *mutable=(XrXirModule *)owned;const XrXirFunction *saved_functions=mutable->functions;
    mutable->functions=NULL;
    CHECK(xir_effects_reference_root(&context,&effects,owned,2,0,&untouched)==XR_XIR_BAD_STRUCTURE && untouched==UINT32_MAX);
    CHECK(root_operation_run(&context,&c)==XR_XIR_BAD_STRUCTURE);mutable->functions=saved_functions;
    XrXirFunction *mutable_functions=(XrXirFunction *)owned->functions;
    const XrXirInstruction *saved_instructions=mutable_functions[2].instructions;
    mutable_functions[2].instructions=NULL;
    CHECK(xir_effects_reference_root(&context,&effects,owned,2,0,&untouched)==XR_XIR_BAD_STRUCTURE && untouched==UINT32_MAX);
    CHECK(root_operation_run(&context,&c)==XR_XIR_BAD_STRUCTURE);mutable_functions[2].instructions=saved_instructions;
    XrXirTypeNode *mutable_nodes=(XrXirTypeNode *)owned->types->nodes;uint32_t saved_flags=mutable_nodes[0].flags;
    mutable_nodes[0].flags&=~XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    XrXirDiagnostic old_diag={0},new_diag={0};
    XrXirStatus old_status=root_scalar_declaration(owned,&effects,&context,&old_diag);
    XrXirStatus new_status=declaration_callable_root_verify(owned,&effects,&context,&new_diag);
    CHECK(old_status!=XR_XIR_OK && old_status==new_status);
    mutable_nodes[0].flags=saved_flags;CHECK(root_operation_run(&context,&c)==XR_XIR_OK);
    uint64_t work=rp_stats(&context).work,limit=rp_caps().work;
    CHECK(less<2 && work+required-less<limit &&
        xr_compile_resources_work(context.resources,limit-work-required+less)==XR_COMPILE_RESOURCE_OK);
    CHECK(root_operation_run(&context,&c)==(less?XR_XIR_BUDGET:XR_XIR_OK));
    effect_context_owner_free(owner);xr_xir_compile_effects_free(root_effects);xr_xir_compile_artifact_free(checked);
    rp_injected=false;rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
static void root_operation_cases(void) {
    for(uint32_t generic=0;generic<2;++generic)for(uint32_t repeats=1;repeats<=16;repeats*=4)
        for(uint32_t less=0;less<2;++less) root_operation_case(generic,repeats,less);
    puts("ROOT_OPERATION current full admission, real masks and lexical owners, generic fallback, first error latch, fresh invocation, all allocations, exact/minus1, null storage, narrowed bound and physical0 PASS");
}

#endif // XIR_REFERENCE_ROOT_OPERATION_CASES_H
