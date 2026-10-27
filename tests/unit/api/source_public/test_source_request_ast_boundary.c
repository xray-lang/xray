/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_request_ast_boundary.c - Detached syntax is pinned immutable request input
 */
#include "xray_source_ast.h"
#include "api/xsource_ast_owner.h"
#include "frontend/parser/xast.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
int main(void) {
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    static const char bytes[]="const answer:i64=42\n";
    XrSourceText text={NULL,"request-ast-admission",bytes,sizeof(bytes)-1};XrSourceAst *ast=NULL;
    CHECK(xray_source_parse(&text,&options,&ast,NULL,NULL)==XR_SOURCE_OK && ast);
    XrXirSourceAstOwner syntax={0};CHECK(xr_source_ast_owned_input(ast,&syntax)==XR_SOURCE_OK);
    AstNode *program=syntax.input.program;
    XrCompileState *state=xr_compiler_arena_compile_state(program->as.program.arena);
    CHECK(state && !xr_compile_state_has_sessions(state));
    XrCompilerSession *active=NULL;
    CHECK(xr_compile_session_new_with_state(state,&active)==XR_COMPILE_RESOURCE_OK && active);
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build_ast(ast,&options,&artifact,&failure,NULL)==XR_SOURCE_BAD_ARGUMENT && !artifact);
    CHECK(failure.status==XR_SOURCE_BAD_ARGUMENT);
    xr_compile_session_free(active);
    CHECK(!xr_compile_state_has_sessions(state));
    program->as.program.owns_arena=false;
    CHECK(xray_source_build_ast(ast,&options,&artifact,&failure,NULL)==XR_SOURCE_BAD_ARGUMENT && !artifact);
    program->as.program.owns_arena=true;
    CHECK(xray_source_build_ast(ast,&options,&artifact,&failure,NULL)==XR_SOURCE_OK && artifact);
    xray_source_ast_free(ast);
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    XrSourceCallOptions call;CHECK(xray_source_call_options_init(&call,sizeof(call))==XR_SOURCE_OK);
    XrSourceResult *result=NULL;
    CHECK(xray_source_artifact_execute(execution,artifact,&call,&result,NULL)==XR_SOURCE_OK && result);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    int64_t answer=-1;CHECK(xray_source_result_i64(result,&answer)==XR_SOURCE_OK && answer==0);
    xray_source_result_free(result);
    puts("Detached AST accepts new requests; foreign active sessions and non-owning syntax reject");return 0;
}
