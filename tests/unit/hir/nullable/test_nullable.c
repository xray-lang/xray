/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_nullable.c - Nullable type relations, interning and retired Built facts
 */
#include "hir/xhir_nullable.h"
#include "xir/xxir_types.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "frontend/parser/xparse.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static XrHirNullable checked(const XrHirNullableContext *context,XrHirNullableInput input,XrXirType fallback) {
    XrHirNullablePlan plan;XrHirNullable node;const char *reason=NULL;
    CHECK(xr_hir_nullable_prepare(context,&input,&plan,&reason)==XR_XIR_OK && !reason);
    CHECK(xr_hir_nullable_check(&plan,fallback,context->control,&node,&reason)==XR_XIR_OK && !reason);return node;
}
static void relations(void) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)258},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)260},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_UNIT}};
    XrXirTypes types={.nodes=nodes,.count=7};XrHirNullableContext context={.types=&types};
    const XrXirType present[]={(XrXirType)256,(XrXirType)257,(XrXirType)259,(XrXirType)261,(XrXirType)262};
    const XrXirType elements[]={XR_XIR_I64,(XrXirType)256,(XrXirType)258,(XrXirType)260,XR_XIR_UNIT};
    for(size_t i=0;i<5;++i) {
        XrHirNullableInput input={.origin={7,3,2,5,2,9},.source=present[i],.expected=present[i],.has_expected=true};
        input.role=XR_HIR_NULLABLE_NULL;XrHirNullable node=checked(&context,input,XR_XIR_UNIT);
        CHECK(node.kind==XR_HIR_NULLABLE_NONE && node.result==present[i] && xr_hir_nullable_instruction(&node,13).op==XR_XIR_NULLABLE_NONE);
        for(unsigned unequal=0;unequal<2;++unequal) {
            input.role=unequal?XR_HIR_NULLABLE_UNEQUAL_NULL:XR_HIR_NULLABLE_EQUAL_NULL;node=checked(&context,input,XR_XIR_UNIT);
            XrXirInstruction op=xr_hir_nullable_instruction(&node,13);
            CHECK(node.kind==XR_HIR_NULLABLE_PRESENCE && node.result==XR_XIR_BOOL && op.op==XR_XIR_NULLABLE_IS_SOME && op.immediate==!unequal && op.args[0]==13);
        }
        for(unsigned forced=0;forced<2;++forced) {
            input.role=forced?XR_HIR_NULLABLE_FORCE:XR_HIR_NULLABLE_UNWRAP;node=checked(&context,input,XR_XIR_UNIT);
            CHECK(node.kind==XR_HIR_NULLABLE_ELEMENT && node.result==elements[i] && xr_hir_nullable_instruction(&node,13).op==XR_XIR_NULLABLE_UNWRAP);
            CHECK(xr_hir_nullable_force_hint(&types,present[i])==elements[i]);
        }
        input.role=XR_HIR_NULLABLE_COALESCE;input.has_expected=false;node=checked(&context,input,elements[i]);
        CHECK(node.kind==XR_HIR_NULLABLE_BRANCH && node.result==elements[i] && node.unwrap_present);
        input.has_expected=true;node=checked(&context,input,present[i]);CHECK(node.result==present[i] && !node.unwrap_present);
        input.has_expected=false;input.null_default=true;XrXirType result=i==1?elements[i]:present[i];node=checked(&context,input,result);
        CHECK(node.result==result && node.unwrap_present==(i==1));
        CHECK(!memcmp(&node.origin,&input.origin,sizeof(node.origin)));
    }
    const XrXirType nonnull[]={XR_XIR_UNIT,XR_XIR_BOOL,XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,
        XR_XIR_U64,XR_XIR_F32,XR_XIR_F64,XR_XIR_STRING,XR_XIR_RUNE,(XrXirType)258,(XrXirType)260};
    for(size_t i=0;i<sizeof(nonnull)/sizeof(nonnull[0]);++i) {
        XrHirNullableInput input={.role=XR_HIR_NULLABLE_FORCE,.source=nonnull[i]};XrHirNullable node=checked(&context,input,XR_XIR_UNIT);
        CHECK(node.kind==XR_HIR_NULLABLE_IDENTITY && node.result==nonnull[i] && xr_hir_nullable_force_hint(&types,nonnull[i])==nonnull[i]);
        input.role=XR_HIR_NULLABLE_COALESCE;node=checked(&context,input,nonnull[i]);CHECK(node.kind==XR_HIR_NULLABLE_IDENTITY && node.result==nonnull[i]);
        const XrHirNullableRole rejected[]={XR_HIR_NULLABLE_NULL,XR_HIR_NULLABLE_EQUAL_NULL,XR_HIR_NULLABLE_UNEQUAL_NULL,XR_HIR_NULLABLE_UNWRAP};
        const char *messages[]={"null requires an exact nullable context","comparison with null requires a nullable operand",
            "comparison with null requires a nullable operand","unwrap requires a nullable operand"};
        for(unsigned r=0;r<4;++r) {
            input.role=rejected[r];input.expected=nonnull[i];input.has_expected=true;
            XrHirNullablePlan output;memset(&output,0xa5,sizeof(output));XrHirNullablePlan before=output;const char *reason=NULL;
            CHECK(xr_hir_nullable_prepare(&context,&input,&output,&reason)==XR_XIR_BAD_TYPE && reason && !strcmp(reason,messages[r]) && !memcmp(&output,&before,sizeof(output)));
        }
    }
    XrHirNullableInput input={.role=XR_HIR_NULLABLE_NULL,.expected=(XrXirType)256};XrHirNullablePlan plan;
    CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_BAD_TYPE);
    input=(XrHirNullableInput){.role=XR_HIR_NULLABLE_COALESCE,.source=(XrXirType)256};CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_OK);
    XrHirNullable node;memset(&node,0xa5,sizeof(node));XrHirNullable before=node;const char *reason=NULL;
    CHECK(xr_hir_nullable_check(&plan,XR_XIR_STRING,NULL,&node,&reason)==XR_XIR_BAD_TYPE && !memcmp(&node,&before,sizeof(node)));
    CHECK(!strcmp(reason,"?? default must match the unwrapped element or its nullable context"));
    input.role=(XrHirNullableRole)99;CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_nullable_prepare(NULL,&input,&plan,NULL)==XR_XIR_BAD_STRUCTURE);
}
typedef struct InternOwner {XrXirTypes types;XrXirTypeNode *storage;XrXirStatus status;unsigned calls;} InternOwner;
static XrXirStatus intern(void *opaque,const XrXirTypeNode *node,XrXirType *type) {
    InternOwner *owner=opaque;++owner->calls;if(owner->status!=XR_XIR_OK)return owner->status;
    XrXirTypeNode copy=*node;XrXirTypeNode *next=xr_malloc((size_t)(owner->types.count+1)*sizeof(*next));CHECK(next);
    memcpy(next,owner->storage,owner->types.count*sizeof(*next));next[owner->types.count]=copy;
    memset(owner->storage,0xa5,owner->types.count*sizeof(*next));xr_free(owner->storage);
    *type=(XrXirType)(256+owner->types.count);owner->storage=next;owner->types.nodes=next;++owner->types.count;return XR_XIR_OK;
}
static void interning(void) {
    InternOwner owner={0};owner.storage=xr_malloc(sizeof(*owner.storage));CHECK(owner.storage);
    owner.storage[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};owner.types=(XrXirTypes){.nodes=owner.storage,.count=1};
    XrHirNullableContext context={&owner.types,NULL,&owner,intern};XrHirNullablePlan plan;
    XrHirNullableInput input={.role=XR_HIR_NULLABLE_DEAD_DEFAULT,.source=XR_XIR_I64,.default_base=XR_XIR_I64,.null_default=true};
    CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_OK && owner.calls==1 && plan.default_type==(XrXirType)257);
    CHECK(owner.types.nodes[1].kind==XR_XIR_TYPE_NULLABLE && owner.types.nodes[1].element==XR_XIR_I64);
    XrHirNullable node;CHECK(xr_hir_nullable_check(&plan,(XrXirType)257,NULL,&node,NULL)==XR_XIR_OK && node.result==XR_XIR_I64 && node.kind==XR_HIR_NULLABLE_IDENTITY);
    input.default_base=(XrXirType)257;CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_OK && owner.calls==1);
    input.default_base=(XrXirType)256;CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_BAD_TYPE && owner.calls==1);
    input.default_base=XR_XIR_STRING;owner.status=XR_XIR_OUT_OF_MEMORY;memset(&plan,0xa5,sizeof(plan));XrHirNullablePlan before=plan;
    CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_OUT_OF_MEMORY && owner.calls==2 && !memcmp(&plan,&before,sizeof(plan)));
    context.intern=NULL;CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_BAD_STRUCTURE);
    memset(owner.storage,0xa5,owner.types.count*sizeof(*owner.storage));xr_free(owner.storage);memset(&owner,0xa5,sizeof(owner));
    CHECK(node.result==XR_XIR_I64 && node.default_type==(XrXirType)257);
}
typedef struct Cancel {unsigned polls;bool stop;} Cancel;
static bool cancel(const void *opaque) {Cancel *state=(Cancel *)opaque;++state->polls;return state->stop;}
static void controls(void) {
    XrXirTypes types={0};Cancel state={0,true};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrHirNullableContext context={.types=&types,.control=&control};XrHirNullableInput input={.role=XR_HIR_NULLABLE_FORCE,.source=XR_XIR_I64};
    XrHirNullablePlan plan;memset(&plan,0xa5,sizeof(plan));XrHirNullablePlan before=plan;
    CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_CANCELLED && state.polls==1 && !memcmp(&plan,&before,sizeof(plan)));
    control=(XrCompileRequestControl){0};state.stop=false;CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_OK);state.stop=true;
    XrHirNullable node;memset(&node,0xa5,sizeof(node));XrHirNullable original=node;
    CHECK(xr_hir_nullable_check(&plan,XR_XIR_UNIT,&control,&node,NULL)==XR_XIR_CANCELLED && !memcmp(&node,&original,sizeof(node)));
    state.polls=0;CHECK(xr_hir_nullable_check(&plan,XR_XIR_UNIT,&control,&node,NULL)==XR_XIR_CANCELLED && !state.polls);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_nullable_prepare(&context,&input,&plan,NULL)==XR_XIR_DEADLINE);
    CHECK(xr_hir_nullable_check(&plan,XR_XIR_UNIT,&control,&node,NULL)==XR_XIR_DEADLINE && !memcmp(&node,&original,sizeof(node)));
    CHECK(xr_hir_nullable_check(&plan,XR_XIR_UNIT,NULL,&node,NULL)==XR_XIR_OK);
}
static XrHirNullable retired_node(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    char text[]="var value:i64?=null\nconst output=value!\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,text,&program)==XR_PARSE_OK && program && program->as.program.count==2);
    AstNode *syntax=program->as.program.statements[1]->as.var_decl.initializer;CHECK(syntax && syntax->type==AST_FORCE_UNWRAP);
    XrXirTypeNode type={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};XrXirTypes types={.nodes=&type,.count=1};XrHirNullableContext context={.types=&types};
    XrHirNullableInput input={.origin={syntax->node_id,3,2,14,2,20},.role=XR_HIR_NULLABLE_FORCE,.source=(XrXirType)256};
    XrHirNullable node=checked(&context,input,XR_XIR_UNIT);memset(text,'!',sizeof(text));memset(&type,0xa5,sizeof(type));
    memset(&types,0xa5,sizeof(types));memset(&input,0xa5,sizeof(input));memset(&context,0xa5,sizeof(context));xr_program_destroy(program);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);xr_compile_resources_release(resources);return node;
}
static void built_after_retirement(void) {
    XrHirNullable node=retired_node();CHECK(node.kind==XR_HIR_NULLABLE_ELEMENT && node.result==XR_XIR_I64 && node.origin.module==3);
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext receiver={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&receiver.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(receiver.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode type={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64};XrXirTypes types={.nodes=&type,.count=1};
    XrXirInstruction ret={.op=XR_XIR_RETURN},ops[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)256,.args={0}},xr_hir_nullable_instruction(&node,1),{.op=XR_XIR_RETURN,.args={2}}};
    XrXirInstruction entry[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={0}}};XrXirBlock one={0,1,0,0},body={0,4,0,0},main_body={0,2,0,0};
    XrXirFunction functions[]={{"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0,XR_XIR_NEVER},
        {"nullable",8,NULL,0,XR_XIR_I64,&body,1,ops,4,NULL,0,XR_XIR_NEVER},{"entry",5,NULL,0,XR_XIR_I64,&main_body,1,entry,2,NULL,0,XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[3]={{0}};XrXirSourceModule module={.name="nullable",.name_length=8,.storage=XR_XIR_MODULE_FRESH_STORAGE};
    XrXirDeclarations declarations={&module,1,identities,NULL,0,NULL,0,0,1,NULL};XrXirModule built={.stage=XR_XIR_BUILT,.types=&types,.functions=functions,.function_count=3,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirConstruction *construction=NULL;CHECK(xr_xir_compile_construction_new_v4(&receiver,NULL,&(XrXirConstructionSpec){NULL,0,NULL,0,NULL},&(XrXirSlotFacts){NULL,0},&construction)==XR_XIR_OK);
    XrXirArtifact *artifact=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_check_v2(&receiver,&built,construction,&artifact,NULL)==XR_XIR_OK);xr_xir_compile_construction_free(construction);
    CHECK(xr_xir_compile_specialize(artifact,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(artifact);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);XrXirCallResult outcome={0};
    for(unsigned ticks=0;;++ticks) {CHECK(ticks<1000);outcome=xr_xir_instance_poll_bounded(instance,32).outcome;if(outcome.status!=XR_XIR_CALL_READY)break;}
    XrXirValue value={0};CHECK(outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==41);
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(xr_compile_resources_stats(receiver.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);xr_compile_resources_release(receiver.resources);
}
int main(void) {relations();interning();controls();built_after_retirement();puts("Nullable HIR: outer-layer relations, real default, growing interner, request failures and retired Built/VM PASS");return 0;}
