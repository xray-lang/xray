/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Scope syntax preserves its distinct exit marker across owned formatting. */
#include "base/xresource_limits.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast_walk.h"
#include "frontend/format/xfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"scope syntax %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const AstNode *scope_node(const AstNode *program,bool propagation) {
    CHECK(program && program->as.program.count==1);
    const AstNode *statement=program->as.program.statements[0];
    if(propagation) {
        CHECK(statement->type==AST_EXPR_STMT);statement=statement->as.expr_stmt;
        CHECK(statement->type==AST_TRY_EXPR && statement->as.try_expr.mode==XR_TRY_PROPAGATE);
        statement=statement->as.try_expr.operand;
    }
    CHECK(statement->type==AST_SCOPE_BLOCK && statement->as.scope_block.body->type==AST_BLOCK);
    return statement;
}
static void roundtrip(XrCompileResources *resources,const char *text,bool propagation,uint8_t mode) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    AstNode *first=NULL,*second=NULL;XrFmtOutput out={0},again={0};
    CHECK(xr_compile_parse(session,text,&first)==XR_PARSE_OK);
    const AstNode *scope=scope_node(first,propagation);CHECK(scope->as.scope_block.scope_mode==mode);
    char before[256],after[256];CHECK(xr_ast_node_signature(scope,before,sizeof(before)));
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),first,NULL,&out)==XR_FMT_OK);
    CHECK(out.text && !strstr(out.text,"linked"));
    CHECK(xr_compile_parse(session,out.text,&second)==XR_PARSE_OK);
    scope=scope_node(second,propagation);CHECK(scope->as.scope_block.scope_mode==mode);
    CHECK(xr_ast_node_signature(scope,after,sizeof(after)) && !strcmp(before,after));
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),second,NULL,&again)==XR_FMT_OK);
    CHECK(out.length==again.length && !memcmp(out.text,again.text,out.length));
    xr_compile_format_output_free(&out);xr_compile_format_output_free(&again);
    xr_compile_session_free(session);
    /* Both complete syntax owners survive the parser's borrowed session. */
    CHECK(scope_node(first,propagation)->as.scope_block.scope_mode==mode);
    xr_program_destroy(first);xr_program_destroy(second);
}
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    roundtrip(resources,"scope { go worker() }",false,XR_SOURCE_SCOPE_NORMAL);
    roundtrip(resources,"scope(isolated) { go worker() }",false,XR_SOURCE_SCOPE_ISOLATED);
    roundtrip(resources,"try scope { go worker() }",true,XR_SOURCE_SCOPE_NORMAL);
    roundtrip(resources,"try scope(isolated) { go worker() }",true,XR_SOURCE_SCOPE_ISOLATED);
    const char *bad[]={"linked scope {go worker()}","linked go worker()","scope(other) {go worker()}",
        "scope(isolated, other) {go worker()}","try? scope {go worker()}","try! scope {go worker()}"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        XrCompilerSession *session=NULL;AstNode *program=NULL;
        CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_parse(session,bad[i],&program)==XR_PARSE_SYNTAX && !program);
        xr_compile_session_free(session);
    }
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(resources);puts("Scope syntax: independent exit marker and roundtrip PASS");return 0;
}
