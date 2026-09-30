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
        AstNode *program = xr_parse(session,sources[spelling]);
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
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    where_same_constraints(session);
    XrArena arena; xr_arena_init(&arena,4096);
    XrCompilerSessionScope scope;
    CHECK(xr_compiler_session_push_arena(session,&arena,"where-limit.xr",&scope));
    Parser parser = {0};
    xr_parser_init(&parser,session,"where U:Sendable","where-limit.xr",&arena);
    bool seen = false;
    xr_parser_set_error_callback(&parser,limit_diagnostic,&seen,0);
    XrGenericParam parameter = {0}; parameter.name = "U"; parameter.constraint_count = INT_MAX;
    XrGenericParam *parameters[] = {&parameter};
    xr_parser_advance(&parser);
    xr_parse_where_clause(&parser,parameters,1);
    CHECK(seen && parser.had_error && parser.error_count == 1);
    CHECK(parameter.constraint_count == INT_MAX && !parameter.constraints);
    xr_type_scope_free(parser.type_scope);
    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena); xr_compiler_session_delete(session);
    return 0;
}
