/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_conversion.c - Complete conversions survive syntax retirement and lowering
 */
#include "hir/xhir_conversion.h"
#include "xir/xxir_types.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "frontend/parser/xparse.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static XrXirCompileContext context_new(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);return context;
}
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats.held_bytes;
}
static XrHirConversion checked(const XrHirConversionContext *context,XrXirType actual,XrXirType expected,
    bool inferred,bool argument,XrXirStatus wanted) {
    XrHirConversionInput input={.actual=actual,.expected=expected,.has_expected=true,.inferred=inferred,.argument_bound=argument};
    XrHirConversion output={0};memset(&output,0xa5,sizeof(output));XrHirConversion untouched=output;const char *reason=NULL;
    CHECK(xr_hir_conversion_check(context,&input,&output,&reason)==wanted);
    if(wanted!=XR_XIR_OK)CHECK(reason && !memcmp(&output,&untouched,sizeof(output)));else CHECK(!reason);
    return output;
}
static void relations(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);
    const XrXirCallableParameter parameter={XR_XIR_I64,0},field={(XrXirType)257,0};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
            .flags=XR_XIR_CALLABLE_ROOT_NONE|XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,
            .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)257},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=&field,.parameter_count=1}};
    XrXirTypes types={.nodes=nodes,.count=5};XrHirConversionContext context={.compile=&owner,.types=&types};
    XrHirConversion node=checked(&context,XR_XIR_I8,XR_XIR_I64,false,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_NUMBER && node.source==XR_XIR_I8 && node.target==XR_XIR_I64);
    CHECK(xr_hir_conversion_instruction(&node,7).op==XR_XIR_CONVERT_NUMBER);
    node=checked(&context,XR_XIR_F32,XR_XIR_F64,false,false,XR_XIR_OK);CHECK(node.kind==XR_HIR_CONVERSION_NUMBER);
    (void)checked(&context,XR_XIR_I64,XR_XIR_I8,false,false,XR_XIR_BAD_TYPE);
    (void)checked(&context,XR_XIR_I8,XR_XIR_U16,false,false,XR_XIR_BAD_TYPE);
    (void)checked(&context,XR_XIR_I64,XR_XIR_F64,false,false,XR_XIR_BAD_TYPE);
    node=checked(&context,XR_XIR_I64,(XrXirType)256,false,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_NULLABLE && !node.copy_nullable_element);
    node=checked(&context,(XrXirType)257,(XrXirType)259,false,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_NULLABLE && node.copy_nullable_element && node.root==XR_HIR_CONVERSION_ROOT_NONE);
    node=checked(&context,(XrXirType)257,(XrXirType)257,false,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_CALLABLE && node.root==XR_HIR_CONVERSION_ROOT_FIXED);
    node=checked(&context,(XrXirType)257,(XrXirType)257,true,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_COPY && node.root==XR_HIR_CONVERSION_ROOT_INFERRED);
    node=checked(&context,(XrXirType)260,(XrXirType)260,false,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_COPY && node.root==XR_HIR_CONVERSION_ROOT_FIXED);
    node=checked(&context,(XrXirType)257,(XrXirType)258,false,false,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_CALLABLE && node.target==(XrXirType)258);
    (void)checked(&context,(XrXirType)258,(XrXirType)257,false,false,XR_XIR_BAD_TYPE);
    node=checked(&context,(XrXirType)257,(XrXirType)258,false,true,XR_XIR_OK);
    CHECK(node.kind==XR_HIR_CONVERSION_COPY && node.source==node.target && node.target==(XrXirType)257 &&
        node.root==XR_HIR_CONVERSION_ROOT_ARGUMENT && node.declared_bound==(XrXirType)258);
    nodes[0].element=(XrXirType)256;bool contains=true;const char *reason=NULL;
    CHECK(xr_hir_conversion_contains_callable(&types,(XrXirType)256,NULL,&contains,&reason)==XR_XIR_BUDGET &&
        contains && reason && !strcmp(reason,"callable flow nesting exhausted"));
    CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);
}
typedef struct Intern {
    XrXirTypes types;
    XrCompileResources *resources;
    XrXirTypeNode *nodes;
    bool fail;
} Intern;
static XrXirStatus relocate(void *context,const XrXirTypeNode *node,XrXirType *output) {
    Intern *pool=context;if(pool->fail)return XR_XIR_OUT_OF_MEMORY;
    XrXirTypeNode *next=NULL;
    CHECK(xr_compile_resources_calloc(pool->resources,pool->types.count+1,sizeof(*next),(void **)&next)==XR_COMPILE_RESOURCE_OK);
    memcpy(next,pool->nodes,pool->types.count*sizeof(*next));next[pool->types.count]=*node;
    memset(pool->nodes,0xa5,pool->types.count*sizeof(*next));xr_compile_resources_free(pool->nodes);
    pool->nodes=next;pool->types.nodes=next;*output=(XrXirType)(256+pool->types.count++);return XR_XIR_OK;
}
static void open_result(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);Intern pool={.resources=owner.resources};
    CHECK(xr_compile_resources_calloc(owner.resources,2,sizeof(*pool.nodes),(void **)&pool.nodes)==XR_COMPILE_RESOURCE_OK);
    pool.nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    pool.nodes[1]=pool.nodes[0];pool.nodes[1].result=XR_XIR_UNIT;pool.nodes[1].flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    pool.types.nodes=pool.nodes;pool.types.count=2;
    XrHirConversionContext context={.compile=&owner,.types=&pool.types,.intern_owner=&pool,.intern=relocate};
    XrHirConversionInput input={.actual=(XrXirType)256,.expected=(XrXirType)257,.has_expected=true,.infer_result=true};
    XrHirConversion node={0};memset(&node,0xa5,sizeof(node));XrHirConversion untouched=node;const char *reason=NULL;
    pool.fail=true;
    CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_OUT_OF_MEMORY && !memcmp(&node,&untouched,sizeof(node)));
    pool.fail=false;
    CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_OK && node.target==(XrXirType)258 &&
        pool.types.count==3 && pool.nodes[2].result==XR_XIR_I64 && node.kind==XR_HIR_CONVERSION_CALLABLE);
    xr_compile_resources_free(pool.nodes);CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);
}
typedef struct Cancel {unsigned polls,trigger;} Cancel;
static bool cancel(const void *context) {Cancel *state=(Cancel *)context;++state->polls;return state->trigger && state->polls>=state->trigger;}
static void controls(void) {
    XrXirCompileContext owner=context_new();XrXirTypes types={0};
    XrHirConversionInput input={.actual=XR_XIR_I8,.expected=XR_XIR_I64,.has_expected=true};
    XrHirConversion node={0};memset(&node,0xa5,sizeof(node));XrHirConversion untouched=node;const char *reason=NULL;
    Cancel state={0,2};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrHirConversionContext context={.compile=&owner,.types=&types,.control=&control};
    CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_CANCELLED && state.polls==2 && !memcmp(&node,&untouched,sizeof(node)));
    state=(Cancel){0};
    CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_CANCELLED && !state.polls && !memcmp(&node,&untouched,sizeof(node)));
    control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_OK && node.target==XR_XIR_I64);
    node=untouched;control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_DEADLINE && !memcmp(&node,&untouched,sizeof(node)));
    context.control=NULL;CHECK(xr_hir_conversion_check(&context,&input,&node,&reason)==XR_XIR_OK);
    xr_compile_resources_release(owner.resources);
}
static XrHirConversion retired_conversion(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.resources,&session)==XR_COMPILE_RESOURCE_OK);
    char source[]="const narrow:i8=42\r\nconst wide:i64=narrow\r\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK && program && program->as.program.count==2);
    AstNode *node=program->as.program.statements[1]->as.var_decl.initializer;
    CHECK(node && node->type==AST_VARIABLE && node->node_id && node->line==2);
    XrXirTypes types={0};XrHirConversionContext context={.compile=&owner,.types=&types};
    XrHirConversionInput input={.origin={node->node_id,0,node->line,node->column,node->end_line,node->end_column},
        .actual=XR_XIR_I8,.expected=XR_XIR_I64,.has_expected=true};XrHirConversion conversion={0};
    CHECK(xr_hir_conversion_check(&context,&input,&conversion,NULL)==XR_XIR_OK && conversion.origin.node_id==node->node_id && conversion.origin.line==2);
    memset(source,'!',sizeof(source));memset(&input,0xa5,sizeof(input));memset(&context,0xa5,sizeof(context));
    xr_compile_session_free(session);xr_program_destroy(program);CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);
    return conversion;
}
static void built_after_retirement(void) {
    XrHirConversion conversion=retired_conversion();XrXirCompileContext receiver=context_new();uint64_t before=held(receiver.resources);
    XrXirInstruction ret={.op=XR_XIR_RETURN},answer[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I8,.immediate=42},xr_hir_conversion_instruction(&conversion,0),{.op=XR_XIR_RETURN,.args={1}}};
    XrXirBlock one={0,1,0,0},three={0,3,0,0};
    XrXirFunction functions[]={
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0,XR_XIR_NEVER},
        {"entry",5,NULL,0,XR_XIR_I64,&three,1,answer,3,NULL,0,XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirSourceModule module={.name="conversion",.name_length=10,.storage=XR_XIR_MODULE_FRESH_STORAGE};
    XrXirDeclarations declarations={&module,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new_v4(&receiver,NULL,&(XrXirConstructionSpec){NULL,0,NULL,0,NULL},&(XrXirSlotFacts){NULL,0},&construction)==XR_XIR_OK);
    XrXirArtifact *checked_artifact=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_check_v2(&receiver,&built,construction,&checked_artifact,NULL)==XR_XIR_OK);xr_xir_compile_construction_free(construction);
    CHECK(xr_xir_compile_specialize(checked_artifact,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked_artifact);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirRunContext run={1024,1024,0,0,0,0};XrXirValue value={0};
    CHECK(xr_xir_compile_vm_run(lowered,1,&run,NULL,0,&value)==XR_XIR_RUN_OK && value.type==XR_XIR_I64 && value.payload==42);
    CHECK(!run.live_bytes && run.allocations==run.frees);
    xr_xir_value_drop(&value);xr_xir_compile_artifact_free(lowered);CHECK(held(receiver.resources)==before);xr_compile_resources_release(receiver.resources);
}
int main(void) {
    relations();open_result();controls();built_after_retirement();
    puts("Typed conversion HIR: complete contextual rules, preserved authority, controls and retired syntax to Built/VM PASS");return 0;
}
