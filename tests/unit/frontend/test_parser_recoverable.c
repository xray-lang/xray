/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_recoverable.c - parser error-recovery contract tests
 *
 * KEY CONCEPT:
 *   xr_parse_recoverable() is the LSP / MCP / fuzz entry point. Its
 *   contract differs from xr_parse() in three load-bearing ways:
 *
 *     1. It MUST return a (possibly partial) AST_PROGRAM even when
 *        errors are present -- LSP shows incomplete symbol lists
 *        from a still-being-typed buffer, so a NULL return on the
 *        first error breaks "as you type" features.
 *
 *     2. It MUST resync after a syntax error and continue parsing
 *        sibling top-level declarations, otherwise a single typo
 *        hides every symbol that follows.
 *
 *     3. The error callback MUST fire with sensible (line, column,
 *        end_line, end_column, message) coordinates -- LSP turns
 *        them directly into a diagnostic range.
 *
 *   Pre-Phase-1 the recoverable path silently returned NULL on
 *   isolated invalid sources and the panic-mode resync was patchy.
 *   These tests pin down the closed-loop contract so a regression
 *   in any of (lexer error reporting / parser panic-mode sync set /
 *   AST partial construction) shows up here.
 *
 *   The test only uses public APIs:
 *     - xr_parser_init / xr_parser_set_error_callback
 *     - xr_parse_recoverable
 *     - parser.had_error / parser.error_count / parser.max_errors
 *     - the AST_PROGRAM root structure
 */

#include "../test_framework.h"

#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "base/xarena.h"
#include "base/xmalloc.h"
#include "xray_isolate.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrayIsolate *g_iso = NULL;

static void setup(void) {
    if (!g_iso) {
        XrayIsolateParams p;
        xray_isolate_params_init(&p);
        g_iso = xray_isolate_new(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

// Diagnostic capture: collects every callback invocation so the test
// can assert on counts AND on the structural fields LSP relies on.
typedef struct DiagRecord {
    int line;
    int column;
    int end_line;
    int end_column;
    char message[256];
} DiagRecord;

#define DIAG_CAP 32

typedef struct DiagSink {
    DiagRecord records[DIAG_CAP];
    int count;
} DiagSink;

static void diag_callback(void *user_data, int line, int column, int end_line, int end_column,
                          const char *message) {
    DiagSink *sink = (DiagSink *) user_data;
    if (sink->count >= DIAG_CAP)
        return;  // overflow -- assert in test
    DiagRecord *r = &sink->records[sink->count++];
    r->line = line;
    r->column = column;
    r->end_line = end_line;
    r->end_column = end_column;
    r->message[0] = '\0';
    if (message) {
        size_t n = strlen(message);
        if (n >= sizeof(r->message))
            n = sizeof(r->message) - 1;
        memcpy(r->message, message, n);
        r->message[n] = '\0';
    }
}

// Drive xr_parse_recoverable with a captured-diagnostic sink.
// Each call owns its own arena so the parser has somewhere to
// allocate AST nodes; LSP/MCP callers in production pre-install
// an arena on the isolate, but in unit-test scope the cleanest
// contract-faithful path is to pass the arena explicitly via the
// API parameter xr_parser_init already exposes.
//
// Returns the parsed AST. The arena is captured into *out_arena
// so the caller can free it after inspection.
static AstNode *parse_recoverable(const char *source, Parser *out_parser, DiagSink *sink,
                                  int max_errors, XrArena **out_arena) {
    sink->count = 0;
    XrArena *arena = (XrArena *) xr_malloc(sizeof(XrArena));
    xr_arena_init(arena, XR_ARENA_SEGMENT_SIZE);
    *out_arena = arena;

    xr_parser_init(out_parser, g_iso, source, "<test>", arena);
    xr_parser_set_error_callback(out_parser, diag_callback, sink, max_errors);
    return xr_parse_recoverable(out_parser);
}

// Release the arena returned by parse_recoverable. AST nodes live
// inside the arena, so this single call frees the entire AST.
static void release_arena(XrArena *arena) {
    if (!arena)
        return;
    xr_arena_destroy(arena);
    xr_free(arena);
}

// Count how many top-level statements made it into the AST.
static int program_decl_count(AstNode *ast) {
    if (!ast || ast->type != AST_PROGRAM)
        return -1;
    return ast->as.program.count;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(clean_source_no_errors) {
    // Baseline: clean source must produce an AST with 0 diagnostics
    // and parser.had_error == 0. This pins the "no false positives"
    // half of the contract.
    setup();
    Parser parser;
    DiagSink sink;
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable("let a = 1;\n"
                                     "let b = 2;\n"
                                     "fn f() { return a + b; }\n",
                                     &parser, &sink, 0, &arena);

    ASSERT_NOT_NULL(ast);
    ASSERT_EQ_INT(parser.had_error, 0);
    ASSERT_EQ_INT(parser.error_count, 0);
    ASSERT_EQ_INT(sink.count, 0);
    ASSERT_EQ_INT(program_decl_count(ast), 3);
    release_arena(arena);
    teardown();
}

TEST(returns_partial_ast_on_error) {
    // The defining property of xr_parse_recoverable: even when the
    // source has a syntax error, the function MUST return an
    // AST_PROGRAM root (not NULL). LSP relies on this to surface
    // partial symbols while the user is still typing.
    setup();
    Parser parser;
    DiagSink sink;
    // Deliberately broken: `let x =` has no expression. The parser
    // must report it AND keep going for the next two decls.
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable("let x =\n"
                                     "let y = 2;\n"
                                     "let z = 3;\n",
                                     &parser, &sink, 0, &arena);

    ASSERT_NOT_NULL(ast);
    ASSERT_EQ_INT(ast->type, AST_PROGRAM);
    ASSERT_TRUE(parser.had_error != 0);
    ASSERT_TRUE(parser.error_count >= 1);
    ASSERT_TRUE(sink.count >= 1);
    release_arena(arena);
    teardown();
}

TEST(resync_after_error_keeps_following_decls) {
    // Panic-mode resynchronisation must let the parser recover at
    // the next statement boundary. With ONE bad declaration in the
    // middle, the surrounding two clean ones must still appear in
    // the AST. Without resync, a typo would silently hide every
    // symbol below it -- a known LSP-killing bug pre-Phase-1.
    setup();
    Parser parser;
    DiagSink sink;
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable("let good_before = 1;\n"
                                     "let *** = ;\n"  // pure garbage -- multiple syntax errors
                                     "let good_after = 3;\n",
                                     &parser, &sink, 0, &arena);

    ASSERT_NOT_NULL(ast);
    ASSERT_TRUE(parser.had_error != 0);

    int decls = program_decl_count(ast);
    // The middle (broken) decl may produce 0, 1, or "garbage" nodes
    // depending on resync granularity. The contract is only that
    // BOTH surrounding clean declarations land in the AST -- so we
    // require at least 2 decls.
    ASSERT_TRUE(decls >= 2);
    release_arena(arena);
    teardown();
}

TEST(multiple_errors_collected) {
    // The error callback must fire ONCE PER DIAGNOSTIC. A pre-
    // Phase-1 bug suppressed every error after the first by
    // staying in panic_mode forever. This test pins down "many
    // separate errors -> many separate callback hits".
    setup();
    Parser parser;
    DiagSink sink;
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable("let a = ;\n"   // error 1
                                     "let b = ;\n"   // error 2
                                     "let c = ;\n",  // error 3
                                     &parser, &sink, 0, &arena);

    ASSERT_NOT_NULL(ast);
    ASSERT_TRUE(sink.count >= 3);
    ASSERT_EQ_INT(parser.error_count, sink.count);

    // Each diagnostic must carry coordinates LSP can convert:
    //   - line >= 1 (1-indexed; LSP later subtracts 1 itself)
    //   - column >= 0 (synthetic anchors at EOL emit column=0,
    //     which LSP maps to character=0 -- still valid)
    //   - end_line >= line (range cannot run backwards)
    //   - message non-empty
    for (int i = 0; i < sink.count; i++) {
        ASSERT_TRUE(sink.records[i].line >= 1);
        ASSERT_TRUE(sink.records[i].column >= 0);
        ASSERT_TRUE(sink.records[i].end_line >= sink.records[i].line);
        ASSERT_TRUE(sink.records[i].message[0] != '\0');
    }
    release_arena(arena);
    teardown();
}

TEST(max_errors_caps_callback_count) {
    // `max_errors` must be respected so a hugely broken source
    // does not flood the LSP transport. The cap is on CALLBACK
    // invocations; the parser may continue internally to keep the
    // AST consistent, but no more than `max_errors` diagnostics
    // are forwarded to the user.
    setup();
    Parser parser;
    DiagSink sink;
    // 8 broken decls; cap at 3.
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable("let a = ;\n"
                                     "let b = ;\n"
                                     "let c = ;\n"
                                     "let d = ;\n"
                                     "let e = ;\n"
                                     "let f = ;\n"
                                     "let g = ;\n"
                                     "let h = ;\n",
                                     &parser, &sink, 3, &arena);

    ASSERT_NOT_NULL(ast);
    // Some implementations include a final "too many errors" note
    // beyond max_errors; allow that single overflow but no more.
    ASSERT_TRUE(sink.count >= 1);
    ASSERT_TRUE(sink.count <= 4);
    release_arena(arena);
    teardown();
}

TEST(broken_function_body_does_not_eat_following_decls) {
    // A specific LSP-killer scenario: a half-typed function body
    // would historically swallow everything until EOF because the
    // resync set lacked `}` or `fn`. This test asserts the modern
    // resync set does the right thing: the let following `fn f`
    // still lands in the AST.
    setup();
    Parser parser;
    DiagSink sink;
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable("fn f() {\n"
                                     "    let oops =\n"  // missing expression + missing `;`
                                     "}\n"
                                     "let after = 99;\n",
                                     &parser, &sink, 0, &arena);

    ASSERT_NOT_NULL(ast);
    ASSERT_TRUE(parser.had_error != 0);
    ASSERT_TRUE(program_decl_count(ast) >= 2);
    release_arena(arena);
    teardown();
}

TEST(empty_and_whitespace_source_no_error) {
    // Edge: empty source / pure-whitespace source must NOT trigger
    // an error. The recoverable parser is the LSP path -- a buffer
    // that the user just opened is empty for a few keystrokes and
    // must not show diagnostics.
    setup();

    {
        Parser parser;
        DiagSink sink;
        XrArena *arena = NULL;
        AstNode *ast = parse_recoverable("", &parser, &sink, 0, &arena);
        ASSERT_NOT_NULL(ast);
        ASSERT_EQ_INT(ast->type, AST_PROGRAM);
        ASSERT_EQ_INT(parser.had_error, 0);
        ASSERT_EQ_INT(sink.count, 0);
        ASSERT_EQ_INT(program_decl_count(ast), 0);
        release_arena(arena);
    }

    {
        Parser parser;
        DiagSink sink;
        XrArena *arena = NULL;
        AstNode *ast = parse_recoverable("   \n\n\t  \n", &parser, &sink, 0, &arena);
        ASSERT_NOT_NULL(ast);
        ASSERT_EQ_INT(parser.had_error, 0);
        ASSERT_EQ_INT(sink.count, 0);
        release_arena(arena);
    }
    teardown();
}

TEST(error_coordinates_in_source_bounds) {
    // Stronger structural check: every reported diagnostic's
    // (line, column) must point INSIDE the source buffer. A
    // pre-Phase-1 bug emitted (line=0, column=0) for "synthetic"
    // errors which broke LSP range conversion.
    setup();
    Parser parser;
    DiagSink sink;
    const char *src = "let a = 1;\n"
                      "let b = ;\n"  // error here at line 2
                      "let c = 3;\n";
    XrArena *arena = NULL;
    AstNode *ast = parse_recoverable(src, &parser, &sink, 0, &arena);

    ASSERT_NOT_NULL(ast);
    ASSERT_TRUE(sink.count >= 1);
    // Count newlines to find max line.
    int max_line = 1;
    for (const char *p = src; *p; p++)
        if (*p == '\n')
            max_line++;
    for (int i = 0; i < sink.count; i++) {
        ASSERT_TRUE(sink.records[i].line >= 1);
        ASSERT_TRUE(sink.records[i].line <= max_line);
    }
    release_arena(arena);
    teardown();
}

TEST(null_parser_returns_null_safely) {
    // NULL-safety: xr_parse_recoverable with NULL parser must NOT
    // crash. Callers (e.g. fuzz harness) rely on this for early-
    // exit on init failure.
    AstNode *ast = xr_parse_recoverable(NULL);
    ASSERT_NULL(ast);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("xr_parse_recoverable contract");
RUN_TEST(clean_source_no_errors);
RUN_TEST(returns_partial_ast_on_error);
RUN_TEST(resync_after_error_keeps_following_decls);
RUN_TEST(multiple_errors_collected);
RUN_TEST(max_errors_caps_callback_count);
RUN_TEST(broken_function_body_does_not_eat_following_decls);
RUN_TEST(empty_and_whitespace_source_no_error);
RUN_TEST(error_coordinates_in_source_bounds);
RUN_TEST(null_parser_returns_null_safely);
TEST_MAIN_END()
