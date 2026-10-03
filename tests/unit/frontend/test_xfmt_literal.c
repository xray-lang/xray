/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xfmt_literal.c - Regression tests for the formatter's string
 *                       and template literal output
 *
 * KEY CONCEPT:
 *   Verifies that the formatter emits round-trippable source for
 *   string literals and template strings. An earlier formatter
 *   wrote string payloads verbatim between two `"` characters and
 *   emitted templates between backticks; both produced source the
 *   lexer rejects.
 *
 *   These tests parse a source snippet, format the resulting AST,
 *   then re-parse the formatter's output and assert that:
 *     1. the formatted source is accepted by the parser, AND
 *     2. the original-string payload is preserved AST-identically.
 */

#include "../test_framework.h"
#include <string.h>
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast_api.h"
#include "frontend/parser/xast_types.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/format/xfmt.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"

/* ========== Test infrastructure ========== */

static XrCompileResources *g_resources;
static XrCompilerSession *g_session;
static void setup(void) {
    if (g_session) return;
    XrCompileResourceLimits limits = {UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
    ASSERT_EQ_INT(xr_compile_resources_new(&limits, &g_resources), XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(xr_compile_session_new(g_resources, &g_session), XR_COMPILER_SESSION_OK);
}
static void teardown(void) {
    xr_compile_session_free(g_session); g_session = NULL;
    xr_compile_resources_release(g_resources); g_resources = NULL;
}
static AstNode *fixture_parse(XrCompilerSession *session, const char *source, const char *path) {
    AstNode *ast = NULL;
    (void)xr_compile_parse_with_trivia(session, source, path, NULL, &ast);
    return ast;
}
static char *fixture_format(AstNode *ast, const XrFmtConfig *config) {
    XrFmtOutput output = {0};
    (void)xr_compile_format_ast(xr_compile_session_compile_state(g_session), ast, config, &output);
    return output.text;
}


// Walk a `var _ = <expr>` program down to the initializer expression.
// Returns NULL if the shape is unexpected (caller should ASSERT).
static AstNode *first_initializer(AstNode *program) {
    if (!program || program->type != AST_PROGRAM)
        return NULL;
    if (program->as.program.count < 1)
        return NULL;
    AstNode *stmt = program->as.program.statements[0];
    if (!stmt || stmt->type != AST_VAR_DECL)
        return NULL;
    return stmt->as.var_decl.initializer;
}

// Format the AST then re-parse the result. Returns NULL if either
// step fails; callers ASSERT on the return.
static AstNode *format_and_reparse(AstNode *program) {
    char *formatted = fixture_format(program, &xfmt_default_config);
    if (!formatted)
        return NULL;
    AstNode *reparsed = fixture_parse(g_session, formatted, "<literal>");
    xr_compile_resources_free(formatted);
    return reparsed;
}

// Format the AST and return the formatted source string. Caller frees.
static char *format_only(AstNode *program) {
    return fixture_format(program, &xfmt_default_config);
}

/* ========== simple ASCII strings round-trip ========== */

TEST(xfmt_string_simple_ascii) {
    setup();
    AstNode *prog =
        fixture_parse(g_session, "var s = \"hello world\"\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_NOT_NULL(init);
    ASSERT_EQ_INT(init->type, AST_LITERAL_STRING);
    ASSERT_STR_EQ(init->as.literal.raw_value.string_val, "hello world");
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

/* ========== embedded double quote ========== */

TEST(xfmt_string_embedded_quote) {
    setup();
    AstNode *prog = fixture_parse(g_session, "var s = \"a\\\"b\"\n", "<literal>");  // source: "a\"b"
    ASSERT_NOT_NULL(prog);
    AstNode *init0 = first_initializer(prog);
    ASSERT_STR_EQ(init0->as.literal.raw_value.string_val, "a\"b");

    char *formatted = format_only(prog);
    ASSERT_NOT_NULL(formatted);
    // The formatted output MUST escape the embedded quote — otherwise
    // the previous-formatter bug surfaces: an unescaped `"` would make
    // re-parse fail.
    ASSERT(strstr(formatted, "\\\"") != NULL);
    xr_compile_resources_free(formatted);

    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_EQ_INT(init->type, AST_LITERAL_STRING);
    ASSERT_STR_EQ(init->as.literal.raw_value.string_val, "a\"b");
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

/* ========== backslash and newline ========== */

TEST(xfmt_string_backslash_and_newline) {
    setup();
    // source string contains \\ and \n
    AstNode *prog =
        fixture_parse(g_session, "var s = \"line1\\nline2\\\\end\"\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    AstNode *init0 = first_initializer(prog);
    ASSERT_STR_EQ(init0->as.literal.raw_value.string_val, "line1\nline2\\end");

    char *formatted = format_only(prog);
    ASSERT_NOT_NULL(formatted);
    // Newline must be re-escaped as `\n`, NOT emitted as a raw 0x0A
    // (which would terminate the string at parse time).
    ASSERT(strstr(formatted, "\\n") != NULL);
    ASSERT(strstr(formatted, "\\\\") != NULL);
    xr_compile_resources_free(formatted);

    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_STR_EQ(init->as.literal.raw_value.string_val, "line1\nline2\\end");
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

/* ========== template string emits no backticks ========== */

TEST(xfmt_template_no_backticks) {
    setup();
    AstNode *prog =
        fixture_parse(g_session, "var s = \"hi ${name}!\"\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    AstNode *init0 = first_initializer(prog);
    ASSERT_EQ_INT(init0->type, AST_TEMPLATE_STRING);

    char *formatted = format_only(prog);
    ASSERT_NOT_NULL(formatted);
    // Backticks were dropped from the lexer; the formatter MUST NOT
    // emit them.
    ASSERT(strchr(formatted, '`') == NULL);
    // Must contain `${` somewhere because the template has an
    // interpolation slot.
    ASSERT(strstr(formatted, "${") != NULL);
    xr_compile_resources_free(formatted);

    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_EQ_INT(init->type, AST_TEMPLATE_STRING);
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

TEST(xfmt_template_expr_string_uses_double_quotes) {
    setup();
    AstNode *prog = fixture_parse(g_session, "var s = \"${\"inner\".toUpperCase()}\"\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    AstNode *init0 = first_initializer(prog);
    ASSERT_EQ_INT(init0->type, AST_TEMPLATE_STRING);

    char *formatted = format_only(prog);
    ASSERT_NOT_NULL(formatted);
    ASSERT(strstr(formatted, "${\"inner\".toUpperCase()}") != NULL);
    ASSERT(strstr(formatted, "${'inner'.toUpperCase()}") == NULL);
    xr_compile_resources_free(formatted);

    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_EQ_INT(init->type, AST_TEMPLATE_STRING);
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

/* ========== template literal `$` is escaped ========== */
//
// A template-string literal part containing `${` would be reparsed as
// an interpolation opener. xfmt_emit_template_string escapes every `$`
// inside literal parts as `\$` to prevent that.

TEST(xfmt_template_dollar_escaped) {
    setup();
    // Source uses `\$` to embed a literal `$` inside the template
    // literal part: parse-time sees "$" between two halves, and one
    // ${name} interpolation. After format+reparse, the literal part
    // must still be a literal `$`, NOT a second interpolation.
    AstNode *prog =
        fixture_parse(g_session, "var s = \"price=\\$${amount}\"\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    AstNode *init0 = first_initializer(prog);
    ASSERT_EQ_INT(init0->type, AST_TEMPLATE_STRING);
    int parts0 = init0->as.template_str.part_count;

    char *formatted = format_only(prog);
    ASSERT_NOT_NULL(formatted);
    // The literal `$` MUST be re-escaped — otherwise reparse would
    // greedily consume `${` as a second interpolation opener.
    ASSERT(strstr(formatted, "\\$") != NULL);
    xr_compile_resources_free(formatted);

    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_EQ_INT(init->type, AST_TEMPLATE_STRING);
    // Same number of parts before and after format+reparse.
    ASSERT_EQ_INT(init->as.template_str.part_count, parts0);
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

/* Control characters in text use Unicode escapes, not byte escapes. */
TEST(xfmt_string_control_byte_hex_escape) {
    setup();
    AstNode *program = fixture_parse(g_session, "var s = \"\\u{1}end\"\n", "<literal>");
    ASSERT_NOT_NULL(program);
    char *formatted = format_only(program);
    ASSERT_NOT_NULL(formatted);
    ASSERT(strchr(formatted, 0x01) == NULL);
    AstNode *again = fixture_parse(g_session, formatted, "<formatted>");
    ASSERT_NOT_NULL(again);
    AstNode *literal = first_initializer(again);
    ASSERT_NOT_NULL(literal);
    const unsigned char expected[] = {1,'e','n','d'};
    ASSERT_EQ_INT(literal->as.literal.string_length, sizeof(expected));
    ASSERT(memcmp(literal->as.literal.raw_value.string_val, expected, sizeof(expected)) == 0);
    xr_compile_resources_free(formatted);
    xr_program_destroy(again);
    xr_program_destroy(program);
    teardown();
}

/* ========== char literals ========== */

TEST(xfmt_rune_literal_roundtrip) {
    setup();
    AstNode *prog =
        fixture_parse(g_session, "var c: char = '\\u{1F600}'\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    AstNode *r = format_and_reparse(prog);
    AstNode *init = first_initializer(r);
    ASSERT_NOT_NULL(init);
    ASSERT_EQ_INT(init->type, AST_LITERAL_RUNE);
    ASSERT_EQ_INT(init->as.literal.raw_value.rune_val, 0x1F600);
    xr_program_destroy(prog);
    xr_program_destroy(r);
    teardown();
}

TEST(xfmt_rune_literal_named_escape) {
    setup();
    AstNode *prog = fixture_parse(g_session, "var c = '\\n'\n", "<literal>");
    ASSERT_NOT_NULL(prog);
    char *formatted = format_only(prog);
    ASSERT_NOT_NULL(formatted);
    ASSERT(strstr(formatted, "'\\n'") != NULL);
    xr_compile_resources_free(formatted);
    xr_program_destroy(prog);
    teardown();
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("xfmt_literal - string round-trip");
RUN_TEST(xfmt_string_simple_ascii);
RUN_TEST(xfmt_string_embedded_quote);
RUN_TEST(xfmt_string_backslash_and_newline);
RUN_TEST(xfmt_string_control_byte_hex_escape);

RUN_TEST_SUITE("xfmt_literal - char round-trip");
RUN_TEST(xfmt_rune_literal_roundtrip);
RUN_TEST(xfmt_rune_literal_named_escape);

RUN_TEST_SUITE("xfmt_literal - template no backticks");
RUN_TEST(xfmt_template_no_backticks);
RUN_TEST(xfmt_template_expr_string_uses_double_quotes);
RUN_TEST(xfmt_template_dollar_escaped);

TEST_MAIN_END()
