/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_literal.c - Typed literal facts survive real syntax and owner retirement
 */
#include "hir/xhir_literal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_generic.h"
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
static void boundaries(void) {
    XrXirTypes types={0};
    const struct {uint64_t magnitude;bool negative;XrXirType expected;XrXirStatus status;uint64_t bits;} cases[]={
        {127,false,XR_XIR_I8,XR_XIR_OK,127},{128,true,XR_XIR_I8,XR_XIR_OK,UINT64_MAX-127},
        {128,false,XR_XIR_I8,XR_XIR_BAD_TYPE,0},{0,true,XR_XIR_U8,XR_XIR_OK,0},
        {1,true,XR_XIR_U8,XR_XIR_BAD_TYPE,0},{UINT64_MAX,false,XR_XIR_U64,XR_XIR_OK,UINT64_MAX},
        {UINT64_C(1)<<63,true,XR_XIR_I64,XR_XIR_OK,UINT64_C(1)<<63},
        {UINT64_C(1)<<63,false,XR_XIR_I64,XR_XIR_BAD_TYPE,0},
        {16777216,false,XR_XIR_F32,XR_XIR_OK,UINT64_C(0x4b800000)},
        {16777217,false,XR_XIR_F32,XR_XIR_BAD_TYPE,0},
        {UINT64_C(9007199254740992),false,XR_XIR_F64,XR_XIR_OK,UINT64_C(0x4340000000000000)},
        {UINT64_C(9007199254740993),false,XR_XIR_F64,XR_XIR_BAD_TYPE,0},
        {0,true,XR_XIR_F64,XR_XIR_OK,UINT64_C(0x8000000000000000)}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrHirLiteralInput input={.expected=true,.expected_type=cases[i].expected,
            .magnitude=cases[i].magnitude,.negative=cases[i].negative};
        XrHirLiteral output={0};memset(&output,0x5a,sizeof(output));XrHirLiteral untouched=output;const char *reason=NULL;
        CHECK(xr_hir_literal_check(&types,&input,NULL,&output,&reason)==cases[i].status);
        if(cases[i].status==XR_XIR_OK)CHECK(output.type==cases[i].expected && output.bits==cases[i].bits && !reason);
        else CHECK(reason && !memcmp(&output,&untouched,sizeof(output)));
    }
    XrHirLiteralInput input={.magnitude=UINT64_MAX};XrHirLiteral output={0};const char *reason=NULL;
    CHECK(xr_hir_literal_check(&types,&input,NULL,&output,&reason)==XR_XIR_BAD_TYPE);
    XrXirTypeNode nullable={.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_F32};types.nodes=&nullable;types.count=1;
    input=(XrHirLiteralInput){.expected=true,.expected_type=(XrXirType)256,.magnitude=16777216};
    CHECK(xr_hir_literal_check(&types,&input,NULL,&output,&reason)==XR_XIR_OK && output.type==XR_XIR_F32 && output.bits==UINT64_C(0x4b800000));
    input.decimal=true;input.decimal_text="not-a-number";input.decimal_length=12;
    CHECK(xr_hir_literal_check(&types,&input,NULL,&output,&reason)==XR_XIR_BAD_TYPE);
}
typedef struct Cancel {unsigned polls,trigger;} Cancel;
static bool cancel(const void *context) {
    Cancel *state=(Cancel *)context;++state->polls;return state->trigger && state->polls>=state->trigger;
}
static void controls(void) {
    XrXirTypes types={0};XrHirLiteralInput input={.magnitude=42};XrHirLiteral output={0};
    memset(&output,0x5a,sizeof(output));XrHirLiteral untouched=output;const char *reason=NULL;
    Cancel state={0,2};XrCompileRequestOptions options={.cancel_poll=cancel,.cancel_context=&state};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_literal_check(&types,&input,&control,&output,&reason)==XR_XIR_CANCELLED && state.polls==2 && !memcmp(&output,&untouched,sizeof(output)));
    state=(Cancel){0};
    CHECK(xr_hir_literal_check(&types,&input,&control,&output,&reason)==XR_XIR_CANCELLED && !state.polls && !memcmp(&output,&untouched,sizeof(output)));
    control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_literal_check(&types,&input,&control,&output,&reason)==XR_XIR_OK && output.bits==42 && state.polls==2);
    output=untouched;
    control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER,.timeout_ns=0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_hir_literal_check(&types,&input,&control,&output,&reason)==XR_XIR_DEADLINE && !memcmp(&output,&untouched,sizeof(output)));
    CHECK(xr_hir_literal_check(&types,&input,&control,&output,&reason)==XR_XIR_DEADLINE && !memcmp(&output,&untouched,sizeof(output)));
    CHECK(xr_hir_literal_check(&types,&input,NULL,&output,&reason)==XR_XIR_OK && output.bits==42);
}
static XrHirLiteral retired_literal(void) {
    XrXirCompileContext owner=context_new();uint64_t before=held(owner.resources);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.resources,&session)==XR_COMPILE_RESOURCE_OK);
    char source[]="const answer:f64=2.5\n";AstNode *program=NULL;
    CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK && program && program->as.program.count==1);
    AstNode *node=program->as.program.statements[0]->as.var_decl.initializer;CHECK(node && node->type==AST_LITERAL_FLOAT && node->node_id && node->line==1);
    XrHirLiteralInput input={.decimal=true,.decimal_text=node->as.literal.decimal_text,.decimal_length=node->as.literal.decimal_length,
        .origin={node->node_id,0,node->line,node->column,node->end_line,node->end_column},.expected=true,.expected_type=XR_XIR_F64};
    XrXirTypes pool={0};XrHirLiteral literal={0};const char *reason=NULL;
    CHECK(xr_hir_literal_check(&pool,&input,NULL,&literal,&reason)==XR_XIR_OK && literal.bits==UINT64_C(0x4004000000000000));
    CHECK(literal.origin.node_id==node->node_id && literal.origin.column==node->column && literal.origin.end_column==node->end_column);
    memset(source,'!',sizeof(source));memset(&input,0xa5,sizeof(input));memset(&pool,0xa5,sizeof(pool));
    xr_compile_session_free(session);xr_program_destroy(program);CHECK(held(owner.resources)==before);xr_compile_resources_release(owner.resources);
    return literal;
}
int main(void) {
    boundaries();controls();(void)retired_literal();
    puts("Typed numeric HIR: exact bounds, context, controls and real AST/text retirement before HIR PASS");return 0;
}
