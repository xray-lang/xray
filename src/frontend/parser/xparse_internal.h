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
#include "xtype_scope.h"
#include "../lexer/xquoted_literal.h"
#include "../../base/xmalloc.h"
#include "../../base/xarena.h"
#include "../../base/xdefs.h"
#include <stdio.h>
#include <limits.h>
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
    PREC_RANGE,             // .. ..= (non-associative; endpoints are arithmetic)
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
 * The current arena must be installed on the compiler session before parsing.
 * Failures remain in the session's shared resource state across lookahead.
 */
XR_FUNC void *ast_alloc(XrCompilerSession *session, size_t size);
XR_FUNC void *ast_alloc_array(XrCompilerSession *session, size_t elem_size, size_t count);
XR_FUNC char *ast_strdup(XrCompilerSession *session, const char *s);
XR_FUNC char *ast_strndup(XrCompilerSession *session, const char *text, size_t length);
XR_FUNC void *ast_array_grow(XrCompilerSession *session, const void *old, size_t element,
                             size_t old_count, size_t new_count);
XR_FUNC bool ast_work(XrCompilerSession *session, size_t units);
XR_FUNC bool ast_copy(XrCompilerSession *session, void *destination, const void *source, size_t bytes);

// Arena-based dynamic-array push: doubles capacity by reallocating into the
// arena. Original buffer is leaked into the arena and reclaimed in bulk.
#define XR_PARSE_PUSH(parser, arr, count, cap, item)                                  \
    do {                                                                            \
        XrCompilerSession *_session = (parser)->compiler_session;                    \
        if (!xr_parser_healthy(parser)) break;                                      \
        if ((count) < 0 || (cap) < 0 || (count) > (cap)) {                          \
            xr_compile_state_fail((parser)->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT); \
            break;                                                                  \
        }                                                                           \
        if ((count) == (cap)) {                                                     \
            if ((cap) > INT_MAX / 2) {                                             \
                xr_compile_state_fail((parser)->state, XR_COMPILE_RESOURCE_BUDGET); \
                break;                                                              \
            }                                                                       \
            int _capacity = (cap) ? (cap) * 2 : 4;                                 \
            void *_grown = ast_array_grow(_session, (arr), sizeof(*(arr)),           \
                                           (size_t) (count), (size_t) _capacity);    \
            if (!_grown) break;                                                     \
            (arr) = _grown;                                                         \
            (cap) = _capacity;                                                      \
        }                                                                           \
        if (!ast_work(_session, sizeof(*(arr)))) break;                             \
        (arr)[(count)++] = (item);                                                  \
    } while (0)

typedef struct XrParsedQuoted {
    const uint8_t *bytes;
    size_t length;
} XrParsedQuoted;
XR_FUNC XrQuotedStatus xr_parser_decode_quoted(Parser *parser, const Token *token, bool escapes,
                                               XrParsedQuoted *output, const char **error);
typedef struct ParsedIntLiteral {
    uint64_t bits;
    bool overflows_i64;
    bool overflows_u64;
} ParsedIntLiteral;

XR_FUNC ParsedIntLiteral xr_parser_integer_literal(Parser *parser, const char *start, int length);

XR_FUNC bool xr_parser_step(const Parser *parser);
XR_FUNC int xr_parser_grow_capacity(Parser *parser, int old);
XR_FUNC char *xr_parser_token_string(Parser *parser, const Token *token);
XR_FUNC char xr_parser_read_byte(const Parser *parser, const char *address);
XR_FUNC size_t xr_parser_string_length(const Parser *parser, const char *text);
XR_FUNC int xr_parser_compare_bytes(const Parser *parser, const void *left, const void *right, size_t length);
XR_FUNC int xr_parser_compare_string_n(const Parser *parser, const char *left, const char *right, size_t length);
XR_FUNC int xr_parser_compare_string(const Parser *parser, const char *left, const char *right);
XR_FUNC void *xr_parser_find_byte(const Parser *parser, const void *text, int value, size_t length);
XR_FUNC char *xr_parser_copy_string_n(const Parser *parser, char *destination, const char *source, size_t length);
XR_FUNC char *xr_parser_copy_string(const Parser *parser, char *destination, const char *source);
XR_FUNC bool xr_parser_utf8_validate(const Parser *parser, const char *text, size_t length);
XR_FUNC int xr_parser_utf8_decode(const Parser *parser, const char *text, size_t length, uint32_t *output);

// Scoped heap owners are tracked in the arena so fatal unwinding cannot leak.
XR_FUNC XrTypeScope *xr_parser_type_scope_new(Parser *parser, XrTypeScope *parent);
XR_FUNC void xr_parser_type_scope_free(Parser *parser, XrTypeScope *scope);
XR_FUNC XrTypeAlias *xr_parser_type_scope_define(Parser *parser, XrTypeScope *scope,
                                                 const char *name, XrTypeRef *type);
XR_FUNC XrParseStatus xr_parser_diagnostic_status(const Parser *parser);
XR_FUNC void xr_parser_diagnostic_fail(Parser *parser, XrParseStatus status);
XR_FUNC bool xr_parser_format(Parser *parser, char *output, size_t capacity, const char *format, ...);
XR_FUNC void xr_parser_diagnostic_print(Parser *parser, int level, int code,
    const char *message, int line, int column, int token_length, const char *token_start);
XR_FUNC void xr_parser_diagnostic_summary(Parser *parser);
static inline bool xr_parser_healthy(const Parser *parser) {
    return parser && xr_compile_state_status(parser->state) == XR_COMPILE_RESOURCE_OK &&
           xr_parser_diagnostic_status(parser) == XR_PARSE_OK;
}

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
// Emit a [Exxx] error anchored at `token`, optionally followed by a note line
// carrying the suggested fix. `note` may be NULL. Sets panic_mode.
XR_FUNC void xr_parser_error_coded_note(Parser *parser, Token *token, int code, const char *title,
                                        const char *note);
XR_FUNC void xr_parser_synchronize(Parser *parser);
typedef bool (*XrParserRecoveryBoundaryFn)(Parser *parser);
XR_FUNC void xr_parser_skip_invalid_construct(Parser *parser, int start_line,
                                              XrParserRecoveryBoundaryFn is_recovery_boundary,
                                              bool stop_at_rbrace);
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
XR_FUNC AstNode *xr_parse_bare_lambda(Parser *parser, AstNode *parameter);
XR_FUNC AstNode *xr_parse_assignment(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_compound_assignment(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_call_expr(Parser *parser, AstNode *callee);
XR_FUNC AstNode *xr_parse_call_argument(Parser *parser);
XR_FUNC AstNode *xr_parse_call_argument_with_access(Parser *parser, XrCallArgAccess *out_access);
XR_FUNC AstNode *xr_parse_try_generic_call_after_lt(Parser *parser, AstNode *callee);
XR_FUNC AstNode *xr_parse_struct_literal_after_type(Parser *parser, AstNode *type_path,
                                                    XrTypeRef **type_args, int type_arg_count);
XR_FUNC AstNode *xr_parse_array_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_object_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_empty_map_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_set_literal_new(Parser *parser);
XR_FUNC AstNode *xr_parse_index_access(Parser *parser, AstNode *array);
XR_FUNC AstNode *xr_parse_member_access(Parser *parser, AstNode *object);
XR_FUNC AstNode *xr_parse_match_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_match_pattern(Parser *parser);
XR_FUNC AstNode *xr_parse_new_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_this_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_super_expression(Parser *parser);

XR_FUNC AstNode *xr_parse_regex_prefix(Parser *parser);
XR_FUNC AstNode *xr_parse_regex_literal(Parser *parser);
XR_FUNC AstNode *xr_parse_fn_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_is(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_type_cast(Parser *parser);
XR_FUNC AstNode *xr_parse_scalar_namespace(Parser *parser);
XR_FUNC AstNode *xr_parse_template_string(Parser *parser);
XR_FUNC AstNode *xr_parse_lt_or_generic(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_force_unwrap(Parser *parser, AstNode *operand);
XR_FUNC AstNode *xr_parse_as_cast(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_comptime_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_range(Parser *parser, AstNode *start);
XR_FUNC AstNode *xr_parse_ternary(Parser *parser, AstNode *condition);
XR_FUNC AstNode *xr_parse_nullish_coalesce(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_optional_chain(Parser *parser, AstNode *object);
XR_FUNC AstNode *xr_parse_optional_index(Parser *parser, AstNode *object);
XR_FUNC AstNode *xr_parse_inc_dec(Parser *parser);
XR_FUNC AstNode *xr_parse_postfix_inc_dec(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_arrow_function_body(Parser *parser, XrParamNode **params, int param_count,
                                              int line);

/* ========== Statement Parsing ========== */

XR_FUNC AstNode *xr_parse_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_expr_statement(Parser *parser);
// Report E0208 when `expr` cannot do anything as a statement. `anchor` is the
// statement's first token so the caret lands where the line started.
XR_FUNC void xr_parser_reject_effectless_expr_stmt(Parser *parser, const AstNode *expr,
                                                   Token *anchor);
XR_FUNC AstNode *xr_parse_standalone_inc_dec(Parser *parser, bool for_step);
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
XR_FUNC XrAttribute *xr_parse_single_attribute(Parser *parser);
/* Shared assertion-attribute validation (task 217 registry-driven). */
XR_FUNC bool xr_parser_reject_duplicate_assertion_attrs(Parser *parser, XrAttribute **attrs,
                                                        int count);
XR_FUNC bool xr_parser_reject_invalid_assertion_attrs(Parser *parser, XrAttribute **attrs,
                                                      int count, bool target_is_fn);
XR_FUNC bool xr_parser_validate_member_attr(Parser *parser, XrAttribute *attr);
XR_FUNC bool xr_parser_validate_enum_method_attr(Parser *parser, XrAttribute *attr);
XR_FUNC AstNode *xr_parse_var_declaration(Parser *parser, int is_const);
XR_FUNC AstNode *xr_parse_single_var_declaration(Parser *parser, int is_const);
XR_FUNC AstNode *xr_parse_function_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_type_alias_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_enum_declaration(Parser *parser);
XR_FUNC void xr_parse_borrow_origin_set(Parser *parser, XrBorrowOriginSyntaxState *out_syntax,
                                        AstBorrowOriginRef **out_origins, int *out_count);

/* ========== OOP Parsing ========== */

XR_FUNC AstNode *xr_parse_class_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_struct_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_union_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_interface_declaration(Parser *parser);
// Parse one interface body entry (method signature or property signature).
XR_FUNC AstNode *xr_parse_interface_member(Parser *parser);
XR_FUNC AstNode *xr_parse_field_declaration(Parser *parser, bool *is_method_out);
XR_FUNC AstNode *xr_parse_method_declaration(Parser *parser, const char *name, int name_line,
                                             int name_column, bool is_private, bool is_static);
XR_FUNC AstNode *xr_parse_operator_method(Parser *parser, bool is_private, bool is_static);
XR_FUNC AstNode *xr_parse_static_constructor(Parser *parser, bool is_private);

/* ========== Module System ========== */

XR_FUNC AstNode *xr_parse_import_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_import_from_declaration(Parser *parser, int line);
XR_FUNC AstNode *xr_parse_export_declaration(Parser *parser);

/* ========== Type Annotations ========== */

typedef enum XrParseParameterFlags {
    XR_PARSE_PARAMETER_ALLOW_MODE = 1u << 0,
    XR_PARSE_PARAMETER_ALLOW_REST = 1u << 1,
    XR_PARSE_PARAMETER_REQUIRE_TYPE = 1u << 2,
    XR_PARSE_PARAMETER_ALLOW_DESTRUCTURE = 1u << 3,
} XrParseParameterFlags;

XR_FUNC XrTypeRef *xr_parse_type_annotation(Parser *parser);
XR_FUNC bool xr_parse_optional_param_type_annotation(Parser *parser, bool allow_mode,
                                                     XrParamMode *out_mode, XrTypeRef **out_type);
XR_FUNC XrParamNode *xr_parse_parameter_at(Parser *parser, uint32_t flags, int param_index);
XR_FUNC XrParamNode *xr_parse_parameter(Parser *parser, uint32_t flags);
XR_FUNC void xr_parse_reject_ref_out_default_param(Parser *parser, const XrParamNode *param);

// True for built-in heap type names constructed via `T(args)` (Map/Array/...).
XR_FUNC bool xr_is_construct_only_type_name(Parser *parser, const char *name);
XR_FUNC XrTypeRef *xr_parse_type_name_ref(Parser *parser, const char *name);
XR_FUNC XrTypeRef *xr_parse_generic_type_name_ref(Parser *parser, const char *name,
                                                  XrTypeRef **args, int arg_count);

/* Parse one or more interface constraints separated by '&', e.g.
 *   T: Comparable
 *   T: Comparable & Hashable & Stringable
 * The leading ':' must already have been consumed.  Returns the array of
 * constraint type refs and writes the count to *out_count.  When no
 * constraint is parseable (allocation failure), returns NULL with count 0.
 */
XR_FUNC XrTypeRef **xr_parse_constraint_list(Parser *parser, int *out_count);

/* Parse a generic parameter list `<T, U: A & B, V = int>` when the next token
 * is '<'; a no-op returning NULL with *out_count == 0 otherwise.
 *
 * Every declaration form that accepts type parameters -- function, class,
 * struct, interface, enum, method, function expression -- shares this one
 * parser. They previously each carried their own copy of the same loop, so a
 * change to the grammar had to be made seven times or silently diverge.
 *
 * On success `parser->type_scope` is left pointing at a fresh scope holding the
 * parameters, so the caller can parse the signature and body against them; the
 * caller is responsible for saving and restoring its own scope. Parameters are
 * defined into that scope one at a time as they are parsed, which is what lets
 * a default name an earlier parameter (`<T, U = T>`).
 *
 * Type aliases do not use this helper: their parameters are names only
 * (LANGUAGE_SPEC 9.1 AliasTypeParams), and the alias entry must be registered
 * in the enclosing scope before its right-hand side is parsed, so the scope
 * this installs would capture the alias name itself. */
XR_FUNC XrGenericParam **xr_parse_generic_params(Parser *parser, int *out_count);

/* Parse an optional `where T: A & B, U: C` clause and merge each constraint
 * into the named parameter's existing constraint list; a no-op when the next
 * token is not `where`.
 *
 * `where` is spelling, not a second mechanism. It produces exactly what
 * `<T: A & B>` produces, so constraint enforcement stays on one path (E0358)
 * and the two forms compose on the same parameter. Call it after the
 * signature, while the generic scope from xr_parse_generic_params is still
 * installed, so constraint types can name the parameters. */
XR_FUNC void xr_parse_where_clause(Parser *parser, XrGenericParam **params, int param_count);
XR_FUNC XrGenericParam **xr_parse_method_conditions(Parser *parser, int *out_count);

/* ========== Destructuring ========== */

XR_FUNC XrDestructurePattern *xr_parse_array_pattern(Parser *parser);
XR_FUNC XrDestructurePattern *xr_parse_tuple_pattern(Parser *parser);
XR_FUNC XrDestructurePattern *xr_parse_object_pattern(Parser *parser);
XR_FUNC XrDestructurePattern *xr_parse_destructure_pattern(Parser *parser);
XR_FUNC AstNode *xr_parse_destructure_declaration(Parser *parser, bool is_const);

XR_FUNC XrDestructurePattern *convert_array_literal_to_pattern(Parser *parser,
                                                               AstNode *array_literal);
XR_FUNC XrDestructurePattern *convert_tuple_literal_to_pattern(Parser *parser,
                                                               AstNode *tuple_literal);
XR_FUNC XrDestructurePattern *convert_object_literal_to_pattern(Parser *parser,
                                                                AstNode *object_literal);

/* ========== Coroutine Parsing ========== */

XR_FUNC AstNode *xr_parse_go_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_go_expr_with_link(Parser *parser, uint8_t link_mode);
XR_FUNC AstNode *xr_parse_await_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_channel_new(Parser *parser);
XR_FUNC AstNode *xr_parse_cancelled_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_move_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_unsafe_expr(Parser *parser);
XR_FUNC AstNode *xr_parse_defer_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_select_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_scope_block(Parser *parser);
XR_FUNC AstNode *xr_parse_scope_block_with_mode(Parser *parser, uint8_t scope_mode);

/* ========== Misc Helpers ========== */

XR_FUNC bool xr_lbrace_starts_destructure_assignment(Parser *parser);

#endif  // XPARSE_INTERNAL_H
