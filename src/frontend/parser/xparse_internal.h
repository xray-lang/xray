/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_internal.h - Parser internal shared declarations
 *
 * KEY CONCEPT:
 *   Internal declarations shared between parser source files.
 */

#ifndef XPARSE_INTERNAL_H
#define XPARSE_INTERNAL_H

#include "xparse.h"
#include "../../base/xmalloc.h"
#include "../../base/xarena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../base/xdefs.h"

// Arena-mandatory allocation helpers used by all parser/AST factory code.
// The current arena must be installed on the Isolate before parsing.
// A missing arena is a programming error and aborts via XR_CHECK.
XR_FUNC void *ast_alloc(XrayIsolate *X, size_t size);
XR_FUNC void *ast_alloc_array(XrayIsolate *X, size_t elem_size, size_t count);
XR_FUNC char *ast_strdup(XrayIsolate *X, const char *s);

// Arena-based dynamic array grow: doubles capacity and copies into the arena.
// No original memory is freed (arena does bulk release).
// `parser` argument provides access to parser->X (Isolate) for allocation.
#define XR_PARSE_PUSH(parser, arr, count, cap, item) do { \
    if ((count) >= (cap)) { \
        int _new_cap = (cap) == 0 ? 4 : (cap) * 2; \
        void *_new_arr = ast_alloc_array((parser)->X, sizeof(*(arr)), (size_t)_new_cap); \
        if ((arr) != NULL && (count) > 0) { \
            memcpy(_new_arr, (arr), sizeof(*(arr)) * (size_t)(count)); \
        } \
        (arr) = _new_arr; \
        (cap) = _new_cap; \
    } \
    (arr)[(count)++] = (item); \
} while (0)

XR_FUNC AstNode *xr_parse_precedence(Parser *parser, Precedence precedence);
XR_FUNC AstNode *xr_parse_expression(Parser *parser);
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
XR_FUNC AstNode *xr_parse_set_literal_new(Parser *parser);
XR_FUNC AstNode *xr_parse_index_access(Parser *parser, AstNode *array);
XR_FUNC AstNode *xr_parse_member_access(Parser *parser, AstNode *object);
XR_FUNC AstNode *xr_parse_yield_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_match_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_new_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_this_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_super_expression(Parser *parser);

/* ========== Statement Parsing ========== */

XR_FUNC AstNode *xr_parse_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_expr_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_print_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_single_var_declaration(Parser *parser, int is_const);
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
XR_FUNC AstNode *xr_parse_function_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_import_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_export_declaration(Parser *parser);

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

/* ========== Expression Helpers (defined in xparse_expr.c) ========== */

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
XR_FUNC AstNode *xr_parse_arrow_function_body(Parser *parser, XrParamNode **params, int param_count, int line);

/* ========== Declaration Helpers (defined in xparse_decl.c) ========== */

XR_FUNC XrDestructurePattern* convert_array_literal_to_pattern(XrayIsolate *X, AstNode *array_literal);
XR_FUNC XrDestructurePattern* convert_object_literal_to_pattern(XrayIsolate *X, AstNode *object_literal);

/* ========== Helpers ========== */

XR_FUNC const ParseRule *xr_get_rule(TokenType type);
XR_FUNC size_t xr_process_escapes(const char *src, size_t src_len, char *out);

#endif // XPARSE_INTERNAL_H
