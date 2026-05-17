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
#include "../../runtime/xisolate_api.h"
#include "xtype_scope.h"
#include "xstring_pool.h"
#include "../xdiag_fmt.h"
#include "../../runtime/xerror_codes.h"

/* ========== Forward Declarations ========== */

// Internal parser init with trivia collection option
static void xr_parser_init_internal(Parser *parser, XrayIsolate *X, const char *source,
                                    const char *source_file, struct XrArena *arena,
                                    bool collect_trivia);

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
    [TK_RANGE] = {NULL, xr_parse_range, PREC_FACTOR},  // .. (range operator)
    [TK_COLON] = {NULL, NULL, PREC_NONE},
    [TK_SEMICOLON] = {NULL, NULL, PREC_NONE},

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
    [TK_EQ_STRICT] = {NULL, xr_parse_binary, PREC_EQUALITY},
    [TK_NE_STRICT] = {NULL, xr_parse_binary, PREC_EQUALITY},
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
    [TK_LET] = {NULL, NULL, PREC_NONE},
    [TK_CONST] = {NULL, NULL, PREC_NONE},
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
    [TK_PUBLIC] = {NULL, NULL, PREC_NONE},
    [TK_MATCH] = {xr_parse_match_expr, NULL, PREC_NONE},  // match expression
    [TK_UNDERSCORE] = {NULL, NULL, PREC_NONE},            // _ wildcard (pattern only)

    // Coroutine keywords
    [TK_GO] = {xr_parse_go_expr, NULL, PREC_NONE},        // go expression
    [TK_AWAIT] = {xr_parse_await_expr, NULL, PREC_NONE},  // await expression
    [TK_SELECT] = {NULL, NULL, PREC_NONE},                // select statement
    [TK_DEFER] = {NULL, NULL, PREC_NONE},                 // defer statement
    [TK_SCOPE] = {NULL, NULL, PREC_NONE},                 // scope block
    // cancelled(), move, and Channel(...) are all contextual keywords
    // handled in xr_parse_variable — they reach the parser as plain
    // TK_NAME tokens (the lexer no longer special-cases them).

    // Literals and identifiers
    [TK_LITERAL_INT] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_FLOAT] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_BIGINT] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_STRING] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_LITERAL_REGEX] = {xr_parse_regex_literal, NULL, PREC_NONE},
    [TK_TEMPLATE_STRING] = {xr_parse_template_string, NULL, PREC_NONE},
    [TK_RAW_STRING] = {xr_parse_literal, NULL, PREC_NONE},
    [TK_RAW_TEMPLATE_STRING] = {xr_parse_template_string, NULL, PREC_NONE},
    [TK_NAME] = {xr_parse_variable, NULL, PREC_NONE},

    // Type cast functions
    [TK_INT] = {xr_parse_type_cast, NULL, PREC_NONE},
    [TK_FLOAT] = {xr_parse_type_cast, NULL, PREC_NONE},
    [TK_STRING] = {xr_parse_type_cast, NULL, PREC_NONE},
    [TK_BOOL] = {xr_parse_type_cast, NULL, PREC_NONE},

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

/* ========== Escape Processing ========== */

// Process escape sequences in string content, write to out buffer.
// out must have at least src_len+1 bytes. Returns output length (excluding NUL).
size_t xr_process_escapes(const char *src, size_t src_len, char *out) {
    size_t dst = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
                case 'n':
                    out[dst++] = '\n';
                    break;
                case 'r':
                    out[dst++] = '\r';
                    break;
                case 't':
                    out[dst++] = '\t';
                    break;
                case '\\':
                    out[dst++] = '\\';
                    break;
                case '"':
                    out[dst++] = '"';
                    break;
                case '\'':
                    out[dst++] = '\'';
                    break;
                case 'b':
                    out[dst++] = '\b';
                    break;
                case 'f':
                    out[dst++] = '\f';
                    break;
                case '0':
                    out[dst++] = '\0';
                    break;
                case '$':
                    out[dst++] = '$';
                    break;
                case '`':
                    out[dst++] = '`';
                    break;
                default:
                    out[dst++] = '\\';
                    out[dst++] = src[i];
                    break;
            }
        } else {
            out[dst++] = src[i];
        }
    }
    return dst;
}

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

/* ========== Token Operations ========== */

// Advance to next token
void xr_parser_advance(Parser *parser) {
    XR_DCHECK(parser != NULL, "parser_advance: NULL parser");
    parser->previous = parser->current;

    // Skip error tokens until valid token found
    while (1) {
        parser->current = xr_scanner_scan(&parser->scanner);
        if (parser->current.type != TK_ERROR)
            break;

        // TK_ERROR carries diagnostic text in error_message (L-03 contract).
        const char *msg = parser->current.error_message;
        xr_parser_error_at_current(parser, msg ? msg : "lexical error");
    }
}

// Check if current token is of specified type
int xr_parser_check(Parser *parser, XrTokenType type) {
    XR_DCHECK(parser != NULL, "parser_check: NULL parser");
    return parser->current.type == type;
}

// If current token matches, consume it and return true
int xr_parser_match(Parser *parser, XrTokenType type) {
    if (!xr_parser_check(parser, type))
        return 0;
    xr_parser_advance(parser);
    return 1;
}

// Consume token of specified type, or report error
void xr_parser_consume(Parser *parser, XrTokenType type, const char *message) {
    XR_DCHECK(parser != NULL, "parser_consume: NULL parser");
    if (parser->current.type == type) {
        xr_parser_advance(parser);
        return;
    }

    // Better error when a keyword is used where an identifier is expected
    if (type == TK_NAME && parser->current.type >= TK_FIRST_KEYWORD &&
        parser->current.type <= TK_LAST_KEYWORD) {
        xr_parser_error_expected_name(parser, message);
        return;
    }

    xr_parser_error_at_current(parser, message);
}

// Contextual keyword check: current token is TK_NAME with specific string content.
bool xr_parser_check_name(Parser *parser, const char *name) {
    if (parser->current.type != TK_NAME)
        return false;
    int len = (int) strlen(name);
    return parser->current.length == len && memcmp(parser->current.start, name, len) == 0;
}

// Contextual keyword match: if current is TK_NAME matching name, consume and return true.
bool xr_parser_match_name(Parser *parser, const char *name) {
    if (!xr_parser_check_name(parser, name))
        return false;
    xr_parser_advance(parser);
    return true;
}

// Unified keyword-as-identifier error reporting.
// context examples: "expected variable name", "expected field name", etc.
void xr_parser_error_expected_name(Parser *parser, const char *context) {
    if (parser->current.type >= TK_FIRST_KEYWORD && parser->current.type <= TK_LAST_KEYWORD) {
        char buf[128];
        int len = parser->current.length;
        if (len > 60)
            len = 60;
        snprintf(buf, sizeof(buf), "'%.*s' is a keyword and cannot be used as an identifier", len,
                 parser->current.start);
        xr_parser_error_at_current(parser, buf);
    } else {
        xr_parser_error_at_current(parser, context);
    }
}

// Detect common cross-language mistakes at ASI (automatic semicolon insertion) boundaries.
// When a statement ends but the next token on the same line isn't a semicolon,
// check if it's a known cross-language keyword before reporting generic ASI error.
bool xr_parser_check_asi_hint(Parser *parser) {
    if (parser->current.type != TK_NAME)
        return false;

    const char *s = parser->current.start;
    int len = parser->current.length;

    if (len == 3 && memcmp(s, "and", 3) == 0) {
        xr_parser_error_at_current(parser,
                                   "'and' is not an operator. Use '&&' for logical AND in Xray");
        return true;
    }
    if (len == 2 && memcmp(s, "or", 2) == 0) {
        xr_parser_error_at_current(parser,
                                   "'or' is not an operator. Use '||' for logical OR in Xray");
        return true;
    }
    if (len == 3 && memcmp(s, "not", 3) == 0) {
        xr_parser_error_at_current(parser,
                                   "'not' is not an operator. Use '!' for logical NOT in Xray");
        return true;
    }
    if (len == 2 && memcmp(s, "do", 2) == 0) {
        xr_parser_error_at_current(
            parser, "'do...while' is not supported. Use 'while (condition) { }' in Xray");
        return true;
    }
    return false;
}

/* ========== Error Handling ========== */

// Set error callback for LSP integration
void xr_parser_set_error_callback(Parser *parser, XrParseErrorCallback callback, void *user_data,
                                  int max_errors) {
    parser->error_callback = callback;
    parser->error_callback_data = user_data;
    parser->max_errors = max_errors;
    parser->error_count = 0;
}

// Report error at a specific token (shared implementation)
static void xr_parser_error_at(Parser *parser, Token *token, const char *message) {
    if (parser->panic_mode)
        return;  // Avoid error cascade

    parser->panic_mode = 1;
    parser->had_error = 1;
    parser->error_count++;

    // Call error callback if set (for LSP)
    if (parser->error_callback) {
        parser->error_callback(parser->error_callback_data, token->line, 0, token->line,
                               token->length, message);
        return;
    }

    xr_diag_print(XR_DIAG_ERROR, 0, message, parser->source_file, token->line, token->column,
                  token->type == TK_EOF ? 0 : token->length, parser->scanner.source, token->start);
}

// Report error at current token
void xr_parser_error_at_current(Parser *parser, const char *message) {
    XR_DCHECK(parser != NULL, "error_at_current: NULL parser");
    xr_parser_error_at(parser, &parser->current, message);
}

// Report error at previous token
void xr_parser_error_at_previous(Parser *parser, const char *message) {
    XR_DCHECK(parser != NULL, "error_at_previous: NULL parser");
    xr_parser_error_at(parser, &parser->previous, message);
}

// Report error at current position
void xr_parser_error(Parser *parser, const char *message) {
    xr_parser_error_at_current(parser, message);
}

// Emit a "removed syntax" diagnostic with help and explanatory note.
// Prints two lines: the primary error (with code and caret) followed by a
// note line that suggests the modern replacement. Skips emission while in
// panic mode so cascade errors are suppressed.
//
// `title` is the short error headline (e.g. "`void` keyword was removed").
// `note`  is the help/explanation that follows the underline.
void xr_parser_emit_removed_syntax(Parser *parser, Token *token, int code, const char *title,
                                   const char *note) {
    XR_DCHECK(parser != NULL, "emit_removed_syntax: NULL parser");
    XR_DCHECK(token != NULL, "emit_removed_syntax: NULL token");
    XR_DCHECK(title != NULL, "emit_removed_syntax: NULL title");

    if (parser->panic_mode)
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
    xr_diag_print(XR_DIAG_ERROR, code, title, parser->source_file, token->line, token->column,
                  tok_len, parser->scanner.source, token->start);
    if (note) {
        xr_diag_print(XR_DIAG_NOTE, 0, note, parser->source_file, token->line, token->column,
                      tok_len, parser->scanner.source, token->start);
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
    XR_DCHECK(parser != NULL, "parser_synchronize: NULL parser");
    parser->panic_mode = 0;

    // Track bracket depth for better recovery
    int brace_depth = 0;    // { }
    int paren_depth = 0;    // ( )
    int bracket_depth = 0;  // [ ]

    while (parser->current.type != TK_EOF) {
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
                case TK_INTERFACE:
                case TK_ENUM:
                case TK_FN:
                case TK_LET:
                case TK_CONST:
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
                case TK_SHARED:
                // Attribute
                case TK_AT:
                    return;
                default:
                    break;
            }
        }

        xr_parser_advance(parser);
    }
}

/* ========== Expression Parsing ========== */

// Pratt parser core: parse expression by precedence
AstNode *xr_parse_precedence(Parser *parser, Precedence precedence) {
    XR_DCHECK(parser != NULL, "parse_precedence: NULL parser");
    // Special handling for regex literals starting with escape sequences like /\d+/
    // When current is TK_SLASH, try regex scanning first to avoid TK_ERROR from backslash
    if (parser->current.type == TK_SLASH) {
        const char *slash_pos = parser->current.start;
        parser->scanner.current = slash_pos;
        Token regex_token = xr_scanner_try_regex(&parser->scanner);

        if (regex_token.type == TK_LITERAL_REGEX) {
            parser->previous = regex_token;
            parser->current = xr_scanner_scan(&parser->scanner);
            return xr_parse_regex_literal(parser);
        }
        // Not a regex, restore scanner and continue normal parsing
        parser->scanner.current = slash_pos;
    }

    xr_parser_advance(parser);

    PrefixParseFn prefix_rule = xr_get_rule(parser->previous.type)->prefix;
    if (prefix_rule == NULL) {
        xr_parser_error(parser, "expected expression");
        return NULL;
    }

    AstNode *left = prefix_rule(parser);

    if (left == NULL) {
        return NULL;
    }

    // Process infix expressions by precedence
    while (precedence <= xr_get_rule(parser->current.type)->precedence) {
        const ParseRule *rule = xr_get_rule(parser->current.type);

        if (rule->infix == NULL) {
            break;
        }
        // `(` and `[` at statement boundaries: a fresh line opening
        // with one of these is the start of a new statement (a tuple
        // literal LHS, a destructure assignment, or an array literal),
        // not a call/index continuation of the previous expression.
        // xray has no `;` terminator, so without this the sequence
        //     let p = (b, a)
        //     (a, b) = p
        // parses as `let p = (b, a)(a, b) = p` and the LHS of `=`
        // ends up an AST_CALL_EXPR. Same rule as Go.
        if ((parser->current.type == TK_LPAREN || parser->current.type == TK_LBRACKET) &&
            parser->current.line > parser->previous.line) {
            break;
        }
        xr_parser_advance(parser);
        left = rule->infix(parser, left);
    }

    return left;
}

// Parse expression (entry point)
AstNode *xr_parse_expression(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_expression: NULL parser");
    return xr_parse_precedence(parser, PREC_ASSIGNMENT);  // Start from lowest precedence
}

/* ========== Statement Parsing ========== */

// Parse expression statement.
// Multi-value forms (a, b = b, a) are no longer accepted: tuple
// destructure-assignment `(a, b) = (b, a)` is the canonical form and
// is handled by xr_parse_assignment via the AST_TUPLE_LITERAL branch.
AstNode *xr_parse_expr_statement(Parser *parser) {
    int line = parser->current.line;
    (void) line;

    // Use PREC_TERNARY to avoid parsing assignment
    AstNode *first_expr = xr_parse_precedence(parser, PREC_TERNARY);

    // Bare comma at statement position is the obsolete multi-value
    // form. Point users at the tuple destructure equivalent instead
    // of letting it fall through with a generic error.
    if (xr_parser_check(parser, TK_COMMA)) {
        xr_parser_error(parser, "bare multi-value assignment is not supported; "
                                "use tuple destructure: (a, b) = (b, a)");
        return NULL;
    }

    // Regular expression statement, check for assignment operators
    if (xr_get_rule(parser->current.type)->precedence == PREC_ASSIGNMENT) {
        while (xr_get_rule(parser->current.type)->precedence >= PREC_ASSIGNMENT) {
            xr_parser_advance(parser);
            InfixParseFn infix_rule = xr_get_rule(parser->previous.type)->infix;
            if (infix_rule) {
                first_expr = infix_rule(parser, first_expr);
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

    return xr_ast_expr_stmt(parser->X, first_expr, parser->previous.line);
}

// Parse print statement: print(expr1, expr2, ...)
AstNode *xr_parse_print_statement(Parser *parser) {
    int line = parser->previous.line;

    if (xr_parser_check(parser, TK_RPAREN)) {
        return xr_ast_print_stmt(parser->X, NULL, 0, line);
    }

    int capacity = 8;
    int count = 0;
    AstNode **exprs = (AstNode **) ast_alloc_array(parser->X, sizeof(AstNode *), (size_t) capacity);

    exprs[count++] = xr_parse_expression(parser);

    while (xr_parser_check(parser, TK_COMMA)) {
        xr_parser_advance(parser);
        XR_PARSE_PUSH(parser, exprs, count, capacity, xr_parse_expression(parser));
    }

    AstNode *node = xr_ast_print_stmt(parser->X, exprs, count, line);
    return node;
}

// Parse statement
AstNode *xr_parse_statement(Parser *parser) {
    // Control flow
    if (parser->current.type == TK_IF) {
        return xr_parse_if_statement(parser);
    }
    if (parser->current.type == TK_WHILE) {
        return xr_parse_while_statement(parser);
    }
    if (parser->current.type == TK_FOR) {
        // Lookahead: for-in vs traditional for
        Parser checkpoint = *parser;

        xr_parser_advance(parser);

        if (xr_parser_check(parser, TK_LPAREN)) {
            xr_parser_advance(parser);

            // Try to identify for-in pattern (support _ as blank
            // identifier and `(...)` tuple destructuring heads).
            // Also detect keywords used as loop variables to give
            // better errors.
            bool might_be_forin =
                xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_UNDERSCORE);
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
                xr_parser_advance(parser);
                int depth = 1;
                while (depth > 0 && !xr_parser_check(parser, TK_EOF)) {
                    if (xr_parser_check(parser, TK_LPAREN))
                        depth++;
                    else if (xr_parser_check(parser, TK_RPAREN))
                        depth--;
                    if (depth == 0) {
                        xr_parser_advance(parser);
                        break;
                    }
                    xr_parser_advance(parser);
                }
                if (xr_parser_check(parser, TK_IN)) {
                    *parser = checkpoint;
                    return xr_parse_for_in_statement(parser);
                }
            } else if (might_be_forin) {
                xr_parser_advance(parser);

                // Check for comma (key-value pattern)
                if (xr_parser_check(parser, TK_COMMA)) {
                    xr_parser_advance(parser);
                    if (xr_parser_check(parser, TK_NAME) ||
                        xr_parser_check(parser, TK_UNDERSCORE)) {
                        xr_parser_advance(parser);
                    }
                }

                // Skip optional type annotation
                if (xr_parser_check(parser, TK_COLON)) {
                    xr_parser_advance(parser);
                    while (!xr_parser_check(parser, TK_IN) && !xr_parser_check(parser, TK_RPAREN) &&
                           !xr_parser_check(parser, TK_EOF)) {
                        xr_parser_advance(parser);
                    }
                }

                // Check for 'in'
                if (xr_parser_check(parser, TK_IN)) {
                    *parser = checkpoint;
                    return xr_parse_for_in_statement(parser);
                }
            }
        }

        // Otherwise traditional for loop
        *parser = checkpoint;
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
        return xr_parse_block(parser);
    }

    // Prefix increment/decrement not supported (use postfix x++, x--)
    if (parser->current.type == TK_INC || parser->current.type == TK_DEC) {
        xr_parser_error_at_current(parser,
                                   "prefix ++/-- not supported, use postfix form (x++, x--)");
        return NULL;
    }

    // Recognize print builtin (as NAME token)
    if (parser->current.type == TK_NAME && parser->current.length == 5 &&
        memcmp(parser->current.start, "print", 5) == 0) {
        xr_parser_advance(parser);
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after print");
        AstNode *stmt = xr_parse_print_statement(parser);
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after print expression");
        return stmt;
    }

    return xr_parse_expr_statement(parser);
}

/* ========== Parser Main Interface ========== */

// Initialize parser
// arena: optional arena for AST allocation (NULL = use malloc)
void xr_parser_init(Parser *parser, XrayIsolate *X, const char *source, const char *source_file,
                    XrArena *arena) {
    XR_DCHECK(parser != NULL, "parser_init: NULL parser");
    XR_DCHECK(source != NULL, "parser_init: NULL source");
    xr_parser_init_internal(parser, X, source, source_file, arena, false);
}

// Internal init with trivia collection option
static void xr_parser_init_internal(Parser *parser, XrayIsolate *X, const char *source,
                                    const char *source_file, XrArena *arena, bool collect_trivia) {
    parser->X = X;
    parser->arena = arena;
    parser->had_error = 0;
    parser->panic_mode = 0;
    parser->source_file = source_file;

    xr_scanner_init_with_trivia(&parser->scanner, source, collect_trivia);

    memset(&parser->current, 0, sizeof(parser->current));
    parser->current.type = TK_ERROR;
    memset(&parser->previous, 0, sizeof(parser->previous));
    parser->previous.type = TK_ERROR;

    parser->type_scope = xr_type_scope_new(NULL);

    // Initialize error callback fields
    parser->error_callback = NULL;
    parser->error_callback_data = NULL;
    parser->error_count = 0;
    parser->max_errors = 0;

    parser->allow_bare_container = false;
    parser->parsing_native_class = false;
}

// Allocate and install a new arena on the Isolate. Returns the previous arena
// which must be restored by the caller via xr_parse_teardown_arena.
// The returned arena pointer is heap-allocated (xr_malloc) so it outlives
// the stack frame; its memory is reclaimed by xr_program_destroy.
static XrArena *xr_parse_setup_arena(XrayIsolate *X, XrArena **saved_out) {
    XrArena *arena = (XrArena *) xr_malloc(sizeof(XrArena));
    XR_CHECK(arena != NULL, "xr_parse: failed to allocate parse arena");
    xr_arena_init(arena, XR_ARENA_SEGMENT_SIZE);
    *saved_out = xr_isolate_get_current_arena(X);
    xr_isolate_set_current_arena(X, arena);
    XrCompileStringPool *pool = xr_string_pool_new(arena);
    xr_isolate_set_string_pool_compile(X, pool);
    return arena;
}

// Restore previous arena on failure, destroying the owned arena.
static void xr_parse_discard_arena(XrayIsolate *X, XrArena *owned, XrArena *saved) {
    xr_isolate_set_string_pool_compile(X, NULL);
    xr_isolate_set_current_arena(X, saved);
    xr_arena_destroy(owned);
    xr_free(owned);
}

// Parse source code, return AST (main entry)
AstNode *xr_parse(XrayIsolate *X, const char *source) {
    XR_DCHECK(source != NULL, "xr_parse: NULL source");
    return xr_parse_with_source(X, source, NULL);
}

// Parse source code with filename, return AST.
// Creates and owns a dedicated arena; ownership is transferred to the returned
// program node. xr_program_destroy on the returned node releases all memory.
AstNode *xr_parse_with_source(XrayIsolate *X, const char *source, const char *source_file) {
    XR_DCHECK(source != NULL, "xr_parse_with_source: NULL source");

    XrArena *saved_arena = NULL;
    XrArena *arena = xr_parse_setup_arena(X, &saved_arena);

    Parser parser;
    xr_parser_init(&parser, X, source, source_file, arena);

// Default max errors for normal compilation
#define XR_PARSE_MAX_ERRORS 20

    AstNode *program = xr_ast_program(X);

    xr_parser_advance(&parser);

    // Parse declarations until EOF
    while (!xr_parser_check(&parser, TK_EOF)) {
        // Stop if too many errors
        if (parser.error_count >= XR_PARSE_MAX_ERRORS) {
            break;
        }

        // Recover from panic mode to continue parsing
        if (parser.panic_mode) {
            xr_parser_synchronize(&parser);
            if (xr_parser_check(&parser, TK_EOF))
                break;
        }

        int stmt_line = parser.current.line;

        AstNode *decl = xr_parse_declaration(&parser);
        if (decl != NULL) {
            xr_ast_program_add(X, program, decl);
        }

        // Smart semicolon handling
        if (xr_parser_check(&parser, TK_SEMICOLON)) {
            xr_parser_advance(&parser);
        } else {
            // TK_QUESTION and TK_COLON may be part of ternary, not statement separator
            if (!xr_parser_check(&parser, TK_EOF) && parser.current.line == stmt_line &&
                parser.current.type != TK_QUESTION && parser.current.type != TK_COLON) {
                if (!xr_parser_check_asi_hint(&parser)) {
                    xr_parser_error_at_current(
                        &parser, "multiple statements on same line must be separated by semicolon");
                }
            }
        }
    }

    // Print error summary
    if (parser.error_count > 0) {
        xr_diag_print_summary(source_file, parser.error_count, 0,
                              parser.error_count >= XR_PARSE_MAX_ERRORS);
    }

    xr_type_scope_free(parser.type_scope);

    if (parser.had_error) {
        xr_parse_discard_arena(X, arena, saved_arena);
        return NULL;
    }

    // Transfer arena ownership to the program node.
    program->as.program.arena = arena;
    program->as.program.owns_arena = true;
    xr_isolate_set_string_pool_compile(X, NULL);
    xr_isolate_set_current_arena(X, saved_arena);
    return program;
}

// Parse source code with trivia collection (for formatter)
AstNode *xr_parse_with_trivia(XrayIsolate *X, const char *source, const char *source_file) {
    XrArena *saved_arena = NULL;
    XrArena *arena = xr_parse_setup_arena(X, &saved_arena);

    Parser parser;
    xr_parser_init_internal(&parser, X, source, source_file, arena, true);

    AstNode *program = xr_ast_program(X);

    xr_parser_advance(&parser);

    // Attach file-level leading trivia to the program node
    if (parser.current.leading_trivia) {
        program->leading_comments = parser.current.leading_trivia;
        parser.current.leading_trivia = NULL;
    }

    // Parse declarations until EOF
    while (!xr_parser_check(&parser, TK_EOF)) {
        // Stop if too many errors
        if (parser.error_count >= XR_PARSE_MAX_ERRORS) {
            break;
        }

        // Recover from panic mode to continue parsing
        if (parser.panic_mode) {
            xr_parser_synchronize(&parser);
            if (xr_parser_check(&parser, TK_EOF))
                break;
        }

        int stmt_line = parser.current.line;

        // Capture leading trivia before parsing declaration
        XrTrivia *leading_trivia = parser.current.leading_trivia;
        parser.current.leading_trivia = NULL;

        AstNode *decl = xr_parse_declaration(&parser);
        if (decl != NULL) {
            // Attach leading trivia to the declaration
            if (leading_trivia) {
                decl->leading_comments = leading_trivia;
            }
        } else if (leading_trivia) {
            // Free trivia if declaration failed
            xr_trivia_free_chain(leading_trivia);
        }

        // Smart semicolon handling
        if (xr_parser_check(&parser, TK_SEMICOLON)) {
            xr_parser_advance(&parser);
        } else {
            if (!xr_parser_check(&parser, TK_EOF) && parser.current.line == stmt_line &&
                parser.current.type != TK_QUESTION && parser.current.type != TK_COLON) {
                if (!xr_parser_check_asi_hint(&parser)) {
                    xr_parser_error_at_current(
                        &parser, "multiple statements on same line must be separated by semicolon");
                }
            }
        }

        // L-06: capture trailing AFTER smart-semicolon handling. The
        // inline comment overwhelmingly lives on the trailing `;` (or
        // closing `}` for block-bodied decls); waiting until parser
        // .previous reflects that final token captures it correctly.
        if (decl != NULL && parser.previous.trailing_trivia) {
            decl->trailing_comments = parser.previous.trailing_trivia;
            parser.previous.trailing_trivia = NULL;
        }
        if (decl != NULL) {
            xr_ast_program_add(X, program, decl);
        }
    }

    xr_type_scope_free(parser.type_scope);

    if (parser.had_error) {
        xr_parse_discard_arena(X, arena, saved_arena);
        return NULL;
    }

    // Transfer arena ownership to the program node.
    program->as.program.arena = arena;
    program->as.program.owns_arena = true;
    xr_isolate_set_string_pool_compile(X, NULL);
    xr_isolate_set_current_arena(X, saved_arena);
    return program;
}

// Parse a single expression as a self-contained translation unit (REPL/DAP).
//
// Returns an AST_PROGRAM node whose first child is the parsed expression,
// so that callers release everything uniformly via xr_program_destroy().
// Returns NULL on parse error (arena is auto-discarded).
//
// Example:
//   AstNode *prog = xr_parse_expression_string(X, "a + b * 2", "<eval>");
//   if (!prog) { /* report error */ return; }
//   AstNode *expr = prog->as.program.statements[0];
//   ... evaluate expr ...
//   xr_program_destroy(prog);
AstNode *xr_parse_expression_string(XrayIsolate *X, const char *source, const char *source_file) {
    XR_DCHECK(source != NULL, "xr_parse_expression_string: NULL source");

    XrArena *saved_arena = NULL;
    XrArena *arena = xr_parse_setup_arena(X, &saved_arena);

    Parser parser;
    xr_parser_init(&parser, X, source, source_file, arena);

    AstNode *program = xr_ast_program(X);

    xr_parser_advance(&parser);

    AstNode *expr = xr_parse_expression(&parser);
    if (expr != NULL) {
        xr_ast_program_add(X, program, expr);
    }

    xr_type_scope_free(parser.type_scope);
    parser.type_scope = NULL;

    if (parser.had_error || expr == NULL) {
        xr_parse_discard_arena(X, arena, saved_arena);
        return NULL;
    }

    // Transfer arena ownership to the program node.
    program->as.program.arena = arena;
    program->as.program.owns_arena = true;
    xr_isolate_set_string_pool_compile(X, NULL);
    xr_isolate_set_current_arena(X, saved_arena);
    return program;
}

// Parse with error recovery (for LSP)
// Returns partial AST even if there are errors
// Errors are reported via callback
AstNode *xr_parse_recoverable(Parser *parser) {
    if (!parser || !parser->X) {
        xr_log_warning("parser", "parse_recoverable: invalid parser");
        return NULL;
    }

    // Set arena in Isolate for AST allocation (explicit, no TLS)
    XrArena *saved_arena = xr_isolate_get_current_arena(parser->X);
    if (parser->arena) {
        xr_isolate_set_current_arena(parser->X, parser->arena);
    }

    AstNode *program = xr_ast_program(parser->X);
    if (!program) {
        xr_log_warning("parser", "parse_recoverable: failed to create program node");
        xr_isolate_set_current_arena(parser->X, saved_arena);
        return NULL;
    }

    xr_parser_advance(parser);

    int iteration = 0;
    (void) iteration;
    // Parse declarations until EOF or max errors reached
    while (!xr_parser_check(parser, TK_EOF)) {
        iteration++;
        // Check max errors
        if (parser->max_errors > 0 && parser->error_count >= parser->max_errors) {
            break;
        }

        // Recover from panic mode
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_EOF))
                break;
        }

        int stmt_line = parser->current.line;

        AstNode *decl = xr_parse_declaration(parser);
        if (decl != NULL) {
            xr_ast_program_add(parser->X, program, decl);
        }

        // Smart semicolon handling
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            xr_parser_advance(parser);
        } else {
            if (!xr_parser_check(parser, TK_EOF) && parser->current.line == stmt_line &&
                parser->current.type != TK_QUESTION && parser->current.type != TK_COLON) {
                if (!xr_parser_check_asi_hint(parser)) {
                    xr_parser_error_at_current(
                        parser, "multiple statements on same line must be separated by semicolon");
                }
                // Don't break - continue parsing for more errors
            }
        }

        // Don't break on error - continue to find more errors
    }

    xr_type_scope_free(parser->type_scope);
    parser->type_scope = NULL;

    // Restore previous arena and clear compile-time pool
    xr_isolate_set_string_pool_compile(parser->X, NULL);
    xr_isolate_set_current_arena(parser->X, saved_arena);

    // Return partial AST even if there were errors
    return program;
}

/* ========== Variable Parsing ========== */

// Try parsing generic type args <T1, T2, ...>
// Uses lookahead to detect identifier<...>( pattern, avoiding confusion with comparison
// Uses space sensitivity: foo<T>() is generic, foo < T is comparison
// Returns: number of type args parsed, 0 means not a generic call
static int try_parse_generic_type_args(Parser *parser, XrType **type_args, int capacity) {
    // disambiguation: if '<' has leading space, treat as comparison
    if (parser->current.type == TK_LT && parser->current.has_leading_space) {
        return 0;  // Space before '<' means comparison, not generic
    }

    // Save parser state for rollback
    Scanner saved_scanner = parser->scanner;
    Token saved_current = parser->current;
    Token saved_previous = parser->previous;
    int saved_had_error = parser->had_error;
    int saved_panic_mode = parser->panic_mode;

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

        XrType *type = xr_parse_type_annotation(parser);

        if (parser->had_error && !saved_had_error) {
            goto rollback;
        }

        if (!type) {
            goto rollback;
        }

        type_args[count++] = type;

    } while (xr_parser_match(parser, TK_COMMA));

    // Must end with >
    if (!xr_parser_match(parser, TK_GT)) {
        goto rollback;
    }

    // Must be followed by ( for generic call or { for generic struct literal
    if (!xr_parser_check(parser, TK_LPAREN) && !xr_parser_check(parser, TK_LBRACE)) {
        goto rollback;
    }

    return count;

rollback:
    // Restore parser state
    parser->scanner = saved_scanner;
    parser->current = saved_current;
    parser->previous = saved_previous;
    parser->had_error = saved_had_error;
    parser->panic_mode = saved_panic_mode;
    return 0;
}

// Parse variable reference: x
// Supports generic call syntax: foo<int, string>(arg1, arg2)
AstNode *xr_parse_variable(Parser *parser) {
    // Context keyword intercept: "linked go" / "supervisor scope" as expression
    Token prev = parser->previous;
    if (prev.length == 6 && memcmp(prev.start, "linked", 6) == 0 &&
        xr_parser_check(parser, TK_GO)) {
        xr_parser_advance(parser);  // consume "go"
        return xr_parse_go_expr_with_link(parser, XR_LINK_LINKED);
    }
    if (prev.length == 10 && memcmp(prev.start, "supervisor", 10) == 0 &&
        xr_parser_check(parser, TK_SCOPE)) {
        xr_parser_advance(parser);  // consume "scope"
        return xr_parse_scope_block_with_mode(parser, XR_SCOPE_SUPERVISOR);
    }

    // Contextual keyword intercept: "cancelled()" expression
    if (prev.length == 9 && memcmp(prev.start, "cancelled", 9) == 0 &&
        xr_parser_check(parser, TK_LPAREN)) {
        return xr_parse_cancelled_expr(parser);
    }
    // Contextual keyword intercept: "Channel(...)" constructs a dedicated
    // AST_CHANNEL_NEW node that codegen / shared-variable preregister /
    // select compilation pattern-match on. Plain references like
    // `Channel.method(...)` (followed by '.') keep flowing through the
    // regular variable path — this lookahead only triggers when the
    // very next token is '('.
    if (prev.length == 7 && memcmp(prev.start, "Channel", 7) == 0 &&
        xr_parser_check(parser, TK_LPAREN)) {
        return xr_parse_channel_new(parser);
    }
    // Contextual keyword intercept: "move var" expression
    // Only trigger when followed by an identifier (the variable to move)
    if (prev.length == 4 && memcmp(prev.start, "move", 4) == 0 &&
        xr_parser_check(parser, TK_NAME)) {
        return xr_parse_move_expr(parser);
    }

    // Detect 'not' keyword misuse: not true, not x, not (expr)
    if (prev.length == 3 && memcmp(prev.start, "not", 3) == 0) {
        const ParseRule *next_rule = xr_get_rule(parser->current.type);
        if (next_rule->prefix != NULL) {
            xr_parser_error_at_previous(
                parser, "'not' is not an operator. Use '!' for logical NOT in Xray");
        }
    }
    // Detect 'lambda' keyword misuse: lambda x: x + 1
    if (prev.length == 6 && memcmp(prev.start, "lambda", 6) == 0) {
        const ParseRule *next_rule = xr_get_rule(parser->current.type);
        if (next_rule->prefix != NULL || parser->current.type == TK_NAME) {
            xr_parser_error_at_previous(
                parser,
                "'lambda' is not supported. Use 'fn(params) { }' or '(params) => expr' in Xray");
        }
    }

    char *name = (char *) ast_alloc(parser->X, (size_t) parser->previous.length + 1);
    memcpy(name, parser->previous.start, parser->previous.length);
    name[parser->previous.length] = '\0';
    int line = parser->previous.line;
    int column = parser->previous.column;

    // Try parsing generic type args <T1, T2, ...>
    XrType *type_args[16];  // Max 16 type args
    int type_arg_count = try_parse_generic_type_args(parser, type_args, 16);

    if (type_arg_count > 0) {
        // Check if this is a generic struct literal: Name<T1, T2>{field: value}
        if (xr_parser_check(parser, TK_LBRACE)) {
            xr_parser_advance(parser);  // Consume {

            char **field_names = NULL;
            AstNode **field_values = NULL;
            int field_count = 0;
            int field_capacity = 0;

            if (!xr_parser_check(parser, TK_RBRACE)) {
                do {
                    if (field_count >= field_capacity) {
                        int old_field_capacity = field_capacity;
                        field_capacity = field_capacity == 0 ? 4 : field_capacity * 2;

                        char **new_names = (char **) ast_alloc_array(parser->X, sizeof(char *),
                                                                     (size_t) field_capacity);
                        if (old_field_capacity > 0 && field_names) {
                            memcpy(new_names, field_names,
                                   sizeof(char *) * (size_t) old_field_capacity);
                        }
                        field_names = new_names;

                        AstNode **new_values = (AstNode **) ast_alloc_array(
                            parser->X, sizeof(AstNode *), (size_t) field_capacity);
                        if (old_field_capacity > 0 && field_values) {
                            memcpy(new_values, field_values,
                                   sizeof(AstNode *) * (size_t) old_field_capacity);
                        }
                        field_values = new_values;
                    }

                    xr_parser_consume(parser, TK_NAME, "expected field name in struct literal");
                    char *fname =
                        (char *) ast_alloc(parser->X, (size_t) parser->previous.length + 1);
                    memcpy(fname, parser->previous.start, parser->previous.length);
                    fname[parser->previous.length] = '\0';
                    field_names[field_count] = fname;

                    xr_parser_consume(parser, TK_COLON, "expected ':' after field name");
                    field_values[field_count] = xr_parse_expression(parser);
                    field_count++;
                } while (xr_parser_match(parser, TK_COMMA));
            }

            xr_parser_consume(parser, TK_RBRACE, "expected '}' to end struct literal");

            AstNode *node = xr_ast_struct_literal(parser->X, name, field_names, field_values,
                                                  field_count, line);
            node->column = column;
            // Attach generic type arguments for monomorphization
            XrType **ta =
                (XrType **) ast_alloc_array(parser->X, sizeof(XrType *), (size_t) type_arg_count);
            memcpy(ta, type_args, sizeof(XrType *) * type_arg_count);
            node->as.struct_literal.type_args = ta;
            node->as.struct_literal.type_arg_count = type_arg_count;
            return node;
        }

        // Generic call detected: identifier<T1, T2>(args)
        AstNode *callee = xr_ast_variable(parser->X, name, line);
        callee->column = column;

        xr_parser_advance(parser);  // Consume (

        // Parse argument list
        AstNode **arguments = NULL;
        int arg_count = 0;
        int arg_capacity = 0;

        if (!xr_parser_check(parser, TK_RPAREN)) {
            do {
                XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity,
                              xr_parse_expression(parser));
            } while (xr_parser_match(parser, TK_COMMA));
        }

        xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");

        return xr_ast_call_expr_generic(parser->X, callee, arguments, arg_count, type_args,
                                        type_arg_count, line);
    }

    // Check for struct literal: Name{field: value, ...}
    // Conditions: '{' on same line, no leading space, followed by 'name:' pattern
    if (xr_parser_check(parser, TK_LBRACE) && !parser->current.has_leading_space &&
        parser->current.line == line) {
        // Lookahead: check if this is { name: ... } pattern (struct literal)
        // vs block statement (which would have statements, not name:value pairs)
        Scanner saved_scanner = parser->scanner;
        Token saved_current = parser->current;
        Token saved_previous = parser->previous;
        int saved_had_error = parser->had_error;
        int saved_panic_mode = parser->panic_mode;

        parser->panic_mode = 1;     // suppress errors during lookahead
        xr_parser_advance(parser);  // consume '{'

        bool is_struct_literal = false;
        if (xr_parser_check(parser, TK_NAME)) {
            xr_parser_advance(parser);  // consume field name
            if (xr_parser_check(parser, TK_COLON)) {
                is_struct_literal = true;
            }
        } else if (xr_parser_check(parser, TK_RBRACE)) {
            // Empty struct literal: Point{}
            is_struct_literal = true;
        }

        // Restore parser state
        parser->scanner = saved_scanner;
        parser->current = saved_current;
        parser->previous = saved_previous;
        parser->had_error = saved_had_error;
        parser->panic_mode = saved_panic_mode;

        if (is_struct_literal) {
            // Parse struct literal
            xr_parser_advance(parser);  // consume '{'

            char **field_names = NULL;
            AstNode **field_values = NULL;
            int field_count = 0;
            int field_capacity = 0;

            if (!xr_parser_check(parser, TK_RBRACE)) {
                do {
                    if (field_count >= field_capacity) {
                        int old_field_capacity = field_capacity;
                        field_capacity = field_capacity == 0 ? 4 : field_capacity * 2;

                        char **new_names = (char **) ast_alloc_array(parser->X, sizeof(char *),
                                                                     (size_t) field_capacity);
                        if (old_field_capacity > 0 && field_names) {
                            memcpy(new_names, field_names,
                                   sizeof(char *) * (size_t) old_field_capacity);
                        }
                        field_names = new_names;

                        AstNode **new_values = (AstNode **) ast_alloc_array(
                            parser->X, sizeof(AstNode *), (size_t) field_capacity);
                        if (old_field_capacity > 0 && field_values) {
                            memcpy(new_values, field_values,
                                   sizeof(AstNode *) * (size_t) old_field_capacity);
                        }
                        field_values = new_values;
                    }

                    xr_parser_consume(parser, TK_NAME, "expected field name in struct literal");
                    char *fname =
                        (char *) ast_alloc(parser->X, (size_t) parser->previous.length + 1);
                    memcpy(fname, parser->previous.start, parser->previous.length);
                    fname[parser->previous.length] = '\0';
                    field_names[field_count] = fname;

                    xr_parser_consume(parser, TK_COLON, "expected ':' after field name");
                    field_values[field_count] = xr_parse_expression(parser);
                    field_count++;
                } while (xr_parser_match(parser, TK_COMMA));
            }

            xr_parser_consume(parser, TK_RBRACE, "expected '}' to end struct literal");

            AstNode *node = xr_ast_struct_literal(parser->X, name, field_names, field_values,
                                                  field_count, line);
            node->column = column;
            return node;
        }
    }

    // Regular variable reference
    AstNode *node = xr_ast_variable(parser->X, name, line);
    node->column = column;
    return node;
}

// Parse assignment: x = expression
AstNode *xr_parse_assignment(Parser *parser, AstNode *left) {
    int line = left->line;
    int column = left->column;

    // Variable assignment: x = 10
    if (left->type == AST_VARIABLE) {
        char *name = ast_strdup(parser->X, left->as.variable.name);
        AstNode *value = xr_parse_expression(parser);

        AstNode *node = xr_ast_assignment(parser->X, name, value, line);
        node->column = column;  // Preserve column for rename
        return node;
    }
    // Index assignment: arr[0] = 10
    else if (left->type == AST_INDEX_GET) {
        AstNode *array = left->as.index_get.array;
        AstNode *index = left->as.index_get.index;

        AstNode *value = xr_parse_expression(parser);
        AstNode *node = xr_ast_index_set(parser->X, array, index, value, line);

        // Don't free left - arena handles it, or reused in node
        return node;
    }
    // Member assignment: obj.field = value
    else if (left->type == AST_MEMBER_ACCESS) {
        AstNode *object = left->as.member_access.object;
        char *member = ast_strdup(parser->X, left->as.member_access.name);

        AstNode *value = xr_parse_expression(parser);
        AstNode *node = xr_ast_member_set(parser->X, object, member, value, line);

        // Arena bulk-frees left/member; no individual free needed.
        return node;
    }
    // Destructure assignment: [a, b] = arr or (a, b) = pair or {x, y} = obj
    else if (left->type == AST_ARRAY_LITERAL) {
        XrDestructurePattern *pattern = convert_array_literal_to_pattern(parser->X, left);
        if (!pattern) {
            xr_parser_error(parser, "destructure target must be variable list, e.g. [a, b]");
            return NULL;
        }

        AstNode *value = xr_parse_expression(parser);
        return xr_ast_destructure_assign(parser->X, pattern, value, line);
    } else if (left->type == AST_TUPLE_LITERAL) {
        XrDestructurePattern *pattern = convert_tuple_literal_to_pattern(parser->X, left);
        if (!pattern) {
            xr_parser_error(parser, "destructure target must be variable list, e.g. (a, b)");
            return NULL;
        }
        AstNode *value = xr_parse_expression(parser);
        return xr_ast_destructure_assign(parser->X, pattern, value, line);
    } else if (left->type == AST_OBJECT_LITERAL) {
        XrDestructurePattern *pattern = convert_object_literal_to_pattern(parser->X, left);
        if (!pattern) {
            xr_parser_error(parser, "destructure target must be variable list, e.g. {x, y}");
            return NULL;
        }

        AstNode *value = xr_parse_expression(parser);
        return xr_ast_destructure_assign(parser->X, pattern, value, line);
    }
    // Invalid assignment target
    else {
        xr_parser_error(parser,
                        "assignment target must be variable, index, member or destructure pattern");
        return NULL;
    }
}

// Parse compound assignment: x += 10, x -= 5, this.field += 10, etc.
AstNode *xr_parse_compound_assignment(Parser *parser, AstNode *left) {
    int line = left->line;
    XrTokenType op_token = parser->previous.type;

    if (left->type == AST_VARIABLE) {
        // Variable compound assignment: x += 10
        char *var_name = ast_strdup(parser->X, left->as.variable.name);
        AstNode *right = xr_parse_expression(parser);
        AstNode *compound_assignment =
            xr_ast_compound_assignment(parser->X, var_name, op_token, right, line);
        return compound_assignment;
    } else if (left->type == AST_MEMBER_ACCESS) {
        // Member compound assignment: this.field += 10
        AstNode *object = left->as.member_access.object;
        char *member_name = ast_strdup(parser->X, left->as.member_access.name);

        AstNode *right = xr_parse_expression(parser);
        AstNode *compound_assignment = xr_ast_member_compound_assignment(
            parser->X, object, member_name, op_token, right, line);
        return compound_assignment;
    } else {
        xr_parser_error(parser, "compound assignment only for variables or member access");
        return NULL;
    }
}

// Parse prefix increment/decrement: ++x, --x
// xray syntax: only postfix form (x++, x--) is supported
AstNode *xr_parse_inc_dec(Parser *parser) {
    xr_parser_error(parser, "prefix ++/-- not supported, use postfix form (x++, x--)");
    return NULL;
}

// Check if token is binary operator (for detecting x++ embedded in expression)
static bool is_binary_operator(XrTokenType type) {
    switch (type) {
        case TK_PLUS:
        case TK_MINUS:
        case TK_STAR:
        case TK_SLASH:
        case TK_PERCENT:
        case TK_EQ:
        case TK_NE:
        case TK_LT:
        case TK_LE:
        case TK_GT:
        case TK_GE:
        case TK_AND:
        case TK_OR:
        case TK_ASSIGN:
        case TK_PLUS_ASSIGN:
        case TK_MINUS_ASSIGN:
        case TK_MUL_ASSIGN:
        case TK_DIV_ASSIGN:
        case TK_MOD_ASSIGN:
        case TK_QUESTION:
        case TK_COMMA:
            return true;
        default:
            return false;
    }
}

// Parse postfix increment/decrement: x++, x--
// Only postfix form supported; must be standalone statement
AstNode *xr_parse_postfix_inc_dec(Parser *parser, AstNode *left) {
    XrTokenType op_token = parser->previous.type;
    int line = left->line;

    if (left->type != AST_VARIABLE) {
        xr_parser_error(parser, "++/-- only for variables");
        return NULL;
    }

    // Check if embedded in expression
    if (is_binary_operator(parser->current.type)) {
        xr_parser_error(parser, "++/-- must be standalone statement, cannot be in expression (e.g. "
                                "y = x++ or a + x++)");
        return NULL;
    }

    char *var_name = ast_strdup(parser->X, left->as.variable.name);
    AstNode *node;
    if (op_token == TK_INC) {
        node = xr_ast_inc(parser->X, var_name, line);
    } else {
        node = xr_ast_dec(parser->X, var_name, line);
    }

    return node;
}

// Parse single variable declaration: let x = 10 or const PI = 3.14
AstNode *xr_parse_single_var_declaration(Parser *parser, int is_const) {
    xr_parser_consume(parser, TK_NAME, "expected variable name");
    char *name = (char *) ast_alloc(parser->X, (size_t) parser->previous.length + 1);
    memcpy(name, parser->previous.start, parser->previous.length);
    name[parser->previous.length] = '\0';
    int line = parser->previous.line;
    int column = parser->previous.column;
    int name_length = parser->previous.length;

    XrType *type_annotation = NULL;
    AstNode *initializer = NULL;

    // Parse optional type annotation (: Type)
    if (xr_parser_match(parser, TK_COLON)) {
        type_annotation = xr_parse_type_annotation(parser);
    }

    if (xr_parser_match(parser, TK_ASSIGN)) {
        initializer = xr_parse_expression(parser);
    } else if (is_const) {
        xr_parser_error(parser, "constants must be initialized");
        return NULL;
    }
    // let variables can be uninitialized
    AstNode *node = xr_ast_var_decl(parser->X, name, initializer, is_const, line);
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
    int line = parser->previous.line;
    AstNode *block = xr_ast_block(parser->X, line);

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        // Error recovery to avoid infinite loop
        if (parser->panic_mode) {
            xr_parser_synchronize(parser);
            if (xr_parser_check(parser, TK_RBRACE) || xr_parser_check(parser, TK_EOF))
                break;
            continue;
        }

        int stmt_line = parser->current.line;

        // Capture leading trivia before parsing statement
        XrTrivia *leading_trivia = parser->current.leading_trivia;
        parser->current.leading_trivia = NULL;

        AstNode *decl = xr_parse_declaration(parser);
        if (decl != NULL) {
            // Attach leading trivia to the statement
            if (leading_trivia && !decl->leading_comments) {
                decl->leading_comments = leading_trivia;
            } else if (leading_trivia) {
                xr_trivia_free_chain(leading_trivia);
            }
        } else if (leading_trivia) {
            xr_trivia_free_chain(leading_trivia);
        }

        // Smart semicolon handling (block-internal variant). The
        // L-06 trailing capture happens AFTER this so we see the `;`
        // or `}`-terminator's trailing trivia, not the final
        // expression token's.
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            xr_parser_advance(parser);
        } else {
            if (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF) &&
                parser->current.line == stmt_line && parser->current.type != TK_QUESTION &&
                parser->current.type != TK_COLON) {
                if (!xr_parser_check_asi_hint(parser)) {
                    xr_parser_error_at_current(
                        parser, "multiple statements on same line must be separated by semicolon");
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
            xr_ast_block_add(parser->X, block, decl);
        }

        if (parser->had_error)
            break;
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to close block");

    // Record closing `}` location (exclusive end). `parser->previous` now
    // points to the consumed `}`; its column is 1-indexed, so the exclusive
    // end column is column + 1.
    block->end_line = parser->previous.line;
    block->end_column = parser->previous.column + 1;

    return block;
}

// Control flow parsing moved to xparse_stmt.c
