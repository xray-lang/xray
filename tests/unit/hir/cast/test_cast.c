/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cast.c - Explicit numeric and text typing after producer retirement
 */
#include "hir/xhir_cast.h"
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
static const XrXirType numbers[]={XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,
    XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,XR_XIR_U64,XR_XIR_F32,XR_XIR_F64};
static XrHirCastTarget target(XrHirCastRole role,XrXirType type) {
    XrXirTypes types={0};XrHirCastTargetInput input={.origin={3,7,2,8,2,12},.role=role,.target=type};
    XrHirCastTarget output={0};const char *reason=NULL;
    CHECK(xr_hir_cast_target_check(&types,&input,NULL,&output,&reason)==XR_XIR_OK && !reason);
    CHECK(output.type==type && output.role==role && !memcmp(&output.origin,&input.origin,sizeof(input.origin)));return output;
}
static void relations(void) {
    XrXirTypeNode nullable={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};XrXirTypes types={.nodes=&nullable,.count=1};
    for(size_t to=0;to<sizeof(numbers)/sizeof(numbers[0]);++to) {
        XrHirCastTarget qualified=target(XR_HIR_CAST_EXPLICIT,numbers[to]);
        for(size_t from=0;from<sizeof(numbers)/sizeof(numbers[0]);++from) {
            XrHirCast node={0};CHECK(xr_hir_cast_check(&types,&qualified,numbers[from],NULL,&node,NULL)==XR_XIR_OK);
            CHECK(node.kind==XR_HIR_CAST_NUMBER && node.source==numbers[from] && node.target==numbers[to]);
            XrXirInstruction op=xr_hir_cast_instruction(&node,7);
            CHECK(op.op==XR_XIR_CONVERT_NUMBER && op.type==numbers[to] && op.args[0]==7 && !op.args[1] && !op.immediate);
        }
        XrHirCastLiteralContext context=xr_hir_cast_literal_context(&qualified,true,false,UINT64_C(9223372036854775808));
        CHECK(context.present && context.type==XR_XIR_U64);
        CHECK(!xr_hir_cast_literal_context(&qualified,true,false,INT64_MAX).present);
        CHECK(!xr_hir_cast_literal_context(&qualified,true,true,UINT64_MAX).present);
        CHECK(!xr_hir_cast_literal_context(&qualified,false,false,UINT64_MAX).present);
    }
    const XrXirType invalid[]={XR_XIR_BOOL,XR_XIR_RUNE,XR_XIR_STRING,XR_XIR_UNIT,XR_XIR_ERROR,XR_XIR_NEVER,
        (XrXirType)256,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        XrHirCastTargetInput input={.role=XR_HIR_CAST_EXPLICIT,.target=invalid[i]};
        XrHirCastTarget qualified;memset(&qualified,0xa5,sizeof(qualified));XrHirCastTarget before=qualified;const char *reason=NULL;
        if(invalid[i]!=XR_XIR_STRING) {
            CHECK(xr_hir_cast_target_check(&types,&input,NULL,&qualified,&reason)==XR_XIR_BAD_TYPE);
            CHECK(reason && !strcmp(reason,"numeric cast requires a concrete nonnullable numeric target") && !memcmp(&qualified,&before,sizeof(qualified)));
        }
        qualified=target(XR_HIR_CAST_EXPLICIT,XR_XIR_I64);XrHirCast node;memset(&node,0xa5,sizeof(node));XrHirCast original=node;
        CHECK(xr_hir_cast_check(&types,&qualified,invalid[i],NULL,&node,&reason)==XR_XIR_BAD_TYPE);
        CHECK(reason && !strcmp(reason,"numeric cast requires a concrete numeric operand") && !memcmp(&node,&original,sizeof(node)));
    }
    for(int role=XR_HIR_CAST_EXPLICIT;role<=XR_HIR_CAST_INTERPOLATION;++role) {
        XrHirCastTarget qualified=target((XrHirCastRole)role,XR_XIR_STRING);
        const XrXirType textual[]={XR_XIR_BOOL,XR_XIR_RUNE,XR_XIR_STRING};
        for(size_t i=0;i<13;++i) {
            XrXirType type=i<10?numbers[i]:textual[i-10];XrHirCast node={0};
            CHECK(xr_hir_cast_check(&types,&qualified,type,NULL,&node,NULL)==XR_XIR_OK && node.target==XR_XIR_STRING && node.source==type);
            CHECK(node.kind==(type==XR_XIR_STRING?XR_HIR_CAST_IDENTITY:XR_HIR_CAST_TO_TEXT));
            if(node.kind!=XR_HIR_CAST_IDENTITY)CHECK(xr_hir_cast_instruction(&node,5).op==XR_XIR_TO_STRING);
        }
        const XrXirType rejected[]={XR_XIR_UNIT,XR_XIR_ERROR,XR_XIR_NEVER,(XrXirType)256,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
        for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
            XrHirCast node;memset(&node,0xa5,sizeof(node));XrHirCast before=node;const char *reason=NULL;
            CHECK(xr_hir_cast_check(&types,&qualified,rejected[i],NULL,&node,&reason)==XR_XIR_BAD_TYPE && !memcmp(&node,&before,sizeof(node)));
            CHECK(reason && !strcmp(reason,role==XR_HIR_CAST_INTERPOLATION?"interpolated value has no admitted string conversion":"value has no admitted string conversion"));
        }
    }
    XrHirCastTargetInput safe={.role=XR_HIR_CAST_EXPLICIT,.target=XR_XIR_I64,.safe=true};XrHirCastTarget qualified;
    memset(&qualified,0xa5,sizeof(qualified));XrHirCastTarget before=qualified;
    CHECK(xr_hir_cast_target_check(&types,&safe,NULL,&qualified,NULL)==XR_XIR_BAD_TYPE && !memcmp(&qualified,&before,sizeof(qualified)));
    safe.role=(XrHirCastRole)99;CHECK(xr_hir_cast_target_check(&types,&safe,NULL,&qualified,NULL)==XR_XIR_BAD_STRUCTURE);
    qualified=target(XR_HIR_CAST_EXPLICIT,XR_XIR_I64);qualified.type=XR_XIR_STRING;XrHirCast node={0};
    CHECK(xr_hir_cast_check(&types,&qualified,XR_XIR_I64,NULL,&node,NULL)==XR_XIR_BAD_STRUCTURE);
}
typedef struct Cancel {unsigned polls;bool stop;} Cancel;
static bool cancel(const void *opaque) {Cancel *state=(Cancel *)opaque;++state->polls;return state->stop;}
static void controls(void) {
    XrXirTypes types={0};XrHirCastTargetInput input={.role=XR_HIR_CAST_EXPLICIT,.target=XR_XIR_I8};
    XrHirCastTarget qualified;memset(&qualified,0xa5,sizeof(qualified));XrHirCastTarget original=qualified;
    Cancel state={0,true};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_cast_target_check(&types,&input,&control,&qualified,NULL)==XR_XIR_CANCELLED && state.polls==1 && !memcmp(&qualified,&original,sizeof(qualified)));
    qualified=target(XR_HIR_CAST_EXPLICIT,XR_XIR_I8);XrHirCast node;memset(&node,0xa5,sizeof(node));XrHirCast before=node;state.polls=0;
    CHECK(xr_hir_cast_check(&types,&qualified,XR_XIR_I64,&control,&node,NULL)==XR_XIR_CANCELLED && !state.polls && !memcmp(&node,&before,sizeof(node)));
    control=(XrCompileRequestControl){0};state.stop=false;CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_cast_check(&types,&qualified,XR_XIR_I64,&control,&node,NULL)==XR_XIR_OK && state.polls==1);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};node=before;
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_cast_check(&types,&qualified,XR_XIR_I64,&control,&node,NULL)==XR_XIR_DEADLINE && !memcmp(&node,&before,sizeof(node)));
    CHECK(xr_hir_cast_target_check(&types,&input,&control,&qualified,NULL)==XR_XIR_DEADLINE);
    CHECK(xr_hir_cast_check(&types,&qualified,XR_XIR_I64,NULL,&node,NULL)==XR_XIR_OK);
}
static XrHirCast retired_cast(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    char source[]="var input:i64=255\r\nvar output=input as i8\r\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK && program && program->as.program.count==2);
    AstNode *syntax=program->as.program.statements[1]->as.var_decl.initializer;CHECK(syntax && syntax->type==AST_AS_EXPR && syntax->node_id);
    XrHirCastTargetInput input={.origin={syntax->node_id,7,syntax->line,syntax->column,syntax->end_line,syntax->end_column},.role=XR_HIR_CAST_EXPLICIT,.target=XR_XIR_I8};
    XrXirTypes types={0};XrHirCastTarget qualified={0};XrHirCast node={0};
    CHECK(xr_hir_cast_target_check(&types,&input,NULL,&qualified,NULL)==XR_XIR_OK);
    CHECK(xr_hir_cast_check(&types,&qualified,XR_XIR_I64,NULL,&node,NULL)==XR_XIR_OK && node.origin.node_id==syntax->node_id);
    CHECK(node.origin.module==7 && node.origin.line==2 && node.origin.column==syntax->column);
    memset(source,'!',sizeof(source));memset(&input,0xa5,sizeof(input));memset(&qualified,0xa5,sizeof(qualified));memset(&types,0xa5,sizeof(types));
    xr_program_destroy(program);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(resources);return node;
}
static void built_after_retirement(void) {
    XrHirCast node=retired_cast();const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext receiver={.limits=xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&receiver.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(receiver.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirInstruction ret={.op=XR_XIR_RETURN},ops[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=255},xr_hir_cast_instruction(&node,0),{.op=XR_XIR_RETURN,.args={1}}};
    XrXirInstruction entry[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={0}}};XrXirBlock one={0,1,0,0},body={0,3,0,0},main_body={0,2,0,0};
    XrXirFunction functions[]={
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0,XR_XIR_NEVER},
        {"cast",4,NULL,0,node.target,&body,1,ops,3,NULL,0,XR_XIR_NEVER},
        {"entry",5,NULL,0,XR_XIR_I64,&main_body,1,entry,2,NULL,0,XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[3]={{0}};XrXirSourceModule module={.name="cast",.name_length=4,.storage=XR_XIR_MODULE_FRESH_STORAGE};
    XrXirDeclarations declarations={&module,1,identities,NULL,0,NULL,0,0,2,NULL};XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirConstruction *construction=NULL;CHECK(xr_xir_compile_construction_new_v4(&receiver,NULL,&(XrXirConstructionSpec){NULL,0,NULL,0,NULL},&(XrXirSlotFacts){NULL,0},&construction)==XR_XIR_OK);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_check_v2(&receiver,&built,construction,&checked,NULL)==XR_XIR_OK);xr_xir_compile_construction_free(construction);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirRunContext run={1024,1024,0,0,0,0};XrXirValue result={0};
    CHECK(xr_xir_compile_vm_run(lowered,1,&run,NULL,0,&result)==XR_XIR_RUN_OK && result.type==XR_XIR_I8 && result.payload==-1);
    CHECK(!run.live_bytes && run.allocations==run.frees);xr_xir_value_drop(&result);xr_xir_compile_artifact_free(lowered);
    CHECK(xr_compile_resources_stats(receiver.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);xr_compile_resources_release(receiver.resources);
}
int main(void) {
    relations();controls();built_after_retirement();
    puts("Cast HIR: complete numeric/text domains, literal context, request failures and retired facts to Built/VM PASS");return 0;
}
