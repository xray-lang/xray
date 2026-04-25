/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lexer.c - Unit tests for lexical analyzer
 */

#include "../test_framework.h"
#include "xlex.h"

/* ========== Helper Functions ========== */

static Token scan_single(const char *source) {
    Scanner scanner;
    xr_scanner_init(&scanner, source);
    return xr_scanner_scan(&scanner);
}

static void assert_token(Token t, TokenType expected_type, const char *expected_text) {
    ASSERT_EQ_INT(t.type, expected_type);
    if (expected_text) {
        ASSERT_EQ_INT(t.length, (int)strlen(expected_text));
        ASSERT_TRUE(strncmp(t.start, expected_text, t.length) == 0);
    }
}

/* ========== Single Character Token Tests ========== */

TEST(lexer_single_chars) {
    assert_token(scan_single("("), TK_LPAREN, "(");
    assert_token(scan_single(")"), TK_RPAREN, ")");
    assert_token(scan_single("{"), TK_LBRACE, "{");
    assert_token(scan_single("}"), TK_RBRACE, "}");
    assert_token(scan_single("["), TK_LBRACKET, "[");
    assert_token(scan_single("]"), TK_RBRACKET, "]");
    assert_token(scan_single(","), TK_COMMA, ",");
    assert_token(scan_single(":"), TK_COLON, ":");
    assert_token(scan_single(";"), TK_SEMICOLON, ";");
    assert_token(scan_single("+"), TK_PLUS, "+");
    assert_token(scan_single("-"), TK_MINUS, "-");
    assert_token(scan_single("*"), TK_STAR, "*");
    assert_token(scan_single("/"), TK_SLASH, "/");
    assert_token(scan_single("%"), TK_PERCENT, "%");
}

/* ========== Comparison Operator Tests ========== */

TEST(lexer_comparison_ops) {
    assert_token(scan_single("=="), TK_EQ, "==");
    assert_token(scan_single("!="), TK_NE, "!=");
    assert_token(scan_single("==="), TK_EQ_STRICT, "===");
    assert_token(scan_single("!=="), TK_NE_STRICT, "!==");
    assert_token(scan_single("<"), TK_LT, "<");
    assert_token(scan_single("<="), TK_LE, "<=");
    assert_token(scan_single(">"), TK_GT, ">");
    assert_token(scan_single(">="), TK_GE, ">=");
}

/* ========== Assignment Operator Tests ========== */

TEST(lexer_assignment_ops) {
    assert_token(scan_single("="), TK_ASSIGN, "=");
    assert_token(scan_single("+="), TK_PLUS_ASSIGN, "+=");
    assert_token(scan_single("-="), TK_MINUS_ASSIGN, "-=");
    assert_token(scan_single("*="), TK_MUL_ASSIGN, "*=");
    assert_token(scan_single("/="), TK_DIV_ASSIGN, "/=");
    assert_token(scan_single("%="), TK_MOD_ASSIGN, "%=");
}

/* ========== Logical Operator Tests ========== */

TEST(lexer_logical_ops) {
    assert_token(scan_single("&&"), TK_AND, "&&");
    assert_token(scan_single("||"), TK_OR, "||");
    assert_token(scan_single("!"), TK_NOT, "!");
}

/* ========== Bitwise Operator Tests ========== */

TEST(lexer_bitwise_ops) {
    assert_token(scan_single("&"), TK_AMP, "&");
    assert_token(scan_single("|"), TK_PIPE, "|");  // TK_PIPE used for both bitwise or and union type
    assert_token(scan_single("^"), TK_CARET, "^");
    assert_token(scan_single("~"), TK_TILDE, "~");
    assert_token(scan_single("<<"), TK_LSHIFT, "<<");
    assert_token(scan_single(">>"), TK_RSHIFT, ">>");
}

/* ========== Increment/Decrement Tests ========== */

TEST(lexer_inc_dec) {
    assert_token(scan_single("++"), TK_INC, "++");
    assert_token(scan_single("--"), TK_DEC, "--");
}

/* ========== Keyword Tests ========== */

TEST(lexer_keywords) {
    assert_token(scan_single("let"), TK_LET, "let");
    assert_token(scan_single("const"), TK_CONST, "const");
    assert_token(scan_single("fn"), TK_FN, "fn");
    assert_token(scan_single("return"), TK_RETURN, "return");
    assert_token(scan_single("if"), TK_IF, "if");
    assert_token(scan_single("else"), TK_ELSE, "else");
    assert_token(scan_single("while"), TK_WHILE, "while");
    assert_token(scan_single("for"), TK_FOR, "for");
    assert_token(scan_single("in"), TK_IN, "in");
    assert_token(scan_single("break"), TK_BREAK, "break");
    assert_token(scan_single("continue"), TK_CONTINUE, "continue");
    assert_token(scan_single("true"), TK_TRUE, "true");
    assert_token(scan_single("false"), TK_FALSE, "false");
    assert_token(scan_single("null"), TK_NULL, "null");
}

TEST(lexer_class_keywords) {
    assert_token(scan_single("class"), TK_CLASS, "class");
    assert_token(scan_single("extends"), TK_EXTENDS, "extends");
    assert_token(scan_single("constructor"), TK_CONSTRUCTOR, "constructor");
    assert_token(scan_single("this"), TK_THIS, "this");
    assert_token(scan_single("new"), TK_NEW, "new");
    assert_token(scan_single("static"), TK_STATIC, "static");
}

TEST(lexer_coro_keywords) {
    assert_token(scan_single("go"), TK_GO, "go");
    assert_token(scan_single("await"), TK_AWAIT, "await");
    assert_token(scan_single("select"), TK_SELECT, "select");
    assert_token(scan_single("defer"), TK_DEFER, "defer");
}

TEST(lexer_other_keywords) {
    assert_token(scan_single("try"), TK_TRY, "try");
    assert_token(scan_single("catch"), TK_CATCH, "catch");
    assert_token(scan_single("finally"), TK_FINALLY, "finally");
    assert_token(scan_single("throw"), TK_THROW, "throw");
    assert_token(scan_single("import"), TK_IMPORT, "import");
    assert_token(scan_single("export"), TK_EXPORT, "export");
    assert_token(scan_single("match"), TK_MATCH, "match");
    assert_token(scan_single("enum"), TK_ENUM, "enum");
}

/* ========== Integer Literal Tests ========== */

TEST(lexer_int_literals) {
    Token t;

    t = scan_single("0");
    assert_token(t, TK_LITERAL_INT, "0");

    t = scan_single("123");
    assert_token(t, TK_LITERAL_INT, "123");

    t = scan_single("999999");
    assert_token(t, TK_LITERAL_INT, "999999");
}

TEST(lexer_hex_literals) {
    Token t;

    t = scan_single("0x0");
    assert_token(t, TK_LITERAL_INT, "0x0");

    t = scan_single("0xFF");
    assert_token(t, TK_LITERAL_INT, "0xFF");

    t = scan_single("0xABCDEF");
    assert_token(t, TK_LITERAL_INT, "0xABCDEF");
}

/* ========== Float Literal Tests ========== */

TEST(lexer_float_literals) {
    Token t;

    t = scan_single("3.14");
    assert_token(t, TK_LITERAL_FLOAT, "3.14");

    t = scan_single("0.5");
    assert_token(t, TK_LITERAL_FLOAT, "0.5");

    t = scan_single("1e10");
    assert_token(t, TK_LITERAL_FLOAT, "1e10");

    t = scan_single("1.5e-3");
    assert_token(t, TK_LITERAL_FLOAT, "1.5e-3");
}

/* ========== String Literal Tests ========== */

TEST(lexer_string_literals) {
    Token t;

    t = scan_single("\"hello\"");
    assert_token(t, TK_LITERAL_STRING, "\"hello\"");

    t = scan_single("'world'");
    assert_token(t, TK_LITERAL_STRING, "'world'");

    t = scan_single("\"\"");
    assert_token(t, TK_LITERAL_STRING, "\"\"");
}

TEST(lexer_string_escapes) {
    Token t;

    t = scan_single("\"hello\\nworld\"");
    assert_token(t, TK_LITERAL_STRING, "\"hello\\nworld\"");

    t = scan_single("\"tab\\there\"");
    assert_token(t, TK_LITERAL_STRING, "\"tab\\there\"");

    t = scan_single("\"quote\\\"inside\"");
    assert_token(t, TK_LITERAL_STRING, "\"quote\\\"inside\"");
}

/* ========== Identifier Tests ========== */

TEST(lexer_identifiers) {
    Token t;

    t = scan_single("foo");
    assert_token(t, TK_NAME, "foo");

    t = scan_single("_bar");
    assert_token(t, TK_NAME, "_bar");

    t = scan_single("camelCase");
    assert_token(t, TK_NAME, "camelCase");

    t = scan_single("with_underscore");
    assert_token(t, TK_NAME, "with_underscore");

    t = scan_single("var123");
    assert_token(t, TK_NAME, "var123");
}

/* ========== Special Token Tests ========== */

TEST(lexer_special_tokens) {
    assert_token(scan_single("=>"), TK_ARROW, "=>");
    assert_token(scan_single("?."), TK_QUESTION_DOT, "?.");
    assert_token(scan_single("??"), TK_NULLISH_COALESCE, "??");
    assert_token(scan_single(".."), TK_RANGE, "..");  // TK_RANGE is used for ".."
    assert_token(scan_single("..."), TK_DOT_DOT_DOT, "...");
}

/* ========== Whitespace and Comment Tests ========== */

TEST(lexer_skip_whitespace) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "   123   ");
    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_LITERAL_INT, "123");
}

TEST(lexer_skip_line_comment) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "// comment\n42");
    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_LITERAL_INT, "42");
}

TEST(lexer_skip_block_comment) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "/* block */ 99");
    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_LITERAL_INT, "99");
}

/* ========== Token Sequence Tests ========== */

TEST(lexer_token_sequence) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "let x = 42");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_LET, "let");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_NAME, "x");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_ASSIGN, "=");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_LITERAL_INT, "42");

    t = xr_scanner_scan(&scanner);
    ASSERT_EQ_INT(t.type, TK_EOF);
}

TEST(lexer_function_def) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "fn add(a, b) { return a + b }");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_FN, "fn");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_NAME, "add");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_LPAREN, "(");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_NAME, "a");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_COMMA, ",");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_NAME, "b");

    t = xr_scanner_scan(&scanner);
    assert_token(t, TK_RPAREN, ")");
}

/* ========== Line Number Tests ========== */

TEST(lexer_line_numbers) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "a\nb\nc");

    t = xr_scanner_scan(&scanner);
    ASSERT_EQ_INT(t.line, 1);

    t = xr_scanner_scan(&scanner);
    ASSERT_EQ_INT(t.line, 2);

    t = xr_scanner_scan(&scanner);
    ASSERT_EQ_INT(t.line, 3);
}

/* ========== EOF Tests ========== */

TEST(lexer_empty_source) {
    Token t = scan_single("");
    ASSERT_EQ_INT(t.type, TK_EOF);
}

TEST(lexer_whitespace_only) {
    Token t = scan_single("   \t\n   ");
    ASSERT_EQ_INT(t.type, TK_EOF);
}

/* ========== Multi-line Token Position (L-02) ========== */

// A multi-line string token must report the START line/column, not the
// position where its closing quote happens to land.
TEST(lexer_multiline_string_start_position) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "\"line1\nline2\nline3\"\nx");
    t = xr_scanner_scan(&scanner);  // string token starts on line 1
    ASSERT_EQ_INT(t.type, TK_LITERAL_STRING);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 1);

    t = xr_scanner_scan(&scanner);  // x on line 4
    ASSERT_EQ_INT(t.type, TK_NAME);
    ASSERT_EQ_INT(t.line, 4);
    ASSERT_EQ_INT(t.column, 1);
}

TEST(lexer_multiline_raw_string_start_position) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "  r\"abc\ndef\"\nx");
    t = xr_scanner_scan(&scanner);  // raw string starts on line 1, col 3
    ASSERT_EQ_INT(t.type, TK_RAW_STRING);
    ASSERT_EQ_INT(t.line, 1);
    ASSERT_EQ_INT(t.column, 3);

    t = xr_scanner_scan(&scanner);  // x on line 3
    ASSERT_EQ_INT(t.type, TK_NAME);
    ASSERT_EQ_INT(t.line, 3);
    ASSERT_EQ_INT(t.column, 1);
}

// After consuming a multi-line block comment, the next token's line/column
// must reflect its actual position, not the start-of-comment line.
TEST(lexer_multiline_block_comment_position) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "/* a\nb\nc */\nx");
    t = xr_scanner_scan(&scanner);  // x on line 4, col 1
    ASSERT_EQ_INT(t.type, TK_NAME);
    ASSERT_EQ_INT(t.line, 4);
    ASSERT_EQ_INT(t.column, 1);
}

/* ========== Error Token Contract (L-03) ========== */

// TK_ERROR carries the diagnostic in error_message; start/length point
// into the source buffer at the offending characters.
TEST(lexer_error_token_message_field) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "\"unterminated");
    t = xr_scanner_scan(&scanner);
    ASSERT_EQ_INT(t.type, TK_ERROR);
    ASSERT_TRUE(t.error_message != NULL);
    ASSERT_TRUE(strstr(t.error_message, "Unterminated") != NULL ||
                strstr(t.error_message, "unterminated") != NULL);
}

TEST(lexer_error_token_unterminated_block_comment) {
    Scanner scanner;
    Token t;

    xr_scanner_init(&scanner, "/* never closes");
    t = xr_scanner_scan(&scanner);
    ASSERT_EQ_INT(t.type, TK_ERROR);
    ASSERT_TRUE(t.error_message != NULL);
    ASSERT_TRUE(strstr(t.error_message, "unterminated") != NULL);
}

// Successful tokens MUST have error_message == NULL so callers can branch
// on the field without inspecting the type tag.
TEST(lexer_normal_token_no_error_message) {
    Token t = scan_single("foo");
    ASSERT_EQ_INT(t.type, TK_NAME);
    ASSERT_TRUE(t.error_message == NULL);

    t = scan_single("123");
    ASSERT_EQ_INT(t.type, TK_LITERAL_INT);
    ASSERT_TRUE(t.error_message == NULL);

    t = scan_single("\"hello\"");
    ASSERT_EQ_INT(t.type, TK_LITERAL_STRING);
    ASSERT_TRUE(t.error_message == NULL);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Single Character Tokens");
    RUN_TEST(lexer_single_chars);

    RUN_TEST_SUITE("Comparison Operators");
    RUN_TEST(lexer_comparison_ops);

    RUN_TEST_SUITE("Assignment Operators");
    RUN_TEST(lexer_assignment_ops);

    RUN_TEST_SUITE("Logical Operators");
    RUN_TEST(lexer_logical_ops);

    RUN_TEST_SUITE("Bitwise Operators");
    RUN_TEST(lexer_bitwise_ops);

    RUN_TEST_SUITE("Increment/Decrement");
    RUN_TEST(lexer_inc_dec);

    RUN_TEST_SUITE("Keywords");
    RUN_TEST(lexer_keywords);
    RUN_TEST(lexer_class_keywords);
    RUN_TEST(lexer_coro_keywords);
    RUN_TEST(lexer_other_keywords);

    RUN_TEST_SUITE("Integer Literals");
    RUN_TEST(lexer_int_literals);
    RUN_TEST(lexer_hex_literals);

    RUN_TEST_SUITE("Float Literals");
    RUN_TEST(lexer_float_literals);

    RUN_TEST_SUITE("String Literals");
    RUN_TEST(lexer_string_literals);
    RUN_TEST(lexer_string_escapes);

    RUN_TEST_SUITE("Identifiers");
    RUN_TEST(lexer_identifiers);

    RUN_TEST_SUITE("Special Tokens");
    RUN_TEST(lexer_special_tokens);

    RUN_TEST_SUITE("Whitespace and Comments");
    RUN_TEST(lexer_skip_whitespace);
    RUN_TEST(lexer_skip_line_comment);
    RUN_TEST(lexer_skip_block_comment);

    RUN_TEST_SUITE("Token Sequences");
    RUN_TEST(lexer_token_sequence);
    RUN_TEST(lexer_function_def);

    RUN_TEST_SUITE("Line Numbers");
    RUN_TEST(lexer_line_numbers);

    RUN_TEST_SUITE("EOF Handling");
    RUN_TEST(lexer_empty_source);
    RUN_TEST(lexer_whitespace_only);

    RUN_TEST_SUITE("Multi-line Token Position (L-02)");
    RUN_TEST(lexer_multiline_string_start_position);
    RUN_TEST(lexer_multiline_raw_string_start_position);
    RUN_TEST(lexer_multiline_block_comment_position);

    RUN_TEST_SUITE("Error Token Contract (L-03)");
    RUN_TEST(lexer_error_token_message_field);
    RUN_TEST(lexer_error_token_unterminated_block_comment);
    RUN_TEST(lexer_normal_token_no_error_message);
}

TEST_MAIN_BEGIN()
    printf("=== xray Lexer Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
