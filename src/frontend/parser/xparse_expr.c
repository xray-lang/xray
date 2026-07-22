/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_expr.c - Prefix and infix expression parsing
 *
 * KEY CONCEPT:
 *   Pratt parser prefix/infix handlers: literals, unary/binary ops,
 *   template strings, type casts, generics, optional chains, etc.
 */

#include "xparse_internal.h"
#include "xtype_ref.h"
#include "xtype_scope.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../base/xutf8.h"
#include "../../runtime/xisolate_api.h"
#include "../xdiag_fmt.h"
#include "../lexer/xquoted_literal.h"

#include <stdint.h>

/* ========== Helpers ========== */

// Strip underscore separators from numeric literal into dst buffer.
// Returns number of characters written (not counting NUL).
static int strip_underscores(const char *src, int src_len, char *dst, int dst_size) {
    int n = 0;
    for (int i = 0; i < src_len && n < dst_size - 1; i++) {
        if (src[i] != '_')
            dst[n++] = src[i];
    }
    dst[n] = '\0';
    return n;
}

/* ========== Prefix Parsing ========== */

typedef struct ParsedIntLiteral {
    uint64_t bits;
    bool overflows_i64;
    bool overflows_u64;
} ParsedIntLiteral;

static int numeric_digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Parse integer literal (supports multiple bases and underscore separators)
// Formats: decimal (123), hex (0xFF), binary (0b1010), octal (0o755)
static ParsedIntLiteral parse_integer_literal(const char *start, int length) {
    ParsedIntLiteral out = {0};
    int base = 10;
    int pos = 0;

    if (length >= 2 && start[0] == '0') {
        char prefix = start[1];
        if (prefix == 'x' || prefix == 'X') {
            base = 16;
            pos = 2;
        } else if (prefix == 'b' || prefix == 'B') {
            base = 2;
            pos = 2;
        } else if (prefix == 'o' || prefix == 'O') {
            base = 8;
            pos = 2;
        }
    }

    for (int i = pos; i < length; i++) {
        if (start[i] == '_')
            continue;
        int digit = numeric_digit_value(start[i]);
        if (digit < 0 || digit >= base)
            continue;
        if (out.bits > (UINT64_MAX - (uint64_t) digit) / (uint64_t) base) {
            out.bits = UINT64_MAX;
            out.overflows_i64 = true;
            out.overflows_u64 = true;
            continue;
        }
        out.bits = out.bits * (uint64_t) base + (uint64_t) digit;
    }

    out.overflows_i64 = out.overflows_u64 || out.bits > (uint64_t) INT64_MAX;
    return out;
}

static int char_hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static const char *parse_rune_literal_payload(const char *src, size_t len, uint32_t *out_cp) {
    if (!src || len == 0)
        return "rune literal cannot be empty";

    if (src[0] == '\\') {
        if (len < 2)
            return "unterminated rune escape";
        uint32_t cp = 0;
        if (src[1] == 'u') {
            if (len < 4 || src[2] != '{')
                return "rune unicode escape must use \\u{...}";
            size_t p = 3;
            uint32_t value = 0;
            int digits = 0;
            while (p < len && src[p] != '}') {
                int h = char_hex_value(src[p]);
                if (h < 0)
                    return "invalid hex digit in rune unicode escape";
                if (digits >= 6)
                    return "rune unicode escape must contain at most 6 hex digits";
                value = (value << 4) | (uint32_t) h;
                digits++;
                p++;
            }
            if (digits == 0)
                return "rune unicode escape requires at least one hex digit";
            if (p >= len || src[p] != '}')
                return "unterminated rune unicode escape";
            if (p + 1 != len)
                return "rune literal must contain exactly one Unicode scalar value";
            cp = value;
        } else {
            switch (src[1]) {
                case 'n':
                    cp = '\n';
                    break;
                case 'r':
                    cp = '\r';
                    break;
                case 't':
                    cp = '\t';
                    break;
                case '\\':
                    cp = '\\';
                    break;
                case '\'':
                    cp = '\'';
                    break;
                case '"':
                    cp = '"';
                    break;
                case 'b':
                    cp = '\b';
                    break;
                case 'f':
                    cp = '\f';
                    break;
                case '0':
                    cp = '\0';
                    break;
                default:
                    return "invalid rune escape";
            }
            if (len != 2)
                return "rune literal must contain exactly one Unicode scalar value";
        }
        if (!xr_unicode_is_scalar(cp))
            return "rune literal must be a valid Unicode scalar value";
        *out_cp = cp;
        return NULL;
    }

    uint32_t cp = 0;
    int consumed = xr_utf8_decode(src, len, &cp);
    if (consumed <= 0)
        return "invalid UTF-8 in rune literal";
    if ((unsigned char) src[0] >= 0x80 && consumed == 1 && cp == XR_UNICODE_INVALID)
        return "invalid UTF-8 in rune literal";
    if (!xr_unicode_is_scalar(cp))
        return "rune literal must be a valid Unicode scalar value";
    if ((size_t) consumed != len)
        return "rune literal must contain exactly one Unicode scalar value";
    *out_cp = cp;
    return NULL;
}

// Parse literal (number, string, bool, null)
AstNode *xr_parse_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_literal: NULL parser");
    int column = parser->previous.column;
    switch (parser->previous.type) {
        case TK_LITERAL_INT: {
            ParsedIntLiteral value =
                parse_integer_literal(parser->previous.start, parser->previous.length);
            if (value.overflows_u64)
                xr_parser_error_at_previous(parser, "integer literal exceeds uint64 range");
            // Full int64 range allowed at parse time; range checks against
            // the target type happen later in the analyzer/compiler.
            AstNode *node = xr_ast_literal_int_bits(parser->compiler_session, value.bits,
                                                    value.overflows_i64, parser->previous.line);
            node->column = column;
            return node;
        }

        case TK_LITERAL_FLOAT: {
            char buf[64];
            strip_underscores(parser->previous.start, parser->previous.length, buf, sizeof(buf));
            xr_Number value = strtod(buf, NULL);
            AstNode *node =
                xr_ast_literal_float(parser->compiler_session, value, parser->previous.line);
            node->column = column;
            return node;
        }

        case TK_LITERAL_BIGINT: {
            // Strip 'n' suffix and underscores
            int length = parser->previous.length - 1;  // Strip 'n' suffix
            char *buf = (char *) xr_malloc(length + 1);
            strip_underscores(parser->previous.start, length, buf, length + 1);
            AstNode *node =
                xr_ast_literal_bigint(parser->compiler_session, buf, parser->previous.line);
            node->column = column;
            xr_free(buf);
            return node;
        }

        case TK_LITERAL_STRING:
        case TK_RAW_STRING: {
            XrQuotedPayload payload = {0};
            const char *error = NULL;
            bool decode_escapes = parser->previous.escape_mode == XR_LITERAL_ESCAPED;
            if (!xr_quoted_payload_decode(&parser->previous, decode_escapes, &payload, &error)) {
                xr_parser_error_at_previous(parser, error ? error : "invalid string literal");
                return NULL;
            }
            if (memchr(payload.bytes, '\0', payload.length) != NULL) {
                xr_quoted_payload_free(&payload);
                xr_parser_error_at_previous(
                    parser, "string literals cannot contain byte escapes; use b\"...\"");
                return NULL;
            }
            if (!xr_utf8_validate((const char *) payload.bytes, payload.length)) {
                xr_quoted_payload_free(&payload);
                xr_parser_error_at_previous(parser, "string literal must be valid UTF-8");
                return NULL;
            }
            AstNode *node = xr_ast_literal_string(
                parser->compiler_session, (const char *) payload.bytes,
                parser->previous.escape_mode, parser->previous.source_form, parser->previous.line);
            node->column = column;
            xr_quoted_payload_free(&payload);
            return node;
        }

        case TK_LITERAL_BYTE_STRING:
        case TK_LITERAL_C_STRING: {
            bool append_nul = parser->previous.type == TK_LITERAL_C_STRING;
            XrQuotedPayload payload = {0};
            const char *error = NULL;
            bool decode_escapes = parser->previous.escape_mode == XR_LITERAL_ESCAPED;
            if (!xr_quoted_payload_decode(&parser->previous, decode_escapes, &payload, &error)) {
                xr_parser_error_at_previous(parser, error ? error : "invalid byte literal");
                return NULL;
            }
            if (append_nul && memchr(payload.bytes, '\0', payload.length) != NULL) {
                xr_quoted_payload_free(&payload);
                xr_parser_error_at_previous(parser, "c literal cannot contain an interior NUL");
                return NULL;
            }
            AstNode *node = xr_ast_fixed_bytes_literal(
                parser->compiler_session, payload.bytes, payload.length, append_nul,
                parser->previous.escape_mode, parser->previous.source_form, parser->previous.line);
            node->column = column;
            xr_quoted_payload_free(&payload);
            return node;
        }

        case TK_LITERAL_RUNE: {
            const char *src = parser->previous.start + 1;
            size_t src_len = (size_t) parser->previous.length - 2;
            uint32_t cp = 0;
            const char *err = parse_rune_literal_payload(src, src_len, &cp);
            if (err)
                xr_parser_error_at_previous(parser, err);
            AstNode *node =
                xr_ast_literal_rune(parser->compiler_session, cp, parser->previous.line);
            node->column = column;
            return node;
        }

        case TK_TRUE: {
            AstNode *node = xr_ast_literal_bool(parser->compiler_session, 1, parser->previous.line);
            node->column = column;
            return node;
        }

        case TK_FALSE: {
            AstNode *node = xr_ast_literal_bool(parser->compiler_session, 0, parser->previous.line);
            node->column = column;
            return node;
        }

        case TK_NULL: {
            AstNode *node = xr_ast_literal_null(parser->compiler_session, parser->previous.line);
            node->column = column;
            return node;
        }

        default:
            xr_parser_error(parser, "unknown literal type");
            return NULL;
    }
}

// Regex prefix parsing (when '/' appears at expression start)
// Backtrack scanner and rescan as regex
AstNode *xr_parse_regex_prefix(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_regex_prefix: NULL parser");
    const char *slash_pos = parser->previous.start;
    parser->scanner.current = slash_pos;

    Token regex_token = xr_scanner_try_regex(&parser->scanner);

    if (regex_token.type == TK_LITERAL_REGEX) {
        parser->previous = regex_token;
        parser->current = xr_scanner_scan(&parser->scanner);
        return xr_parse_regex_literal(parser);
    } else {
        xr_parser_error(parser, "invalid regex literal");
        return NULL;
    }
}

// Parse regex literal: /pattern/flags
AstNode *xr_parse_regex_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_regex_literal: NULL parser");
    const char *start = parser->previous.start;
    int length = parser->previous.length;

    // Skip opening '/'
    start++;
    length--;

    // Find closing '/'
    const char *end_slash = NULL;
    for (int i = length - 1; i >= 0; i--) {
        if (start[i] == '/') {
            end_slash = start + i;
            break;
        }
    }

    if (!end_slash) {
        xr_parser_error(parser, "invalid regex literal format");
        return NULL;
    }

    // Extract pattern
    int pattern_len = (int) (end_slash - start);
    char *pattern = (char *) xr_malloc(pattern_len + 1);
    memcpy(pattern, start, pattern_len);
    pattern[pattern_len] = '\0';

    // Extract flags
    const char *flags_start = end_slash + 1;
    int flags_len = length - pattern_len - 1;
    char *flags = (char *) xr_malloc(flags_len + 1);
    if (flags_len > 0) {
        memcpy(flags, flags_start, flags_len);
    }
    flags[flags_len] = '\0';

    // Create AST node
    AstNode *node =
        xr_ast_literal_regex(parser->compiler_session, pattern, flags, parser->previous.line);

    xr_free(pattern);
    xr_free(flags);

    return node;
}

// Parse type cast: int(x), float(x), string(x), bool(x)
AstNode *xr_parse_type_cast(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_cast: NULL parser");
    const char *type_name = NULL;
    switch (parser->previous.type) {
        case TK_INT:
            type_name = "int";
            break;
        case TK_FLOAT:
            type_name = "float";
            break;
        case TK_STRING:
            type_name = "string";
            break;
        case TK_BOOL:
            type_name = "bool";
            break;
        case TK_RUNE:
            type_name = "rune";
            break;
        default:
            xr_parser_error(parser, "expected type keyword");
            return NULL;
    }

    int line = parser->previous.line;

    /* Primitive type keywords also name their native type object for static
     * members (for example string.fromUtf8(...)). A following dot therefore
     * starts ordinary member access; only a following '(' is a cast. */
    if (xr_parser_check(parser, TK_DOT))
        return xr_ast_variable(parser->compiler_session, type_name, line);

    if (!xr_parser_match(parser, TK_LPAREN)) {
        xr_parser_error(parser, "expected '(' after type cast");
        return NULL;
    }
    AstNode *arg = xr_parse_expression(parser);
    if (!arg) {
        return NULL;
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after type cast argument");

    AstNode *callee = xr_ast_variable(parser->compiler_session, type_name, line);
    AstNode **arguments =
        (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 1);
    arguments[0] = arg;

    return xr_ast_call_expr(parser->compiler_session, callee, arguments, NULL, 1, line);
}

AstNode *xr_parse_comptime_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_comptime_expr: NULL parser");
    int line = parser->previous.line;
    int column = parser->previous.column;

    AstNode *expr = NULL;
    if (xr_parser_match(parser, TK_LBRACE)) {
        expr = xr_parse_block(parser);
    } else {
        expr = xr_parse_precedence(parser, PREC_TERNARY);
    }
    if (!expr) {
        xr_parser_error_at_previous(parser, "expected expression after 'comptime'");
        return NULL;
    }
    return xr_ast_comptime_expr(parser->compiler_session, expr, line, column);
}

// Helper: create string literal node from a template string part.
// For normal template strings, applies escape processing.
// For raw template strings, copies verbatim.
static AstNode *make_template_part(Parser *parser, const char *src, int len, bool is_raw,
                                   XrLiteralSourceForm source_form) {
    char *buf = (char *) xr_malloc(len + 1);
    size_t out_len;
    if (is_raw) {
        memcpy(buf, src, len);
        out_len = (size_t) len;
    } else {
        // Same decoder as plain string literals (xr_quoted_payload_decode)
        // so escape semantics cannot drift between the two surfaces.
        const char *error = NULL;
        if (!xr_escaped_bytes_decode((const uint8_t *) src, (size_t) len, (uint8_t *) buf, &out_len,
                                     &error)) {
            xr_free(buf);
            xr_parser_error_at_previous(parser,
                                        error ? error : "invalid escape in template string");
            return NULL;
        }
        if (memchr(buf, '\0', out_len) != NULL) {
            xr_free(buf);
            xr_parser_error_at_previous(
                parser, "string literals cannot contain byte escapes; use b\"...\"");
            return NULL;
        }
    }
    buf[out_len] = '\0';
    AstNode *node = xr_ast_literal_string(parser->compiler_session, buf,
                                          is_raw ? XR_LITERAL_RAW : XR_LITERAL_ESCAPED, source_form,
                                          parser->previous.line);
    xr_free(buf);
    return node;
}

static bool template_skip_rune(const char *src, int len, int *pos) {
    while (*pos < len) {
        char c = src[*pos];
        if (c == '\'') {
            (*pos)++;
            return true;
        }
        if (c == '\\') {
            *pos += (*pos + 1 < len) ? 2 : 1;
            continue;
        }
        (*pos)++;
    }
    return false;
}

static bool template_skip_quoted_token(const char *src, int len, int *pos) {
    if (!src || !pos || *pos < 0 || *pos >= len)
        return false;
    char lead = src[*pos];
    if (lead != '"' && lead != 'r' && lead != 'b' && lead != 'c')
        return false;
    Scanner scanner;
    xr_scanner_init(&scanner, src + *pos);
    Token token = xr_scanner_scan(&scanner);
    switch (token.type) {
        case TK_LITERAL_STRING:
        case TK_LITERAL_BYTE_STRING:
        case TK_LITERAL_C_STRING:
        case TK_TEMPLATE_STRING:
        case TK_RAW_STRING:
        case TK_RAW_TEMPLATE_STRING:
            if (token.start != src + *pos || token.length <= 0 || *pos + token.length > len)
                return false;
            *pos += token.length;
            return true;
        default:
            return false;
    }
}

static void template_skip_line_comment(const char *src, int len, int *pos) {
    while (*pos < len && src[*pos] != '\n') {
        (*pos)++;
    }
}

static bool template_skip_block_comment(const char *src, int len, int *pos) {
    *pos += 2;
    int depth = 1;
    while (*pos < len && depth > 0) {
        if (*pos + 1 < len && src[*pos] == '/' && src[*pos + 1] == '*') {
            *pos += 2;
            depth++;
            continue;
        }
        if (*pos + 1 < len && src[*pos] == '*' && src[*pos + 1] == '/') {
            *pos += 2;
            depth--;
            continue;
        }
        (*pos)++;
    }
    return depth == 0;
}

static bool template_find_expr_end(const char *src, int len, int expr_start, int *expr_end) {
    int brace_count = 1;
    int j = expr_start + 2;
    while (j < len && brace_count > 0) {
        char c = src[j];
        if (template_skip_quoted_token(src, len, &j))
            continue;
        if (c == '\'') {
            j++;
            if (!template_skip_rune(src, len, &j)) {
                return false;
            }
            continue;
        }
        if (c == '/' && j + 1 < len && src[j + 1] == '/') {
            template_skip_line_comment(src, len, &j);
            continue;
        }
        if (c == '/' && j + 1 < len && src[j + 1] == '*') {
            if (!template_skip_block_comment(src, len, &j)) {
                return false;
            }
            continue;
        }
        if (c == '{') {
            brace_count++;
            j++;
            continue;
        }
        if (c == '}') {
            brace_count--;
            if (brace_count == 0) {
                *expr_end = j;
                return true;
            }
            j++;
            continue;
        }
        j++;
    }
    return false;
}

// Parse template string: "Hello, ${name}!" or r"raw ${name}"
AstNode *xr_parse_template_string(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_template_string: NULL parser");
    bool is_raw = parser->previous.escape_mode == XR_LITERAL_RAW;
    XrLiteralSourceForm source_form = parser->previous.source_form;
    XrQuotedPayload payload = {0};
    const char *decode_error = NULL;
    if (!xr_quoted_payload_decode(&parser->previous, false, &payload, &decode_error)) {
        xr_parser_error_at_previous(parser,
                                    decode_error ? decode_error : "invalid template string");
        return NULL;
    }
    const char *tmpl = (const char *) payload.bytes;
    int tmpl_len = (int) payload.length;
    AstNode **parts = NULL;
    int part_count = 0;
    int part_capacity = 4;

    parts = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                         (size_t) part_capacity);
    if (!parts) {
        xr_quoted_payload_free(&payload);
        xr_parser_error(parser, "memory allocation failed");
        return NULL;
    }

    int i = 0;
    while (i < tmpl_len) {
        // Find next ${ (for normal mode, \$ escapes the dollar sign)
        int expr_start = -1;
        for (int j = i; j < tmpl_len - 1; j++) {
            if (!is_raw && tmpl[j] == '\\' && j + 1 < tmpl_len) {
                j++;
                continue;
            }
            if (tmpl[j] == '$' && tmpl[j + 1] == '{') {
                expr_start = j;
                break;
            }
        }

        if (expr_start == -1) {
            // No more interpolations, rest is string
            if (i < tmpl_len) {
                AstNode *str_node =
                    make_template_part(parser, tmpl + i, tmpl_len - i, is_raw, source_form);
                if (!str_node) {
                    xr_quoted_payload_free(&payload);
                    return NULL;
                }
                XR_PARSE_PUSH(parser, parts, part_count, part_capacity, str_node);
            }
            break;
        }

        // Add string part before ${
        if (expr_start > i) {
            AstNode *str_node =
                make_template_part(parser, tmpl + i, expr_start - i, is_raw, source_form);
            if (!str_node) {
                xr_quoted_payload_free(&payload);
                return NULL;
            }
            XR_PARSE_PUSH(parser, parts, part_count, part_capacity, str_node);
        }

        int expr_end = -1;
        if (!template_find_expr_end(tmpl, tmpl_len, expr_start, &expr_end)) {
            xr_quoted_payload_free(&payload);
            xr_parser_error(parser, "missing closing } in template string");
            return NULL;
        }

        // Parse interpolation expression
        int expr_len = expr_end - (expr_start + 2);
        if (expr_len > 0) {
            char *expr_code = (char *) xr_malloc(expr_len + 1);
            memcpy(expr_code, tmpl + expr_start + 2, expr_len);
            expr_code[expr_len] = '\0';

            Scanner expr_scanner;
            xr_scanner_init(&expr_scanner, expr_code);
            int expr_line = parser->previous.line + (source_form == XR_LITERAL_BLOCK ? 1 : 0);
            for (int p = 0; p < expr_start + 2; p++) {
                if (tmpl[p] == '\n')
                    expr_line++;
            }
            expr_scanner.line = expr_line;
            expr_scanner.start_line = expr_line;

            Parser expr_parser;
            memset(&expr_parser, 0, sizeof(expr_parser));
            expr_parser.scanner = expr_scanner;
            expr_parser.compiler_session = parser->compiler_session;
            expr_parser.had_error = 0;
            expr_parser.panic_mode = 0;

            xr_parser_advance(&expr_parser);
            AstNode *expr_node = xr_parse_expression(&expr_parser);

            if (expr_parser.had_error || !xr_parser_check(&expr_parser, TK_EOF)) {
                expr_node = NULL;
                xr_parser_error(parser, "invalid expression in template string");
            }

            xr_free(expr_code);

            if (!expr_node) {
                xr_quoted_payload_free(&payload);
                return NULL;
            }
            XR_PARSE_PUSH(parser, parts, part_count, part_capacity, expr_node);
        }

        i = expr_end + 1;  // Skip }
    }

    if (part_count == 0) {
        xr_quoted_payload_free(&payload);
        return xr_ast_literal_string(parser->compiler_session, "",
                                     is_raw ? XR_LITERAL_RAW : XR_LITERAL_ESCAPED, source_form,
                                     parser->previous.line);
    }

    AstNode *node = xr_ast_template_string(parser->compiler_session, parts, part_count,
                                           is_raw ? XR_LITERAL_RAW : XR_LITERAL_ESCAPED,
                                           source_form, parser->previous.line);
    xr_quoted_payload_free(&payload);
    return node;
}

// Parse grouping expression: (expression)
AstNode *xr_parse_grouping(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_grouping: NULL parser");
    int line = parser->previous.line;

    // Case 1: `() -> expr` no-param arrow function, or `()` unit literal.
    // Arrow closures cannot declare an explicit return type — use
    // `fn() -> T { ... }` or annotate the binding (`var f: () -> T = ...`).
    if (xr_parser_check(parser, TK_RPAREN)) {
        xr_parser_advance(parser);
        if (xr_parser_check(parser, TK_COLON)) {
            xr_parser_error(parser, "arrow closures cannot declare an explicit return type; "
                                    "use `fn() -> T { ... }` or annotate the binding");
            return NULL;
        }
        if (xr_parser_match(parser, TK_ARROW)) {
            return xr_parse_arrow_function_body(parser, NULL, 0, line);
        }
        /* `()` is the unit literal — the unique value of the unit type
         * `()`. Constant-folded to a singleton at lower time. */
        return xr_ast_tuple_literal(parser->compiler_session, NULL, 0, line);
    }

    // Case 2: arrow-function head — `(...) -> body`.
    //
    // To disambiguate from a tuple / grouping expression without committing
    // to a single parse, scan ahead through balanced parens for the matching
    // `)` and peek at the next token. `->` immediately after the closing
    // `)` is unambiguously an arrow head (no other expression-context syntax
    // produces `-> ...` there), so we only enter the arrow path on a positive
    // match. Anything else falls through to the general expression-list parse
    // below. Arrow closures cannot declare an explicit return type.
    bool is_arrow_head = false;
    {
        Scanner saved_scan = parser->scanner;
        Token saved_tok = parser->current;
        int depth = 1;
        while (depth > 0 && !xr_parser_check(parser, TK_EOF)) {
            if (xr_parser_check(parser, TK_LPAREN)) {
                depth++;
                xr_parser_advance(parser);
            } else if (xr_parser_check(parser, TK_RPAREN)) {
                depth--;
                if (depth == 0)
                    break;
                xr_parser_advance(parser);
            } else {
                xr_parser_advance(parser);
            }
        }
        if (xr_parser_check(parser, TK_RPAREN))
            xr_parser_advance(parser);
        is_arrow_head = xr_parser_check(parser, TK_ARROW);
        parser->scanner = saved_scan;
        parser->current = saved_tok;
    }

    if (is_arrow_head && xr_parser_check(parser, TK_NAME)) {
        // Collect params as XrParamNode. The array lives in the parse
        // arena because it is shallow-copied into the function_expr node.
        XrParamNode **params = NULL;
        int param_count = 0;
        int param_capacity = 0;

        XrParamNode *first_param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
        XR_PARSE_PUSH(parser, params, param_count, param_capacity, first_param);

        while (xr_parser_match(parser, TK_COMMA)) {
            if (xr_parser_check(parser, TK_RPAREN))
                break;
            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
            XR_PARSE_PUSH(parser, params, param_count, param_capacity, param);
        }

        if (!xr_parser_match(parser, TK_RPAREN)) {
            xr_parser_error(parser, "expected ')' or '->'");
            return NULL;
        }

        if (xr_parser_check(parser, TK_COLON)) {
            xr_parser_error(parser, "arrow closures cannot declare an explicit return type; "
                                    "use `fn(p: T) -> R { ... }` or annotate the binding");
            return NULL;
        }
        if (!xr_parser_match(parser, TK_ARROW)) {
            xr_parser_error(parser, "expected '->' after parameter list");
            return NULL;
        }
        return xr_parse_arrow_function_body(parser, params, param_count, line);
    }

    // Case 3: parenthesised expression list — tuple if any comma appears
    // (including a trailing comma for unary tuples `(x,)`), grouping
    // otherwise. Each element may be a spread (`...expr`) which is
    // statically expanded by the analyzer into the host tuple.
    AstNode *first = NULL;
    int first_line = parser->current.line;
    if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
        AstNode *inner = xr_parse_expression(parser);
        if (!inner)
            return NULL;
        first = xr_ast_spread_expr(parser->compiler_session, inner, first_line);
    } else {
        first = xr_parse_expression(parser);
    }
    if (!xr_parser_check(parser, TK_COMMA)) {
        if (first && first->type == AST_SPREAD_EXPR) {
            xr_parser_error(parser,
                            "spread '...' is only valid inside a tuple literal of arity >= 1; "
                            "wrap with a trailing comma to form a tuple");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close grouping");
        return xr_ast_grouping(parser->compiler_session, first, line);
    }

    AstNode **elems = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 16);
    int count = 0;
    int cap = 16;
    elems[count++] = first;
    while (xr_parser_match(parser, TK_COMMA)) {
        // Trailing comma is allowed and required for unary tuple `(x,)`.
        if (xr_parser_check(parser, TK_RPAREN))
            break;
        if (count >= cap) {
            int new_cap = cap * 2;
            AstNode **resized = (AstNode **) ast_alloc_array(parser->compiler_session,
                                                             sizeof(AstNode *), (size_t) new_cap);
            for (int i = 0; i < count; i++)
                resized[i] = elems[i];
            elems = resized;
            cap = new_cap;
        }
        int elem_line = parser->current.line;
        if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
            AstNode *inner = xr_parse_expression(parser);
            if (!inner)
                return NULL;
            elems[count++] = xr_ast_spread_expr(parser->compiler_session, inner, elem_line);
        } else {
            elems[count++] = xr_parse_expression(parser);
        }
    }
    xr_parser_consume(parser, TK_RPAREN, "expected ')' to close tuple literal");
    return xr_ast_tuple_literal(parser->compiler_session, elems, count, line);
}

// Parse a bare single-parameter lambda after Pratt consumed `->`.
// Giving `->` assignment-level precedence lets arrow-delimited constructs
// such as `select { value from channel -> ... }` stop at the delimiter by
// parsing their head with PREC_CALL, while ordinary expressions accept the
// same `parameter -> body` form in any position.
AstNode *xr_parse_bare_lambda(Parser *parser, AstNode *parameter) {
    if (!parameter || parameter->type != AST_VARIABLE) {
        xr_parser_error_at_previous(
            parser, "bare lambda parameter must be one identifier; use `(params) -> body`");
        return NULL;
    }

    XrParamNode **params =
        (XrParamNode **) ast_alloc_array(parser->compiler_session, sizeof(XrParamNode *), 1);
    params[0] = xr_param_node_new(parser->compiler_session, parameter->as.variable.name,
                                  parameter->line, parameter->column);
    return xr_parse_arrow_function_body(parser, params, 1, parameter->line);
}

// Parse arrow function body
// Supports: -> expr (auto return) or -> { ... } (block)
AstNode *xr_parse_arrow_function_body(Parser *parser, XrParamNode **params, int param_count,
                                      int line) {
    AstNode *body;

    parser->scope_depth++;
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Block body: -> { ... }
        body = xr_parse_block(parser);
    } else {
        // Expression body: -> expr (auto-wrap in return)
        AstNode *expr = xr_parse_expression(parser);
        if (!expr) {
            parser->scope_depth--;
            return NULL;
        }

        // return_stmt shallow-copies values into the AST node; must be arena.
        AstNode **values =
            (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 1);
        values[0] = expr;
        AstNode *return_stmt = xr_ast_return_stmt(parser->compiler_session, values, 1, expr->line);

        body = xr_ast_block(parser->compiler_session, line);
        xr_ast_block_add(parser->compiler_session, body, return_stmt);
    }

    parser->scope_depth--;

    // params ownership transferred to func_expr
    return xr_ast_function_expr(parser->compiler_session, params, param_count, body, line);
}

// Parse fn anonymous function expression
// Syntax: fn() { ... } or fn(a, b) { return a + b }
AstNode *xr_parse_fn_expression(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_fn_expression: NULL parser");
    int line = parser->previous.line;

    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;
    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;
            char *param_name =
                (char *) ast_alloc(parser->compiler_session, (size_t) param_token.length + 1);
            memcpy(param_name, param_token.start, param_token.length);
            param_name[param_token.length] = '\0';

            XrTypeRef **constraints = NULL;
            int constraint_count = 0;
            if (xr_parser_match(parser, TK_COLON)) {
                constraints = xr_parse_constraint_list(parser, &constraint_count);
            }

            XrGenericParam *gp =
                (XrGenericParam *) ast_alloc(parser->compiler_session, sizeof(XrGenericParam));
            gp->name = param_name;
            gp->constraints = constraints;
            gp->constraint_count = constraint_count;
            XR_PARSE_PUSH(parser, type_params, type_param_count, type_param_capacity, gp);
        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));
        xr_parser_consume(parser, TK_GT, "expected '>' to close generic params");
    }

    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *type_param =
                xr_tref_type_param(parser->compiler_session, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, type_param);
        }
        parser->type_scope = generic_scope;
    }

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after fn");
    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);

            XR_PARSE_PUSH(parser, params, param_count, param_capacity, param);
        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after parameter list");

    // Parse optional return type annotation: `fn(...) -> T { ... }`.
    // The unified arrow `->` is the only legal separator.
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_COLON)) {
        xr_parser_advance(parser);  // consume ':'
        xr_parser_error(parser, "use '->' instead of ':' for function return type, "
                                "e.g. fn(p: T) -> R");
        parser->panic_mode = 0;
        return_type = xr_parse_type_annotation(parser);
    }

    // Parse function body (must be block)
    xr_parser_consume(parser, TK_LBRACE, "fn function body must use braces { }");
    parser->scope_depth++;
    AstNode *body = xr_parse_block(parser);
    parser->scope_depth--;

    AstNode *func_expr =
        xr_ast_function_expr(parser->compiler_session, params, param_count, body, line);
    func_expr->as.function_expr.return_type = return_type;
    func_expr->as.function_expr.type_params = type_params;
    func_expr->as.function_expr.type_param_count = type_param_count;

    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    return func_expr;
}

// Parse unary operators: -expr, !expr, ~expr
AstNode *xr_parse_unary(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_unary: NULL parser");
    XrTokenType operator_type = parser->previous.type;
    int line = parser->previous.line;

    AstNode *operand = xr_parse_precedence(parser, PREC_UNARY);
    switch (operator_type) {
        case TK_MINUS:
            return xr_ast_unary(parser->compiler_session, AST_UNARY_NEG, operand, line);
        case TK_NOT:
            return xr_ast_unary(parser->compiler_session, AST_UNARY_NOT, operand, line);
        case TK_TILDE:
            return xr_ast_unary(parser->compiler_session, AST_UNARY_BNOT, operand, line);
        default:
            xr_parser_error(parser, "unknown unary operator");
            return NULL;
    }
}

/* ========== Infix Parsing ========== */

// Try to parse generic call: callee<Type, ...>(args)
// Returns NULL if not a generic call (should fallback to comparison)
AstNode *xr_parse_try_generic_call_after_lt(Parser *parser, AstNode *callee) {
    // Only try if callee is an identifier or member access
    if (callee->type != AST_VARIABLE && callee->type != AST_MEMBER_ACCESS) {
        return NULL;
    }

    int line = parser->previous.line;
    Parser checkpoint = *parser;
    int saved_panic_mode = parser->panic_mode;
    int saved_error_count = parser->error_count;

    // Suppress error output during speculative parsing
    parser->panic_mode = 1;

    // Try to parse type arguments
    XrTypeRef *type_args[16];
    int type_arg_count = 0;

    // Already consumed '<', now parse type list
    do {
        if (type_arg_count >= 16)
            break;

        XrTypeRef *type = xr_parse_type_annotation(parser);
        if (parser->error_count > saved_error_count) {
            return NULL;
        }
        if (!type || parser->had_error) {
            // Not valid type args, restore and return NULL
            *parser = checkpoint;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
        type_args[type_arg_count++] = type;

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));

    // Must have '>' followed by '('
    if (!xr_parser_match(parser, TK_GT)) {
        // Handle '>>' case
        if (parser->current.type == TK_RSHIFT) {
            parser->current.type = TK_GT;
            parser->current.start++;
            parser->current.length = 1;
        } else {
            *parser = checkpoint;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
    }

    // Must be followed by '(' for function call
    if (!xr_parser_check(parser, TK_LPAREN)) {
        *parser = checkpoint;
        parser->panic_mode = saved_panic_mode;
        return NULL;
    }

    // Restore panic_mode now that we confirmed it's a valid generic call
    parser->panic_mode = saved_panic_mode;

    // Parse the function call
    xr_parser_advance(parser);  // consume '('

    AstNode **arguments = NULL;
    XrCallArgAccess *arg_accesses = NULL;
    int arg_count = 0;
    int arg_capacity = 0;
    int access_count = 0;
    int access_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrCallArgAccess access = XR_CALL_ARG_PLAIN;
            AstNode *arg = xr_parse_call_argument_with_access(parser, &access);
            XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, arg);
            XR_PARSE_PUSH(parser, arg_accesses, access_count, access_capacity, access);
        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");

    // `Map<K,V>()` / `Array<T>()` / `Channel<T>(n)` etc. construct built-in
    // heap types directly (no `new`); route to the construction node so the
    // generic type arguments drive element/key/value layout.
    if (callee->type == AST_VARIABLE && xr_is_construct_only_type_name(callee->as.variable.name)) {
        return xr_ast_new_expr(parser->compiler_session, NULL, callee->as.variable.name, arguments,
                               arg_accesses, arg_count, type_args, type_arg_count, line);
    }

    return xr_ast_call_expr_generic(parser->compiler_session, callee, arguments, arg_accesses,
                                    arg_count, type_args, type_arg_count, line);
}

// Parse '<' which could be comparison or generic call
// Uses space sensitivity: foo<T>() is generic, foo < T is comparison
AstNode *xr_parse_lt_or_generic(Parser *parser, AstNode *left) {
    // If '<' has leading space, treat as comparison
    // e.g., "a < b" is comparison, "a<b>" could be generic
    if (parser->previous.has_leading_space) {
        // Fall back to comparison
        int line = parser->previous.line;
        const ParseRule *rule = xr_get_rule(TK_LT);
        AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
        return xr_ast_binary(parser->compiler_session, AST_BINARY_LT, left, right, line);
    }

    // Try generic call first (no space before '<')
    int saved_error_count = parser->error_count;
    AstNode *generic_call = xr_parse_try_generic_call_after_lt(parser, left);
    if (generic_call) {
        return generic_call;
    }
    if (parser->error_count > saved_error_count) {
        return left;
    }

    // Fall back to comparison
    int line = parser->previous.line;
    const ParseRule *rule = xr_get_rule(TK_LT);
    AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
    return xr_ast_binary(parser->compiler_session, AST_BINARY_LT, left, right, line);
}

// XrTokenType -> AstNodeType mapping for binary operators
static const AstNodeType binary_op_map[] = {
    [TK_PLUS] = AST_BINARY_ADD,      [TK_MINUS] = AST_BINARY_SUB,   [TK_STAR] = AST_BINARY_MUL,
    [TK_SLASH] = AST_BINARY_DIV,     [TK_PERCENT] = AST_BINARY_MOD, [TK_AMP] = AST_BINARY_BAND,
    [TK_PIPE] = AST_BINARY_BOR,      [TK_CARET] = AST_BINARY_BXOR,  [TK_LSHIFT] = AST_BINARY_LSHIFT,
    [TK_RSHIFT] = AST_BINARY_RSHIFT, [TK_EQ] = AST_BINARY_EQ,       [TK_NE] = AST_BINARY_NE,
    [TK_LT] = AST_BINARY_LT,         [TK_LE] = AST_BINARY_LE,       [TK_GT] = AST_BINARY_GT,
    [TK_GE] = AST_BINARY_GE,         [TK_AND] = AST_BINARY_AND,     [TK_OR] = AST_BINARY_OR,
};

// Parse binary operators: left op right
AstNode *xr_parse_binary(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_binary: NULL parser");
    XrTokenType operator_type = parser->previous.type;
    int line = parser->previous.line;

    const ParseRule *rule = xr_get_rule(operator_type);

    // Parse right operand (left-associative: precedence + 1)
    AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);

    AstNodeType ast_type = 0;
    if (operator_type >= 0 &&
        operator_type < (XrTokenType) (sizeof(binary_op_map) / sizeof(binary_op_map[0]))) {
        ast_type = binary_op_map[operator_type];
    }
    if (ast_type == 0) {
        xr_parser_error(parser, "unknown binary operator");
        return NULL;
    }

    return xr_ast_binary(parser->compiler_session, ast_type, left, right, line);
}

// Parse 'is' expression: expr is Type
AstNode *xr_parse_is(Parser *parser, AstNode *left) {
    int line = parser->previous.line;

    XrTypeRef *type = xr_parse_type_annotation(parser);
    if (!type) {
        xr_parser_error(parser, "expected type after 'is'");
        return NULL;
    }

    return xr_ast_is_expr(parser->compiler_session, left, type, line);
}

// Parse ternary expression: condition ? trueValue : falseValue
AstNode *xr_parse_ternary(Parser *parser, AstNode *condition) {
    XR_DCHECK(parser != NULL, "parse_ternary: NULL parser");
    int line = parser->previous.line;

    AstNode *true_expr = xr_parse_precedence(parser, PREC_TERNARY + 1);

    xr_parser_consume(parser, TK_COLON, "expected ':' in ternary expression");

    AstNode *false_expr = xr_parse_precedence(parser, PREC_TERNARY);

    return xr_ast_ternary(parser->compiler_session, condition, true_expr, false_expr, line);
}

// Parse nullish coalescing: value ?? defaultValue
AstNode *xr_parse_nullish_coalesce(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_nullish_coalesce: NULL parser");
    int line = parser->previous.line;

    AstNode *right = xr_parse_precedence(parser, PREC_NULLISH_COALESCE + 1);

    return xr_ast_binary(parser->compiler_session, AST_NULLISH_COALESCE, left, right, line);
}

// Parse force unwrap: expr! (panics at runtime if value is null)
AstNode *xr_parse_force_unwrap(Parser *parser, AstNode *operand) {
    XR_DCHECK(parser != NULL, "parse_force_unwrap: NULL parser");
    int line = parser->previous.line;
    return xr_ast_unary(parser->compiler_session, AST_FORCE_UNWRAP, operand, line);
}

// Parse as cast: expr as Type / expr as Type?
AstNode *xr_parse_as_cast(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_as_cast: NULL parser");
    int line = parser->previous.line;
    XrTypeRef *target_type = xr_parse_type_annotation(parser);
    if (!target_type) {
        xr_parser_error(parser, "expected type after 'as'");
        return left;
    }
    // Check for safe cast: as Type? (XrTypeRef uses XR_TREF_OPTIONAL kind)
    bool is_safe = xr_tref_is_nullable(target_type);
    return xr_ast_as_expr(parser->compiler_session, left, target_type, is_safe, line);
}

// Parse optional chain: obj?.prop, obj?.method(), func?.()
AstNode *xr_parse_optional_chain(Parser *parser, AstNode *object) {
    XR_DCHECK(parser != NULL, "parse_optional_chain: NULL parser");
    int line = parser->previous.line;

    if (parser->current.type == TK_LPAREN) {
        return xr_ast_optional_chain(parser->compiler_session, object, NULL, NULL, 3, line);
    }

    if (parser->current.type == TK_NAME) {
        // Property access: obj?.prop
        xr_parser_advance(parser);
        const char *name = parser->previous.start;
        int name_len = parser->previous.length;
        char *name_str = (char *) ast_alloc(parser->compiler_session, name_len + 1);
        memcpy(name_str, name, name_len);
        name_str[name_len] = '\0';

        // Check for method call
        if (parser->current.type == TK_LPAREN) {
            return xr_ast_optional_chain(parser->compiler_session, object, name_str, NULL, 2, line);
        }

        return xr_ast_optional_chain(parser->compiler_session, object, name_str, NULL, 0, line);
    } else {
        xr_parser_error(parser, "expected property name after '?.'");
        return NULL;
    }
}

// Parse optional index access: obj?[index]
AstNode *xr_parse_optional_index(Parser *parser, AstNode *object) {
    XR_DCHECK(parser != NULL, "parse_optional_index: NULL parser");
    int line = parser->previous.line;

    // '[' already consumed by lexer as part of '?[' token
    AstNode *index = xr_parse_expression(parser);
    xr_parser_consume(parser, TK_RBRACKET, "expected ']' after optional index expression");
    return xr_ast_optional_chain(parser->compiler_session, object, NULL, index, 1, line);
}

// Parse range expression: start..end / start..=end
AstNode *xr_parse_range(Parser *parser, AstNode *start) {
    XR_DCHECK(parser != NULL, "parse_range: NULL parser");
    int line = parser->previous.line;
    bool inclusive_end = parser->previous.type == TK_RANGE_INCLUSIVE;

    AstNode *end = xr_parse_precedence(parser, PREC_FACTOR + 1);

    return xr_ast_range(parser->compiler_session, start, end, inclusive_end, line);
}
