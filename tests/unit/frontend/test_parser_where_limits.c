/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_where_limits.c - Count overflow cannot weaken a constraint clause
 *
 * KEY CONCEPT:
 *   The real parser helper rejects impossible merge sizes before indexing prior arrays.
 */
#include "frontend/parser/xparse_internal.h"
#include "frontend/parser/xtype_scope.h"
#include "frontend/parser/xtype_ref.h"
#include "base/xarena.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static void limit_diagnostic(void *data, int line, int column, int end_line,
    int end_column, const char *message) {
    (void)line; (void)column; (void)end_line; (void)end_column;
    bool *seen = data;
    CHECK(strstr(message,"where constraint count exceeds parser limits"));
    *seen = true;
}
static void where_same_constraints(XrCompilerSession *session) {
    const char *sources[] = {
        "interface I<A:Sendable>{map<U:Evidence<A> & Sendable,V>(value:U,tag:V)->U}",
        "interface I<A:Sendable>{map<U,V>(value:U,tag:V)->U where U:Evidence<A> & Sendable}"
    };
    for (uint32_t spelling = 0; spelling < 2; ++spelling) {
        AstNode *program = NULL;
        CHECK(xr_compile_parse(session,sources[spelling],&program) == XR_PARSE_OK);
        CHECK(program && program->type == AST_PROGRAM && program->as.program.count == 1);
        AstNode *node = program->as.program.statements[0];
        CHECK(node->type == AST_INTERFACE_DECL);
        InterfaceDeclNode *declaration = &node->as.interface_decl;
        CHECK(declaration->type_param_count == 1 && declaration->type_params[0]->constraint_count == 1);
        CHECK(!strcmp(declaration->type_params[0]->constraints[0]->name,"Sendable"));
        CHECK(declaration->method_count == 1 && declaration->methods[0]->type == AST_INTERFACE_METHOD);
        InterfaceMethodNode *method = &declaration->methods[0]->as.interface_method;
        CHECK(method->type_param_count == 2 && method->type_params[0]->constraint_count == 2);
        CHECK(method->type_params[1]->constraint_count == 0);
        const XrTypeRef *application = method->type_params[0]->constraints[0];
        CHECK(!strcmp(application->name,"Evidence") && application->nchildren == 1);
        CHECK(!strcmp(application->children[0]->name,"A"));
        CHECK(!strcmp(method->type_params[0]->constraints[1]->name,"Sendable"));
        xr_program_destroy(program);
    }
}
int main(void) {
    /* All three fixed parser operations share one finite ledger. */
    const XrCompileResourceLimits limits = {1024 * 1024, 512 * 1024, 1024 * 1024};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0}, observed = {0};
    CHECK(xr_compile_resources_stats(resources,&baseline) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(resources,&session) == XR_COMPILER_SESSION_OK);
    where_same_constraints(session);
    XrCompileState *state = xr_compile_session_compile_state(session);
    CHECK(xr_compile_state_resources(state) == resources);
    XrArena arena = {0};
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(state,&backing) == XR_ARENA_OK);
    CHECK(xr_arena_open(&arena,4096,&backing) == XR_ARENA_OK);
    CHECK(xr_compiler_arena_matches_state(&arena,state));
    Parser parser = {0};
    CHECK(xr_compile_parser_open(&parser,session,"where U:Sendable","where-limit.xr",&arena) == XR_PARSE_OK);
    CHECK(parser.state == state && xr_compile_session_current_arena(session) == &arena);
    bool seen = false;
    xr_parser_set_error_callback(&parser,limit_diagnostic,&seen,0);
    XrGenericParam parameter = {0}; parameter.name = "U"; parameter.constraint_count = INT_MAX;
    XrGenericParam *parameters[] = {&parameter};
    xr_parser_advance(&parser);
    xr_parse_where_clause(&parser,parameters,1);
    CHECK(seen && parser.had_error && parser.error_count == 1);
    CHECK(parameter.constraint_count == INT_MAX && !parameter.constraints);
    CHECK(xr_compile_parser_status(&parser) == XR_PARSE_SYNTAX);
    CHECK(xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK);
    xr_compile_parser_close(&parser);
    CHECK(!parser.type_scope && !xr_compile_session_current_arena(session));
    xr_arena_destroy(&arena);
    xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources,&observed) == XR_COMPILE_RESOURCE_OK);
    CHECK(observed.live_bytes == baseline.live_bytes);
    CHECK(observed.allocated_bytes <= limits.allocated_bytes && observed.peak_bytes <= limits.live_bytes &&
          observed.work <= limits.work);
    printf("where fixed root allocated=%llu peak=%llu work=%llu owner-live-baseline=%llu\n",
        (unsigned long long)observed.allocated_bytes,(unsigned long long)observed.peak_bytes,
        (unsigned long long)observed.work,(unsigned long long)observed.live_bytes);
    xr_compile_resources_release(resources);
    return 0;
}
