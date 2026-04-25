/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lexer_trivia.c - L-06 acceptance tests
 *
 * KEY CONCEPT:
 *   The lexer's trivia model splits comments into two flavours per
 *   token:
 *
 *     - leading_trivia : comments that appeared on lines BEFORE the
 *                        token (or at file start). Always available
 *                        when collect_trivia is on.
 *     - trailing_trivia: at most one inline `//` line comment or
 *                        a single-line block comment (delimited by
 *                        slash-star ... star-slash) that begins on
 *                        the SAME source line as the token, before any
 *                        newline. Anything after a newline -- even a
 *                        single line comment on the next line --
 *                        belongs to the FOLLOWING token's
 *                        leading_trivia, never to the preceding
 *                        token's trailing.
 *
 *   This file pins down that contract so a future regression in
 *   xlex.c::scan_inline_trailing_trivia / make_token can be caught
 *   without running the full parser/formatter pipeline.
 */

#include "../test_framework.h"
#include "frontend/lexer/xlex.h"

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

// Scan `source` with trivia collection enabled and return the FIRST
// non-EOF token. Caller is responsible for freeing the trivia chains.
static Token first_token_with_trivia(Scanner *s, const char *source) {
    xr_scanner_init_with_trivia(s, source, true);
    return xr_scanner_scan(s);
}

// Scan exactly two non-EOF tokens; mirrors `first_token` but exposes
// both for cases that test "comment after token A, before token B".
static void scan_pair(const char *source, Token *a, Token *b) {
    Scanner s;
    xr_scanner_init_with_trivia(&s, source, true);
    *a = xr_scanner_scan(&s);
    *b = xr_scanner_scan(&s);
}

// Trivia chain accessors with safe defaults.
static int trivia_count(XrTrivia *t) {
    int n = 0;
    while (t) { n++; t = t->next; }
    return n;
}

static bool trivia_text_eq(XrTrivia *t, const char *expected) {
    if (!t) return expected == NULL;
    int elen = (int)strlen(expected);
    if (t->length != elen) return false;
    return memcmp(t->start, expected, (size_t)elen) == 0;
}

static void free_token_trivia(Token *t) {
    if (t->leading_trivia)  xr_trivia_free_chain(t->leading_trivia);
    if (t->trailing_trivia) xr_trivia_free_chain(t->trailing_trivia);
    t->leading_trivia = NULL;
    t->trailing_trivia = NULL;
}

/* ====================================================================== */
/* Trailing-attach tests                                                   */
/* ====================================================================== */

TEST(trailing_line_comment_same_line) {
    // `;` and `// trailing` are on the same source line -> attach.
    Token a, b;
    scan_pair("let;// trailing\n", &a, &b);

    // First scan returns TK_LET (no leading, no trailing -- comment
    // is on the SAME line but BEHIND the next token).
    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_NULL(a.leading_trivia);
    ASSERT_NULL(a.trailing_trivia);

    // Second scan returns ';', which DOES have the trailing comment.
    ASSERT_EQ_INT(b.type, TK_SEMICOLON);
    ASSERT_NULL(b.leading_trivia);
    ASSERT_NOT_NULL(b.trailing_trivia);
    ASSERT_EQ_INT(b.trailing_trivia->type, TRIVIA_LINE_COMMENT);
    ASSERT_TRUE(trivia_text_eq(b.trailing_trivia, " trailing"));
    ASSERT_EQ_INT(trivia_count(b.trailing_trivia), 1);

    free_token_trivia(&a);
    free_token_trivia(&b);
}

TEST(trailing_line_comment_after_horizontal_whitespace) {
    // Whitespace / tabs between the token and `//` are skipped
    // transparently and do not disqualify the trailing attachment.
    Scanner s;
    Token a = first_token_with_trivia(&s, "let  \t//note\n");

    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_NOT_NULL(a.trailing_trivia);
    ASSERT_EQ_INT(a.trailing_trivia->type, TRIVIA_LINE_COMMENT);
    ASSERT_TRUE(trivia_text_eq(a.trailing_trivia, "note"));

    free_token_trivia(&a);
}

TEST(comment_after_newline_is_leading_of_next) {
    // Comment lives on line 2, separated from the previous token by a
    // newline. It must NOT attach as trailing to `let`; it must attach
    // as leading to `;`.
    Token a, b;
    scan_pair("let\n// next-line\n;\n", &a, &b);

    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_NULL(a.leading_trivia);
    ASSERT_NULL(a.trailing_trivia);

    ASSERT_EQ_INT(b.type, TK_SEMICOLON);
    ASSERT_NOT_NULL(b.leading_trivia);
    ASSERT_NULL(b.trailing_trivia);
    ASSERT_TRUE(trivia_text_eq(b.leading_trivia, " next-line"));

    free_token_trivia(&a);
    free_token_trivia(&b);
}

TEST(trailing_inline_block_comment_same_line) {
    // /* ... */ that BEGINS and ENDS on the same line -> trailing.
    Token a, b;
    scan_pair("let /* hi */;\n", &a, &b);

    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_NOT_NULL(a.trailing_trivia);
    ASSERT_EQ_INT(a.trailing_trivia->type, TRIVIA_BLOCK_COMMENT);
    ASSERT_TRUE(trivia_text_eq(a.trailing_trivia, " hi "));

    ASSERT_EQ_INT(b.type, TK_SEMICOLON);
    ASSERT_NULL(b.leading_trivia);

    free_token_trivia(&a);
    free_token_trivia(&b);
}

TEST(multiline_block_comment_is_leading_of_next) {
    // /* ... */ that spans newlines must NOT attach as trailing to
    // the preceding token. It belongs to the following token's
    // leading trivia, exactly like a line comment after `\n`.
    Token a, b;
    scan_pair("let /* first\n   second */ ;\n", &a, &b);

    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_NULL(a.trailing_trivia);

    ASSERT_EQ_INT(b.type, TK_SEMICOLON);
    ASSERT_NOT_NULL(b.leading_trivia);
    ASSERT_EQ_INT(b.leading_trivia->type, TRIVIA_BLOCK_COMMENT);

    free_token_trivia(&a);
    free_token_trivia(&b);
}

TEST(eof_token_never_has_trailing) {
    // Even when the source ends with an inline comment, the EOF
    // token's trailing must be NULL (nothing follows EOF). The comment
    // is captured as leading on EOF (existing pre-L-06 behaviour) so
    // it is not lost.
    Scanner s;
    xr_scanner_init_with_trivia(&s, "// only-comment", true);
    Token eof = xr_scanner_scan(&s);

    ASSERT_EQ_INT(eof.type, TK_EOF);
    ASSERT_NULL(eof.trailing_trivia);
    ASSERT_NOT_NULL(eof.leading_trivia);

    free_token_trivia(&eof);
}

TEST(no_trailing_means_no_state_leak) {
    // When trailing scan rewinds because no inline comment was found,
    // the next token's `has_leading_space` must still reflect that
    // there WAS whitespace before it. This is the parser's smart-
    // semicolon / generic disambiguation contract.
    Token a, b;
    // `5  6` -- two integer literals separated by spaces only; the
    // trailing scan after `5` must restore current so that the next
    // scan sees the gap.
    scan_pair("5  6", &a, &b);

    ASSERT_EQ_INT(a.type, TK_LITERAL_INT);
    ASSERT_NULL(a.trailing_trivia);

    ASSERT_EQ_INT(b.type, TK_LITERAL_INT);
    ASSERT_TRUE(b.has_leading_space);

    free_token_trivia(&a);
    free_token_trivia(&b);
}

TEST(collect_trivia_off_disables_trailing) {
    // When the formatter is not in the loop, the parser uses the
    // non-trivia init: trailing must remain NULL even on inline
    // comments, matching pre-L-06 behaviour and keeping non-formatter
    // call sites zero-cost.
    Scanner s;
    xr_scanner_init(&s, "let // c\n;");  // collect_trivia = false
    Token a = xr_scanner_scan(&s);

    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_NULL(a.leading_trivia);
    ASSERT_NULL(a.trailing_trivia);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("L-06 trailing trivia attachment");
    RUN_TEST(trailing_line_comment_same_line);
    RUN_TEST(trailing_line_comment_after_horizontal_whitespace);
    RUN_TEST(comment_after_newline_is_leading_of_next);
    RUN_TEST(trailing_inline_block_comment_same_line);
    RUN_TEST(multiline_block_comment_is_leading_of_next);
    RUN_TEST(eof_token_never_has_trailing);
    RUN_TEST(no_trailing_means_no_state_leak);
    RUN_TEST(collect_trivia_off_disables_trailing);
TEST_MAIN_END()
