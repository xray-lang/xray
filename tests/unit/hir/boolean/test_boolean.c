/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_boolean.c - Bool qualifications, request failure and retired typed facts
 */
#include "hir/xhir_boolean.h"
#include "xir/xxir_types.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
#include "frontend/parser/xparse.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)

static void relations(void) {
    const XrHirBooleanOperation roles[]={XR_HIR_BOOLEAN_NOT,XR_HIR_BOOLEAN_AND,XR_HIR_BOOLEAN_OR,
        XR_HIR_BOOLEAN_CONDITIONAL,XR_HIR_BOOLEAN_IF,XR_HIR_BOOLEAN_LOOP};
    const char *messages[]={"logical operand must be bool","logical operand must be bool","logical operand must be bool",
        "conditional requires bool","if requires bool","loop condition requires bool"};
    XrXirTypeNode nullable={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL};XrXirTypes types={.nodes=&nullable,.count=1};
    const XrXirType invalid[]={XR_XIR_UNIT,XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,XR_XIR_U8,XR_XIR_U16,
        XR_XIR_U32,XR_XIR_U64,XR_XIR_F32,XR_XIR_F64,XR_XIR_RUNE,XR_XIR_STRING,XR_XIR_ERROR,XR_XIR_NEVER,
        (XrXirType)256,(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    for(size_t i=0;i<sizeof(roles)/sizeof(roles[0]);++i) {
        XrHirBooleanInput input={.origin={3,7,2,8,2,10},.operation=roles[i],.operand=XR_XIR_BOOL};
        XrHirBoolean typed={0};const char *reason=NULL;
        CHECK(xr_hir_boolean_check(&types,&input,NULL,&typed,&reason)==XR_XIR_OK && !reason);
        CHECK(typed.type==XR_XIR_BOOL && typed.operation==roles[i] && !memcmp(&typed.origin,&input.origin,sizeof(input.origin)));
        if(i<3)CHECK(xr_hir_boolean_result_type(roles[i])==XR_XIR_BOOL);
        for(size_t j=0;j<sizeof(invalid)/sizeof(invalid[0]);++j) {
            input.operand=invalid[j];memset(&typed,0xa5,sizeof(typed));XrHirBoolean before=typed;
            CHECK(xr_hir_boolean_check(&types,&input,NULL,&typed,&reason)==XR_XIR_BAD_TYPE);
            CHECK(reason && !strcmp(reason,messages[i]) && !memcmp(&typed,&before,sizeof(typed)));
        }
    }
    XrHirBooleanInput input={.operation=XR_HIR_BOOLEAN_INVALID,.operand=XR_XIR_BOOL};
    XrHirBoolean typed;memset(&typed,0xa5,sizeof(typed));XrHirBoolean before=typed;
    CHECK(xr_hir_boolean_check(&types,&input,NULL,&typed,NULL)==XR_XIR_BAD_STRUCTURE && !memcmp(&typed,&before,sizeof(typed)));
    CHECK(xr_hir_boolean_check(NULL,&input,NULL,&typed,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_boolean_check(&types,NULL,NULL,&typed,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_boolean_check(&types,&input,NULL,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
}
typedef struct Cancel {unsigned polls;bool stop;} Cancel;
static bool cancel(const void *opaque) {Cancel *state=(Cancel *)opaque;++state->polls;return state->stop;}
static void controls(void) {
    XrXirTypes types={0};XrHirBooleanInput input={.operation=XR_HIR_BOOLEAN_NOT,.operand=XR_XIR_BOOL};
    XrHirBoolean typed;memset(&typed,0xa5,sizeof(typed));XrHirBoolean before=typed;
    Cancel state={0,true};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_boolean_check(&types,&input,&control,&typed,NULL)==XR_XIR_CANCELLED && state.polls==1 && !memcmp(&typed,&before,sizeof(typed)));
    state.polls=0;CHECK(xr_hir_boolean_check(&types,&input,&control,&typed,NULL)==XR_XIR_CANCELLED && !state.polls);
    control=(XrCompileRequestControl){0};state.stop=false;
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_boolean_check(&types,&input,&control,&typed,NULL)==XR_XIR_OK && state.polls==1);
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};typed=before;
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_boolean_check(&types,&input,&control,&typed,NULL)==XR_XIR_DEADLINE && !memcmp(&typed,&before,sizeof(typed)));
    CHECK(xr_hir_boolean_check(&types,&input,NULL,&typed,NULL)==XR_XIR_OK);
}
static XrHirBoolean retired_boolean(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    char source[]="var a=true\r\nvar b=!a\r\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK && program && program->as.program.count==2);
    AstNode *syntax=program->as.program.statements[1]->as.var_decl.initializer;
    CHECK(syntax && syntax->type==AST_UNARY_NOT && syntax->node_id);
    XrXirTypes types={0};XrHirBooleanInput input={.origin={syntax->node_id,7,syntax->line,syntax->column,syntax->end_line,syntax->end_column},
        .operation=XR_HIR_BOOLEAN_NOT,.operand=XR_XIR_BOOL};XrHirBoolean typed={0};
    CHECK(xr_hir_boolean_check(&types,&input,NULL,&typed,NULL)==XR_XIR_OK && typed.origin.node_id==syntax->node_id);
    CHECK(typed.origin.module==7 && typed.origin.line==2 && typed.origin.column==syntax->column);
    memset(source,'!',sizeof(source));memset(&input,0xa5,sizeof(input));memset(&types,0xa5,sizeof(types));
    xr_program_destroy(program);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(resources);return typed;
}
int main(void) {
    relations();controls();(void)retired_boolean();
    puts("Boolean HIR: all operand roles, unchanged refusals, request failure and retired facts to HIR PASS");return 0;
}
