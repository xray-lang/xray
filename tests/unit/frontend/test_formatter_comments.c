/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_formatter_comments.c - Phase 1 formatter comment fidelity
 *
 * KEY CONCEPT:
 *   End-to-end check that the parser+formatter pipeline preserves
 *   leading and trailing comments through a round-trip:
 *
 *     source -> xr_parse_with_trivia -> xfmt_format_ast -> output
 *
 *   The contract under test:
 *
 *     1. A line comment that lives on a line BEFORE a statement
 *        re-appears as a leading comment on the same statement, on
 *        its own line, with the original indentation level.
 *     2. An inline comment on the SAME line as a statement re-emerges
 *        as a trailing comment after the statement body, before the
 *        terminating newline (L-06).
 *     3. Formatting is idempotent: format(format(x)) == format(x).
 *
 *   We also verify a few negative cases that would silently break if
 *   the lexer / parser regressed:
 *
 *     - a comment after a newline NEVER attaches as trailing to the
 *       preceding statement;
 *     - a multi-line block comment NEVER attaches as trailing.
 */

#include "../test_framework.h"

#include "frontend/format/xfmt.h"
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast.h"
#include "xray_isolate.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrayIsolate *g_iso = NULL;

static void setup(void) {
    XrayIsolateParams p;
    xray_isolate_params_init(&p);
    g_iso = xray_isolate_new(&p);
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

// Parse + format a snippet, returning a heap string the caller frees.
// On parse failure returns NULL so the test fails fast with a clear
// message via ASSERT_NOT_NULL().
static char *parse_and_format(const char *source) {
    AstNode *ast = xr_parse_with_trivia(g_iso, source, "<test>");
    if (!ast) return NULL;
    char *out = xfmt_format_ast(ast, NULL, g_iso);
    xr_program_destroy(ast);
    return out;
}

// strstr() but takes a length-bounded haystack for safety.
static bool contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(leading_line_comment_preserved) {
    setup();
    const char *src =
        "// pre-amble\n"
        "let x = 5;\n";
    char *out = parse_and_format(src);
    ASSERT_NOT_NULL(out);
    // The pre-amble comment must precede the let on its own line.
    ASSERT_TRUE(contains(out, "// pre-amble\nlet x"));
    free(out);
    teardown();
}

TEST(trailing_line_comment_preserved) {
    setup();
    const char *src = "let x = 5; // tail\n";
    char *out = parse_and_format(src);
    ASSERT_NOT_NULL(out);
    // The trailing comment must live on the SAME line as the
    // statement body and use the canonical "  // tail" form (two-
    // space gap, line comment) before the terminating newline.
    ASSERT_TRUE(contains(out, "  // tail\n"));
    // No second copy on a separate line.
    ASSERT_FALSE(contains(out, "// tail\nlet"));
    free(out);
    teardown();
}

TEST(trailing_comment_does_not_steal_next_line_leading) {
    setup();
    // A comment that lives on a line BY ITSELF must remain leading
    // for the NEXT statement, not trailing of the previous one.
    const char *src =
        "let x = 5;\n"
        "// belongs to y\n"
        "let y = 6;\n";
    char *out = parse_and_format(src);
    ASSERT_NOT_NULL(out);

    // The comment must NOT be glued to `5;` as a trailing.
    ASSERT_FALSE(contains(out, "x = 5  // belongs"));
    // It must precede `y` on its own line.
    ASSERT_TRUE(contains(out, "// belongs to y\nlet y"));
    free(out);
    teardown();
}

TEST(multiline_block_comment_stays_leading) {
    setup();
    // Multi-line `/* ... */` after a statement is NOT a trailing
    // candidate; it becomes the next statement's leading.
    const char *src =
        "let x = 5;\n"
        "/* multi\n"
        "   line */\n"
        "let y = 6;\n";
    char *out = parse_and_format(src);
    ASSERT_NOT_NULL(out);

    // Block comment must NOT be inlined as trailing.
    ASSERT_FALSE(contains(out, "x = 5  /*"));
    // It must appear before `y`.
    ASSERT_TRUE(contains(out, "*/\nlet y"));
    free(out);
    teardown();
}

TEST(format_is_idempotent_with_comments) {
    setup();
    const char *src =
        "// header\n"
        "let x = 5; // tail-x\n"
        "let y = 6;\n"
        "// before-z\n"
        "let z = 7;\n";

    char *first = parse_and_format(src);
    ASSERT_NOT_NULL(first);

    char *second = parse_and_format(first);
    ASSERT_NOT_NULL(second);

    // Round-trip stability: re-formatting the formatted output must
    // produce byte-identical bytes (canonical form is a fixed point).
    ASSERT_STR_EQ(second, first);

    free(first);
    free(second);
    teardown();
}

TEST(trailing_comment_on_block_decl) {
    setup();
    // Trailing comments on closing braces of block-bodied declarations
    // must travel with the outer declaration, not the inner block.
    const char *src =
        "fn foo() {\n"
        "    let x = 1;\n"
        "} // end-foo\n";
    char *out = parse_and_format(src);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(contains(out, "}  // end-foo\n"));
    free(out);
    teardown();
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("Formatter comment fidelity (L-06 / F)");
    RUN_TEST(leading_line_comment_preserved);
    RUN_TEST(trailing_line_comment_preserved);
    RUN_TEST(trailing_comment_does_not_steal_next_line_leading);
    RUN_TEST(multiline_block_comment_stays_leading);
    RUN_TEST(format_is_idempotent_with_comments);
    RUN_TEST(trailing_comment_on_block_decl);
TEST_MAIN_END()
