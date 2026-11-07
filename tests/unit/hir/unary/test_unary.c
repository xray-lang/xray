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
int main(void) {
    relations();controls();(void)retired_unary();







    puts("Numeric unary HIR: widths, refusals, controls, retired syntax, AST lifetime PASS");return 0;
}
