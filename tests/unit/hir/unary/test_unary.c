/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_unary.c - Concrete unary typing, request controls and retired facts
 */
#include "hir/xhir_unary.h"
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

static XrHirUnary checked(XrHirUnaryOperator operation,XrXirType type) {
    XrXirTypes pool={0};XrHirUnaryInput input={.operation=operation,.operand=type};
    XrHirUnary node={0};const char *reason=NULL;
    CHECK(xr_hir_unary_check(&pool,&input,NULL,&node,&reason)==XR_XIR_OK && !reason);
    CHECK(node.type==type);return node;
}
static void relations(void) {
    const XrXirType types[]={XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,
        XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,XR_XIR_U64};
    const int64_t complement[]={-1,-1,-1,-1,255,65535,INT64_C(4294967295),-1};
    for(size_t i=0;i<sizeof(types)/sizeof(types[0]);++i) {
        XrHirUnary neg=checked(XR_HIR_UNARY_NEGATE,types[i]);
        CHECK(neg.has_seed && !neg.seed && neg.operation==XR_XIR_SUB_INT);
        XrHirUnary inv=checked(XR_HIR_UNARY_COMPLEMENT,types[i]);
        CHECK(inv.has_seed && inv.seed==complement[i] && inv.operation==XR_XIR_XOR_INT);
    }
    CHECK(checked(XR_HIR_UNARY_NEGATE,XR_XIR_F32).operation==XR_XIR_NEG_FLOAT);
    CHECK(!checked(XR_HIR_UNARY_NEGATE,XR_XIR_F64).has_seed);
    XrXirTypes pool={0};
    const XrXirType invalid[]={XR_XIR_BOOL,XR_XIR_STRING,XR_XIR_UNIT,XR_XIR_RUNE,
        (XrXirType)256,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        XrHirUnaryInput input={.operation=XR_HIR_UNARY_NEGATE,.operand=invalid[i]};
        XrHirUnary node;memset(&node,0xa5,sizeof(node));XrHirUnary original=node;const char *reason=NULL;
        CHECK(xr_hir_unary_check(&pool,&input,NULL,&node,&reason)==XR_XIR_BAD_TYPE);
        CHECK(reason && !strcmp(reason,"unary arithmetic requires an integer") && !memcmp(&node,&original,sizeof(node)));
    }
    XrHirUnaryInput input={.operation=XR_HIR_UNARY_COMPLEMENT,.operand=XR_XIR_F64};
    XrHirUnary node;memset(&node,0xa5,sizeof(node));XrHirUnary original=node;
    CHECK(xr_hir_unary_check(&pool,&input,NULL,&node,NULL)==XR_XIR_BAD_TYPE && !memcmp(&node,&original,sizeof(node)));
    input.operation=(XrHirUnaryOperator)99;
    CHECK(xr_hir_unary_check(&pool,&input,NULL,&node,NULL)==XR_XIR_BAD_STRUCTURE && !memcmp(&node,&original,sizeof(node)));
    CHECK(xr_hir_unary_check(NULL,&input,NULL,&node,NULL)==XR_XIR_BAD_STRUCTURE);
}
typedef struct Cancel {unsigned polls;bool stop;} Cancel;
static bool cancel(const void *opaque) {Cancel *state=(Cancel *)opaque;++state->polls;return state->stop;}
static void controls(void) {
    XrXirTypes pool={0};XrHirUnaryInput input={.operation=XR_HIR_UNARY_NEGATE,.operand=XR_XIR_I8};
    XrHirUnary node;memset(&node,0xa5,sizeof(node));XrHirUnary original=node;
    Cancel state={0,true};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_unary_check(&pool,&input,&control,&node,NULL)==XR_XIR_CANCELLED && state.polls==1 && !memcmp(&node,&original,sizeof(node)));
    state.polls=0;
    CHECK(xr_hir_unary_check(&pool,&input,&control,&node,NULL)==XR_XIR_CANCELLED && !state.polls && !memcmp(&node,&original,sizeof(node)));
    control=(XrCompileRequestControl){0};state.stop=false;
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_unary_check(&pool,&input,&control,&node,NULL)==XR_XIR_OK && state.polls==1);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};node=original;
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_unary_check(&pool,&input,&control,&node,NULL)==XR_XIR_DEADLINE && !memcmp(&node,&original,sizeof(node)));
    CHECK(xr_hir_unary_check(&pool,&input,NULL,&node,NULL)==XR_XIR_OK);
}
static XrHirUnary retired_unary(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *memory=NULL;
    CHECK(xr_compile_resources_new(&limits,&memory)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(memory,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(memory,&session)==XR_COMPILE_RESOURCE_OK);
    char source[]="var a:i8=-42\r\nvar b=-a\r\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK && program && program->as.program.count==2);
    AstNode *syntax=program->as.program.statements[1]->as.var_decl.initializer;
    CHECK(syntax && syntax->type==AST_UNARY_NEG && syntax->node_id);
    XrXirTypes pool={0};XrHirUnaryInput input={.origin={syntax->node_id,7,syntax->line,syntax->column,syntax->end_line,syntax->end_column},
        .operation=XR_HIR_UNARY_NEGATE,.operand=XR_XIR_I8};XrHirUnary node={0};
    CHECK(xr_hir_unary_check(&pool,&input,NULL,&node,NULL)==XR_XIR_OK && node.origin.node_id==syntax->node_id && node.origin.module==7);
    CHECK(node.origin.line==2 && node.origin.column==syntax->column);
    memset(source,'!',sizeof(source));memset(&input,0xa5,sizeof(input));memset(&pool,0xa5,sizeof(pool));
    xr_program_destroy(program);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(memory,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(memory);return node;
}
static void run_node(XrHirUnary node,uint64_t input,uint64_t expected) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext receiver={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&receiver.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(receiver.resources,&before)==XR_COMPILE_RESOURCE_OK);
    int64_t bits=0;memcpy(&bits,&input,sizeof(bits));
    XrXirInstruction ret={.op=XR_XIR_RETURN},instructions[3]={{0}};uint32_t count=0,seed=0;
    if(node.has_seed){seed=count+1;instructions[count++]=xr_hir_unary_seed_instruction(&node);}
    uint32_t result=count+1;instructions[count++]=xr_hir_unary_instruction(&node,0,seed);
    instructions[count++]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={result}};
    XrXirBlock one={0,1,0,0},body={0,count,0,0},entry_block={0,2,0,0};
    XrXirInstruction entry[]={ {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={0}} };
    XrXirFunction functions[]={
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0,XR_XIR_NEVER},
        {"unary",5,&node.type,1,node.type,&body,1,instructions,count,NULL,0,XR_XIR_NEVER},
        {"entry",5,NULL,0,XR_XIR_I64,&entry_block,1,entry,2,NULL,0,XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[3]={{0}};XrXirSourceModule module={.name="unary",.name_length=5,.storage=XR_XIR_MODULE_FRESH_STORAGE};
    XrXirDeclarations declarations={&module,1,identities,NULL,0,NULL,0,0,2,NULL};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirConstruction *construction=NULL;
    CHECK(xr_xir_compile_construction_new_v4(&receiver,NULL,&(XrXirConstructionSpec){NULL,0,NULL,0,NULL},&(XrXirSlotFacts){NULL,0},&construction, NULL)==XR_XIR_OK);
    XrXirArtifact *checked_artifact=NULL,*closed=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_check_v2(&receiver,&built,construction,&checked_artifact,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"unary type%u: status%d at%u/%u/%u reason%d\n",(unsigned)node.type,(int)status,
        diagnostic.function,diagnostic.block,diagnostic.instruction,(int)diagnostic.reason);
    CHECK(status==XR_XIR_OK);xr_xir_compile_construction_free(construction);
    CHECK(xr_xir_compile_specialize(checked_artifact,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked_artifact);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirRunContext run={1024,1024,0,0,0,0};XrXirValue value={0};
    XrXirValue argument={node.type,0,bits};
    CHECK(xr_xir_compile_vm_run(lowered,1,&run,&argument,1,&value)==XR_XIR_RUN_OK && value.type==(uint32_t)node.type);
    if((uint64_t)value.payload!=expected)fprintf(stderr,"type%u: actual%llx expected%llx\n",(unsigned)node.type,(unsigned long long)value.payload,(unsigned long long)expected);
    CHECK((uint64_t)value.payload==expected && !run.live_bytes && run.allocations==run.frees);
    xr_xir_value_drop(&value);xr_xir_compile_artifact_free(lowered);
    CHECK(xr_compile_resources_stats(receiver.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(receiver.resources);
}
int main(void) {
    relations();controls();
    run_node(retired_unary(),(uint64_t)-42,42);
    run_node(checked(XR_HIR_UNARY_NEGATE,XR_XIR_I8),(uint64_t)-128,(uint64_t)-128);
    run_node(checked(XR_HIR_UNARY_NEGATE,XR_XIR_U8),1,255);
    run_node(checked(XR_HIR_UNARY_COMPLEMENT,XR_XIR_U64),0,UINT64_MAX);
    run_node(checked(XR_HIR_UNARY_NEGATE,XR_XIR_F32),0,UINT64_C(0x80000000));
    run_node(checked(XR_HIR_UNARY_NEGATE,XR_XIR_F64),UINT64_C(0x8000000000000000),0);
    run_node(checked(XR_HIR_UNARY_NEGATE,XR_XIR_F64),UINT64_C(0xfff8000000000042),UINT64_C(0x7ff8000000000000));
    puts("Numeric unary HIR: widths, refusals, controls, retired syntax, float zero/NaN and Built VM PASS");return 0;
}
