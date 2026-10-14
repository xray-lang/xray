/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_type_surface.c - Retired type qualifiers and owned type formatting
 *
 * KEY CONCEPT:
 *   Read-only permissions belong to bindings, fields and view types.
 *   A retired qualifier cannot create a type or survive a formatter roundtrip.
 */
#include "frontend/parser/xparse.h"
#include "frontend/format/xfmt.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"

typedef struct RejectedType { const char *source; int column; } RejectedType;
typedef struct Diagnostic { int column; unsigned count; } Diagnostic;

static void retired_type_error(void *data, int line, int column, int end_line,
    int end_column, const char *message) {
    if (!strstr(message, "const type qualifier was removed; use const on bindings or fields")) return;
    Diagnostic *diagnostic = data;
    if (!diagnostic->count) {
        CHECK(line == 1 && column == diagnostic->column);
        CHECK(end_line == 1 && end_column == column + 5);
    }
    ++diagnostic->count;
}

static void rejected_types(XrCompilerSession *session) {
    const RejectedType cases[] = {
        {"fn inspect(value: const Array<string>) {}", 19},
        {"fn inspect(value: Array<const string>) {}", 25},
        {"fn inspect()->const i64{return 7}", 15},
        {"var value: const const Array<i64>", 12},
        {"type Alias = const i64", 14},
        {"struct Box { value: const i64 }", 21},
        {"type Callback = fn(const Slice<u8>) -> i64", 20},
        {"type Pair = (i64, const string)", 19},
        {"type Choice = i64 | const string", 21},
        {"fn inspect(value: move const Array<i64>) {}", 24},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        AstNode *ast = NULL; Diagnostic diagnostic = {cases[i].column, 0};
        XrParseDiagnostics diagnostics = {retired_type_error, &diagnostic, 16};
        CHECK(xr_compile_parse_with_trivia(session, cases[i].source, "type-rejected.xr", &diagnostics, &ast) == XR_PARSE_SYNTAX);
        if (ast || !diagnostic.count) fprintf(stderr, "case %u: %s\n", i, cases[i].source);
        CHECK(!ast && diagnostic.count);
    }
}

int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK);
    rejected_types(session);
    const char *source =
        "const count:i64=7\n"
        "struct Box{const value:i64 init(value){this.value=value}}\n"
        "type Callback=fn(data:Slice<u8>)->Slice<u8> from data\n"
        "fn inspect(data:Slice<u8>, output:ref MutSlice<u8>)->Slice<u8> from data{return data}\n";
    AstNode *ast = NULL, *roundtrip = NULL;
    CHECK(xr_compile_parse_with_trivia(session, source, "type-positive.xr", NULL, &ast) == XR_PARSE_OK);
    XrFmtOutput formatted = {0};
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session), ast, &xfmt_default_config, &formatted) == XR_FMT_OK);
    CHECK(formatted.text && strstr(formatted.text, "const count") && strstr(formatted.text, "const value"));
    CHECK(strstr(formatted.text, "Slice<u8>") && strstr(formatted.text, "MutSlice<u8>") && strstr(formatted.text, "from data"));
    CHECK(!strstr(formatted.text, "const Slice") && !strstr(formatted.text, "const i64"));
    CHECK(xr_compile_parse_with_trivia(session, formatted.text, "type-formatted.xr", NULL, &roundtrip) == XR_PARSE_OK);
    xr_program_destroy(roundtrip); xr_program_destroy(ast); xr_compile_format_output_free(&formatted);
    xr_compile_session_free(session); source_fixture_owner_free(&owner);
    puts("Ten exact retired type diagnostics, const bindings/fields and owned view formatter roundtrip passed");
    return 0;
}
