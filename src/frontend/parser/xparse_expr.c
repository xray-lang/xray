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
#include "../../shared/xr_decimal_float.h"

static bool parser_decimal_charge(void *context, uint64_t units) {
    return xr_compile_state_work((XrCompileState *) context, units) == XR_COMPILE_RESOURCE_OK;
}

#include <stdint.h>

/* ========== Helpers ========== */

static const char *XR_ARROW_RETURN_TYPE_DIAGNOSTIC =
    "arrow lambda has no return-type position; use `fn(x: T) -> R { ... }`, "
    "annotate the binding (`var f: (T) -> R = x -> ...`), or rely on the call-site signature";

// Strip underscore separators from numeric literal into dst buffer.
// Returns number of characters written (not counting NUL).
static int strip_underscores(Parser *parser, const char *src, int src_len, char *dst, int dst_size) {
    if (!xr_parser_healthy(parser)) return 0;
    int n = 0;
    for (int i = 0; xr_parser_step(parser) && (i < src_len && n < dst_size - 1); i++) {
        if (xr_parser_read_byte(parser, src + (i)) != '_')
            do {
                dst[n++] = xr_parser_read_byte(parser, src + (i));
                if (!xr_parser_healthy(parser)) return 0;
            } while (0);
    }
    do { if (!ast_work(parser->compiler_session, 1)) return 0; dst[n] = '\0'; } while (0);
    return n;
}

typedef struct XrArrowHeadLookahead {
    bool is_arrow_head;
    bool has_return_type_position;
    bool has_return_type_after_arrow;
} XrArrowHeadLookahead;

static bool xr_arrow_reserved_type_token(XrTokenType type) {
    return type >= TK_STRING && type <= TK_USIZE;
}

/* Recognise the unambiguous subset of a removed arrow return annotation. A
 * user type followed by `{` remains a legal struct-literal expression body,
 * so only reserved scalar types and function types ending in a reserved scalar
 * are diagnosed here. */
static bool xr_arrow_removed_return_type_before_block(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    XrParserStreamState saved = xr_parser_stream_save(parser);
    if (!xr_parser_healthy(parser)) return false;
    bool recognised = false;

    if (xr_arrow_reserved_type_token(parser->current.type)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_QUESTION)) {
        }
        do {
            recognised = xr_parser_check(parser, TK_LBRACE);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
    } else if (xr_parser_match(parser, TK_LPAREN)) {
        int depth = 1;
        while (xr_parser_healthy(parser) && depth > 0 && !xr_parser_check(parser, TK_EOF)) {
            if (xr_parser_check(parser, TK_LPAREN)) {
                depth++;
            } else if (xr_parser_check(parser, TK_RPAREN)) {
                depth--;
            }
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return false;
            } while (0);
        }
        if (depth == 0 && xr_parser_check(parser, TK_LBRACE)) {
            /* Unit and tuple return types are unambiguous here: a grouped
             * expression cannot be followed directly by a block. */
            recognised = true;
        } else if (depth == 0 && xr_parser_match(parser, TK_ARROW) &&
                   xr_arrow_reserved_type_token(parser->current.type)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return false;
            } while (0);
            while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_QUESTION)) {
            }
            do {
                recognised = xr_parser_check(parser, TK_LBRACE);
                if (!xr_parser_healthy(parser)) return false;
            } while (0);
        }
    }

    do {
        xr_parser_stream_restore(parser, &saved);
        if (!xr_parser_healthy(parser)) return false;
    } while (0);
    return recognised;
}

/* Inspect the contents of an already-open `(` without committing the token
 * stream. Parameter syntax is parsed later by the shared parameter parser; the
 * lookahead only disambiguates grouping from an arrow head and recognises the
 * two removed return-annotation positions. */
static XrArrowHeadLookahead xr_scan_arrow_head(Parser *parser) {
    if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
    XrArrowHeadLookahead result = {0};
    XrParserStreamState saved = xr_parser_stream_save(parser);
    if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
    int depth = 1;

    while (xr_parser_healthy(parser) && depth > 0 && !xr_parser_check(parser, TK_EOF)) {
        XrTokenType type = parser->current.type;
        if (type == TK_LPAREN) {
            depth++;
        } else if (type == TK_RPAREN) {
            depth--;
            if (depth == 0)
                break;
        }
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
        } while (0);
    }

    if (xr_parser_check(parser, TK_RPAREN))
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
        } while (0);

    if (xr_parser_check(parser, TK_COLON)) {
        int nested = 0;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
        } while (0);
        while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_EOF)) {
            XrTokenType type = parser->current.type;
            if (type == TK_ARROW && nested == 0) {
                result.has_return_type_position = true;
                break;
            }
            if (nested == 0 && (type == TK_SEMICOLON || type == TK_RBRACE))
                break;
            if (type == TK_LPAREN || type == TK_LBRACKET)
                nested++;
            else if ((type == TK_RPAREN || type == TK_RBRACKET) && nested > 0)
                nested--;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
            } while (0);
        }
    } else {
        do {
            result.is_arrow_head = xr_parser_check(parser, TK_ARROW);
            if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
        } while (0);
        if (result.is_arrow_head) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
            } while (0);
            do {
                result.has_return_type_after_arrow =
                xr_arrow_removed_return_type_before_block(parser);
                if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
            } while (0);
        }
    }

    do {
        xr_parser_stream_restore(parser, &saved);
        if (!xr_parser_healthy(parser)) return (XrArrowHeadLookahead){0};
    } while (0);
    return result;
}

/* ========== Prefix Parsing ========== */

static int char_hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static const char *parse_rune_literal_payload(Parser *parser, const char *src, size_t len, uint32_t *out_cp) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (!src || len == 0)
        return "rune literal cannot be empty";

    if (xr_parser_read_byte(parser, src + (0)) == '\\') {
        if (len < 2)
            return "unterminated rune escape";
        uint32_t cp = 0;
        if (xr_parser_read_byte(parser, src + (1)) == 'u') {
            if (len < 4 || xr_parser_read_byte(parser, src + (2)) != '{')
                return "rune unicode escape must use \\u{...}";
            size_t p = 3;
            uint32_t value = 0;
            int digits = 0;
            while (xr_parser_healthy(parser) && p < len && xr_parser_read_byte(parser, src + (p)) != '}') {
                int h = char_hex_value(xr_parser_read_byte(parser, src + (p)));
                if (!xr_parser_healthy(parser)) return NULL;
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
            if (p >= len || xr_parser_read_byte(parser, src + (p)) != '}')
                return "unterminated rune unicode escape";
            if (p + 1 != len)
                return "rune literal must contain exactly one Unicode scalar value";
            cp = value;
        } else {
            switch (xr_parser_read_byte(parser, src + (1))) {
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
    int consumed = xr_parser_utf8_decode(parser, src, len, &cp);
    if (!xr_parser_healthy(parser)) return NULL;
    if (consumed <= 0)
        return "invalid UTF-8 in rune literal";
    if ((unsigned char) xr_parser_read_byte(parser, src + (0)) >= 0x80 && consumed == 1 && cp == XR_UNICODE_INVALID)
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
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_literal: NULL parser");
    int column = parser->previous.column;
    switch (parser->previous.type) {
        case TK_LITERAL_INT: {
            ParsedIntLiteral value =
                xr_parser_integer_literal(parser, parser->previous.start, parser->previous.length);
            if (!xr_parser_healthy(parser)) return NULL;
            if (value.overflows_u64)
                do {
                    xr_parser_error_at_previous(parser, "integer literal exceeds u64 range");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            // Full i64 range allowed at parse time; range checks against
            // the target type happen later in the analyzer/compiler.
            AstNode *node = xr_ast_literal_int_bits(parser->compiler_session, value.bits,
                                                    value.overflows_i64, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;
            return node;
        }

        case TK_LITERAL_FLOAT: {
            size_t length = (size_t) parser->previous.length;
            uint64_t bits = 0;
            XrDecimalWork work = {parser->state, parser_decimal_charge, false};
            XrDecimalStatus decimal = xr_decimal_float_parse_work(
                &work, parser->previous.start, length, 64, &bits);
            if (!xr_parser_healthy(parser)) return NULL;
            if (decimal == XR_DECIMAL_WORK_LIMIT) {
                xr_compile_state_fail(parser->state, XR_COMPILE_RESOURCE_BUDGET);
                return NULL;
            }
            if (decimal != XR_DECIMAL_OK)
                do {
                    xr_parser_error_at_previous(parser, "invalid decimal floating literal");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            _Static_assert(sizeof(xr_Number) == sizeof(bits), "binary64 AST value width");
            xr_Number value;
            do { if (!ast_copy(parser->compiler_session, &value, &bits, sizeof(bits))) return NULL; } while (0);
            AstNode *node = xr_ast_literal_float(parser->compiler_session, value, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            char *text = ast_alloc(parser->compiler_session, length + 1);
            if (!xr_parser_healthy(parser)) return NULL;
            do { if (!ast_copy(parser->compiler_session, text, parser->previous.start, length)) return NULL; } while (0);
            if (!ast_work(parser->compiler_session, 1)) return NULL;
            text[length] = 0;
            node->as.literal.decimal_text = text; node->as.literal.decimal_length = length;
            node->column = column;
            return node;
        }

        case TK_LITERAL_BIGINT: {
            // Strip 'n' suffix and underscores
            int length = parser->previous.length - 1;  // Strip 'n' suffix
            char *buf = (char *) ast_alloc(parser->compiler_session, length + 1);
            if (!xr_parser_healthy(parser)) return NULL;
            strip_underscores(parser, parser->previous.start, length, buf, length + 1);
            AstNode *node =
                xr_ast_literal_bigint(parser->compiler_session, buf, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;

            return node;
        }

        case TK_LITERAL_STRING:
        case TK_RAW_STRING: {
            XrParsedQuoted payload = {0};
            const char *error = NULL;
            bool decode_escapes = parser->previous.escape_mode == XR_LITERAL_ESCAPED;
            if (xr_parser_decode_quoted(parser, &parser->previous, decode_escapes, &payload, &error) != XR_QUOTED_OK) {
                do {
                    xr_parser_error_at_previous(parser, error ? error : "invalid string literal");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            if (xr_parser_find_byte(parser, payload.bytes, '\0', payload.length) != NULL) {

                do {
                    xr_parser_error_at_previous(
                    parser, "string literals cannot contain byte escapes; use b\"...\"");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            if (!xr_parser_utf8_validate(parser, (const char *) payload.bytes, payload.length)) {

                do {
                    xr_parser_error_at_previous(parser, "string literal must be valid UTF-8");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            AstNode *node = xr_ast_literal_string(
                parser->compiler_session, (const char *) payload.bytes, payload.length,
                parser->previous.escape_mode, parser->previous.source_form, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;

            return node;
        }

        case TK_LITERAL_BYTE_STRING:
        case TK_LITERAL_C_STRING: {
            bool append_nul = parser->previous.type == TK_LITERAL_C_STRING;
            XrParsedQuoted payload = {0};
            const char *error = NULL;
            bool decode_escapes = parser->previous.escape_mode == XR_LITERAL_ESCAPED;
            if (xr_parser_decode_quoted(parser, &parser->previous, decode_escapes, &payload, &error) != XR_QUOTED_OK) {
                do {
                    xr_parser_error_at_previous(parser, error ? error : "invalid byte literal");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            if (append_nul && xr_parser_find_byte(parser, payload.bytes, '\0', payload.length) != NULL) {

                do {
                    xr_parser_error_at_previous(parser, "c literal cannot contain an interior NUL");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            AstNode *node = xr_ast_fixed_bytes_literal(
                parser->compiler_session, payload.bytes, payload.length, append_nul,
                parser->previous.escape_mode, parser->previous.source_form, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;

            return node;
        }

        case TK_LITERAL_RUNE: {
            const char *src = parser->previous.start + 1;
            size_t src_len = (size_t) parser->previous.length - 2;
            uint32_t cp = 0;
            const char *err = parse_rune_literal_payload(parser, src, src_len, &cp);
            if (!xr_parser_healthy(parser)) return NULL;
            if (err)
                do {
                    xr_parser_error_at_previous(parser, err);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            AstNode *node =
                xr_ast_literal_rune(parser->compiler_session, cp, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;
            return node;
        }

        case TK_TRUE: {
            AstNode *node = xr_ast_literal_bool(parser->compiler_session, 1, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;
            return node;
        }

        case TK_FALSE: {
            AstNode *node = xr_ast_literal_bool(parser->compiler_session, 0, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;
            return node;
        }

        case TK_NULL: {
            AstNode *node = xr_ast_literal_null(parser->compiler_session, parser->previous.line);
            if (!xr_parser_healthy(parser)) return NULL;
            node->column = column;
            return node;
        }

        default:
            do {
                xr_parser_error(parser, "unknown literal type");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
    }
}

// Regex prefix parsing (when '/' appears at expression start)
// Backtrack scanner and rescan as regex
AstNode *xr_parse_regex_prefix(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_regex_prefix: NULL parser");
    const char *slash_pos = parser->previous.start;
    parser->scanner.current = slash_pos;

    Token regex_token = xr_scanner_try_regex(&parser->scanner);
    if (!xr_parser_healthy(parser)) return NULL;

    if (regex_token.type == TK_LITERAL_REGEX) {
        parser->previous = regex_token;
        do {
            parser->current = xr_scanner_scan(&parser->scanner);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return xr_parse_regex_literal(parser);
    } else {
        do {
            xr_parser_error(parser, "invalid regex literal");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
}

// Parse regex literal: /pattern/flags
AstNode *xr_parse_regex_literal(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_regex_literal: NULL parser");
    const char *start = parser->previous.start;
    int length = parser->previous.length;

    // Skip opening '/'
    start++;
    length--;

    // Find closing '/'
    const char *end_slash = NULL;
    for (int i = length - 1; xr_parser_step(parser) && (xr_parser_healthy(parser) && i >= 0); i--) {
        if (xr_parser_read_byte(parser, start + (i)) == '/') {
            end_slash = start + i;
            break;
        }
    }

    if (!end_slash) {
        do {
            xr_parser_error(parser, "invalid regex literal format");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // Extract pattern
    int pattern_len = (int) (end_slash - start);
    char *pattern = (char *) ast_alloc(parser->compiler_session, pattern_len + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, pattern, start, pattern_len)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; pattern[pattern_len] = '\0'; } while (0);

    // Extract flags
    const char *flags_start = end_slash + 1;
    int flags_len = length - pattern_len - 1;
    char *flags = (char *) ast_alloc(parser->compiler_session, flags_len + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    if (flags_len > 0) {
        do { if (!ast_copy(parser->compiler_session, flags, flags_start, flags_len)) return NULL; } while (0);
    }
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; flags[flags_len] = '\0'; } while (0);

    // Create AST node
    AstNode *node =
        xr_ast_literal_regex(parser->compiler_session, pattern, flags, parser->previous.line);
    if (!xr_parser_healthy(parser)) return NULL;



    return node;
}

// Parse the remaining value conversion keywords: string(x), bool(x), rune(x).
AstNode *xr_parse_type_cast(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_type_cast: NULL parser");
    const char *type_name = NULL;
    switch (parser->previous.type) {
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
            do {
                xr_parser_error(parser, "expected type keyword");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
    }

    int line = parser->previous.line;

    /* Primitive type keywords also name their native type object for static
     * members (for example string.fromUtf8(...)). A following dot therefore
     * starts ordinary member access; only a following '(' is a cast. */
    if (xr_parser_check(parser, TK_DOT))
        return xr_ast_variable(parser->compiler_session, type_name, line);

    if (!xr_parser_match(parser, TK_LPAREN)) {
        do {
            xr_parser_error(parser, "expected '(' after type cast");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    AstNode *arg = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!arg) {
        return NULL;
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after type cast argument");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *callee = xr_ast_variable(parser->compiler_session, type_name, line);
    if (!xr_parser_healthy(parser)) return NULL;
    AstNode **arguments =
        (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 1);
    if (!xr_parser_healthy(parser)) return NULL;
    arguments[0] = arg;

    return xr_ast_call_expr(parser->compiler_session, callee, arguments, NULL, 1, line);
}

/* Exact scalar keywords also introduce primitive type namespaces. They are
 * ordinary resolved namespace values here; numeric conversion remains the
 * `as` operator and is never inferred from call syntax. */
AstNode *xr_parse_scalar_namespace(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_scalar_namespace: NULL parser");
    const char *name = NULL;
    switch (parser->previous.type) {
#define XR_EXACT_SCALAR(id, stable_id, source_name, native_type, family, range_class, flags)       \
    case TK_##id:                                                                                  \
        name = source_name;                                                                        \
        break;
#include "../../shared/xr_exact_scalar_registry.def"
#undef XR_EXACT_SCALAR
        default:
            do {
                xr_parser_error(parser, "expected exact scalar keyword");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
    }
    return xr_ast_variable(parser->compiler_session, name, parser->previous.line);
}

AstNode *xr_parse_comptime_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_comptime_expr: NULL parser");
    int line = parser->previous.line;
    int column = parser->previous.column;

    AstNode *expr = NULL;
    if (xr_parser_match(parser, TK_LBRACE)) {
        /* Like `unsafe { }`, a comptime block's trailing expression statement
         * is the block's value, so it is observed rather than discarded and
         * E0208 must stay quiet inside it. */
        bool saved_observed = parser->expr_value_observed;
        parser->expr_value_observed = true;
        do {
            expr = xr_parse_block(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->expr_value_observed = saved_observed;
    } else {
        do {
            expr = xr_parse_precedence(parser, PREC_TERNARY);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    if (!expr) {
        do {
            xr_parser_error_at_previous(parser, "expected expression after 'comptime'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    return xr_ast_comptime_expr(parser->compiler_session, expr, line, column);
}

// Helper: create string literal node from a template string part.
// For normal template strings, applies escape processing.
// For raw template strings, copies verbatim.
static AstNode *make_template_part(Parser *parser, const char *src, int len, bool is_raw,
                                   XrLiteralSourceForm source_form) {
    if (!xr_parser_healthy(parser)) return NULL;
    char *buf = (char *) ast_alloc(parser->compiler_session, len + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    size_t out_len;
    if (is_raw) {
        do { if (!ast_copy(parser->compiler_session, buf, src, len)) return NULL; } while (0);
        out_len = (size_t) len;
    } else {
        // Same decoder as plain string literals (xr_compile_quoted_payload_decode)
        // so escape semantics cannot drift between the two surfaces.
        const char *error = NULL;
        if (xr_compile_escaped_bytes_decode(parser->state, (const uint8_t *) src, (size_t) len, (uint8_t *) buf, &out_len,
                                     &error) != XR_QUOTED_OK) {

            do {
                xr_parser_error_at_previous(parser,
                                        error ? error : "invalid escape in template string");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        if (xr_parser_find_byte(parser, buf, '\0', out_len) != NULL) {

            do {
                xr_parser_error_at_previous(
                parser, "string literals cannot contain byte escapes; use b\"...\"");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; buf[out_len] = '\0'; } while (0);
    AstNode *node = xr_ast_literal_string(parser->compiler_session, buf, out_len,
                                          is_raw ? XR_LITERAL_RAW : XR_LITERAL_ESCAPED, source_form,
                                          parser->previous.line);
    if (!xr_parser_healthy(parser)) return NULL;

    if (node)
        node->as.literal.is_template_chunk = true;
    return node;
}

static bool template_skip_rune(Parser *parser, const char *src, int len, int *pos) {
    if (!xr_parser_healthy(parser)) return false;
    while (xr_parser_healthy(parser) && *pos < len) {
        char c = xr_parser_read_byte(parser, src + (*pos));
        if (!xr_parser_healthy(parser)) return false;
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

static bool template_skip_quoted_token(Parser *parser, const char *src, int len, int *pos) {
    if (!xr_parser_healthy(parser)) return false;
    if (!src || !pos || *pos < 0 || *pos >= len)
        return false;
    char lead = xr_parser_read_byte(parser, src + (*pos));
    if (!xr_parser_healthy(parser)) return false;
    if (lead != '"' && lead != 'r' && lead != 'b' && lead != 'c')
        return false;
    Scanner scanner;
    if (xr_compile_scanner_open(&scanner, parser->state, parser->arena, src + *pos) != XR_COMPILE_RESOURCE_OK) return false;
    Token token = xr_scanner_scan(&scanner);
    if (!xr_parser_healthy(parser)) return false;
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

static void template_skip_line_comment(Parser *parser, const char *src, int len, int *pos) {
    if (!xr_parser_healthy(parser)) return;
    while (xr_parser_healthy(parser) && *pos < len && xr_parser_read_byte(parser, src + (*pos)) != '\n') {
        (*pos)++;
    }
}

static bool template_skip_block_comment(Parser *parser, const char *src, int len, int *pos) {
    if (!xr_parser_healthy(parser)) return false;
    *pos += 2;
    int depth = 1;
    while (xr_parser_healthy(parser) && *pos < len && depth > 0) {
        if (*pos + 1 < len && xr_parser_read_byte(parser, src + (*pos)) == '/' && xr_parser_read_byte(parser, src + (*pos + 1)) == '*') {
            *pos += 2;
            depth++;
            continue;
        }
        if (*pos + 1 < len && xr_parser_read_byte(parser, src + (*pos)) == '*' && xr_parser_read_byte(parser, src + (*pos + 1)) == '/') {
            *pos += 2;
            depth--;
            continue;
        }
        (*pos)++;
    }
    return depth == 0;
}

static bool template_find_expr_end(Parser *parser, const char *src, int len, int expr_start, int *expr_end) {
    if (!xr_parser_healthy(parser)) return false;
    int brace_count = 1;
    int j = expr_start + 2;
    while (xr_parser_healthy(parser) && j < len && brace_count > 0) {
        char c = xr_parser_read_byte(parser, src + (j));
        if (!xr_parser_healthy(parser)) return false;
        if (template_skip_quoted_token(parser, src, len, &j))
            continue;
        if (c == '\'') {
            j++;
            if (!template_skip_rune(parser, src, len, &j)) {
                return false;
            }
            continue;
        }
        if (c == '/' && j + 1 < len && xr_parser_read_byte(parser, src + (j + 1)) == '/') {
            template_skip_line_comment(parser, src, len, &j);
            continue;
        }
        if (c == '/' && j + 1 < len && xr_parser_read_byte(parser, src + (j + 1)) == '*') {
            if (!template_skip_block_comment(parser, src, len, &j)) {
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
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_template_string: NULL parser");
    bool is_raw = parser->previous.escape_mode == XR_LITERAL_RAW;
    XrLiteralSourceForm source_form = parser->previous.source_form;
    XrParsedQuoted payload = {0};
    const char *decode_error = NULL;
    if (xr_parser_decode_quoted(parser, &parser->previous, false, &payload, &decode_error) != XR_QUOTED_OK) {
        do {
            xr_parser_error_at_previous(parser,
                                    decode_error ? decode_error : "invalid template string");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    const char *tmpl = (const char *) payload.bytes;
    int tmpl_len = (int) payload.length;
    AstNode **parts = NULL;
    int part_count = 0;
    int part_capacity = 4;

    do {
        parts = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                         (size_t) part_capacity);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    if (!parts) {

        do {
            xr_parser_error(parser, "memory allocation failed");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    int i = 0;
    while (xr_parser_healthy(parser) && i < tmpl_len) {
        // Find next ${ (for normal mode, \$ escapes the dollar sign)
        int expr_start = -1;
        for (int j = i; xr_parser_step(parser) && (xr_parser_healthy(parser) && j < tmpl_len - 1); j++) {
            if (!is_raw && xr_parser_read_byte(parser, tmpl + (j)) == '\\' && j + 1 < tmpl_len) {
                j++;
                continue;
            }
            if (xr_parser_read_byte(parser, tmpl + (j)) == '$' && xr_parser_read_byte(parser, tmpl + (j + 1)) == '{') {
                expr_start = j;
                break;
            }
        }

        if (expr_start == -1) {
            // No more interpolations, rest is string
            if (i < tmpl_len) {
                AstNode *str_node =
                    make_template_part(parser, tmpl + i, tmpl_len - i, is_raw, source_form);
                if (!xr_parser_healthy(parser)) return NULL;
                if (!str_node) {

                    return NULL;
                }
                do {
                    XR_PARSE_PUSH(parser, parts, part_count, part_capacity, str_node);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            break;
        }

        // Add string part before ${
        if (expr_start > i) {
            AstNode *str_node =
                make_template_part(parser, tmpl + i, expr_start - i, is_raw, source_form);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!str_node) {

                return NULL;
            }
            do {
                XR_PARSE_PUSH(parser, parts, part_count, part_capacity, str_node);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        int expr_end = -1;
        if (!template_find_expr_end(parser, tmpl, tmpl_len, expr_start, &expr_end)) {

            do {
                xr_parser_error(parser, "missing closing } in template string");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        // Parse interpolation expression
        int expr_len = expr_end - (expr_start + 2);
        if (expr_len > 0) {
            char *expr_code = (char *) ast_alloc(parser->compiler_session, expr_len + 1);
            if (!xr_parser_healthy(parser)) return NULL;
            do { if (!ast_copy(parser->compiler_session, expr_code, tmpl + expr_start + 2, expr_len)) return NULL; } while (0);
            do { if (!ast_work(parser->compiler_session, 1)) return NULL; expr_code[expr_len] = '\0'; } while (0);

            Scanner expr_scanner;
            if (xr_compile_scanner_open(&expr_scanner, parser->state, parser->arena, expr_code) != XR_COMPILE_RESOURCE_OK) return NULL;
            int expr_line = parser->previous.line + (source_form == XR_LITERAL_BLOCK ? 1 : 0);
            for (int p = 0; xr_parser_step(parser) && (xr_parser_healthy(parser) && p < expr_start + 2); p++) {
                if (xr_parser_read_byte(parser, tmpl + (p)) == '\n')
                    expr_line++;
            }
            expr_scanner.line = expr_line;
            expr_scanner.start_line = expr_line;

            Parser expr_parser;
            if (xr_compile_state_copy(parser->state, &expr_parser, parser, sizeof(expr_parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
            if (xr_compile_state_copy(parser->state, &expr_parser.scanner, &expr_scanner, sizeof(expr_scanner)) != XR_COMPILE_RESOURCE_OK) return NULL;
            expr_parser.compiler_session = parser->compiler_session;
            expr_parser.had_error = 0;
            expr_parser.panic_mode = 0;
            expr_parser.error_count = 0;
            expr_parser.bracket_bits = 0;
            expr_parser.bracket_depth = 0;
            if (xr_compile_state_zero(parser->state, &expr_parser.current, sizeof(Token)) != XR_COMPILE_RESOURCE_OK ||
                xr_compile_state_zero(parser->state, &expr_parser.previous, sizeof(Token)) != XR_COMPILE_RESOURCE_OK) return NULL;

            do {
                xr_parser_advance(&expr_parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            AstNode *expr_node = xr_parse_expression(&expr_parser);
            if (!xr_parser_healthy(parser)) return NULL;

            if (expr_parser.had_error || !xr_parser_check(&expr_parser, TK_EOF)) {
                expr_node = NULL;
                do {
                    xr_parser_error(parser, "invalid expression in template string");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }


            if (!expr_node) {

                return NULL;
            }
            do {
                XR_PARSE_PUSH(parser, parts, part_count, part_capacity, expr_node);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        i = expr_end + 1;  // Skip }
    }

    if (part_count == 0) {

        return xr_ast_literal_string(parser->compiler_session, "", 0,
                                     is_raw ? XR_LITERAL_RAW : XR_LITERAL_ESCAPED, source_form,
                                     parser->previous.line);
    }

    AstNode *node = xr_ast_template_string(parser->compiler_session, parts, part_count,
                                           is_raw ? XR_LITERAL_RAW : XR_LITERAL_ESCAPED,
                                           source_form, parser->previous.line);
    if (!xr_parser_healthy(parser)) return NULL;

    return node;
}

// Parse grouping expression: (expression)
AstNode *xr_parse_grouping(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_grouping: NULL parser");
    int line = parser->previous.line;

    // Case 1: `() -> expr` no-param arrow function, or `()` unit literal.
    // Arrow lambdas never have a return-type position.
    if (xr_parser_check(parser, TK_RPAREN)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (xr_parser_check(parser, TK_COLON)) {
            do {
                xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        if (xr_parser_match(parser, TK_ARROW)) {
            if (xr_arrow_removed_return_type_before_block(parser)) {
                do {
                    xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            return xr_parse_arrow_function_body(parser, NULL, 0, line);
        }
        /* `()` is the unit literal — the unique value of the unit type
         * `()`. Constant-folded to a singleton at lower time. */
        return xr_ast_tuple_literal(parser->compiler_session, NULL, 0, line);
    }

    // Case 2: arrow-function head — `(...) -> body`.
    //
    // The lookahead also recognises the two removed annotation positions so
    // they cannot fall through to unrelated grouping/type-cast diagnostics.
    XrArrowHeadLookahead arrow = xr_scan_arrow_head(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (arrow.has_return_type_position) {
        do {
            xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    if (arrow.has_return_type_after_arrow) {
        do {
            xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    if (arrow.is_arrow_head) {
        // Collect params as XrParamNode. The array lives in the parse
        // arena because it is shallow-copied into the function_expr node.
        XrParamNode **params = NULL;
        int param_count = 0;
        int param_capacity = 0;

        if (xr_parser_check(parser, TK_DOT_DOT_DOT)) {
            do {
                xr_parser_error(parser,
                            "arrow lambda parameters cannot be rest parameters; use a named "
                            "function declaration");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        XrParamNode *first_param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
        if (!xr_parser_healthy(parser)) return NULL;
        do {
            XR_PARSE_PUSH(parser, params, param_count, param_capacity, first_param);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (xr_parser_check(parser, TK_ASSIGN)) {
            do {
                xr_parser_error(parser,
                            "arrow lambda parameters cannot have default values; use a named "
                            "function declaration or supply the argument explicitly");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA)) {
            if (xr_parser_check(parser, TK_RPAREN))
                break;
            if (xr_parser_check(parser, TK_DOT_DOT_DOT)) {
                do {
                    xr_parser_error(parser,
                                "arrow lambda parameters cannot be rest parameters; use a named "
                                "function declaration");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                XR_PARSE_PUSH(parser, params, param_count, param_capacity, param);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_ASSIGN)) {
                do {
                    xr_parser_error(parser,
                                "arrow lambda parameters cannot have default values; use a named "
                                "function declaration or supply the argument explicitly");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }

        if (!xr_parser_match(parser, TK_RPAREN)) {
            do {
                xr_parser_error(parser, "expected ')' after arrow lambda parameters");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        if (xr_parser_check(parser, TK_COLON)) {
            do {
                xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        if (!xr_parser_match(parser, TK_ARROW)) {
            do {
                xr_parser_error(parser, "expected '->' after arrow lambda parameters");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        if (xr_arrow_removed_return_type_before_block(parser)) {
            do {
                xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
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
        if (!xr_parser_healthy(parser)) return NULL;
        if (!inner)
            return NULL;
        do {
            first = xr_ast_spread_expr(parser->compiler_session, inner, first_line);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else {
        do {
            first = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    if (!xr_parser_check(parser, TK_COMMA)) {
        if (first && first->type == AST_SPREAD_EXPR) {
            do {
                xr_parser_error(parser,
                            "spread '...' is only valid inside a tuple literal of arity >= 1; "
                            "wrap with a trailing comma to form a tuple");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        do {
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to close grouping");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return xr_ast_grouping(parser->compiler_session, first, line);
    }

    AstNode **elems = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 16);
    if (!xr_parser_healthy(parser)) return NULL;
    int count = 0;
    int cap = 16;
    elems[count++] = first;
    while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA)) {
        // Trailing comma is allowed and required for unary tuple `(x,)`.
        if (xr_parser_check(parser, TK_RPAREN))
            break;
        if (count >= cap) {
            int new_cap = xr_parser_grow_capacity(parser, cap);
            if (!xr_parser_healthy(parser)) return NULL;
            AstNode **resized = (AstNode **) ast_alloc_array(parser->compiler_session,
                                                             sizeof(AstNode *), (size_t) new_cap);
            if (!xr_parser_healthy(parser)) return NULL;
            for (int i = 0; xr_parser_step(parser) && (i < count); i++)
                resized[i] = elems[i];
            elems = resized;
            cap = new_cap;
        }
        int elem_line = parser->current.line;
        if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
            AstNode *inner = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!inner)
                return NULL;
            do {
                elems[count++] = xr_ast_spread_expr(parser->compiler_session, inner, elem_line);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            do {
                elems[count++] = xr_parse_expression(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }
    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close tuple literal");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_ast_tuple_literal(parser->compiler_session, elems, count, line);
}

// Parse a bare single-parameter lambda after Pratt consumed `->`.
// Giving `->` assignment-level precedence lets arrow-delimited constructs
// such as `select { value from channel -> ... }` stop at the delimiter by
// parsing their head with PREC_CALL, while ordinary expressions accept the
// same `parameter -> body` form in any position.
AstNode *xr_parse_bare_lambda(Parser *parser, AstNode *parameter) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (!parameter || parameter->type != AST_VARIABLE) {
        do {
            xr_parser_error_at_previous(
            parser,
            "unparenthesized arrow lambda parameter must be one identifier; use `(params) -> body`");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    XrParamNode **params =
        (XrParamNode **) ast_alloc_array(parser->compiler_session, sizeof(XrParamNode *), 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        params[0] = xr_param_node_new(parser->compiler_session, parameter->as.variable.name,
                                  parameter->line, parameter->column);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    if (xr_arrow_removed_return_type_before_block(parser)) {
        do {
            xr_parser_error(parser, XR_ARROW_RETURN_TYPE_DIAGNOSTIC);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    return xr_parse_arrow_function_body(parser, params, 1, parameter->line);
}

// Parse arrow function body
// Supports: -> expr (auto return) or -> { ... } (block)
AstNode *xr_parse_arrow_function_body(Parser *parser, XrParamNode **params, int param_count,
                                      int line) {
    if (!xr_parser_healthy(parser)) return NULL;
    AstNode *body;

    parser->scope_depth++;
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Block body: -> { ... }
        do {
            body = xr_parse_block(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else {
        // Expression body: -> expr (auto-wrap in return)
        AstNode *expr = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!expr) {
            parser->scope_depth--;
            return NULL;
        }

        // return_stmt shallow-copies values into the AST node; must be arena.
        AstNode **values =
            (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 1);
        if (!xr_parser_healthy(parser)) return NULL;
        values[0] = expr;
        AstNode *return_stmt = xr_ast_return_stmt(parser->compiler_session, values, 1, expr->line);
        if (!xr_parser_healthy(parser)) return NULL;

        do {
            body = xr_ast_block(parser->compiler_session, line);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        xr_ast_block_add(parser->compiler_session, body, return_stmt);
    }

    parser->scope_depth--;

    // params ownership transferred to func_expr
    return xr_ast_function_expr(parser->compiler_session, params, param_count, body, line);
}

// Parse fn anonymous function expression
// Syntax: fn() { ... } or fn(a, b) { return a + b }
AstNode *xr_parse_fn_expression(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_fn_expression: NULL parser");
    int line = parser->previous.line;

    XrTypeScope *saved_scope = parser->type_scope;
    int type_param_count = 0;
    XrGenericParam **type_params = xr_parse_generic_params(parser, &type_param_count);
    if (!xr_parser_healthy(parser)) return NULL;
    XrTypeScope *generic_scope = type_param_count > 0 ? parser->type_scope : NULL;

    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after fn");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrParamNode *param = xr_parse_parameter(parser, XR_PARSE_PARAMETER_ALLOW_MODE);
            if (!xr_parser_healthy(parser)) return NULL;

            do {
                XR_PARSE_PUSH(parser, params, param_count, param_capacity, param);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after parameter list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse optional return type annotation: `fn(...) -> T { ... }`.
    // The unified arrow `->` is the only legal separator.
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else if (xr_parser_check(parser, TK_COLON)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // consume ':'
        do {
            xr_parser_error(parser, "use '->' instead of ':' for function return type, "
                                "e.g. fn(p: T) -> R");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->panic_mode = 0;
        do {
            return_type = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Parse function body (must be block)
    do {
        xr_parser_consume(parser, TK_LBRACE, "fn function body must use braces { }");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    parser->scope_depth++;
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    parser->scope_depth--;

    AstNode *func_expr =
        xr_ast_function_expr(parser->compiler_session, params, param_count, body, line);
    if (!xr_parser_healthy(parser)) return NULL;
    func_expr->as.function_expr.return_type = return_type;
    func_expr->as.function_expr.type_params = type_params;
    func_expr->as.function_expr.type_param_count = type_param_count;

    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
        do {
            xr_parser_type_scope_free(parser, generic_scope);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    return func_expr;
}

// Parse unary operators: -expr, !expr, ~expr
AstNode *xr_parse_unary(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_unary: NULL parser");
    XrTokenType operator_type = parser->previous.type;
    int line = parser->previous.line;

    AstNode *operand = xr_parse_precedence(parser, PREC_UNARY);
    if (!xr_parser_healthy(parser)) return NULL;
    switch (operator_type) {
        case TK_MINUS:
            return xr_ast_unary(parser->compiler_session, AST_UNARY_NEG, operand, line);
        case TK_NOT:
            return xr_ast_unary(parser->compiler_session, AST_UNARY_NOT, operand, line);
        case TK_TILDE:
            return xr_ast_unary(parser->compiler_session, AST_UNARY_BNOT, operand, line);
        default:
            do {
                xr_parser_error(parser, "unknown unary operator");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
    }
}

/* ========== Infix Parsing ========== */

static void xr_parser_recover_struct_literal_field(Parser *parser) {
    if (!xr_parser_healthy(parser)) return;
    if (!parser)
        return;
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_EOF)) {
        XrTokenType type = parser->current.type;
        if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0 &&
            (type == TK_COMMA || type == TK_RBRACE))
            break;
        if (type == TK_LPAREN)
            paren_depth++;
        else if (type == TK_RPAREN && paren_depth > 0)
            paren_depth--;
        else if (type == TK_LBRACKET)
            bracket_depth++;
        else if (type == TK_RBRACKET && bracket_depth > 0)
            bracket_depth--;
        else if (type == TK_LBRACE)
            brace_depth++;
        else if (type == TK_RBRACE && brace_depth > 0)
            brace_depth--;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
    }
    parser->panic_mode = 0;
}

AstNode *xr_parse_struct_literal_after_type(Parser *parser, AstNode *type_path,
                                            XrTypeRef **type_args, int type_arg_count) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_struct_literal_after_type: NULL parser");
    XR_DCHECK(type_path != NULL, "parse_struct_literal_after_type: NULL type path");
    XR_DCHECK(type_arg_count >= 0, "parse_struct_literal_after_type: negative type argument count");
    if (!parser || !type_path || type_arg_count < 0 || !xr_parser_match(parser, TK_LBRACE))
        return NULL;

    char **field_names = NULL;
    AstNode **field_values = NULL;
    int field_count = 0;
    int field_capacity = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (field_count >= field_capacity) {
            int old_capacity = field_capacity;
            do {
                field_capacity = xr_parser_grow_capacity(parser, field_capacity);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            char **grown_names = (char **) ast_alloc_array(parser->compiler_session, sizeof(char *),
                                                           (size_t) field_capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            AstNode **grown_values = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) field_capacity);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!grown_names || !grown_values)
                return NULL;
            if (old_capacity > 0) {
                do { if (!ast_copy(parser->compiler_session, grown_names, field_names, sizeof(char *) * (size_t) old_capacity)) return NULL; } while (0);
                do { if (!ast_copy(parser->compiler_session, grown_values, field_values, sizeof(AstNode *) * (size_t) old_capacity)) return NULL; } while (0);
            }
            field_names = grown_names;
            field_values = grown_values;
        }

        if (!xr_parser_check(parser, TK_NAME)) {
            do {
                xr_parser_error(parser, "expected field name in struct literal");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_recover_struct_literal_field(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_match(parser, TK_COMMA))
                continue;
            break;
        }
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        char *field_name =
            (char *) ast_alloc(parser->compiler_session, (size_t) parser->previous.length + 1u);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!field_name)
            return NULL;
        do { if (!ast_copy(parser->compiler_session, field_name, parser->previous.start, (size_t) parser->previous.length)) return NULL; } while (0);
        do { if (!ast_work(parser->compiler_session, 1)) return NULL; field_name[parser->previous.length] = '\0'; } while (0);
        field_names[field_count] = field_name;
        if (!xr_parser_match(parser, TK_COLON)) {
            do {
                xr_parser_error(parser, "expected ':' after field name");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_recover_struct_literal_field(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_match(parser, TK_COMMA))
                continue;
            break;
        }
        AstNode *field_value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!field_value) {
            do {
                xr_parser_recover_struct_literal_field(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_match(parser, TK_COMMA))
                continue;
            break;
        }
        field_values[field_count] = field_value;
        field_count++;

        if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_match(parser, TK_COMMA)) {
            do {
                xr_parser_error(parser, "expected ',' or '}' in struct literal");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                xr_parser_recover_struct_literal_field(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_match(parser, TK_COMMA))
                continue;
            break;
        }
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to end struct literal");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *node = xr_ast_struct_literal(parser->compiler_session, type_path, field_names,
                                          field_values, field_count, type_path->line);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!node)
        return NULL;
    node->column = type_path->column;
    node->end_line = parser->previous.line;
    node->end_column = parser->previous.column + parser->previous.length;
    if (type_arg_count > 0) {
        XrTypeRef **copy = (XrTypeRef **) ast_alloc_array(
            parser->compiler_session, sizeof(XrTypeRef *), (size_t) type_arg_count);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!copy)
            return NULL;
        do { if (!ast_copy(parser->compiler_session, copy, type_args, sizeof(XrTypeRef *) * (size_t) type_arg_count)) return NULL; } while (0);
        node->as.struct_literal.type_args = copy;
        node->as.struct_literal.type_arg_count = type_arg_count;
    }
    return node;
}

static bool generic_reference_boundary(const Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    if (parser->current.line > parser->previous.line) return true;
    switch (parser->current.type) {
    case TK_COMMA: case TK_SEMICOLON: case TK_RPAREN: case TK_RBRACKET:
    case TK_RBRACE: case TK_COLON: case TK_EOF: case TK_DOT: return true;
    default: return false;
    }
}

// Try to parse generic invocation, function reference or aggregate construction.
// Returns NULL when '<' is a comparison rather than a generic suffix.
AstNode *xr_parse_try_generic_call_after_lt(Parser *parser, AstNode *callee) {
    if (!xr_parser_healthy(parser)) return NULL;
    // Only try if callee is an identifier or member access
    if (callee->type != AST_VARIABLE && callee->type != AST_MEMBER_ACCESS) {
        return NULL;
    }

    int line = parser->previous.line;
    Parser checkpoint;
    if (xr_compile_state_copy(parser->state, &checkpoint, parser, sizeof(checkpoint)) != XR_COMPILE_RESOURCE_OK) return NULL;
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
        if (!xr_parser_healthy(parser)) return NULL;
        if (parser->error_count > saved_error_count) {
            if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
        if (!type) {
            // Not valid type args, restore and return NULL
            if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
        type_args[type_arg_count++] = type;

    } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));

    // Must have '>' followed by '('
    if (!xr_parser_match(parser, TK_GT)) {
        // Handle '>>' case
        if (parser->current.type == TK_RSHIFT) {
            parser->current.type = TK_GT;
            parser->current.start++;
            parser->current.length = 1;
        } else {
            if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
    }

    if (xr_parser_check(parser, TK_LBRACE)) {
        if (callee->type != AST_MEMBER_ACCESS) {
            if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
        parser->panic_mode = saved_panic_mode;
        return xr_parse_struct_literal_after_type(parser, callee, type_args, type_arg_count);
    }

    if (!xr_parser_check(parser, TK_LPAREN) && generic_reference_boundary(parser)) {
        parser->panic_mode = saved_panic_mode;
        return xr_ast_function_ref(parser->compiler_session, callee, type_args, type_arg_count, line);
    }
    if (!xr_parser_check(parser, TK_LPAREN)) {
        if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
        parser->panic_mode = saved_panic_mode;
        return NULL;
    }

    // Restore panic_mode now that we confirmed it's a valid generic call
    parser->panic_mode = saved_panic_mode;

    // Parse the function call
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);  // consume '('

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
            if (!xr_parser_healthy(parser)) return NULL;
            do {
                XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, arg);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                XR_PARSE_PUSH(parser, arg_accesses, access_count, access_capacity, access);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // `Map<K,V>()` / `Array<T>()` / `Channel<T>(n)` etc. construct built-in
    // heap types directly (no `new`); route to the construction node so the
    // generic type arguments drive element/key/value layout.
    if (callee->type == AST_VARIABLE && xr_is_construct_only_type_name(parser, callee->as.variable.name)) {
        return xr_ast_new_expr(parser->compiler_session, NULL, callee->as.variable.name, arguments,
                               arg_accesses, arg_count, type_args, type_arg_count, line);
    }

    return xr_ast_call_expr_generic(parser->compiler_session, callee, arguments, arg_accesses,
                                    arg_count, type_args, type_arg_count, line);
}

// Parse '<' which could be comparison or generic call
// Uses space sensitivity: foo<T>() is generic, foo < T is comparison
AstNode *xr_parse_lt_or_generic(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    // If '<' has leading space, treat as comparison
    // e.g., "a < b" is comparison, "a<b>" could be generic
    if (parser->previous.has_leading_space) {
        // Fall back to comparison
        int line = parser->previous.line;
        const ParseRule *rule = xr_get_rule(TK_LT);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
        if (!xr_parser_healthy(parser)) return NULL;
        return xr_ast_binary(parser->compiler_session, AST_BINARY_LT, left, right, line);
    }

    // Try generic call first (no space before '<')
    int saved_error_count = parser->error_count;
    AstNode *generic_call = xr_parse_try_generic_call_after_lt(parser, left);
    if (!xr_parser_healthy(parser)) return NULL;
    if (generic_call) {
        return generic_call;
    }
    if (parser->error_count > saved_error_count) {
        return left;
    }

    // Fall back to comparison
    int line = parser->previous.line;
    const ParseRule *rule = xr_get_rule(TK_LT);
    if (!xr_parser_healthy(parser)) return NULL;
    AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
    if (!xr_parser_healthy(parser)) return NULL;
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
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_binary: NULL parser");
    XrTokenType operator_type = parser->previous.type;
    int line = parser->previous.line;
    /* The operator token is the node's own source coordinate. Two operators on
     * one line therefore get distinct positions, which is what keeps a
     * diagnostic pointed at the operator and what separates the identities of
     * `a + b` and `a + b + c` once an overloaded operator becomes a call. */
    int column = parser->previous.column;

    const ParseRule *rule = xr_get_rule(operator_type);
    if (!xr_parser_healthy(parser)) return NULL;

    // Parse right operand (left-associative: precedence + 1)
    AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
    if (!xr_parser_healthy(parser)) return NULL;

    AstNodeType ast_type = 0;
    if (operator_type >= 0 &&
        operator_type < (XrTokenType) (sizeof(binary_op_map) / sizeof(binary_op_map[0]))) {
        ast_type = binary_op_map[operator_type];
    }
    if (ast_type == 0) {
        do {
            xr_parser_error(parser, "unknown binary operator");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *node = xr_ast_binary(parser->compiler_session, ast_type, left, right, line);
    if (!xr_parser_healthy(parser)) return NULL;
    if (node) {
        node->column = column;
        /* The binary node starts at the operator token (its stable semantic
         * identity) and ends at the last token consumed by its right-hand
         * operand.  parser->previous is the exact consumed token boundary, so
         * this remains complete even when a leaf factory has no end range. */
        if (parser->previous.line > 0 && parser->previous.column > 0 &&
            parser->previous.length > 0) {
            node->end_line = parser->previous.line;
            node->end_column = parser->previous.column + parser->previous.length;
        }
    }
    return node;
}

// Parse 'is' expression: expr is Type
AstNode *xr_parse_is(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;

    XrTypeRef *type = xr_parse_type_annotation(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!type) {
        do {
            xr_parser_error(parser, "expected type after 'is'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_is_expr(parser->compiler_session, left, type, line);
}

// Parse ternary expression: condition ? trueValue : falseValue
AstNode *xr_parse_ternary(Parser *parser, AstNode *condition) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_ternary: NULL parser");
    int line = parser->previous.line;

    AstNode *true_expr = xr_parse_precedence(parser, PREC_TERNARY + 1);
    if (!xr_parser_healthy(parser)) return NULL;

    do {
        xr_parser_consume(parser, TK_COLON, "expected ':' in ternary expression");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *false_expr = xr_parse_precedence(parser, PREC_TERNARY);
    if (!xr_parser_healthy(parser)) return NULL;

    return xr_ast_ternary(parser->compiler_session, condition, true_expr, false_expr, line);
}

// Parse nullish coalescing: value ?? defaultValue
AstNode *xr_parse_nullish_coalesce(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_nullish_coalesce: NULL parser");
    int line = parser->previous.line;

    AstNode *right = xr_parse_precedence(parser, PREC_NULLISH_COALESCE + 1);
    if (!xr_parser_healthy(parser)) return NULL;

    return xr_ast_binary(parser->compiler_session, AST_NULLISH_COALESCE, left, right, line);
}

// Parse force unwrap: expr! (panics at runtime if value is null)
AstNode *xr_parse_force_unwrap(Parser *parser, AstNode *operand) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_force_unwrap: NULL parser");
    int line = parser->previous.line;
    return xr_ast_unary(parser->compiler_session, AST_FORCE_UNWRAP, operand, line);
}

// Parse as cast: expr as Type / expr as Type?
AstNode *xr_parse_as_cast(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_as_cast: NULL parser");
    int line = parser->previous.line;
    XrTypeRef *target_type = xr_parse_type_annotation(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!target_type) {
        do {
            xr_parser_error(parser, "expected type after 'as'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return left;
    }
    // Check for safe cast: as Type? (XrTypeRef uses XR_TREF_OPTIONAL kind)
    bool is_safe = xr_tref_is_nullable(target_type);
    if (!xr_parser_healthy(parser)) return NULL;
    return xr_ast_as_expr(parser->compiler_session, left, target_type, is_safe, line);
}

// Parse optional chain: obj?.prop, obj?.method(), func?.()
AstNode *xr_parse_optional_chain(Parser *parser, AstNode *object) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_optional_chain: NULL parser");
    int line = parser->previous.line;

    if (parser->current.type == TK_LPAREN) {
        return xr_ast_optional_chain(parser->compiler_session, object, NULL, NULL, 3, line);
    }

    if (parser->current.type == TK_NAME) {
        // Property access: obj?.prop
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        const char *name = parser->previous.start;
        int name_len = parser->previous.length;
        char *name_str = (char *) ast_alloc(parser->compiler_session, name_len + 1);
        if (!xr_parser_healthy(parser)) return NULL;
        do { if (!ast_copy(parser->compiler_session, name_str, name, name_len)) return NULL; } while (0);
        do { if (!ast_work(parser->compiler_session, 1)) return NULL; name_str[name_len] = '\0'; } while (0);

        // Check for method call
        if (parser->current.type == TK_LPAREN) {
            return xr_ast_optional_chain(parser->compiler_session, object, name_str, NULL, 2, line);
        }

        return xr_ast_optional_chain(parser->compiler_session, object, name_str, NULL, 0, line);
    } else {
        do {
            xr_parser_error(parser, "expected property name after '?.'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
}

// Parse optional index access: obj?[index]
AstNode *xr_parse_optional_index(Parser *parser, AstNode *object) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_optional_index: NULL parser");
    int line = parser->previous.line;

    // '[' already consumed by lexer as part of '?[' token
    AstNode *index = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_consume(parser, TK_RBRACKET, "expected ']' after optional index expression");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return xr_ast_optional_chain(parser->compiler_session, object, NULL, index, 1, line);
}

// Parse range expression: start..end / start..=end
//
// `..` binds looser than every arithmetic operator, so endpoints such as
// `0..n+1` or `1..len(a)*2` group as `0..(n+1)` / `1..(len(a)*2)`. It binds
// tighter than comparison and logical operators, so a range is produced as a
// whole value before it is compared or tested. Parsing the endpoint at
// PREC_RANGE + 1 stops before a second `..`, which makes the operator
// non-associative: `a..b..c` is a syntax error rather than a silent `(a..b)..c`.
AstNode *xr_parse_range(Parser *parser, AstNode *start) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_range: NULL parser");
    int line = parser->previous.line;
    bool inclusive_end = parser->previous.type == TK_RANGE_INCLUSIVE;

    AstNode *end = xr_parse_precedence(parser, PREC_RANGE + 1);
    if (!xr_parser_healthy(parser)) return NULL;

    if (xr_parser_check(parser, TK_RANGE) || xr_parser_check(parser, TK_RANGE_INCLUSIVE)) {
        do {
            xr_parser_error(parser,
                        "range operator cannot be chained; wrap an endpoint in parentheses if a "
                        "nested range is intended");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    return xr_ast_range(parser->compiler_session, start, end, inclusive_end, line);
}
