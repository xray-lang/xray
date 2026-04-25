/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lexer_positions.c - L-02 acceptance tests
 *
 * KEY CONCEPT:
 *   Pre-Phase-1 the lexer reported a token's `line` / `column` using
 *   `scanner->line` / `scanner->line_start` AFTER the scan finished.
 *   For multi-line tokens (multi-line strings, raw strings, template
 *   strings, block comments that consumed newlines) those fields had
 *   already advanced past the token, so the reported position
 *   pointed at the END of the token, not its START.
 *
 *   Phase 1 (L-02) introduced `start_line` / `start_line_start`
 *   snapshots captured by xr_scanner_scan() before consuming the
 *   token's first character. These tests pin down the contract:
 *
 *     1. A single-line token reports its 1-indexed start line and
 *        column.
 *     2. A multi-line string reports the line/column where the
 *        opening `"` was, NOT where the closing `"` ended.
 *     3. The token AFTER a multi-line string reports a position on
 *        the line it actually starts on (i.e. lexer's line_start was
 *        synchronised correctly inside the string body).
 *     4. The token AFTER a multi-line block comment reports a
 *        sensible line / column on the line it actually begins.
 *     5. Tokens preceded by tabs / spaces still report 1-indexed
 *        columns (the lexer does not silently treat tabs as a
 *        single column or expand them).
 */

#include "../test_framework.h"
#include "frontend/lexer/xlex.h"

/* ====================================================================== */
/* Helpers                                                                 */
/* ====================================================================== */

static Token scan_nth(const char *source, int n) {
    Scanner s;
    xr_scanner_init(&s, source);
    Token t;
    for (int i = 0; i <= n; i++) {
        t = xr_scanner_scan(&s);
        if (t.type == TK_EOF && i < n) return t;
    }
    return t;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(single_line_token_position) {
    // Token positions are 1-indexed.
    Scanner s;
    xr_scanner_init(&s, "let x");
    Token a = xr_scanner_scan(&s);
    Token b = xr_scanner_scan(&s);

    ASSERT_EQ_INT(a.type, TK_LET);
    ASSERT_EQ_INT(a.line, 1);
    ASSERT_EQ_INT(a.column, 1);

    ASSERT_EQ_INT(b.type, TK_NAME);
    ASSERT_EQ_INT(b.line, 1);
    // "let " is 4 chars, x starts at column 5.
    ASSERT_EQ_INT(b.column, 5);
}

TEST(token_on_second_line) {
    Scanner s;
    xr_scanner_init(&s, "let\nx");
    (void)xr_scanner_scan(&s);  // consume `let`
    Token b = xr_scanner_scan(&s);

    ASSERT_EQ_INT(b.type, TK_NAME);
    ASSERT_EQ_INT(b.line, 2);
    ASSERT_EQ_INT(b.column, 1);
}

TEST(multiline_string_reports_start_position) {
    // The string starts on line 1 column 1; the closing quote is
    // on line 3. Pre-L-02 this used to report line 3.
    const char *src = "\"line1\nline2\nline3\"";
    Scanner s;
    xr_scanner_init(&s, src);
    Token t = xr_scanner_scan(&s);

    ASSERT_EQ_INT(t.type, TK_LITERAL_STRING);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 1);
}

TEST(token_after_multiline_string_uses_correct_line) {
    // After a 3-line string, the next token must report line 3
    // (the closing quote's line) and the column relative to it.
    const char *src = "\"a\nb\nc\" + 1";
    Scanner s;
    xr_scanner_init(&s, src);
    Token str = xr_scanner_scan(&s);
    Token plus = xr_scanner_scan(&s);
    Token one = xr_scanner_scan(&s);

    ASSERT_EQ_INT(str.type, TK_LITERAL_STRING);
    ASSERT_EQ_INT(str.line, 1);

    // Line 3 is `c" + 1`: c@1, "@2, space@3, +@4, space@5, 1@6.
    ASSERT_EQ_INT(plus.type, TK_PLUS);
    ASSERT_EQ_INT(plus.line, 3);
    ASSERT_EQ_INT(plus.column, 4);

    ASSERT_EQ_INT(one.type, TK_LITERAL_INT);
    ASSERT_EQ_INT(one.line, 3);
    ASSERT_EQ_INT(one.column, 6);
}

TEST(token_after_multiline_block_comment) {
    // Multi-line block comment must NOT corrupt the lexer's
    // line_start tracking. The token AFTER the comment must report
    // the right line and column for the line it actually starts on.
    const char *src = "/* spans\n   two */ x";
    Scanner s;
    xr_scanner_init(&s, src);
    Token t = xr_scanner_scan(&s);

    ASSERT_EQ_INT(t.type, TK_NAME);
    ASSERT_EQ_INT(t.line, 2);  // comment closes on line 2
    // "   two */ " = 3 spaces + "two" + " */" + " " ; x is at col 11
    ASSERT_EQ_INT(t.column, 11);
}

TEST(template_string_reports_start_position) {
    // Template strings are also multi-token internally; the START
    // (the leading `"`) determines the reported position.
    const char *src = "\"a${1}\nb\"";
    Token t = scan_nth(src, 0);

    // First token of a template is the literal-string segment;
    // the lexer reports its start at line 1, column 1.
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 1);
}

TEST(tab_indented_token_column_is_byte_offset) {
    // The lexer reports columns as 1-indexed BYTE offsets. Tabs
    // count as 1 column (no expansion). This is the contract LSP
    // assumes elsewhere; test it explicitly so a "smart" tab
    // expansion regression would surface.
    Scanner s;
    xr_scanner_init(&s, "\t\tlet");
    Token t = xr_scanner_scan(&s);

    ASSERT_EQ_INT(t.type, TK_LET);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 3);  // 2 tabs (cols 1, 2), `l` at col 3
}

TEST(carriage_return_does_not_double_count_lines) {
    // CRLF line endings must produce the same line numbers as LF.
    Scanner s;
    xr_scanner_init(&s, "let\r\nx");
    Token a = xr_scanner_scan(&s);
    Token b = xr_scanner_scan(&s);

    ASSERT_EQ_INT(a.line, 1);
    ASSERT_EQ_INT(b.type, TK_NAME);
    ASSERT_EQ_INT(b.line, 2);
    ASSERT_EQ_INT(b.column, 1);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("L-02 multi-line token positions");
    RUN_TEST(single_line_token_position);
    RUN_TEST(token_on_second_line);
    RUN_TEST(multiline_string_reports_start_position);
    RUN_TEST(token_after_multiline_string_uses_correct_line);
    RUN_TEST(token_after_multiline_block_comment);
    RUN_TEST(template_string_reports_start_position);
    RUN_TEST(tab_indented_token_column_is_byte_offset);
    RUN_TEST(carriage_return_does_not_double_count_lines);
TEST_MAIN_END()
