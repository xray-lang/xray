/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_nosuspend_type.c - Real alias, formatting and syntax identity consumers
 */
#include "frontend/parser/xparse.h"
#include "frontend/parser/xtype_ref.h"
#include "frontend/parser/xast_walk.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static AstNode *parse(XrCompilerSession *session,const char *text) {
    AstNode *program=NULL;CHECK(xr_compile_parse(session,text,&program)==XR_PARSE_OK && program);return program;
}
static XrTypeRef *parameter(AstNode *program,unsigned function) {
    AstNode *node=program->as.program.statements[function];CHECK(node->type==AST_FUNCTION_DECL && node->as.function_decl.param_count==1);
    return node->as.function_decl.params[0]->type;
}
int main(void) {
    XrCompileResources *resources=NULL;const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    char input[]="type Callback<E> = nosuspend fn(i64) throws(E)->i64\nfn apply(cb:(Callback<Failure>)?){}\n";
    AstNode *alias=parse(session,input);XrTypeRef *optional=parameter(alias,1);
    CHECK(optional->kind==XR_TREF_OPTIONAL && optional->nchildren==1);
    XrTypeRef *function=optional->children[0];
    CHECK(function->kind==XR_TREF_FUNCTION && function->requires_nosuspend && !function->requires_nothrow);
    CHECK(function->throws_clause.syntax==XR_THROWS_TYPED && function->throws_clause.type &&
        !strcmp(function->throws_clause.type->name,"Failure"));
    char printed[512]={0};
    CHECK(xr_compile_tref_to_string_structural(xr_compile_session_compile_state(session),optional,printed,sizeof(printed))>0);
    CHECK(strstr(printed,"nosuspend fn(") && strstr(printed,"throws(Failure)"));
    char text[1024];int written=snprintf(text,sizeof(text),"fn apply(cb:%s){}\n",printed);CHECK(written>0 && (size_t)written<sizeof(text));
    AstNode *formatted=parse(session,text);XrTypeRef *again=parameter(formatted,0);
    CHECK(again->kind==XR_TREF_OPTIONAL && again->children[0]->requires_nosuspend &&
        again->children[0]->throws_clause.syntax==XR_THROWS_TYPED &&
        !strcmp(again->children[0]->throws_clause.type->name,"Failure"));
    AstNode *qualified=parse(session,"fn apply(cb:nosuspend fn(i64)->i64){}\n");
    AstNode *ordinary=parse(session,"fn apply(cb:fn(i64)->i64){}\n");
    char restricted[4096],unrestricted[4096];
    CHECK(xr_ast_node_signature(qualified->as.program.statements[0],restricted,sizeof(restricted)) &&
        xr_ast_node_signature(ordinary->as.program.statements[0],unrestricted,sizeof(unrestricted)) && strcmp(restricted,unrestricted));
    memset(input,'!',sizeof(input));memset(text,'!',sizeof(text));xr_compile_session_free(session);
    xr_program_destroy(alias);xr_program_destroy(formatted);xr_program_destroy(qualified);xr_program_destroy(ordinary);
    CHECK(strstr(printed,"nosuspend fn(") && strstr(restricted,"/nosuspend"));
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==before.held_bytes);
    xr_compile_resources_release(resources);puts("Source nosuspend: alias E/nullable, real format roundtrip, syntax identity and retired owners PASS");return 0;
}
