/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_initializer_surface.c - Contextual initializer syntax and owned formatting
 *
 * KEY CONCEPT:
 *   The same ordinary identifier becomes an initializer only at a type member.
 *   Retired spelling is rejected before it can acquire construction authority.
 */
#include "frontend/parser/xparse.h"
#include "frontend/format/xfmt.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"

static void retired_error(void *data, int line, int column, int end_line,
    int end_column, const char *message) {
    if (!strstr(message, "constructor spelling was removed; use init")) return;
    CHECK(line == 1 && column == 14 && end_line == 1 && end_column == 25);
    *(bool *)data = true;
}

int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrCompileState *state = xr_compile_session_compile_state(session);
    const char *source = "fn init()->i64{return 7}\nstruct Box {\n value:i64\n init(value){this.value=value}\n}\n";
    AstNode *ast = NULL, *roundtrip = NULL;
    CHECK(xr_compile_parse_with_trivia(session, source, "initializer.xr", NULL, &ast) == XR_PARSE_OK);
    CHECK(ast && ast->type == AST_PROGRAM && ast->as.program.count == 2);
    Scanner scanner;
    CHECK(xr_compile_scanner_open(&scanner, state, ast->as.program.arena, "init constructor") == XR_COMPILE_RESOURCE_OK);
    Token init = xr_scanner_scan(&scanner), retired = xr_scanner_scan(&scanner);
    CHECK(init.type == TK_NAME && init.length == 4 && !memcmp(init.start, "init", 4));
    CHECK(retired.type == TK_NAME && retired.length == 11 && !memcmp(retired.start, "constructor", 11));
    CHECK(xr_scanner_scan(&scanner).type == TK_EOF);
    AstNode *function = ast->as.program.statements[0], *box = ast->as.program.statements[1];
    CHECK(function->type == AST_FUNCTION_DECL && !strcmp(function->as.function_decl.name, "init"));
    CHECK(box->type == AST_STRUCT_DECL && box->as.struct_decl.method_count == 1);
    MethodDeclNode *method = &box->as.struct_decl.methods[0]->as.method_decl;
    CHECK(method->is_constructor && !strcmp(method->name, "init") && method->param_count == 1);
    CHECK(!method->params[0]->type && box->as.struct_decl.methods[0]->line == 4 && box->as.struct_decl.methods[0]->column == 2);
    XrFmtOutput formatted = {0};
    CHECK(xr_compile_format_ast(state, ast, &xfmt_default_config, &formatted) == XR_FMT_OK);
    CHECK(formatted.text && strstr(formatted.text, "fn init()") && strstr(formatted.text, "init(value)"));
    CHECK(!strstr(formatted.text, "constructor"));
    CHECK(xr_compile_parse_with_trivia(session, formatted.text, "formatted.xr", NULL, &roundtrip) == XR_PARSE_OK);
    CHECK(roundtrip->as.program.statements[1]->as.struct_decl.methods[0]->as.method_decl.is_constructor);
    xr_program_destroy(ast); xr_program_destroy(roundtrip); xr_compile_format_output_free(&formatted);
    ast = NULL; roundtrip = NULL;

    bool diagnosed = false; AstNode *rejected = NULL;
    XrParseDiagnostics diagnostics = {retired_error, &diagnosed, 8};
    CHECK(xr_compile_parse_with_trivia(session, "struct Old { constructor() {} }", "retired.xr", &diagnostics, &rejected) == XR_PARSE_SYNTAX);
    CHECK(diagnosed && !rejected);
    CHECK(xr_compile_parse_with_trivia(session, "struct Box{value:i64 init(other=41){this.value=other}}", "default.xr", NULL, &ast) == XR_PARSE_OK);
    method = &ast->as.program.statements[0]->as.struct_decl.methods[0]->as.method_decl;
    CHECK(!method->params[0]->type && method->params[0]->default_value);
    xr_program_destroy(ast); xr_compile_session_free(session); source_fixture_owner_free(&owner);
    puts("Contextual initializer, exact retired diagnostic and owned formatter roundtrip passed");
    return 0;
}
