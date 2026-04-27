/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_internal.h - Parser internal shared declarations
 *
 * KEY CONCEPT (P-02):
 *   Everything that USED to live in xparse.h but is only consumed by
 *   the parser sources moved here. Downstream subsystems must NOT
 *   include this header; only xparse.h is the public contract.
 */

#ifndef XPARSE_INTERNAL_H
#define XPARSE_INTERNAL_H

#include "xparse.h"
#include "../../base/xmalloc.h"
#include "../../base/xarena.h"
#include "../../base/xdefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Pratt Parser Tables ========== */

// Operator precedence (higher value = higher precedence).
typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,        // = (lowest)
    PREC_TERNARY,           // ? : (ternary, just above assignment)
    PREC_NULLISH_COALESCE,  // ?? (nullish coalescing)
    PREC_OR,                // ||
    PREC_AND,               // &&
    PREC_BIT_OR,            // | (bitwise or)
    PREC_BIT_XOR,           // ^ (bitwise xor)
    PREC_BIT_AND,           // & (bitwise and)
    PREC_EQUALITY,          // == !=
    PREC_COMPARISON,        // < > <= >=
    PREC_SHIFT,             // << >>
    PREC_TERM,              // + -
    PREC_FACTOR,            // * / %
    PREC_UNARY,             // ! - ~
    PREC_CALL,              // . () []
    PREC_POSTFIX,           // ++ --
    PREC_PRIMARY            // literals, parentheses
} Precedence;

typedef AstNode *(*PrefixParseFn)(Parser *parser);
typedef AstNode *(*InfixParseFn)(Parser *parser, AstNode *left);

typedef struct ParseRule {
    PrefixParseFn prefix;
    InfixParseFn infix;
    Precedence precedence;
} ParseRule;

XR_FUNC const ParseRule *xr_get_rule(XrTokenType type);

/* ========== Arena-backed AST Allocation ==========
 *
 * The current arena must be installed on the Isolate before parsing.
 * A missing arena is a programming error and aborts via XR_CHECK.
 */
XR_FUNC void *ast_alloc(XrayIsolate *X, size_t size);
XR_FUNC void *ast_alloc_array(XrayIsolate *X, size_t elem_size, size_t count);
XR_FUNC char *ast_strdup(XrayIsolate *X, const char *s);

// Arena-based dynamic-array push: doubles capacity by reallocating into the
// arena. Original buffer is leaked into the arena and reclaimed in bulk.
#define XR_PARSE_PUSH(parser, arr, count, cap, item)                                               \
    do {                                                                                           \
        if ((count) >= (cap)) {                                                                    \
            int _new_cap = (cap) == 0 ? 4 : (cap) * 2;                                             \
            void *_new_arr = ast_alloc_array((parser)->X, sizeof(*(arr)), (size_t) _new_cap);      \
            if ((arr) != NULL && (count) > 0) {                                                    \
                memcpy(_new_arr, (arr), sizeof(*(arr)) * (size_t) (count));                        \
            }                                                                                      \
            (arr) = _new_arr;                                                                      \
            (cap) = _new_cap;                                                                      \
        }                                                                                          \
        (arr)[(count)++] = (item);                                                                 \
    } while (0)

/* ========== Token-Stream Helpers ========== */

XR_FUNC void xr_parser_advance(Parser *parser);
XR_FUNC int xr_parser_check(Parser *parser, XrTokenType type);
XR_FUNC int xr_parser_match(Parser *parser, XrTokenType type);
XR_FUNC void xr_parser_consume(Parser *parser, XrTokenType type, const char *message);

// Soft-keyword helpers: TK_NAME tokens whose lexeme equals the given string.
XR_FUNC bool xr_parser_check_name(Parser *parser, const char *name);
XR_FUNC bool xr_parser_match_name(Parser *parser, const char *name);

/* ========== Error Helpers ========== */

XR_FUNC void xr_parser_error(Parser *parser, const char *message);
XR_FUNC void xr_parser_error_at_current(Parser *parser, const char *message);
XR_FUNC void xr_parser_error_at_previous(Parser *parser, const char *message);
XR_FUNC void xr_parser_synchronize(Parser *parser);
XR_FUNC void xr_parser_error_expected_name(Parser *parser, const char *context);
XR_FUNC bool xr_parser_check_asi_hint(Parser *parser);

/* ========== Expression Parsing ========== */

XR_FUNC AstNode *xr_parse_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_precedence(Parser *parser, Precedence precedence);
XR_FUNC AstNode *xr_parse_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_grouping(Parser *parser);
XR_FUNC AstNode *xr_parse_unary(Parser *parser);
XR_FUNC AstNode *xr_parse_binary(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_variable(Parser *parser);
XR_FUNC AstNode *xr_parse_assignment(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_compound_assignment(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_call_expr(Parser *parser, AstNode *callee);
XR_FUNC AstNode *xr_parse_array_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_object_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_empty_map_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_set_literal_new(Parser *parser);
XR_FUNC AstNode *xr_parse_index_access(Parser *parser, AstNode *array);
XR_FUNC AstNode *xr_parse_member_access(Parser *parser, AstNode *object);
XR_FUNC AstNode *xr_parse_match_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_new_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_this_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_super_expression(Parser *parser);

XR_FUNC AstNode *xr_parse_regex_prefix(Parser *parser);
XR_FUNC AstNode *xr_parse_regex_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_fn_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_is(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_type_cast(Parser *parser);
XR_FUNC AstNode *xr_parse_type_keyword_as_variable(Parser *parser);
XR_FUNC AstNode *xr_parse_container_constructor(Parser *parser);
XR_FUNC AstNode *xr_parse_template_string(Parser *parser);
XR_FUNC AstNode *xr_parse_lt_or_generic(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_force_unwrap(Parser *parser, AstNode *operand);
XR_FUNC AstNode *xr_parse_as_cast(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_range(Parser *parser, AstNode *start);
XR_FUNC AstNode *xr_parse_ternary(Parser *parser, AstNode *condition);
XR_FUNC AstNode *xr_parse_nullish_coalesce(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_optional_chain(Parser *parser, AstNode *object);
XR_FUNC AstNode *xr_parse_inc_dec(Parser *parser);
XR_FUNC AstNode *xr_parse_postfix_inc_dec(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_arrow_function_body(Parser *parser, XrParamNode **params, int param_count,
                                              int line);

/* ========== Statement Parsing ========== */

XR_FUNC AstNode *xr_parse_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_expr_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_print_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_block(Parser *parser);
XR_FUNC AstNode *xr_parse_if_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_while_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_for_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_for_in_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_break_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_continue_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_return_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_try_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_throw_statement(Parser *parser);

/* ========== Declaration Parsing ========== */

XR_FUNC AstNode *xr_parse_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_var_declaration(Parser *parser, int is_const);
XR_FUNC AstNode *xr_parse_single_var_declaration(Parser *parser, int is_const);
XR_FUNC AstNode *xr_parse_function_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_type_alias_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_enum_declaration(Parser *parser);

/* ========== OOP Parsing ========== */

XR_FUNC AstNode *xr_parse_class_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_struct_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_interface_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_interface_method(Parser *parser);
XR_FUNC AstNode *xr_parse_field_declaration(Parser *parser, bool *is_method_out);
XR_FUNC AstNode *xr_parse_method_declaration(Parser *parser, const char *name, int name_line,
                                             int name_column, bool is_private, bool is_static,
                                             bool is_abstract);
XR_FUNC AstNode *xr_parse_operator_method(Parser *parser, bool is_private, bool is_static);
XR_FUNC AstNode *xr_parse_static_constructor(Parser *parser, bool is_private);

/* ========== Module System ========== */

XR_FUNC AstNode *xr_parse_import_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_import_from_declaration(Parser *parser, int line);
XR_FUNC AstNode *xr_parse_export_declaration(Parser *parser);

/* ========== Type Annotations ========== */

XR_FUNC XrType *xr_parse_type_annotation(Parser *parser);

/* ========== Destructuring ========== */

XR_FUNC XrDestructurePattern *xr_parse_array_pattern(Parser *parser);
XR_FUNC XrDestructurePattern *xr_parse_object_pattern(Parser *parser);
XR_FUNC XrDestructurePattern *xr_parse_destructure_pattern(Parser *parser);
XR_FUNC AstNode *xr_parse_destructure_declaration(Parser *parser, bool is_const);

XR_FUNC XrDestructurePattern *convert_array_literal_to_pattern(XrayIsolate *X,
                                                               AstNode *array_literal);
XR_FUNC XrDestructurePattern *convert_object_literal_to_pattern(XrayIsolate *X,
                                                                AstNode *object_literal);

/* ========== Coroutine Parsing ========== */

XR_FUNC AstNode *xr_parse_go_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_go_expr_with_link(Parser *parser, uint8_t link_mode);
XR_FUNC AstNode *xr_parse_await_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_channel_new(Parser *parser);
XR_FUNC AstNode *xr_parse_cancelled_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_move_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_defer_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_select_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_scope_block(Parser *parser);
XR_FUNC AstNode *xr_parse_scope_block_with_mode(Parser *parser, uint8_t scope_mode);

/* ========== Misc Helpers ========== */

XR_FUNC size_t xr_process_escapes(const char *src, size_t src_len, char *out);

#endif  // XPARSE_INTERNAL_H
