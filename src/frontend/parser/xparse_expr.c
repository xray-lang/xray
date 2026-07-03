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

// Parse integer literal (supports multiple bases and underscore separators)
// Formats: decimal (123), hex (0xFF), binary (0b1010), octal (0o755)
static xr_Integer parse_integer_literal(const char *start, int length) {
    char buf[64];
    int buf_len = strip_underscores(start, length, buf, sizeof(buf));

    // Detect base
    if (buf_len >= 2 && buf[0] == '0') {
        char prefix = buf[1];
        if (prefix == 'x' || prefix == 'X') {
            return strtoll(buf + 2, NULL, 16);  // Hex
        } else if (prefix == 'b' || prefix == 'B') {
            return strtoll(buf + 2, NULL, 2);  // Binary
        } else if (prefix == 'o' || prefix == 'O') {
            return strtoll(buf + 2, NULL, 8);  // Octal
        }
    }

    return strtoll(buf, NULL, 10);  // Decimal
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

static const char *parse_char_literal_payload(const char *src, size_t len, uint32_t *out_cp) {
    if (!src || len == 0)
        return "char literal cannot be empty";

    if (src[0] == '\\') {
        if (len < 2)
            return "unterminated char escape";
        uint32_t cp = 0;
        if (src[1] == 'u') {
            if (len < 4 || src[2] != '{')
                return "char unicode escape must use \\u{...}";
            size_t p = 3;
            uint32_t value = 0;
            int digits = 0;
            while (p < len && src[p] != '}') {
                int h = char_hex_value(src[p]);
                if (h < 0)
                    return "invalid hex digit in char unicode escape";
                if (digits >= 6)
                    return "char unicode escape must contain at most 6 hex digits";
                value = (value << 4) | (uint32_t) h;
                digits++;
                p++;
            }
            if (digits == 0)
                return "char unicode escape requires at least one hex digit";
            if (p >= len || src[p] != '}')
                return "unterminated char unicode escape";
            if (p + 1 != len)
                return "char literal must contain exactly one Unicode scalar value";
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
                    return "invalid char escape";
            }
            if (len != 2)
                return "char literal must contain exactly one Unicode scalar value";
        }
        if (!xr_unicode_is_scalar(cp))
            return "char literal must be a valid Unicode scalar value";
        *out_cp = cp;
        return NULL;
    }

    uint32_t cp = 0;
    int consumed = xr_utf8_decode(src, len, &cp);
    if (consumed <= 0)
        return "invalid UTF-8 in char literal";
    if ((unsigned char) src[0] >= 0x80 && consumed == 1 && cp == XR_UNICODE_INVALID)
        return "invalid UTF-8 in char literal";
    if (!xr_unicode_is_scalar(cp))
        return "char literal must be a valid Unicode scalar value";
    if ((size_t) consumed != len)
        return "char literal must contain exactly one Unicode scalar value";
    *out_cp = cp;
    return NULL;
}

// Parse literal (number, string, bool, null)
AstNode *xr_parse_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_literal: NULL parser");
    int column = parser->previous.column;
    switch (parser->previous.type) {
        case TK_LITERAL_INT: {
            xr_Integer value =
                parse_integer_literal(parser->previous.start, parser->previous.length);
            // Full int64 range allowed at parse time; range checks against
            // the target type happen later in the analyzer/compiler.
            AstNode *node =
                xr_ast_literal_int(parser->compiler_session, value, parser->previous.line);
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

        case TK_LITERAL_STRING: {
            const char *src = parser->previous.start + 1;
            size_t src_len = parser->previous.length - 2;
            char *str = (char *) xr_malloc(src_len + 1);
            size_t dst_pos = xr_process_escapes(src, src_len, str);
            str[dst_pos] = '\0';
            AstNode *node =
                xr_ast_literal_string(parser->compiler_session, str, parser->previous.line);
            node->column = column;
            xr_free(str);
            return node;
        }

        case TK_LITERAL_CHAR: {
            const char *src = parser->previous.start + 1;
            size_t src_len = (size_t) parser->previous.length - 2;
            uint32_t cp = 0;
            const char *err = parse_char_literal_payload(src, src_len, &cp);
            if (err)
                xr_parser_error_at_previous(parser, err);
            AstNode *node =
                xr_ast_literal_char(parser->compiler_session, cp, parser->previous.line);
            node->column = column;
            return node;
        }

        case TK_RAW_STRING: {
            // r"content" - no escape processing
            const char *src = parser->previous.start + 2;
            size_t src_len = parser->previous.length - 3;
            char *str = (char *) xr_malloc(src_len + 1);
            memcpy(str, src, src_len);
            str[src_len] = '\0';
            AstNode *node =
                xr_ast_literal_string(parser->compiler_session, str, parser->previous.line);
            node->column = column;
            xr_free(str);
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
        case TK_CHAR:
            type_name = "char";
            break;
        default:
            xr_parser_error(parser, "expected type keyword");
            return NULL;
    }

    int line = parser->previous.line;

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

    return xr_ast_call_expr(parser->compiler_session, callee, arguments, 1, line);
}

// Helper: create string literal node from a template string part.
// For normal template strings, applies escape processing.
// For raw template strings, copies verbatim.
static AstNode *make_template_part(Parser *parser, const char *src, int len, bool is_raw) {
    char *buf = (char *) xr_malloc(len + 1);
    size_t out_len;
    if (is_raw) {
        memcpy(buf, src, len);
        out_len = len;
    } else {
        out_len = xr_process_escapes(src, len, buf);
    }
    buf[out_len] = '\0';
    AstNode *node = xr_ast_literal_string(parser->compiler_session, buf, parser->previous.line);
    xr_free(buf);
    return node;
}

static bool template_find_expr_end(const char *src, int len, int expr_start, int *expr_end);

static bool template_skip_string(const char *src, int len, int *pos, char quote, bool is_raw) {
    while (*pos < len) {
        char c = src[*pos];
        if (c == quote) {
            (*pos)++;
            return true;
        }
        if (!is_raw && c == '\\') {
            *pos += (*pos + 1 < len) ? 2 : 1;
            continue;
        }
        if (c == '$' && *pos + 1 < len && src[*pos + 1] == '{') {
            int nested_end = -1;
            if (!template_find_expr_end(src, len, *pos, &nested_end)) {
                return false;
            }
            *pos = nested_end + 1;
            continue;
        }
        (*pos)++;
    }
    return false;
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
        if (c == '"' || c == '\'') {
            j++;
            if (!template_skip_string(src, len, &j, c, false)) {
                return false;
            }
            continue;
        }
        if (c == 'r' && j + 1 < len && (src[j + 1] == '"' || src[j + 1] == '\'')) {
            char raw_quote = src[j + 1];
            j += 2;
            if (!template_skip_string(src, len, &j, raw_quote, true)) {
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
    bool is_raw = (parser->previous.type == TK_RAW_TEMPLATE_STRING);
    int skip = is_raw ? 2 : 1;  // r" vs "
    const char *tmpl = parser->previous.start + skip;
    int tmpl_len = parser->previous.length - skip - 1;
    AstNode **parts = NULL;
    int part_count = 0;
    int part_capacity = 4;

    parts = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                         (size_t) part_capacity);
    if (!parts) {
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
                AstNode *str_node = make_template_part(parser, tmpl + i, tmpl_len - i, is_raw);
                XR_PARSE_PUSH(parser, parts, part_count, part_capacity, str_node);
            }
            break;
        }

        // Add string part before ${
        if (expr_start > i) {
            AstNode *str_node = make_template_part(parser, tmpl + i, expr_start - i, is_raw);
            XR_PARSE_PUSH(parser, parts, part_count, part_capacity, str_node);
        }

        int expr_end = -1;
        if (!template_find_expr_end(tmpl, tmpl_len, expr_start, &expr_end)) {
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

            Parser expr_parser;
            memset(&expr_parser, 0, sizeof(expr_parser));
            expr_parser.scanner = expr_scanner;
            expr_parser.compiler_session = parser->compiler_session;
            expr_parser.had_error = 0;
            expr_parser.panic_mode = 0;

            xr_parser_advance(&expr_parser);
            AstNode *expr_node = xr_parse_expression(&expr_parser);

            xr_free(expr_code);

            if (expr_node) {
                XR_PARSE_PUSH(parser, parts, part_count, part_capacity, expr_node);
            }
        }

        i = expr_end + 1;  // Skip }
    }

    if (part_count == 0) {
        return xr_ast_literal_string(parser->compiler_session, "", parser->previous.line);
    }

    AstNode *node =
        xr_ast_template_string(parser->compiler_session, parts, part_count, parser->previous.line);
    return node;
}

// Parse grouping expression: (expression)
AstNode *xr_parse_grouping(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_grouping: NULL parser");
    int line = parser->previous.line;

    // Case 1: `() -> expr` no-param arrow function, or `()` unit literal.
    // Arrow closures cannot declare an explicit return type — use
    // `fn() -> T { ... }` or annotate the binding (`let f: () -> T = ...`).
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
        XrParamNode **params =
            (XrParamNode **) ast_alloc_array(parser->compiler_session, sizeof(XrParamNode *), 10);
        int param_count = 0;
        char name_buf[256];

        // First param
        Token first_name = parser->current;
        xr_parser_advance(parser);
        snprintf(name_buf, sizeof(name_buf), "%.*s", first_name.length, first_name.start);
        params[param_count] = xr_param_node_new(parser->compiler_session, name_buf, first_name.line,
                                                first_name.column);
        if (xr_parser_match(parser, TK_COLON)) {
            params[param_count]->type = xr_parse_type_annotation(parser);
        }
        param_count++;

        while (xr_parser_match(parser, TK_COMMA)) {
            if (xr_parser_check(parser, TK_RPAREN))
                break;
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            Token param = parser->previous;
            snprintf(name_buf, sizeof(name_buf), "%.*s", param.length, param.start);
            params[param_count] =
                xr_param_node_new(parser->compiler_session, name_buf, param.line, param.column);
            if (xr_parser_match(parser, TK_COLON)) {
                params[param_count]->type = xr_parse_type_annotation(parser);
            }
            param_count++;
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

static char *expr_token_to_ast_string(Parser *parser, Token tok) {
    char *s = (char *) ast_alloc(parser->compiler_session, (size_t) tok.length + 1);
    memcpy(s, tok.start, (size_t) tok.length);
    s[tok.length] = '\0';
    return s;
}

static XrParallelLocalBinding *parse_expr_parallel_locals(Parser *parser, int *out_count) {
    XrParallelLocalBinding *locals = NULL;
    int count = 0;
    int capacity = 0;
    while (xr_parser_match_name(parser, "local")) {
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            xr_parser_error_expected_name(parser, "expected parallel local variable name");
            *out_count = 0;
            return NULL;
        }
        XrParallelLocalBinding binding;
        memset(&binding, 0, sizeof(binding));
        binding.name = expr_token_to_ast_string(parser, parser->current);
        xr_parser_advance(parser);
        if (xr_parser_match(parser, TK_IN)) {
            binding.is_initializer = false;
        } else if (xr_parser_match(parser, TK_ASSIGN)) {
            binding.is_initializer = true;
        } else {
            xr_parser_error_at_current(parser,
                                       "expected 'in' or '=' after parallel local variable name");
            *out_count = 0;
            return NULL;
        }
        binding.source = xr_parse_expression(parser);
        if (!binding.source) {
            *out_count = 0;
            return NULL;
        }
        XR_PARSE_PUSH(parser, locals, count, capacity, binding);
    }
    *out_count = count;
    return locals;
}

static AstNode *parse_expr_parallel_final(Parser *parser) {
    if (!xr_parser_match(parser, TK_FINAL))
        return NULL;
    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "parallel final block requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    return xr_parse_block(parser);
}

static AstNode *xr_parse_parallel_reduce_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_parallel_reduce_expr: NULL parser");
    int line = parser->previous.line;

    if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
        xr_parser_error_expected_name(parser, "expected parallel reduce item variable name");
        return NULL;
    }
    char *item_name = expr_token_to_ast_string(parser, parser->current);
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_IN, "expected 'in' after parallel reduce item variable");
    AstNode *range = xr_parse_expression(parser);
    if (!range)
        return NULL;

    AstNode *worker_count = NULL;
    if (xr_parser_match_name(parser, "workers")) {
        worker_count = xr_parse_expression(parser);
        if (!worker_count)
            return NULL;
    }

    char *worker_name = NULL;
    if (xr_parser_match_name(parser, "worker")) {
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            xr_parser_error_expected_name(parser, "expected parallel reduce worker variable name");
            return NULL;
        }
        worker_name = expr_token_to_ast_string(parser, parser->current);
        xr_parser_advance(parser);
    }

    int local_count = 0;
    XrParallelLocalBinding *locals = parse_expr_parallel_locals(parser, &local_count);
    if (parser->panic_mode)
        return NULL;

    if (!xr_parser_match_name(parser, "init")) {
        xr_parser_error_at_current(parser, "expected 'init' in parallel reduce expression");
        return NULL;
    }
    AstNode *initial = xr_parse_expression(parser);
    if (!initial)
        return NULL;

    if (!xr_parser_match_name(parser, "combine")) {
        xr_parser_error_at_current(parser, "expected 'combine' in parallel reduce expression");
        return NULL;
    }
    AstNode *combine = xr_parse_expression(parser);
    if (!combine)
        return NULL;

    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "parallel reduce expression requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *body = xr_parse_block(parser);

    return xr_ast_parallel_reduce_expr(parser->compiler_session, item_name, range, worker_count,
                                       worker_name, locals, local_count, initial, combine, body,
                                       line);
}

static AstNode *xr_parse_parallel_range_reduce_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_parallel_range_reduce_expr: NULL parser");
    int line = parser->previous.line;

    if (!xr_parser_match_name(parser, "reduce")) {
        xr_parser_error_at_current(
            parser, "expected 'reduce' after 'parallel range' in expression context");
        return NULL;
    }

    if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
        xr_parser_error_expected_name(parser, "expected parallel range reduce begin variable name");
        return NULL;
    }
    char *begin_name = expr_token_to_ast_string(parser, parser->current);
    xr_parser_advance(parser);

    if (!xr_parser_match(parser, TK_COMMA)) {
        xr_parser_error(parser, "expected ',' between parallel range reduce begin and end names");
        return NULL;
    }
    if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
        xr_parser_error_expected_name(parser, "expected parallel range reduce end variable name");
        return NULL;
    }
    char *end_name = expr_token_to_ast_string(parser, parser->current);
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_IN, "expected 'in' after parallel range reduce variables");
    AstNode *range = xr_parse_expression(parser);
    if (!range)
        return NULL;

    AstNode *worker_count = NULL;
    if (xr_parser_match_name(parser, "workers")) {
        worker_count = xr_parse_expression(parser);
        if (!worker_count)
            return NULL;
    }

    char *worker_name = NULL;
    if (xr_parser_match_name(parser, "worker")) {
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            xr_parser_error_expected_name(parser,
                                          "expected parallel range reduce worker variable name");
            return NULL;
        }
        worker_name = expr_token_to_ast_string(parser, parser->current);
        xr_parser_advance(parser);
    }

    int local_count = 0;
    XrParallelLocalBinding *locals = parse_expr_parallel_locals(parser, &local_count);
    if (parser->panic_mode)
        return NULL;

    if (!xr_parser_match_name(parser, "init")) {
        xr_parser_error_at_current(parser, "expected 'init' in parallel range reduce expression");
        return NULL;
    }
    AstNode *initial = xr_parse_expression(parser);
    if (!initial)
        return NULL;

    if (!xr_parser_match_name(parser, "combine")) {
        xr_parser_error_at_current(parser,
                                   "expected 'combine' in parallel range reduce expression");
        return NULL;
    }
    AstNode *combine = xr_parse_expression(parser);
    if (!combine)
        return NULL;

    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "parallel range reduce expression requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *body = xr_parse_block(parser);

    return xr_ast_parallel_range_reduce_expr(parser->compiler_session, begin_name, end_name, range,
                                             worker_count, worker_name, locals, local_count,
                                             initial, combine, body, line);
}

// Parse parallel collect expression:
// parallel for i in 0..n workers workers worker wid collect [into results] { ...expr }
static AstNode *xr_parse_parallel_collect_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_parallel_collect_expr: NULL parser");
    int line = parser->previous.line;

    if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
        xr_parser_error_expected_name(parser, "expected parallel collect item variable name");
        return NULL;
    }
    char *item_name = expr_token_to_ast_string(parser, parser->current);
    xr_parser_advance(parser);

    xr_parser_consume(parser, TK_IN, "expected 'in' after parallel collect item variable");
    AstNode *range = xr_parse_expression(parser);
    if (!range)
        return NULL;

    AstNode *worker_count = NULL;
    if (xr_parser_match_name(parser, "workers")) {
        worker_count = xr_parse_expression(parser);
        if (!worker_count)
            return NULL;
    }

    char *worker_name = NULL;
    if (xr_parser_match_name(parser, "worker")) {
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            xr_parser_error_expected_name(parser, "expected parallel collect worker variable name");
            return NULL;
        }
        worker_name = expr_token_to_ast_string(parser, parser->current);
        xr_parser_advance(parser);
    }

    int local_count = 0;
    XrParallelLocalBinding *locals = parse_expr_parallel_locals(parser, &local_count);
    if (parser->panic_mode)
        return NULL;

    AstNode *final_body = parse_expr_parallel_final(parser);
    if (parser->panic_mode)
        return NULL;

    if (!xr_parser_match_name(parser, "collect")) {
        xr_parser_error_at_current(parser, "expected 'collect' before parallel collect body");
        return NULL;
    }
    AstNode *into = NULL;
    if (xr_parser_match_name(parser, "into")) {
        into = xr_parse_precedence(parser, PREC_UNARY);
        if (!into) {
            xr_parser_error(parser, "expected result array expression after 'into'");
            return NULL;
        }
    }
    if (!xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_error_at_current(parser, "parallel collect expression requires braces { }");
        return NULL;
    }
    xr_parser_advance(parser);
    AstNode *body = xr_parse_block(parser);

    return xr_ast_parallel_collect_expr(parser->compiler_session, item_name, range, worker_count,
                                        worker_name, locals, local_count, into, final_body, body,
                                        line);
}

AstNode *xr_parse_parallel_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_parallel_expr: NULL parser");
    if (xr_parser_match_name(parser, "range"))
        return xr_parse_parallel_range_reduce_expr(parser);
    if (xr_parser_match_name(parser, "reduce"))
        return xr_parse_parallel_reduce_expr(parser);
    if (xr_parser_match(parser, TK_FOR))
        return xr_parse_parallel_collect_expr(parser);
    xr_parser_error_at_previous(
        parser, "expected 'range', 'reduce' or 'for' after 'parallel' in expression context");
    return NULL;
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
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            Token param_token = parser->previous;

            char param_name[256];
            snprintf(param_name, sizeof(param_name), "%.*s", param_token.length, param_token.start);

            XrParamNode *param = xr_param_node_new(parser->compiler_session, param_name,
                                                   param_token.line, param_token.column);

            // Parse optional type annotation
            if (xr_parser_match(parser, TK_COLON)) {
                param->type = xr_parse_type_annotation(parser);
            }

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
static AstNode *try_parse_generic_call(Parser *parser, AstNode *callee) {
    // Only try if callee is an identifier or member access
    if (callee->type != AST_VARIABLE && callee->type != AST_MEMBER_ACCESS) {
        return NULL;
    }

    int line = parser->previous.line;
    Parser checkpoint = *parser;
    int saved_panic_mode = parser->panic_mode;

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
    int arg_count = 0;
    int arg_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity,
                          xr_parse_call_argument(parser));
        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");

    // `Map<K,V>()` / `Array<T>()` / `Channel<T>(n)` etc. construct built-in
    // heap types directly (no `new`); route to the construction node so the
    // generic type arguments drive element/key/value layout.
    if (callee->type == AST_VARIABLE && xr_is_construct_only_type_name(callee->as.variable.name)) {
        return xr_ast_new_expr(parser->compiler_session, NULL, callee->as.variable.name, arguments,
                               arg_count, type_args, type_arg_count, line);
    }

    return xr_ast_call_expr_generic(parser->compiler_session, callee, arguments, arg_count,
                                    type_args, type_arg_count, line);
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
    AstNode *generic_call = try_parse_generic_call(parser, left);
    if (generic_call) {
        return generic_call;
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
// Bare container types allowed: 'x is Array' checks runtime type without element type.
AstNode *xr_parse_is(Parser *parser, AstNode *left) {
    int line = parser->previous.line;

    // Allow bare container types for runtime type checks
    bool saved = parser->allow_bare_container;
    parser->allow_bare_container = true;
    XrTypeRef *type = xr_parse_type_annotation(parser);
    parser->allow_bare_container = saved;
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
// Bare container types allowed: 'x as Array' for runtime type casts.
AstNode *xr_parse_as_cast(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_as_cast: NULL parser");
    int line = parser->previous.line;
    // Allow bare container types for runtime type casts
    bool saved = parser->allow_bare_container;
    parser->allow_bare_container = true;
    XrTypeRef *target_type = xr_parse_type_annotation(parser);
    parser->allow_bare_container = saved;
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
