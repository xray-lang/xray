/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_binary.c - Typed operators preserve declaration authority and retired syntax
 */
#include "hir/xhir_binary.h"
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
static XrHirBinary checked(const XrHirConversionContext *context,XrHirBinaryOperator operation,
    XrXirType left,XrXirType right,XrXirStatus wanted,const char *message) {
    XrHirBinaryInput input={.operation=operation,.left=left,.right=right};
    XrHirBinary output={0};memset(&output,0xa5,sizeof(output));XrHirBinary untouched=output;const char *reason=NULL;
    CHECK(xr_hir_binary_check(context,&input,&output,&reason)==wanted);
    if(wanted!=XR_XIR_OK)CHECK(reason && !strcmp(reason,message) && !memcmp(&output,&untouched,sizeof(output)));
    else CHECK(!reason);
    return output;
}
static void relations(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);
    XrXirTypeNode nullable={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};
    XrXirTypes types={.nodes=&nullable,.count=1};XrXirModule module={.types=&types};
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XrHirConversionContext context={.compile=&owner,.types=&types,.proof=&proof};
    const XrXirOp integer[]={XR_XIR_ADD_INT,XR_XIR_SUB_INT,XR_XIR_MUL_INT,XR_XIR_DIV_INT,XR_XIR_REM_INT,
        XR_XIR_AND_INT,XR_XIR_OR_INT,XR_XIR_XOR_INT,XR_XIR_SHL_INT,XR_XIR_SHR_INT};
    for(unsigned i=0;i<sizeof(integer)/sizeof(integer[0]);++i) {
        XrHirBinary node=checked(&context,(XrHirBinaryOperator)i,XR_XIR_I8,XR_XIR_I64,XR_XIR_OK,NULL);
        CHECK(node.operation==integer[i] && node.result==(i>=XR_HIR_BINARY_SHL?XR_XIR_I8:XR_XIR_I64));
        CHECK(node.right.target==XR_XIR_I64);
        CHECK(node.left.kind==(i>=XR_HIR_BINARY_SHL?XR_HIR_CONVERSION_IDENTITY:XR_HIR_CONVERSION_NUMBER));
    }
    const XrHirBinaryOperator order[]={XR_HIR_BINARY_LT,XR_HIR_BINARY_LE,XR_HIR_BINARY_GT,XR_HIR_BINARY_GE};
    const XrXirOp ordered[][3]={{XR_XIR_LT_INT,XR_XIR_LT_FLOAT,XR_XIR_LT_STRING},
        {XR_XIR_LE_INT,XR_XIR_LE_FLOAT,XR_XIR_LE_STRING},{XR_XIR_GT_INT,XR_XIR_GT_FLOAT,XR_XIR_GT_STRING},
        {XR_XIR_GE_INT,XR_XIR_GE_FLOAT,XR_XIR_GE_STRING}};
    const XrXirType left[]={XR_XIR_U8,XR_XIR_F32,XR_XIR_STRING},right[]={XR_XIR_U64,XR_XIR_F64,XR_XIR_STRING};
    for(unsigned i=0;i<4;++i)for(unsigned k=0;k<3;++k) {
        XrHirBinary node=checked(&context,order[i],left[k],right[k],XR_XIR_OK,NULL);
        CHECK(node.operation==ordered[i][k] && node.result==XR_XIR_BOOL);
    }
    const XrXirOp floating[]={XR_XIR_ADD_FLOAT,XR_XIR_SUB_FLOAT,XR_XIR_MUL_FLOAT,XR_XIR_DIV_FLOAT};
    for(unsigned i=0;i<4;++i) {
        XrHirBinary node=checked(&context,(XrHirBinaryOperator)i,XR_XIR_F32,XR_XIR_F64,XR_XIR_OK,NULL);
        CHECK(node.operation==floating[i] && node.result==XR_XIR_F64 && node.left.target==XR_XIR_F64);
    }
    CHECK(checked(&context,XR_HIR_BINARY_ADD,XR_XIR_STRING,XR_XIR_STRING,XR_XIR_OK,NULL).operation==XR_XIR_CONCAT_STRING);
    XrHirBinary node=checked(&context,XR_HIR_BINARY_NE,XR_XIR_I64,(XrXirType)256,XR_XIR_OK,NULL);
    CHECK(node.operation==XR_XIR_EQUAL && node.immediate==1 && node.result==XR_XIR_BOOL && node.left.kind==XR_HIR_CONVERSION_NULLABLE);
    node=checked(&context,XR_HIR_BINARY_EQ,(XrXirType)256,XR_XIR_I64,XR_XIR_OK,NULL);
    CHECK(node.right.kind==XR_HIR_CONVERSION_NULLABLE && !node.immediate);
    (void)checked(&context,XR_HIR_BINARY_EQ,XR_XIR_I8,XR_XIR_I64,XR_XIR_BAD_TYPE,"equality requires the same declared operand type");
    (void)checked(&context,XR_HIR_BINARY_ADD,XR_XIR_I8,XR_XIR_U8,XR_XIR_BAD_TYPE,"integer signedness requires an explicit cast");
    (void)checked(&context,XR_HIR_BINARY_ADD,XR_XIR_I64,XR_XIR_F64,XR_XIR_BAD_TYPE,"operator requires a declared concrete operand contract");
    (void)checked(&context,XR_HIR_BINARY_REM,XR_XIR_F32,XR_XIR_F64,XR_XIR_BAD_TYPE,"operator is not defined for floating operands");
    (void)checked(&context,XR_HIR_BINARY_SUB,XR_XIR_STRING,XR_XIR_STRING,XR_XIR_BAD_TYPE,"operator is not defined for string operands");
    (void)checked(&context,(XrHirBinaryOperator)99,XR_XIR_I64,XR_XIR_I64,XR_XIR_BAD_STRUCTURE,"unknown arithmetic operator");
    XrXirConstraint constraint={0};XrXirGeneric generic={.constraints=&constraint,.parameter_count=1};
    XrXirFunction function={0};module.functions=&function;module.function_count=1;module.generics=&generic;
    proof.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,0,0};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    (void)checked(&context,XR_HIR_BINARY_EQ,parameter,parameter,XR_XIR_BAD_TYPE,"equality requires its declaration predicate");
    constraint.markers=XR_XIR_CONSTRAINT_EQUAL;
    CHECK(checked(&context,XR_HIR_BINARY_EQ,parameter,parameter,XR_XIR_OK,NULL).operation==XR_XIR_EQUAL);
    CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);
}
typedef struct Cancel {unsigned polls,trigger;} Cancel;
static bool cancel(const void *context) {Cancel *state=(Cancel *)context;++state->polls;return state->trigger && state->polls>=state->trigger;}
static void controls(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);XrXirTypes types={0};
    XrHirBinaryInput input={.operation=XR_HIR_BINARY_ADD,.left=XR_XIR_I8,.right=XR_XIR_I64};
    XrHirBinary node={0};memset(&node,0xa5,sizeof(node));XrHirBinary untouched=node;const char *reason=NULL;
    Cancel state={0,3};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrHirConversionContext context={.compile=&owner,.types=&types,.control=&control};
    CHECK(xr_hir_binary_check(&context,&input,&node,&reason)==XR_XIR_CANCELLED && state.polls==3 && !memcmp(&node,&untouched,sizeof(node)));
    state=(Cancel){0};CHECK(xr_hir_binary_check(&context,&input,&node,&reason)==XR_XIR_CANCELLED && !state.polls && !memcmp(&node,&untouched,sizeof(node)));
    control=(XrCompileRequestControl){0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_binary_check(&context,&input,&node,&reason)==XR_XIR_OK && node.result==XR_XIR_I64);
    node=untouched;control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_binary_check(&context,&input,&node,&reason)==XR_XIR_DEADLINE && !memcmp(&node,&untouched,sizeof(node)));
    context.control=NULL;CHECK(xr_hir_binary_check(&context,&input,&node,&reason)==XR_XIR_OK);
    CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);
}
static XrHirBinary retired_binary(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.resources,&session)==XR_COMPILE_RESOURCE_OK);
    char source[]="const answer=11+31\r\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK && program && program->as.program.count==1);
    AstNode *syntax=program->as.program.statements[0]->as.var_decl.initializer;CHECK(syntax && syntax->type==AST_BINARY_ADD && syntax->node_id);
    XrXirTypes types={0};XrHirConversionContext context={.compile=&owner,.types=&types};
    XrHirBinaryInput input={.origin={syntax->node_id,7,syntax->line,syntax->column,syntax->end_line,syntax->end_column},
        .operation=XR_HIR_BINARY_ADD,.left=XR_XIR_I8,.right=XR_XIR_I64};XrHirBinary node={0};
    CHECK(xr_hir_binary_check(&context,&input,&node,NULL)==XR_XIR_OK && node.origin.node_id==syntax->node_id && node.origin.module==7);
    CHECK(node.origin.column==syntax->column && node.origin.end_column==syntax->end_column);
    memset(source,'!',sizeof(source));memset(&input,0xa5,sizeof(input));memset(&context,0xa5,sizeof(context));
    xr_compile_session_free(session);xr_program_destroy(program);CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);return node;
}
static void built_after_retirement(void) {
    XrHirBinary node=retired_binary();XrXirCompileContext receiver=context_new();uint64_t before=held(receiver.resources);
    XrXirInstruction ret={.op=XR_XIR_RETURN},answer[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I8,.immediate=11},{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=31},
        xr_hir_conversion_instruction(&node.left,0),xr_hir_binary_instruction(&node,2,1),{.op=XR_XIR_RETURN,.args={3}}};
    XrXirBlock one={0,1,0,0},five={0,5,0,0};
    XrXirFunction functions[]={
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0,XR_XIR_NEVER},
        {"entry",5,NULL,0,XR_XIR_I64,&five,1,answer,5,NULL,0,XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[2]={{0}};XrXirSourceModule module={.name="binary",.name_length=6,.storage=XR_XIR_MODULE_FRESH_STORAGE};
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
    CHECK(!run.live_bytes && run.allocations==run.frees);xr_xir_value_drop(&value);xr_xir_compile_artifact_free(lowered);
    CHECK(held(receiver.resources)==before);xr_compile_resources_release(receiver.resources);
}
int main(void) {
    relations();controls();built_after_retirement();
    puts("Binary HIR: complete operator rules, declaration authority, controls and retired syntax to Built/VM PASS");return 0;
}
