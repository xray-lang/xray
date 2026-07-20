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
 *     2. A block string reports the line/column where the opening
 *        quote run was, NOT where the closing quote run ended.
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
        if (t.type == TK_EOF && i < n)
            return t;
    }
    return t;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(single_line_token_position) {
    // Token positions are 1-indexed.
    Scanner s;
    xr_scanner_init(&s, "var x");
    Token a = xr_scanner_scan(&s);
    Token b = xr_scanner_scan(&s);

    ASSERT_EQ_INT(a.type, TK_VAR);
    ASSERT_EQ_INT(a.line, 1);
    ASSERT_EQ_INT(a.column, 1);

    ASSERT_EQ_INT(b.type, TK_NAME);
    ASSERT_EQ_INT(b.line, 1);
    // "var " is 4 chars, x starts at column 5.
    ASSERT_EQ_INT(b.column, 5);
}

TEST(token_on_second_line) {
    Scanner s;
    xr_scanner_init(&s, "var\nx");
    (void) xr_scanner_scan(&s);  // consume `var`
    Token b = xr_scanner_scan(&s);

    ASSERT_EQ_INT(b.type, TK_NAME);
    ASSERT_EQ_INT(b.line, 2);
    ASSERT_EQ_INT(b.column, 1);
}

TEST(multiline_string_reports_start_position) {
    // The block starts on line 1 column 1; the closer is on line 5.
    const char *src = "\"\"\"\nline1\nline2\nline3\n\"\"\"";
    Scanner s;
    xr_scanner_init(&s, src);
    Token t = xr_scanner_scan(&s);

    ASSERT_EQ_INT(t.type, TK_LITERAL_STRING);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 1);
}

TEST(token_after_multiline_string_uses_correct_line) {
    // A block closer is isolated; the next token starts on line 6.
    const char *src = "\"\"\"\na\nb\nc\n\"\"\"\n+ 1";
    Scanner s;
    xr_scanner_init(&s, src);
    Token str = xr_scanner_scan(&s);
    Token plus = xr_scanner_scan(&s);
    Token one = xr_scanner_scan(&s);

    ASSERT_EQ_INT(str.type, TK_LITERAL_STRING);
    ASSERT_EQ_INT(str.line, 1);

    ASSERT_EQ_INT(plus.type, TK_PLUS);
    ASSERT_EQ_INT(plus.line, 6);
    ASSERT_EQ_INT(plus.column, 1);

    ASSERT_EQ_INT(one.type, TK_LITERAL_INT);
    ASSERT_EQ_INT(one.line, 6);
    ASSERT_EQ_INT(one.column, 3);
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
    const char *src = "\"\"\"\na${1}\nb\n\"\"\"";
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
    xr_scanner_init(&s, "\t\tvar");
    Token t = xr_scanner_scan(&s);

    ASSERT_EQ_INT(t.type, TK_VAR);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 3);  // 2 tabs (cols 1, 2), `v` at col 3
}

TEST(carriage_return_does_not_double_count_lines) {
    // CRLF line endings must produce the same line numbers as LF.
    Scanner s;
    xr_scanner_init(&s, "var\r\nx");
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
