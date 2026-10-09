/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse.c - Pratt parser implementation
 *
 * KEY CONCEPT:
 *   Converts token stream to AST using operator precedence parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xparse_internal.h"
#include "../../base/xarena.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xtype_scope.h"
#include "xstring_pool.h"
#include "../xdiag_fmt.h"
#include "../../runtime/xerror_codes.h"
#include "../../toolchain/xcompiler_session.h"

/* ========== Forward Declarations ========== */

// Declarations now in xparse_internal.h; definitions in xparse_expr.c and xparse_decl.c

/* ========== Parse Rules Table ========== */

// Parse rules table: defines parsing rules for each token type
// This is the core data structure of the Pratt parser
static ParseRule rules[] = {
    // Token type              prefix fn        infix fn        precedence
    [TK_LPAREN] = {xr_parse_grouping, xr_parse_call_expr, PREC_CALL},
    [TK_RPAREN] = {NULL, NULL, PREC_NONE},
    [TK_LBRACE] = {xr_parse_object_literal, NULL, PREC_NONE},  // Object literal
    [TK_RBRACE] = {NULL, NULL, PREC_NONE},
    [TK_LBRACKET] = {xr_parse_array_literal, xr_parse_index_access, PREC_CALL},
    [TK_RBRACKET] = {NULL, NULL, PREC_NONE},
    [TK_COMMA] = {NULL, NULL, PREC_NONE},
    [TK_DOT] = {NULL, xr_parse_member_access, PREC_CALL},
    [TK_RANGE] = {NULL, xr_parse_range, PREC_RANGE},            // .. (range operator)
    [TK_RANGE_INCLUSIVE] = {NULL, xr_parse_range, PREC_RANGE},  // ..= (inclusive range)
    [TK_COLON] = {NULL, NULL, PREC_NONE},
    [TK_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TK_ARROW] = {NULL, xr_parse_bare_lambda, PREC_ASSIGNMENT},

    // Arithmetic operators
    [TK_PLUS] = {NULL, xr_parse_binary, PREC_TERM},
    [TK_MINUS] = {xr_parse_unary, xr_parse_binary, PREC_TERM},
    [TK_STAR] = {NULL, xr_parse_binary, PREC_FACTOR},
    [TK_SLASH] = {xr_parse_regex_prefix, xr_parse_binary, PREC_FACTOR},
    [TK_PERCENT] = {NULL, xr_parse_binary, PREC_FACTOR},
    [TK_HASH] = {NULL, NULL, PREC_NONE},  // # (standalone, reserved)

    // Bitwise operators
    [TK_AMP] = {NULL, xr_parse_binary, PREC_BIT_AND},
    [TK_PIPE] = {NULL, xr_parse_binary, PREC_BIT_OR},
    [TK_CARET] = {NULL, xr_parse_binary, PREC_BIT_XOR},
    [TK_TILDE] = {xr_parse_unary, NULL, PREC_NONE},

    // Shift operators
    [TK_LSHIFT] = {NULL, xr_parse_binary, PREC_SHIFT},
    [TK_RSHIFT] = {NULL, xr_parse_binary, PREC_SHIFT},

    // New syntax tokens
    [TK_EMPTY_MAP_START] = {xr_parse_empty_map_literal, NULL, PREC_NONE},  // #{ - empty Map
    [TK_SET_START] = {xr_parse_set_literal_new, NULL, PREC_NONE},          // #[ - Set literal

    // Comparison operators
    [TK_EQ] = {NULL, xr_parse_binary, PREC_EQUALITY},
    [TK_NE] = {NULL, xr_parse_binary, PREC_EQUALITY},
    [TK_LT] = {NULL, xr_parse_lt_or_generic, PREC_COMPARISON},
    [TK_LE] = {NULL, xr_parse_binary, PREC_COMPARISON},
    [TK_GT] = {NULL, xr_parse_binary, PREC_COMPARISON},
    [TK_GE] = {NULL, xr_parse_binary, PREC_COMPARISON},
    [TK_IS] = {NULL, xr_parse_is, PREC_COMPARISON},
    [TK_AS] = {NULL, xr_parse_as_cast, PREC_COMPARISON},

    // Logical operators
    [TK_AND] = {NULL, xr_parse_binary, PREC_AND},
    [TK_OR] = {NULL, xr_parse_binary, PREC_OR},
    [TK_NOT] = {xr_parse_unary, xr_parse_force_unwrap, PREC_POSTFIX},

    // Increment/Decrement
    [TK_INC] = {xr_parse_inc_dec, xr_parse_postfix_inc_dec, PREC_POSTFIX},
    [TK_DEC] = {xr_parse_inc_dec, xr_parse_postfix_inc_dec, PREC_POSTFIX},

    // Ternary and nullish coalescing
    [TK_QUESTION] = {NULL, xr_parse_ternary, PREC_TERNARY},
    [TK_NULLISH_COALESCE] = {NULL, xr_parse_nullish_coalesce, PREC_NULLISH_COALESCE},
    [TK_QUESTION_DOT] = {NULL, xr_parse_optional_chain, PREC_CALL},
    [TK_QUESTION_LBRACKET] = {NULL, xr_parse_optional_index, PREC_CALL},

    // Assignment
    [TK_ASSIGN] = {NULL, xr_parse_assignment, PREC_ASSIGNMENT},
    [TK_PLUS_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_MINUS_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_MUL_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_DIV_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_MOD_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_AND_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_OR_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_XOR_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_LSHIFT_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},
    [TK_RSHIFT_ASSIGN] = {NULL, xr_parse_compound_assignment, PREC_ASSIGNMENT},

    // Keywords
    [TK_VAR] = {NULL, NULL, PREC_NONE},
    [TK_CONST] = {NULL, NULL, PREC_NONE},
    [TK_COMPTIME] = {xr_parse_comptime_expr, NULL, PREC_NONE},
    [TK_IF] = {NULL, NULL, PREC_NONE},
    [TK_ELSE] = {NULL, NULL, PREC_NONE},
    [TK_WHILE] = {NULL, NULL, PREC_NONE},
    [TK_FOR] = {NULL, NULL, PREC_NONE},
    [TK_RETURN] = {NULL, NULL, PREC_NONE},
    [TK_YIELD] = {NULL, NULL, PREC_NONE},  // yield: parsed only as statement
    [TK_NULL] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_TRUE] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_FALSE] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_CLASS] = {NULL, NULL, PREC_NONE},
    [TK_EXTENDS] = {NULL, NULL, PREC_NONE},
    [TK_FN] = {xr_parse_fn_expression, NULL, PREC_NONE},
    [TK_NEW] = {xr_parse_new_expression, NULL, PREC_NONE},
    [TK_THIS] = {xr_parse_this_expression, NULL, PREC_NONE},
    [TK_SUPER] = {xr_parse_super_expression, NULL, PREC_NONE},
    [TK_CONSTRUCTOR] = {NULL, NULL, PREC_NONE},
    [TK_STATIC] = {NULL, NULL, PREC_NONE},
    [TK_PRIVATE] = {NULL, NULL, PREC_NONE},
    [TK_MATCH] = {xr_parse_match_expr, NULL, PREC_NONE},  // match expression
    [TK_TRY] = {NULL, NULL, PREC_NONE},
    [TK_CATCH] = {NULL, NULL, PREC_NONE},
    [TK_UNDERSCORE] = {NULL, NULL, PREC_NONE},  // _ wildcard (pattern only)

    // Coroutine keywords
    [TK_GO] = {xr_parse_go_expr, NULL, PREC_NONE},          // go expression
    [TK_AWAIT] = {xr_parse_await_expr, NULL, PREC_NONE},    // await expression
    [TK_SELECT] = {NULL, NULL, PREC_NONE},                  // select statement
    [TK_DEFER] = {NULL, NULL, PREC_NONE},                   // defer statement
    [TK_SCOPE] = {NULL, NULL, PREC_NONE},                   // scope block
    [TK_UNSAFE] = {xr_parse_unsafe_expr, NULL, PREC_NONE},  // unsafe { expr }
    // cancelled(), move, and Channel(...) are all contextual keywords
    // handled in xr_parse_variable — they reach the parser as plain
    // TK_NAME tokens (the lexer no longer special-cases them).

    // Literals and identifiers
    [TK_LITERAL_INT] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_FLOAT] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_BIGINT] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_STRING] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_BYTE_STRING] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_C_STRING] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_RUNE] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_REGEX] = {xr_parse_regex_literal, NULL, PREC_NONE},
    [TK_TEMPLATE_STRING] = {xr_parse_template_string, NULL, PREC_NONE},
    [TK_RAW_STRING] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_RAW_TEMPLATE_STRING] = {xr_parse_template_string, NULL, PREC_NONE},
    [TK_NAME] = {xr_parse_variable, NULL, PREC_NONE},

// Exact scalar type namespaces. Numeric conversion is expressed with `as`.
#define XR_EXACT_SCALAR(id, stable_id, source_name, native_type, family, range_class, flags)       \
    [TK_##id] = {xr_parse_scalar_namespace, NULL, PREC_NONE},
#include "../../shared/xr_exact_scalar_registry.def"
#undef XR_EXACT_SCALAR

    // Value conversion keywords
    [TK_STRING] = {xr_parse_type_cast, NULL, PREC_NONE},
    [TK_BOOL] = {xr_parse_type_cast, NULL, PREC_NONE},
    [TK_RUNE] = {xr_parse_type_cast, NULL, PREC_NONE},

    // Container constructors. Array / Map / Set are no longer keywords;
    // a call like `Array(1, 2, 3)` reaches xr_compile_call_builtin via
    // the regular call_expr(variable("Array"), ...) path, which is
    // semantically identical to the legacy xr_parse_container_constructor
    // shortcut. Channel keeps its keyword because the parser folds
    // Channel(...) into a dedicated AST_CHANNEL_NEW node.

    // Special
    [TK_EOF] = {NULL, NULL, PREC_NONE},
    [TK_ERROR] = {NULL, NULL, PREC_NONE},
};

/* ========== Helpers ========== */

// Get parse rule for a token type
const ParseRule *xr_get_rule(XrTokenType type) {
    size_t rules_size = sizeof(rules) / sizeof(rules[0]);

    if (type >= (XrTokenType) rules_size || type < 0) {
        // Return default rule (PREC_NONE, no prefix/infix)
        static ParseRule default_rule = {NULL, NULL, PREC_NONE};
        return &default_rule;
    }
    return &rules[type];
}

/* ========== Statement Boundaries (L-04) ==========
 *
 * Xray has no mandatory `;`: a line break ends a statement. The parser must
 * therefore decide, at every line break inside an expression, whether the next
 * line continues that expression or starts a new statement.
 *
 * The rule (same shape as Go's automatic semicolon insertion, decided here in
 * the parser rather than in the lexer):
 *
 *   A line break terminates the expression iff
 *     (1) the last consumed token can END an expression, AND
 *     (2) the token starting the new line can BEGIN one, AND
 *     (3) the parser is not inside `(` / `[` — where no statement can begin.
 *
 * (1) and (2) are only ever both true for tokens carrying both a prefix and an
 * infix role: `!` (logical not / force unwrap), `-` (negate / subtract), `/`
 * (regex literal / divide), `++`, `--`, `(` and `[`. Without this rule
 *
 *     var x = a
 *     !b
 *
 * silently parses as `var x = a!` followed by `b`, and the formatter then
 * writes that misreading back into the source file.
 *
 * Continuation lines starting with a token that has no prefix role — `.`, `?`,
 * `:`, `&&`, `||`, `+`, `*`, `as`, `is`, … — are unambiguous and keep working.
 * To continue a line with `-` or `/`, put the operator at the end of the
 * previous line or wrap the whole expression in parentheses.
 *
 * xr_token_can_end_expr() is the (1) half; keep it in sync with the prefix
 * column of the rules table above — tests/unit/frontend/test_parser_asi.c fails
 * the build if a new dual-role token escapes this rule.
 */

bool xr_token_can_end_expr(XrTokenType type) {
    switch (type) {
        // Primary expressions
        case TK_NAME:
        case TK_LITERAL_INT:
        case TK_LITERAL_FLOAT:
        case TK_LITERAL_BIGINT:
        case TK_LITERAL_STRING:
        case TK_LITERAL_BYTE_STRING:
        case TK_LITERAL_C_STRING:
        case TK_LITERAL_RUNE:
        case TK_LITERAL_REGEX:
        case TK_TEMPLATE_STRING:
        case TK_RAW_STRING:
        case TK_RAW_TEMPLATE_STRING:
        case TK_TRUE:
        case TK_FALSE:
        case TK_NULL:
        case TK_THIS:
        case TK_SUPER:
        case TK_UNDERSCORE:
        // Closers of a completed grouping / call / index / literal
        case TK_RPAREN:
        case TK_RBRACKET:
        case TK_RBRACE:
        // Postfix operators, already absorbed into the expression
        case TK_NOT:
        case TK_INC:
        case TK_DEC:
        case TK_QUESTION:
            return true;
        default:
            // Scalar type keywords terminate `expr as int` / `expr is f64`.
            return type >= TK_STRING && type <= TK_USIZE;
    }
}

// (1) + (2) + (3): does the line break before `current` end the expression?
static bool line_break_ends_expr(const Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    if (parser->current.line <= parser->previous.line)
        return false;
    if (!xr_token_can_end_expr(parser->previous.type))
        return false;
    /* Inside a match arm's expression body, a line starting with `is` opens
     * the next arm's type pattern. `is` is infix-only (no prefix rule), so
     * the generic check below would otherwise glue it onto the previous arm
     * body as an infix type test. */
    if (parser->match_arm_body_depth > 0 && parser->current.type == TK_IS)
        return !xr_parser_in_group(parser);
    if (xr_get_rule(parser->current.type)->prefix == NULL)
        return false;
    return !xr_parser_in_group(parser);
}

/* ========== Token Operations ========== */

// Update the open-bracket bitstack for a token that has just been consumed.
// See Parser::bracket_bits. `(` `[` `?[` `#[` open a *group* (bit 1: no
// statement can begin inside, so line breaks must not terminate anything);
// `{` `#{` open a brace scope (bit 0: statements do begin inside, so line
// breaks terminate normally).
static void track_bracket(Parser *parser, XrTokenType type) {
    if (!xr_parser_healthy(parser)) return;
    switch (type) {
        case TK_LPAREN:
        case TK_LBRACKET:
        case TK_QUESTION_LBRACKET:
        case TK_SET_START:
            if (parser->bracket_depth < XR_PARSER_MAX_BRACKET_BITS)
                parser->bracket_bits |= (uint64_t) 1 << parser->bracket_depth;
            parser->bracket_depth++;
            break;
        case TK_LBRACE:
        case TK_EMPTY_MAP_START:
            if (parser->bracket_depth < XR_PARSER_MAX_BRACKET_BITS)
                parser->bracket_bits &= ~((uint64_t) 1 << parser->bracket_depth);
            parser->bracket_depth++;
            break;
        case TK_RPAREN:
        case TK_RBRACKET:
        case TK_RBRACE:
            if (parser->bracket_depth > 0)
                parser->bracket_depth--;
            break;
        default:
            break;
    }
}

// True when the innermost still-open bracket is `(` or `[`, i.e. the parser is
// inside a grouped expression where a statement cannot start.
bool xr_parser_in_group(const Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    XR_DCHECK(parser != NULL, "parser_in_group: NULL parser");
    if (parser->bracket_depth <= 0 || parser->bracket_depth > XR_PARSER_MAX_BRACKET_BITS)
        return false;
    return (parser->bracket_bits >> (parser->bracket_depth - 1)) & 1u;
}

XrParserStreamState xr_parser_stream_save(const Parser *parser) {
    if (!xr_parser_healthy(parser)) return (XrParserStreamState){0};
    XR_DCHECK(parser != NULL, "parser_stream_save: NULL parser");
    if (xr_compile_state_work(parser->state, sizeof(Scanner) + 2 * sizeof(Token) +
            sizeof(parser->bracket_bits) + sizeof(parser->bracket_depth)) != XR_COMPILE_RESOURCE_OK)
        return (XrParserStreamState) {0};
    XrParserStreamState saved = {
        .scanner = parser->scanner,
        .current = parser->current,
        .previous = parser->previous,
        .bracket_bits = parser->bracket_bits,
        .bracket_depth = parser->bracket_depth,
    };
    return saved;
}

void xr_parser_stream_restore(Parser *parser, const XrParserStreamState *saved) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "parser_stream_restore: NULL parser");
    XR_DCHECK(saved != NULL, "parser_stream_restore: NULL state");
    if (xr_compile_state_work(parser->state, sizeof(Scanner) + 2 * sizeof(Token) +
            sizeof(parser->bracket_bits) + sizeof(parser->bracket_depth)) != XR_COMPILE_RESOURCE_OK) return;
    parser->scanner = saved->scanner;
    parser->current = saved->current;
    parser->previous = saved->previous;
    parser->bracket_bits = saved->bracket_bits;
    parser->bracket_depth = saved->bracket_depth;
}

// Advance to next token
void xr_parser_advance(Parser *parser) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "parser_advance: NULL parser");
    if (xr_compile_state_copy(parser->state, &parser->previous, &parser->current,
                              sizeof(Token)) != XR_COMPILE_RESOURCE_OK) return;
    track_bracket(parser, parser->previous.type);

    // Skip error tokens until valid token found
    while (xr_parser_healthy(parser) && 1) {
        do {
            parser->current = xr_scanner_scan(&parser->scanner);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        if (parser->current.type != TK_ERROR)
            break;

        // TK_ERROR carries diagnostic text in error_message (L-03 contract).
        const char *msg = parser->current.error_message;
        do {
            xr_parser_error_at_current(parser, msg ? msg : "lexical error");
            if (!xr_parser_healthy(parser)) return;
        } while (0);
    }
}

// Check if current token is of specified type
int xr_parser_check(Parser *parser, XrTokenType type) {
    if (!xr_parser_healthy(parser)) return type == TK_EOF;
    XR_DCHECK(parser != NULL, "parser_check: NULL parser");
    return parser->current.type == type;
}

// If current token matches, consume it and return true
int xr_parser_match(Parser *parser, XrTokenType type) {
    if (!xr_parser_healthy(parser)) return 0;
    if (!xr_parser_check(parser, type))
        return 0;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return 0;
    } while (0);
    return 1;
}

// Consume token of specified type, or report error
void xr_parser_consume(Parser *parser, XrTokenType type, const char *message) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "parser_consume: NULL parser");
    if (parser->current.type == type) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        return;
    }

    // Better error when a keyword is used where an identifier is expected
    if (type == TK_NAME && parser->current.type >= TK_FIRST_KEYWORD &&
        parser->current.type <= TK_LAST_KEYWORD) {
        do {
            xr_parser_error_expected_name(parser, message);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        return;
    }

    do {
        xr_parser_error_at_current(parser, message);
        if (!xr_parser_healthy(parser)) return;
    } while (0);
}

// Contextual keyword check: current token is TK_NAME with specific string content.
bool xr_parser_check_name(Parser *parser, const char *name) {
    if (!xr_parser_healthy(parser)) return false;
    if (parser->current.type != TK_NAME)
        return false;
    int len = (int) xr_parser_string_length(parser, name);
    if (!xr_parser_healthy(parser)) return false;
    return parser->current.length == len && xr_parser_compare_bytes(parser, parser->current.start, name, len) == 0;
}

// Contextual keyword match: if current is TK_NAME matching name, consume and return true.
bool xr_parser_match_name(Parser *parser, const char *name) {
    if (!xr_parser_healthy(parser)) return false;
    if (!xr_parser_check_name(parser, name))
        return false;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return false;
    } while (0);
    return true;
}

// Unified keyword-as-identifier error reporting.
// context examples: "expected variable name", "expected field name", etc.
void xr_parser_error_expected_name(Parser *parser, const char *context) {
    if (!xr_parser_healthy(parser)) return;
    if (parser->current.type >= TK_FIRST_KEYWORD && parser->current.type <= TK_LAST_KEYWORD) {
        char buf[128];
        int len = parser->current.length;
        if (len > 60)
            len = 60;
        xr_parser_format(parser, buf, sizeof(buf), "'%.*s' is a keyword and cannot be used as an identifier", len,
                 parser->current.start);
        do {
            xr_parser_error_at_current(parser, buf);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
    } else {
        do {
            xr_parser_error_at_current(parser, context);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
    }
}

// Detect common cross-language mistakes at ASI (automatic semicolon insertion) boundaries.
// When a statement ends but the next token on the same line isn't a semicolon,
// check if it's a known cross-language keyword before reporting generic ASI error.
bool xr_parser_check_asi_hint(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    if (parser->current.type != TK_NAME)
        return false;

    const char *s = parser->current.start;
    int len = parser->current.length;

    if (len == 3 && xr_parser_compare_bytes(parser, s, "and", 3) == 0) {
        do {
            xr_parser_error_at_current(parser,
                                   "'and' is not an operator. Use '&&' for logical AND in Xray");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return true;
    }
    if (len == 2 && xr_parser_compare_bytes(parser, s, "or", 2) == 0) {
        do {
            xr_parser_error_at_current(parser,
                                   "'or' is not an operator. Use '||' for logical OR in Xray");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return true;
    }
    if (len == 3 && xr_parser_compare_bytes(parser, s, "not", 3) == 0) {
        do {
            xr_parser_error_at_current(parser,
                                   "'not' is not an operator. Use '!' for logical NOT in Xray");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return true;
    }
    if (len == 2 && xr_parser_compare_bytes(parser, s, "do", 2) == 0) {
        do {
            xr_parser_error_at_current(
            parser, "'do...while' is not supported. Use 'while (condition) { }' in Xray");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return true;
    }
    return false;
}

/* ========== Error Handling ========== */

// Set error callback for LSP integration
void xr_parser_set_error_callback(Parser *parser, XrParseErrorCallback callback, void *user_data,
                                  int max_errors) {
    if (!xr_parser_healthy(parser)) return;
    parser->error_callback = callback;
    parser->error_callback_data = user_data;
    parser->max_errors = max_errors;
    parser->error_count = 0;
}

// Report error at a specific token (shared implementation)
static void xr_parser_error_at(Parser *parser, Token *token, const char *message) {
    if (!xr_parser_healthy(parser)) return;
    if (parser->panic_mode || (parser->max_errors > 0 && parser->error_count >= parser->max_errors))
        return;  // Avoid error cascade

    parser->panic_mode = 1;
    parser->had_error = 1;
    parser->error_count++;

    // Call error callback if set (for LSP)
    if (parser->error_callback) {
        parser->error_callback(parser->error_callback_data, token->line, token->column, token->line,
                               token->column + token->length, message);
        return;
    }

    xr_parser_diagnostic_print(parser, XR_DIAG_ERROR, 0, message, token->line, token->column,
                               token->type == TK_EOF ? 0 : token->length, token->start);
}

// Report error at current token
void xr_parser_error_at_current(Parser *parser, const char *message) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "error_at_current: NULL parser");
    do {
        xr_parser_error_at(parser, &parser->current, message);
        if (!xr_parser_healthy(parser)) return;
    } while (0);
}

// Report error at previous token
void xr_parser_error_at_previous(Parser *parser, const char *message) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "error_at_previous: NULL parser");
    do {
        xr_parser_error_at(parser, &parser->previous, message);
        if (!xr_parser_healthy(parser)) return;
    } while (0);
}

// Report error at current position
void xr_parser_error(Parser *parser, const char *message) {
    if (!xr_parser_healthy(parser)) return;
    do {
        xr_parser_error_at_current(parser, message);
        if (!xr_parser_healthy(parser)) return;
    } while (0);
}

// Emit a "removed syntax" diagnostic with help and explanatory note.
// Prints two lines: the primary error (with code and caret) followed by a
// note line that suggests the modern replacement. Skips emission while in
// panic mode so cascade errors are suppressed.
//
// `title` is the short error headline (e.g. "`void` keyword was removed").
// `note`  is the help/explanation that follows the underline.
void xr_parser_error_coded_note(Parser *parser, Token *token, int code, const char *title,
                                const char *note) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "error_coded_note: NULL parser");
    XR_DCHECK(token != NULL, "error_coded_note: NULL token");
    XR_DCHECK(title != NULL, "error_coded_note: NULL title");

    if (parser->panic_mode || (parser->max_errors > 0 && parser->error_count >= parser->max_errors))
        return;

    parser->panic_mode = 1;
    parser->had_error = 1;
    parser->error_count++;

    if (parser->error_callback) {
        parser->error_callback(parser->error_callback_data, token->line, token->column, token->line,
                               token->column + token->length, title);
        return;
    }

    int tok_len = token->type == TK_EOF ? 0 : token->length;
    xr_parser_diagnostic_print(parser, XR_DIAG_ERROR, code, title, token->line, token->column,
                               tok_len, token->start);
    if (note) {
        xr_parser_diagnostic_print(parser, XR_DIAG_NOTE, 0, note, token->line, token->column,
                                   tok_len, token->start);
    }
}

/*
 * Error recovery: synchronize to next statement boundary
 *
 * Strategy:
 * 1. Skip tokens until we find a statement boundary
 * 2. Statement boundaries are:
 *    - Semicolon (previous token)
 *    - Statement-starting keywords (current token)
 *    - Matching closing brackets (to exit nested structures)
 * 3. Track bracket depth to avoid over-skipping
 */
void xr_parser_synchronize(Parser *parser) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "parser_synchronize: NULL parser");
    parser->panic_mode = 0;

    // Track bracket depth for better recovery
    int brace_depth = 0;    // { }
    int paren_depth = 0;    // ( )
    int bracket_depth = 0;  // [ ]

    while (xr_parser_healthy(parser) && parser->current.type != TK_EOF) {
        // Check if previous was semicolon
        if (parser->previous.type == TK_SEMICOLON) {
            return;
        }

        // Track bracket depth
        switch (parser->previous.type) {
            case TK_LBRACE:
                brace_depth++;
                break;
            case TK_RBRACE:
                brace_depth--;
                break;
            case TK_LPAREN:
                paren_depth++;
                break;
            case TK_RPAREN:
                paren_depth--;
                break;
            case TK_LBRACKET:
                bracket_depth++;
                break;
            case TK_RBRACKET:
                bracket_depth--;
                break;
            default:
                break;
        }

        // If we closed all brackets, we're at a good boundary
        if (brace_depth < 0 || paren_depth < 0 || bracket_depth < 0) {
            return;
        }

        // Only sync at statement-starting keywords if at top level (no nested brackets)
        if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
            switch (parser->current.type) {
                // Declaration keywords
                case TK_CLASS:
                case TK_STRUCT:
                case TK_UNION:
                case TK_PACKED:
                case TK_INTERFACE:
                case TK_ENUM:
                case TK_FN:
                case TK_VAR:
                case TK_CONST:
                case TK_COMPTIME:
                case TK_TYPE_ALIAS:
                case TK_IMPORT:
                case TK_EXPORT:
                // Control flow keywords
                case TK_FOR:
                case TK_IF:
                case TK_WHILE:
                case TK_MATCH:
                case TK_RETURN:
                case TK_BREAK:
                case TK_CONTINUE:
                case TK_THROW:
                case TK_TRY:
                // Concurrency keywords
                case TK_GO:
                case TK_DEFER:
                case TK_SELECT:
                // Attribute
                case TK_AT:
                    return;
                default:
                    break;
            }
        }

        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
    }
}

void xr_parser_skip_invalid_construct(Parser *parser, int start_line,
                                      XrParserRecoveryBoundaryFn is_recovery_boundary,
                                      bool stop_at_rbrace) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "skip_invalid_construct: NULL parser");

    int brace_depth = 0;
    int paren_depth = 0;
    int bracket_depth = 0;
    bool saw_body = false;
    bool consumed = false;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_EOF)) {
        if (!saw_body && brace_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
            if (stop_at_rbrace && xr_parser_check(parser, TK_RBRACE))
                break;
            if (xr_parser_check(parser, TK_SEMICOLON)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return;
                } while (0);
                break;
            }
            if (consumed && parser->current.line > start_line && is_recovery_boundary &&
                is_recovery_boundary(parser)) {
                break;
            }
        }

        XrTokenType type = parser->current.type;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        consumed = true;

        switch (type) {
            case TK_LBRACE:
                brace_depth++;
                saw_body = true;
                break;
            case TK_RBRACE:
                if (brace_depth > 0) {
                    brace_depth--;
                    if (saw_body && brace_depth == 0) {
                        parser->panic_mode = 0;
                        return;
                    }
                }
                break;
            case TK_LPAREN:
                paren_depth++;
                break;
            case TK_RPAREN:
                if (paren_depth > 0)
                    paren_depth--;
                break;
            case TK_LBRACKET:
                bracket_depth++;
                break;
            case TK_RBRACKET:
                if (bracket_depth > 0)
                    bracket_depth--;
                break;
            default:
                break;
        }
    }

    parser->panic_mode = 0;
}

/* ========== Expression Parsing ========== */

static void skip_invalid_inline_attribute_tail(Parser *parser, int start_line) {
    if (!xr_parser_healthy(parser)) return;
    int brace_depth = 0;
    int paren_depth = 0;
    int bracket_depth = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_EOF)) {
        if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
            if (xr_parser_check(parser, TK_SEMICOLON) || parser->current.line > start_line) {
                return;
            }
        }

        XrTokenType type = parser->current.type;
        switch (type) {
            case TK_LBRACE:
                brace_depth++;
                break;
            case TK_RBRACE:
                if (brace_depth == 0) {
                    return;
                }
                brace_depth--;
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return;
                } while (0);
                if (brace_depth == 0 && paren_depth == 0 && bracket_depth == 0) {
                    return;
                }
                continue;
            case TK_LPAREN:
                paren_depth++;
                break;
            case TK_RPAREN:
                if (paren_depth == 0) {
                    return;
                }
                paren_depth--;
                break;
            case TK_LBRACKET:
                bracket_depth++;
                break;
            case TK_RBRACKET:
                if (bracket_depth == 0) {
                    return;
                }
                bracket_depth--;
                break;
            default:
                break;
        }

        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
    }
}

// Keep speculative state out of ordinary expression recursion frames. A full
// Parser snapshot is needed only when the token stream can start type arguments.
static AstNode *try_generic_suffix(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "try_generic_suffix: NULL parser");
    Parser checkpoint;
    if (xr_compile_state_copy(parser->state, &checkpoint, parser, sizeof(checkpoint)) != XR_COMPILE_RESOURCE_OK) return NULL;
    int error_count = parser->error_count;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *result = xr_parse_try_generic_call_after_lt(parser, left);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!result && parser->error_count == error_count)
        if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
    return result;
}

// Pratt parser core: parse expression by precedence.
// Inner implementation; the public xr_parse_precedence wraps this with the
// recursion-depth guard.
static AstNode *parse_precedence_inner(Parser *parser, Precedence precedence) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_precedence: NULL parser");
    // Special handling for regex literals starting with escape sequences like /\d+/
    // When current is TK_SLASH, try regex scanning first to avoid TK_ERROR from backslash
    if (parser->current.type == TK_SLASH) {
        const char *slash_pos = parser->current.start;
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
        }
        // Not a regex, restore scanner and continue normal parsing
        parser->scanner.current = slash_pos;
    }

    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    PrefixParseFn prefix_rule = xr_get_rule(parser->previous.type)->prefix;
    if (!xr_parser_healthy(parser)) return NULL;
    if (prefix_rule == NULL) {
        if (parser->previous.type == TK_AT) {
            do {
                xr_parser_error_at_previous(
                parser,
                "attributes can only annotate declaration items; use 'fn name(...)' for function "
                "item attributes");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            skip_invalid_inline_attribute_tail(parser, parser->previous.line);
        } else {
            do {
                xr_parser_error(parser, "expected expression");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
        return NULL;
    }

    AstNode *left = prefix_rule(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    if (left == NULL) {
        return NULL;
    }

    // Process infix expressions by precedence
    for (; xr_parser_step(parser) && (true);) {
        // L-04 statement boundary: a line break ends the expression when the
        // new line could just as well start a statement. Checked before the
        // generic-call probe below because `<` has no prefix role and is
        // therefore never a statement start.
        if (line_break_ends_expr(parser))
            break;

        if (parser->current.type == TK_LT && !parser->current.has_leading_space &&
            precedence <= PREC_CALL) {
            int before_error_count = parser->error_count;
            AstNode *generic_call = try_generic_suffix(parser, left);
            if (!xr_parser_healthy(parser)) return NULL;
            if (generic_call) {
                left = generic_call;
                continue;
            }
            if (parser->error_count > before_error_count) {
                return left;
            }
        }

        if (precedence > xr_get_rule(parser->current.type)->precedence)
            break;

        const ParseRule *rule = xr_get_rule(parser->current.type);
        if (!xr_parser_healthy(parser)) return NULL;

        if (rule->infix == NULL) {
            break;
        }
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            left = rule->infix(parser, left);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    return left;
}

// Public Pratt entry: recursion-depth guard around parse_precedence_inner.
// All expression recursion (parens, unary, binary, ternary, member/index
// chains) funnels through here, so one guard covers the whole expression grammar.
AstNode *xr_parse_precedence(Parser *parser, Precedence precedence) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_precedence: NULL parser");
    if (++parser->recursion_depth > XR_PARSER_MAX_DEPTH) {
        parser->recursion_depth--;
        do {
            xr_parser_error(parser, "expression nesting too deep (max 1000 levels)");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    AstNode *result = parse_precedence_inner(parser, precedence);
    if (!xr_parser_healthy(parser)) return NULL;
    parser->recursion_depth--;
    return result;
}

// Parse expression (entry point)
AstNode *xr_parse_expression(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_expression: NULL parser");
    return xr_parse_precedence(parser, PREC_ASSIGNMENT);  // Start from lowest precedence
}

/* ========== Statement Parsing ========== */

// Parse expression statement.
// Multi-value forms (a, b = b, a) are no longer accepted: tuple
// destructure-assignment `(a, b) = (b, a)` is the canonical form and
// is handled by xr_parse_assignment via the AST_TUPLE_LITERAL branch.
AstNode *xr_parse_expr_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    AstNode *inc_dec = xr_parse_standalone_inc_dec(parser, false);
    if (!xr_parser_healthy(parser)) return NULL;
    if (inc_dec)
        return inc_dec;

    // First token of the statement: the E0208 caret belongs on the `!` / `-`
    // that opened the line, not on wherever the expression happened to end.
    Token stmt_start = parser->current;

    // Use PREC_TERNARY to avoid parsing assignment
    AstNode *first_expr = xr_parse_precedence(parser, PREC_TERNARY);
    if (!xr_parser_healthy(parser)) return NULL;

    // Recoverable parsing may reject the prefix expression while leaving an
    // assignment-like token (for example the removed `=>` match arm syntax)
    // as the current token. Do not dispatch an infix rule with a NULL left
    // operand; the outer recoverable loop will synchronize at the error.
    if (!first_expr)
        return NULL;

    // Bare comma at statement position is the obsolete multi-value
    // form. Point users at the tuple destructure equivalent instead
    // of letting it fall through with a generic error.
    if (xr_parser_check(parser, TK_COMMA)) {
        do {
            xr_parser_error(parser, "bare multi-value assignment is not supported; "
                                "use tuple destructure: (a, b) = (b, a)");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // Regular expression statement, check for assignment operators
    if (xr_get_rule(parser->current.type)->precedence == PREC_ASSIGNMENT) {
        while (xr_parser_healthy(parser) && xr_get_rule(parser->current.type)->precedence >= PREC_ASSIGNMENT) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            InfixParseFn infix_rule = xr_get_rule(parser->previous.type)->infix;
            if (!xr_parser_healthy(parser)) return NULL;
            if (infix_rule) {
                do {
                    first_expr = infix_rule(parser, first_expr);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
        }
    }

    // Destructure-assign is a statement, not an expression: it
    // produces no value and must dispatch through xi_lower_stmt's
    // dedicated case. Wrapping it in AST_EXPR_STMT routes it through
    // xi_lower_expr which has no matching case, fails silently in
    // release builds, and yields a NULL IR.
    if (first_expr && first_expr->type == AST_DESTRUCTURE_ASSIGN) {
        return first_expr;
    }

    // L-04's loud half: an expression that cannot do anything is never a
    // statement the author meant to write — most often it is a continuation
    // line that the statement-boundary rule correctly refused to glue on.
    do {
        xr_parser_reject_effectless_expr_stmt(parser, first_expr, &stmt_start);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    return xr_ast_expr_stmt(parser->compiler_session, first_expr, parser->previous.line);
}

AstNode *xr_parse_standalone_inc_dec(Parser *parser, bool for_step) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (!xr_parser_check(parser, TK_NAME))
        return NULL;

    Parser checkpoint;
    if (xr_compile_state_copy(parser->state, &checkpoint, parser, sizeof(checkpoint)) != XR_COMPILE_RESOURCE_OK) return NULL;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    Token name = parser->previous;
    if (!xr_parser_check(parser, TK_INC) && !xr_parser_check(parser, TK_DEC)) {
        if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
        return NULL;
    }

    XrTokenType op = parser->current.type;
    Token op_token = parser->current;
    char *var_name = (char *) ast_alloc(parser->compiler_session, (size_t) name.length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, var_name, name.start, (size_t) name.length)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; var_name[name.length] = '\0'; } while (0);

    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (for_step) {
        if (!xr_parser_check(parser, TK_RPAREN)) {
            do {
                xr_parser_error_at_current(parser,
                                       "postfix ++/-- for-step must be the entire step expression");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    } else if (!xr_parser_check(parser, TK_SEMICOLON) && !xr_parser_check(parser, TK_RBRACE) &&
               !xr_parser_check(parser, TK_EOF) && parser->current.line == op_token.line &&
               !xr_parser_check_asi_hint(parser)) {
        do {
            xr_parser_error_at_current(parser, "postfix ++/-- must be a complete standalone statement");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    return op == TK_INC ? xr_ast_inc(parser->compiler_session, var_name, name.line)
                        : xr_ast_dec(parser->compiler_session, var_name, name.line);
}

bool xr_lbrace_starts_destructure_assignment(Parser *parser) {
    if (!xr_parser_healthy(parser)) return false;
    Parser probe;
    if (xr_compile_state_copy(parser->state, &probe, parser, sizeof(probe)) != XR_COMPILE_RESOURCE_OK) return false;
    int depth = 0;

    while (xr_parser_healthy(parser) && !xr_parser_check(&probe, TK_EOF)) {
        if (xr_parser_check(&probe, TK_LBRACE)) {
            depth++;
        } else if (xr_parser_check(&probe, TK_RBRACE)) {
            depth--;
            do {
                xr_parser_advance(&probe);
                if (!xr_parser_healthy(parser)) return false;
            } while (0);
            if (depth == 0)
                return xr_parser_check(&probe, TK_ASSIGN);
            continue;
        }

        do {
            xr_parser_advance(&probe);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
    }

    return false;
}

// Parse statement
AstNode *xr_parse_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    if (parser->current.type == TK_NAME) {
        Parser checkpoint;
        if (xr_compile_state_copy(parser->state, &checkpoint, parser, sizeof(checkpoint)) != XR_COMPILE_RESOURCE_OK) return NULL;
        Token label_tok = parser->current;
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (xr_parser_match(parser, TK_COLON)) {
            if (parser->current.type == TK_FOR || parser->current.type == TK_WHILE) {
                char *label =
                    (char *) ast_alloc(parser->compiler_session, (size_t) label_tok.length + 1);
                if (!xr_parser_healthy(parser)) return NULL;
                do { if (!ast_copy(parser->compiler_session, label, label_tok.start, (size_t) label_tok.length)) return NULL; } while (0);
                do { if (!ast_work(parser->compiler_session, 1)) return NULL; label[label_tok.length] = '\0'; } while (0);

                AstNode *stmt = xr_parse_statement(parser);
                if (!xr_parser_healthy(parser)) return NULL;
                if (!stmt)
                    return NULL;
                switch (stmt->type) {
                    case AST_WHILE_STMT:
                        stmt->as.while_stmt.label = label;
                        return stmt;
                    case AST_FOR_STMT:
                        stmt->as.for_stmt.label = label;
                        return stmt;
                    case AST_FOR_IN_STMT:
                        stmt->as.for_in_stmt.label = label;
                        return stmt;
                    default:
                        do {
                            xr_parser_error_at_current(parser, "loop labels can only annotate loops");
                            if (!xr_parser_healthy(parser)) return NULL;
                        } while (0);
                        return NULL;
                }
            }
            if (parser->current.type != TK_ASSIGN) {
                // The name and ':' are already known to form an invalid loop
                // label. Keep them consumed so recoverable parsing cannot
                // revisit the same token pair indefinitely.
                do {
                    xr_parser_error_at_current(parser, "loop labels can only annotate loops");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }
        if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
    }

    // Control flow
    if (parser->current.type == TK_IF) {
        return xr_parse_if_statement(parser);
    }
    if (parser->current.type == TK_WHILE) {
        return xr_parse_while_statement(parser);
    }
    if (parser->current.type == TK_FOR) {
        // Lookahead: for-in vs traditional for
        Parser checkpoint;
        if (xr_compile_state_copy(parser->state, &checkpoint, parser, sizeof(checkpoint)) != XR_COMPILE_RESOURCE_OK) return NULL;

        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        if (xr_parser_check(parser, TK_LPAREN)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            // Try to identify for-in pattern (support _ as blank
            // identifier and `(...)` tuple destructuring heads).
            // Also detect keywords used as loop variables to give
            // better errors.
            bool might_be_forin =
                xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_UNDERSCORE);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!might_be_forin && parser->current.type >= TK_FIRST_KEYWORD &&
                parser->current.type <= TK_LAST_KEYWORD) {
                // Keyword used as loop variable — route to for-in for proper error
                might_be_forin = true;
            }
            if (xr_parser_check(parser, TK_LPAREN)) {
                /* `for ((` -- a tuple destructuring head. Skip the
                 * balanced parens so the `in` check below sees the
                 * token after `)`. We do not interpret the contents;
                 * any malformed pattern is reported by the destructure
                 * parser when we re-enter from xr_parse_for_in_statement. */
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                int depth = 1;
                while (xr_parser_healthy(parser) && depth > 0 && !xr_parser_check(parser, TK_EOF)) {
                    if (xr_parser_check(parser, TK_LPAREN))
                        depth++;
                    else if (xr_parser_check(parser, TK_RPAREN))
                        depth--;
                    if (depth == 0) {
                        do {
                            xr_parser_advance(parser);
                            if (!xr_parser_healthy(parser)) return NULL;
                        } while (0);
                        break;
                    }
                    do {
                        xr_parser_advance(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                }
                if (xr_parser_check(parser, TK_IN)) {
                    if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
                    return xr_parse_for_in_statement(parser);
                }
            } else if (might_be_forin) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

                // Check for comma (key-value pattern)
                if (xr_parser_check(parser, TK_COMMA)) {
                    do {
                        xr_parser_advance(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    if (xr_parser_check(parser, TK_NAME) ||
                        xr_parser_check(parser, TK_UNDERSCORE)) {
                        do {
                            xr_parser_advance(parser);
                            if (!xr_parser_healthy(parser)) return NULL;
                        } while (0);
                    }
                }

                // Skip optional type annotation
                if (xr_parser_check(parser, TK_COLON)) {
                    do {
                        xr_parser_advance(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_IN) && !xr_parser_check(parser, TK_RPAREN) &&
                           !xr_parser_check(parser, TK_EOF)) {
                        do {
                            xr_parser_advance(parser);
                            if (!xr_parser_healthy(parser)) return NULL;
                        } while (0);
                    }
                }

                // Check for 'in'
                if (xr_parser_check(parser, TK_IN)) {
                    if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
                    return xr_parse_for_in_statement(parser);
                }
            }
        }

        // Otherwise traditional for loop
        if (xr_compile_state_copy(parser->state, parser, &checkpoint, sizeof(*parser)) != XR_COMPILE_RESOURCE_OK) return NULL;
        return xr_parse_for_statement(parser);
    }
    if (parser->current.type == TK_BREAK) {
        return xr_parse_break_statement(parser);
    }
    if (parser->current.type == TK_CONTINUE) {
        return xr_parse_continue_statement(parser);
    }
    if (parser->current.type == TK_RETURN) {
        return xr_parse_return_statement(parser);
    }

    // Exception handling
    if (parser->current.type == TK_TRY) {
        return xr_parse_try_statement(parser);
    }
    if (parser->current.type == TK_THROW) {
        return xr_parse_throw_statement(parser);
    }

    // Block
    if (parser->current.type == TK_LBRACE) {
        if (xr_lbrace_starts_destructure_assignment(parser))
            return xr_parse_expr_statement(parser);
        return xr_parse_block(parser);
    }

    // Prefix increment/decrement not supported (use postfix x++, x--)
    if (parser->current.type == TK_INC || parser->current.type == TK_DEC) {
        do {
            xr_parser_error_at_current(parser,
                                   "prefix ++/-- not supported, use postfix form (x++, x--)");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0); /* consume ++/-- so the parser can recover */
        return NULL;
    }

    return xr_parse_expr_statement(parser);
}

/* ========== Variable Parsing ========== */

// Try parsing generic type args <T1, T2, ...>
// Uses lookahead to detect identifier<...>( pattern, avoiding confusion with comparison
// Uses space sensitivity: foo<T>() is generic, foo < T is comparison
// Returns: number of type args parsed, 0 means not a generic call/type namespace.
static int try_parse_generic_type_args(Parser *parser, XrTypeRef **type_args, int capacity,
                                       bool allow_dot_follow) {
    if (!xr_parser_healthy(parser)) return 0;
    // disambiguation: if '<' has leading space, treat as comparison
    if (parser->current.type == TK_LT && parser->current.has_leading_space) {
        return 0;  // Space before '<' means comparison, not generic
    }

    // Save parser state for rollback
    XrParserStreamState saved_stream = xr_parser_stream_save(parser);
    if (!xr_parser_healthy(parser)) return 0;
    int saved_had_error = parser->had_error;
    int saved_panic_mode = parser->panic_mode;
    int saved_error_count = parser->error_count;

    // Tentative parsing: disable error output
    parser->panic_mode = 1;

    if (!xr_parser_match(parser, TK_LT)) {
        parser->panic_mode = saved_panic_mode;
        return 0;
    }

    // Parse type argument list
    int count = 0;
    do {
        if (count >= capacity) {
            goto rollback;
        }

        XrTypeRef *type = xr_parse_type_annotation(parser);
        if (!xr_parser_healthy(parser)) return 0;
        if (parser->error_count > saved_error_count) {
            parser->panic_mode = 1;
            return 0;
        }

        if (parser->had_error && !saved_had_error) {
            goto rollback;
        }

        if (!type) {
            goto rollback;
        }

        type_args[count++] = type;

    } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));

    // Must end with >
    if (!xr_parser_match(parser, TK_GT)) {
        goto rollback;
    }

    // Must be followed by ( for generic call, { for generic struct literal,
    // or (for selected builtins) . for type namespace access.
    if (!xr_parser_check(parser, TK_LPAREN) && !xr_parser_check(parser, TK_LBRACE) &&
        !(allow_dot_follow && xr_parser_check(parser, TK_DOT))) {
        goto rollback;
    }

    parser->panic_mode = saved_panic_mode;
    return count;

rollback:
    // Restore parser state
    do {
        xr_parser_stream_restore(parser, &saved_stream);
        if (!xr_parser_healthy(parser)) return 0;
    } while (0);
    parser->had_error = saved_had_error;
    parser->panic_mode = saved_panic_mode;
    return 0;
}

// Parse variable reference: x
// Supports generic call syntax: foo<int, string>(arg1, arg2)
AstNode *xr_parse_variable(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    // Context keyword intercept: "linked go" as expression.
    Token prev = parser->previous;
    if (prev.length == 6 && xr_parser_compare_bytes(parser, prev.start, "linked", 6) == 0 &&
        xr_parser_check(parser, TK_GO)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // consume "go"
        return xr_parse_go_expr_with_link(parser, XR_LINK_LINKED);
    }

    // Contextual keyword intercept: "cancelled()" expression
    if (prev.length == 9 && xr_parser_compare_bytes(parser, prev.start, "cancelled", 9) == 0 &&
        xr_parser_check(parser, TK_LPAREN)) {
        return xr_parse_cancelled_expr(parser);
    }
    // Contextual keyword intercept: "Channel(...)" constructs a dedicated
    // AST_CHANNEL_NEW node that codegen / shared-variable preregister /
    // select compilation pattern-match on. Plain references like
    // `Channel.method(...)` (followed by '.') keep flowing through the
    // regular variable path — this lookahead only triggers when the
    // very next token is '('.
    if (prev.length == 7 && xr_parser_compare_bytes(parser, prev.start, "Channel", 7) == 0 &&
        xr_parser_check(parser, TK_LPAREN)) {
        return xr_parse_channel_new(parser);
    }
    // Contextual keyword intercept: "move var" expression
    // Only trigger when followed by an identifier (the variable to move)
    if (prev.length == 4 && xr_parser_compare_bytes(parser, prev.start, "move", 4) == 0 &&
        xr_parser_check(parser, TK_NAME)) {
        return xr_parse_move_expr(parser);
    }

    // Detect 'not' keyword misuse: not true, not x, not (expr)
    if (prev.length == 3 && xr_parser_compare_bytes(parser, prev.start, "not", 3) == 0) {
        const ParseRule *next_rule = xr_get_rule(parser->current.type);
        if (!xr_parser_healthy(parser)) return NULL;
        if (next_rule->prefix != NULL) {
            do {
                xr_parser_error_at_previous(
                parser, "'not' is not an operator. Use '!' for logical NOT in Xray");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }
    // Detect 'lambda' keyword misuse: lambda x: x + 1
    if (prev.length == 6 && xr_parser_compare_bytes(parser, prev.start, "lambda", 6) == 0) {
        const ParseRule *next_rule = xr_get_rule(parser->current.type);
        if (!xr_parser_healthy(parser)) return NULL;
        if (next_rule->prefix != NULL || parser->current.type == TK_NAME) {
            do {
                xr_parser_error_at_previous(
                parser,
                "'lambda' is not supported. Use 'fn(params) { }' or '(params) -> expr' in Xray");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    char *name = (char *) ast_alloc(parser->compiler_session, (size_t) parser->previous.length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, name, parser->previous.start, parser->previous.length)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; name[parser->previous.length] = '\0'; } while (0);
    int line = parser->previous.line;
    int column = parser->previous.column;

    // Try parsing generic type args <T1, T2, ...>
    XrTypeRef *type_args[16];  // Max 16 type args
    int before_generic_error_count = parser->error_count;
    int type_arg_count = try_parse_generic_type_args(parser, type_args, 16, true);
    if (!xr_parser_healthy(parser)) return NULL;
    if (parser->error_count > before_generic_error_count) {
        AstNode *node = xr_ast_variable(parser->compiler_session, name, line);
        if (!xr_parser_healthy(parser)) return NULL;
        node->column = column;
        return node;
    }

    if (type_arg_count > 0) {
        if (xr_parser_check(parser, TK_DOT)) {
            AstNode *type_namespace = xr_ast_new_expr(parser->compiler_session, NULL, name, NULL,
                                                      NULL, 0, type_args, type_arg_count, line);
            if (!xr_parser_healthy(parser)) return NULL;
            if (type_namespace)
                type_namespace->as.new_expr.is_type_namespace = true;
            return type_namespace;
        }

        // Check if this is a generic struct literal: Name<T1, T2>{field: value}
        if (xr_parser_check(parser, TK_LBRACE)) {
            AstNode *type_path = xr_ast_variable(parser->compiler_session, name, line);
            if (!xr_parser_healthy(parser)) return NULL;
            if (type_path)
                type_path->column = column;
            return xr_parse_struct_literal_after_type(parser, type_path, type_args, type_arg_count);
        }

        // Generic call detected: identifier<T1, T2>(args)
        AstNode *callee = xr_ast_variable(parser->compiler_session, name, line);
        if (!xr_parser_healthy(parser)) return NULL;
        callee->column = column;

        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // Consume (

        // Parse argument list
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

        // `Map<K,V>()` / `Array<T>()` / `Channel<T>(n)` construct built-in heap
        // types directly (no `new`); route to the construction node.
        if (xr_is_construct_only_type_name(parser, name)) {
            return xr_ast_new_expr(parser->compiler_session, NULL, name, arguments, arg_accesses,
                                   arg_count, type_args, type_arg_count, line);
        }

        return xr_ast_call_expr_generic(parser->compiler_session, callee, arguments, arg_accesses,
                                        arg_count, type_args, type_arg_count, line);
    }

    // Check for struct literal: Name{field: value, ...}
    // Conditions: '{' on same line, no leading space, followed by 'name:' pattern
    if (xr_parser_check(parser, TK_LBRACE) && !parser->current.has_leading_space &&
        parser->current.line == line) {
        // Lookahead: check if this is { name: ... } pattern (struct literal)
        // vs block statement (which would have statements, not name:value pairs)
        XrParserStreamState saved_stream = xr_parser_stream_save(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        int saved_had_error = parser->had_error;
        int saved_panic_mode = parser->panic_mode;

        parser->panic_mode = 1;     // suppress errors during lookahead
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // consume '{'

        bool is_struct_literal = false;
        if (xr_parser_check(parser, TK_NAME)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // consume field name
            if (xr_parser_check(parser, TK_COLON)) {
                is_struct_literal = true;
            }
        } else if (xr_parser_check(parser, TK_RBRACE)) {
            // Empty struct literal: Point{}
            is_struct_literal = true;
        }

        // Restore parser state
        do {
            xr_parser_stream_restore(parser, &saved_stream);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        parser->had_error = saved_had_error;
        parser->panic_mode = saved_panic_mode;

        if (is_struct_literal) {
            AstNode *type_path = xr_ast_variable(parser->compiler_session, name, line);
            if (!xr_parser_healthy(parser)) return NULL;
            if (type_path)
                type_path->column = column;
            return xr_parse_struct_literal_after_type(parser, type_path, NULL, 0);
        }
    }

    // Regular variable reference
    AstNode *node = xr_ast_variable(parser->compiler_session, name, line);
    if (!xr_parser_healthy(parser)) return NULL;
    node->column = column;
    return node;
}

// Parse assignment: x = expression
AstNode *xr_parse_assignment(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = left->line;
    int column = left->column;

    // Variable assignment: x = 10
    if (left->type == AST_VARIABLE) {
        char *name = ast_strdup(parser->compiler_session, left->as.variable.name);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;

        AstNode *node = xr_ast_assignment(parser->compiler_session, name, value, line);
        if (!xr_parser_healthy(parser)) return NULL;
        node->column = column;  // Preserve column for rename
        return node;
    }
    // Index assignment: arr[0] = 10
    else if (left->type == AST_INDEX_GET) {
        AstNode *array = left->as.index_get.array;
        AstNode *index = left->as.index_get.index;

        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *node = xr_ast_index_set(parser->compiler_session, array, index, value, line);
        if (!xr_parser_healthy(parser)) return NULL;

        // Don't free left - arena handles it, or reused in node
        return node;
    }
    // Member assignment: obj.field = value
    else if (left->type == AST_MEMBER_ACCESS) {
        AstNode *object = left->as.member_access.object;
        char *member = ast_strdup(parser->compiler_session, left->as.member_access.name);
        if (!xr_parser_healthy(parser)) return NULL;

        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *node = xr_ast_member_set(parser->compiler_session, object, member, value, line);
        if (!xr_parser_healthy(parser)) return NULL;

        // Arena bulk-frees left/member; no individual free needed.
        return node;
    }
    // Destructure assignment: [a, b] = arr or (a, b) = pair or {x, y} = obj
    else if (left->type == AST_ARRAY_LITERAL) {
        XrDestructurePattern *pattern =
            convert_array_literal_to_pattern(parser, left);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!pattern) {
            do {
                xr_parser_error(parser, "destructure target must be variable list, e.g. [a, b]");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        return xr_ast_destructure_assign(parser->compiler_session, pattern, value, line);
    } else if (left->type == AST_TUPLE_LITERAL) {
        XrDestructurePattern *pattern =
            convert_tuple_literal_to_pattern(parser, left);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!pattern) {
            do {
                xr_parser_error(parser, "destructure target must be variable list, e.g. (a, b)");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        return xr_ast_destructure_assign(parser->compiler_session, pattern, value, line);
    } else if (left->type == AST_OBJECT_LITERAL) {
        XrDestructurePattern *pattern =
            convert_object_literal_to_pattern(parser, left);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!pattern) {
            do {
                xr_parser_error(parser, "destructure target must be variable list, e.g. {x, y}");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        AstNode *value = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        return xr_ast_destructure_assign(parser->compiler_session, pattern, value, line);
    }
    // Invalid assignment target
    else {
        do {
            xr_parser_error(parser,
                        "assignment target must be variable, index, member or destructure pattern");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
}

// Parse compound assignment: x += 10, x -= 5, this.field += 10, etc.
AstNode *xr_parse_compound_assignment(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = left->line;
    XrTokenType op_token = parser->previous.type;

    if (left->type == AST_VARIABLE) {
        // Variable compound assignment: x += 10
        char *var_name = ast_strdup(parser->compiler_session, left->as.variable.name);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *right = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *compound_assignment =
            xr_ast_compound_assignment(parser->compiler_session, var_name, op_token, right, line);
        if (!xr_parser_healthy(parser)) return NULL;
        return compound_assignment;
    } else if (left->type == AST_MEMBER_ACCESS) {
        // Member compound assignment: this.field += 10
        AstNode *object = left->as.member_access.object;
        char *member_name = ast_strdup(parser->compiler_session, left->as.member_access.name);
        if (!xr_parser_healthy(parser)) return NULL;

        AstNode *right = xr_parse_expression(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *compound_assignment = xr_ast_member_compound_assignment(
            parser->compiler_session, object, member_name, op_token, right, line);
        if (!xr_parser_healthy(parser)) return NULL;
        return compound_assignment;
    } else {
        do {
            xr_parser_error(parser, "compound assignment only for variables or member access");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
}

// Parse prefix increment/decrement: ++x, --x
// xray syntax: only postfix form (x++, x--) is supported
AstNode *xr_parse_inc_dec(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_error(parser, "prefix ++/-- not supported, use postfix form (x++, x--)");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0); /* consume ++/-- so the parser can recover */
    return NULL;
}

// Parse postfix increment/decrement: x++, x--
// Only postfix form is supported, and only through the statement-only parser
// entrypoints. Expression parsing reaches this function only for illegal
// embedded uses such as `var y = x++`, `f(x++)`, or `a[i++]`.
AstNode *xr_parse_postfix_inc_dec(Parser *parser, AstNode *left) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = left->line;
    (void) line;

    if (left->type != AST_VARIABLE) {
        do {
            xr_parser_error(parser, "++/-- only for variables");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    do {
        xr_parser_error_at_previous(
        parser, "postfix ++/-- can only be used as a standalone statement or for-step");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    return NULL;
}

// Parse single variable declaration: var x = 10 or const PI = 3.14
AstNode *xr_parse_single_var_declaration(Parser *parser, int is_const) {
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_consume(parser, TK_NAME, "expected variable name");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *name = (char *) ast_alloc(parser->compiler_session, (size_t) parser->previous.length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, name, parser->previous.start, parser->previous.length)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; name[parser->previous.length] = '\0'; } while (0);
    int line = parser->previous.line;
    int column = parser->previous.column;
    int name_length = parser->previous.length;

    XrTypeRef *type_annotation = NULL;
    AstNode *initializer = NULL;

    // Parse optional type annotation (: Type)
    if (xr_parser_match(parser, TK_COLON)) {
        do {
            type_annotation = xr_parse_type_annotation(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    if (xr_parser_match(parser, TK_ASSIGN)) {
        do {
            initializer = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else if (is_const) {
        do {
            xr_parser_error(parser, "constants must be initialized");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    // var variables can be uninitialized
    AstNode *node = xr_ast_var_decl(parser->compiler_session, name, initializer, is_const, line);
    if (!xr_parser_healthy(parser)) return NULL;
    node->column = column;
    // End span extends to the initializer when present; otherwise just the name.
    if (initializer && initializer->end_line > 0) {
        node->end_line = initializer->end_line;
        node->end_column = initializer->end_column;
    } else {
        node->end_line = line;
        node->end_column = column + name_length;
    }
    node->as.var_decl.type_annotation = type_annotation;
    return node;
}

// Parse block: { ... }
AstNode *xr_parse_block(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    bool saved_tail = parser->block_tail_value_observed;
    parser->block_tail_value_observed = parser->match_value_block_pending;
    parser->match_value_block_pending = false;
    int line = parser->previous.line;
    int initial_errors = parser->error_count;
    AstNode *block = xr_ast_block(parser->compiler_session, line);
    if (!xr_parser_healthy(parser)) return NULL;

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery to avoid infinite loop
        if (parser->panic_mode) {
            do {
                xr_parser_synchronize(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        int stmt_line = parser->current.line;

        // Capture leading trivia before parsing statement
        XrTrivia *leading_trivia = parser->current.leading_trivia;
        parser->current.leading_trivia = NULL;

        AstNode *decl = xr_parse_declaration(parser);
        if (!xr_parser_healthy(parser)) return NULL;
        if (decl != NULL) {
            // Attach leading trivia to the statement
            if (leading_trivia && !decl->leading_comments) {
                decl->leading_comments = leading_trivia;
            } else if (leading_trivia) {
                (void) (leading_trivia);
            }
        } else if (leading_trivia) {
            (void) (leading_trivia);
        }

        // Smart semicolon handling (block-internal variant). The
        // L-06 trailing capture happens AFTER this so we see the `;`
        // or `}`-terminator's trailing trivia, not the final
        // expression token's.
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF) &&
                parser->current.line == stmt_line && parser->current.type != TK_QUESTION &&
                parser->current.type != TK_COLON) {
                if (!xr_parser_check_asi_hint(parser)) {
                    do {
                        xr_parser_error_at_current(
                        parser, "multiple statements on same line must be separated by semicolon");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                }
                break;
            }
        }

        // L-06: capture trailing AFTER smart-semicolon advance, then
        // commit the statement to the block.
        if (decl != NULL) {
            if (parser->previous.trailing_trivia && !decl->trailing_comments) {
                decl->trailing_comments = parser->previous.trailing_trivia;
                parser->previous.trailing_trivia = NULL;
            }
            xr_ast_block_add(parser->compiler_session, block, decl);
        }

        if (parser->error_count != initial_errors)
            break;
    }

    do {
        xr_parser_consume(parser, TK_RBRACE, "expected '}' to close block");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Record closing `}` location (exclusive end). `parser->previous` now
    // points to the consumed `}`; its column is 1-indexed, so the exclusive
    // end column is column + 1.
    block->end_line = parser->previous.line;
    block->end_column = parser->previous.column + 1;
    parser->block_tail_value_observed = saved_tail;

    return block;
}

// Control flow parsing moved to xparse_stmt.c
