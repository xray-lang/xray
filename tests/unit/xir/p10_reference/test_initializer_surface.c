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

static void struct_spread_surface(XrCompilerSession *session) {
    AstNode *ast = NULL;
    const char *source = "const value=Pair{...source, x, y:2}\n";
    CHECK(xr_compile_parse_with_trivia(session,source,"spread.xr",NULL,&ast) == XR_PARSE_OK);
    StructLiteralNode *literal = &ast->as.program.statements[0]->as.var_decl.initializer->as.struct_literal;
    CHECK(literal->field_count == 3 && !literal->field_names[0]);
    CHECK(literal->field_values[0]->type == AST_SPREAD_EXPR &&
        literal->field_values[0]->as.spread_expr.expr->type == AST_VARIABLE);
    CHECK(!strcmp(literal->field_names[1],"x") && literal->field_values[1]->type == AST_VARIABLE);
    for (unsigned wrap = 0; wrap < 2; ++wrap) {
        XrFmtConfig config = xfmt_default_config;
        config.wrap_long_lines = (int)wrap; config.max_line_length = 24;
        XrFmtOutput formatted = {0}; AstNode *roundtrip = NULL;
        CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),ast,&config,&formatted) == XR_FMT_OK);
        CHECK(formatted.text && strstr(formatted.text,"...source") && !strstr(formatted.text,"(null)"));
        CHECK(xr_compile_parse_with_trivia(session,formatted.text,"spread-formatted.xr",NULL,&roundtrip) == XR_PARSE_OK);
        literal = &roundtrip->as.program.statements[0]->as.var_decl.initializer->as.struct_literal;
        CHECK(literal->field_count == 3 && !literal->field_names[0] &&
            literal->field_values[0]->type == AST_SPREAD_EXPR);
        CHECK(!strcmp(literal->field_names[1],"x") && !strcmp(literal->field_names[2],"y"));
        xr_program_destroy(roundtrip); xr_compile_format_output_free(&formatted);
    }
    xr_program_destroy(ast);
    ast = NULL;
    CHECK(xr_compile_parse_with_trivia(session,"const value=module.Pair{...source,x:2}\n",
        "qualified-spread.xr",NULL,&ast) == XR_PARSE_OK);
    XrFmtOutput formatted = {0}; AstNode *roundtrip = NULL;
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),ast,&xfmt_default_config,&formatted) == XR_FMT_OK);
    CHECK(formatted.text && strstr(formatted.text,"...source"));
    CHECK(xr_compile_parse_with_trivia(session,formatted.text,"qualified-formatted.xr",NULL,&roundtrip) == XR_PARSE_OK);
    EnumConstructNode *qualified = &roundtrip->as.program.statements[0]->as.var_decl.initializer->as.enum_construct;
    CHECK(qualified->field_count == 2 && !qualified->field_names[0] &&
        qualified->field_values[0]->type == AST_SPREAD_EXPR);
    xr_program_destroy(ast); xr_program_destroy(roundtrip); xr_compile_format_output_free(&formatted);
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
    xr_program_destroy(ast); struct_spread_surface(session);
    xr_compile_session_free(session); source_fixture_owner_free(&owner);
    puts("Contextual initializer, exact retired diagnostic and owned formatter roundtrip passed");
    return 0;
}
