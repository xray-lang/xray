/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_api.h - AST node creation functions
 *
 * KEY CONCEPT:
 *   Provides factory functions for creating all AST node types.
 */

#ifndef XAST_API_H
#define XAST_API_H

#include "xast_nodes.h"
#include "../../runtime/value/xvalue.h"
#include "../../base/xforward_decl.h"
#include "../../base/xdefs.h"

XR_FUNC AstNode *xr_ast_literal_int(XrCompilerSession *session, xr_Integer value, int line);
XR_FUNC AstNode *xr_ast_literal_float(XrCompilerSession *session, xr_Number value, int line);
XR_FUNC AstNode *xr_ast_literal_bigint(XrCompilerSession *session, const char *value, int line);
XR_FUNC AstNode *xr_ast_literal_string(XrCompilerSession *session, const char *value, int line);
XR_FUNC AstNode *xr_ast_literal_char(XrCompilerSession *session, uint32_t value, int line);
XR_FUNC AstNode *xr_ast_literal_regex(XrCompilerSession *session, const char *pattern,
                                      const char *flags, int line);
XR_FUNC AstNode *xr_ast_literal_null(XrCompilerSession *session, int line);
XR_FUNC AstNode *xr_ast_literal_bool(XrCompilerSession *session, int value, int line);

XR_FUNC AstNode *xr_ast_template_string(XrCompilerSession *session, AstNode **parts, int part_count,
                                        int line);

XR_FUNC AstNode *xr_ast_binary(XrCompilerSession *session, AstNodeType type, AstNode *left,
                               AstNode *right, int line);

XR_FUNC AstNode *xr_ast_unary(XrCompilerSession *session, AstNodeType type, AstNode *operand,
                              int line);

XR_FUNC AstNode *xr_ast_grouping(XrCompilerSession *session, AstNode *expr, int line);
XR_FUNC AstNode *xr_ast_expr_stmt(XrCompilerSession *session, AstNode *expr, int line);
XR_FUNC AstNode *xr_ast_print_stmt(XrCompilerSession *session, AstNode **exprs, int expr_count,
                                   int line);
XR_FUNC AstNode *xr_ast_program(XrCompilerSession *session);
XR_FUNC void xr_ast_program_add(XrCompilerSession *session, AstNode *program, AstNode *stmt);
XR_FUNC AstNode *xr_ast_block(XrCompilerSession *session, int line);
XR_FUNC void xr_ast_block_add(XrCompilerSession *session, AstNode *block, AstNode *stmt);
XR_FUNC AstNode *xr_ast_var_decl(XrCompilerSession *session, const char *name, AstNode *initializer,
                                 bool is_const, int line);

// Create variable declaration with storage mode (shared)
XR_FUNC AstNode *xr_ast_var_decl_with_mode(XrCompilerSession *session, const char *name,
                                           AstNode *initializer, bool is_const,
                                           uint8_t storage_mode, int line);

// Create variable reference node
XR_FUNC AstNode *xr_ast_variable(XrCompilerSession *session, const char *name, int line);

// Create assignment node
XR_FUNC AstNode *xr_ast_assignment(XrCompilerSession *session, const char *name, AstNode *value,
                                   int line);

// Create compound assignment node
XR_FUNC AstNode *xr_ast_compound_assignment(XrCompilerSession *session, const char *name,
                                            XrTokenType op, AstNode *value, int line);

// Create member compound assignment node
XR_FUNC AstNode *xr_ast_member_compound_assignment(XrCompilerSession *session, AstNode *object,
                                                   const char *name, XrTokenType op, AstNode *value,
                                                   int line);

// Create increment node
XR_FUNC AstNode *xr_ast_inc(XrCompilerSession *session, const char *name, int line);

// Create decrement node
XR_FUNC AstNode *xr_ast_dec(XrCompilerSession *session, const char *name, int line);

// Create if statement node
XR_FUNC AstNode *xr_ast_if_stmt(XrCompilerSession *session, AstNode *condition,
                                AstNode *then_branch, AstNode *else_branch, int line);

// Create while loop node
XR_FUNC AstNode *xr_ast_while_stmt(XrCompilerSession *session, const char *label,
                                   AstNode *condition, AstNode *body, int line);

// Create for loop node
XR_FUNC AstNode *xr_ast_for_stmt(XrCompilerSession *session, const char *label,
                                 AstNode *initializer, AstNode *condition, AstNode *increment,
                                 AstNode *body, int line);

// Create for-in loop node
XR_FUNC AstNode *xr_ast_for_in_stmt(XrCompilerSession *session, const char *label,
                                    const char *item_name, XrTypeRef *item_type,
                                    AstNode *collection, AstNode *body, int line);

// Create for-in key-value loop node
XR_FUNC AstNode *xr_ast_for_in_keyvalue_stmt(XrCompilerSession *session, const char *key_name,
                                             const char *value_name, const char *label,
                                             XrTypeRef *item_type, AstNode *collection,
                                             AstNode *body, int line);

// Create break statement node
XR_FUNC AstNode *xr_ast_break_stmt(XrCompilerSession *session, const char *label, int line);

// Create continue statement node
XR_FUNC AstNode *xr_ast_continue_stmt(XrCompilerSession *session, const char *label, int line);

// Create parameter node
XR_FUNC XrParamNode *xr_param_node_new(XrCompilerSession *session, const char *name, int line,
                                       int column);

// Create function declaration node
XR_FUNC AstNode *xr_ast_function_decl(XrCompilerSession *session, const char *name,
                                      XrParamNode **params, int param_count, AstNode *body,
                                      int line);

// Create function expression node
XR_FUNC AstNode *xr_ast_function_expr(XrCompilerSession *session, XrParamNode **params,
                                      int param_count, AstNode *body, int line);

// Create function call node
XR_FUNC AstNode *xr_ast_call_expr(XrCompilerSession *session, AstNode *callee, AstNode **arguments,
                                  int arg_count, int line);

// Create function call node with generic type arguments
XR_FUNC AstNode *xr_ast_call_expr_generic(XrCompilerSession *session, AstNode *callee,
                                          AstNode **arguments, int arg_count, XrTypeRef **type_args,
                                          int type_arg_count, int line);

// Create return statement node
XR_FUNC AstNode *xr_ast_return_stmt(XrCompilerSession *session, AstNode **values, int count,
                                    int line);

// Create is expression node (runtime type check)
XR_FUNC AstNode *xr_ast_is_expr(XrCompilerSession *session, AstNode *expr, XrTypeRef *type,
                                int line);

// Create as expression node (explicit type cast)
XR_FUNC AstNode *xr_ast_as_expr(XrCompilerSession *session, AstNode *expr, XrTypeRef *type,
                                bool is_safe, int line);

// Create array literal node
XR_FUNC AstNode *xr_ast_array_literal(XrCompilerSession *session, AstNode **elements, int count,
                                      int line);

// Create tuple literal node.
// count == 0 produces a unit literal `()`; count == 1 is a unary tuple
// `(x,)`. The caller owns `elements`; the factory copies the array
// into the parse arena.
XR_FUNC AstNode *xr_ast_tuple_literal(XrCompilerSession *session, AstNode **elements, int count,
                                      int line);

// Create spread element node: `...expr` (only valid as a tuple-literal
// element or as a function-call argument). The wrapped expression must
// evaluate to a tuple of statically known arity.
XR_FUNC AstNode *xr_ast_spread_expr(XrCompilerSession *session, AstNode *expr, int line);

// Create object literal node
XR_FUNC AstNode *xr_ast_object_literal(XrCompilerSession *session, AstNode **keys, AstNode **values,
                                       bool *computed, int count, int line);

// Create Map literal node
XR_FUNC AstNode *xr_ast_map_literal(XrCompilerSession *session, AstNode **keys, AstNode **values,
                                    int count, int line);

// Create Set literal node
XR_FUNC AstNode *xr_ast_set_literal(XrCompilerSession *session, AstNode **elements, int count,
                                    int line);

// Create index access node
XR_FUNC AstNode *xr_ast_index_get(XrCompilerSession *session, AstNode *array, AstNode *index,
                                  int line);

// Create index assignment node
XR_FUNC AstNode *xr_ast_index_set(XrCompilerSession *session, AstNode *array, AstNode *index,
                                  AstNode *value, int line);

// Create slice expression node
XR_FUNC AstNode *xr_ast_slice_expr(XrCompilerSession *session, AstNode *source, AstNode *start,
                                   AstNode *end, int line);

// Create member access node
XR_FUNC AstNode *xr_ast_member_access(XrCompilerSession *session, AstNode *object, const char *name,
                                      int line);

// Create class declaration node
XR_FUNC AstNode *xr_ast_class_decl(XrCompilerSession *session, const char *name,
                                   const char *super_name, AstNode **fields, int field_count,
                                   AstNode **methods, int method_count, int line);

// Create struct declaration node (value type, reuses ClassDeclNode layout)
XR_FUNC AstNode *xr_ast_struct_decl(XrCompilerSession *session, const char *name, AstNode **fields,
                                    int field_count, AstNode **methods, int method_count, int line);

// Create struct literal node: Point{x: 1.0, y: 2.0}
XR_FUNC AstNode *xr_ast_struct_literal(XrCompilerSession *session, const char *name,
                                       char **field_names, AstNode **field_values, int field_count,
                                       int line);

// Create interface declaration node
XR_FUNC AstNode *xr_ast_interface_decl(XrCompilerSession *session, const char *name,
                                       XrTypeRef **extends, int extends_count, AstNode **methods,
                                       int method_count, AstNode **properties, int property_count,
                                       XrGenericParam **type_params, int type_param_count,
                                       int line);

// Create interface method signature node
XR_FUNC AstNode *xr_ast_interface_method(XrCompilerSession *session, const char *name,
                                         char **parameters, XrTypeRef **param_types,
                                         int param_count, XrTypeRef *return_type, int line);

// Create interface property signature node (e.g. `length: int`)
XR_FUNC AstNode *xr_ast_interface_property(XrCompilerSession *session, const char *name,
                                           XrTypeRef *prop_type, bool is_readonly, int line);

// Create field declaration node
XR_FUNC AstNode *xr_ast_field_decl(XrCompilerSession *session, const char *name,
                                   XrTypeRef *field_type, bool is_private, bool is_static,
                                   AstNode *initializer, int line);

// Create method declaration node
XR_FUNC AstNode *xr_ast_method_decl(XrCompilerSession *session, const char *name, char **parameters,
                                    XrTypeRef **param_types, int param_count,
                                    XrTypeRef *return_type, AstNode *body, bool is_constructor,
                                    bool is_static, bool is_private, bool is_getter, bool is_setter,
                                    int line);

// Create new expression node (supports new module.Class() and new Box<int>() syntax)
XR_FUNC AstNode *xr_ast_new_expr(XrCompilerSession *session, const char *module_name,
                                 const char *class_name, AstNode **arguments, int arg_count,
                                 XrTypeRef **type_args, int type_arg_count, int line);

// Create this expression node
XR_FUNC AstNode *xr_ast_this_expr(XrCompilerSession *session, int line);

// Create super call node
XR_FUNC AstNode *xr_ast_super_call(XrCompilerSession *session, const char *method_name,
                                   AstNode **arguments, int arg_count, int line);

// Create member assignment node
XR_FUNC AstNode *xr_ast_member_set(XrCompilerSession *session, AstNode *object, const char *member,
                                   AstNode *value, int line);

// Create enum declaration node (ADT enum with optional generics, methods, interfaces)
XR_FUNC AstNode *xr_ast_enum_decl(XrCompilerSession *session, const char *name,
                                  const char *type_hint, AstNode **members, int member_count,
                                  AstNode **methods, int method_count, XrGenericParam **type_params,
                                  int type_param_count, XrTypeRef **interfaces, int interface_count,
                                  int line);

// Create enum member node (ADT variant with optional payload)
XR_FUNC AstNode *xr_ast_enum_member(XrCompilerSession *session, const char *name, AstNode *value,
                                    char **payload_names, XrTypeRef **payload_types,
                                    int payload_count, int line);

// Create enum access node
XR_FUNC AstNode *xr_ast_enum_access(XrCompilerSession *session, const char *enum_name,
                                    const char *member_name, int line);

// Create enum conversion node
XR_FUNC AstNode *xr_ast_enum_convert(XrCompilerSession *session, const char *enum_name,
                                     AstNode *value_expr, int line);

// Create enum index node (compiler-generated for for-in desugaring)
XR_FUNC AstNode *xr_ast_enum_index(XrCompilerSession *session, AstNode *collection,
                                   AstNode *index_expr, int line);

// Allocate a catch clause (caller fills body afterwards if needed)
XR_FUNC XrCatchClause *xr_ast_catch_clause(XrCompilerSession *session, const char *var_name,
                                           int var_line, int var_column, XrTypeRef *type,
                                           AstNode *body);

// Create try-catch statement node (multi-catch)
XR_FUNC AstNode *xr_ast_try_catch(XrCompilerSession *session, AstNode *try_body,
                                  XrCatchClause **clauses, int catch_count, int line);

// Create throw statement node
XR_FUNC AstNode *xr_ast_throw_stmt(XrCompilerSession *session, AstNode *expression, int line);

// Destroy a program AST and release its owning arena in O(1).
// Only valid for AST_PROGRAM nodes. Non-owning programs (LSP) are no-ops.
XR_FUNC void xr_program_destroy(AstNode *program);

// Create import statement node
XR_FUNC AstNode *xr_ast_import_stmt(XrCompilerSession *session, const char *module_name,
                                    const char *alias, bool is_quoted, int line);

// Create import statement node (extended, supports named imports)
XR_FUNC AstNode *xr_ast_import_stmt_ex(XrCompilerSession *session, const char *module_name,
                                       const char *alias, bool is_quoted, ImportMember *members,
                                       int member_count, int line);

// Create export statement node
XR_FUNC AstNode *xr_ast_export_stmt(XrCompilerSession *session, AstNode *declaration,
                                    const char *export_name, int line);

// Create export list statement node
XR_FUNC AstNode *xr_ast_export_list(XrCompilerSession *session, char **names, int count, int line);

// Create re-export statement node
XR_FUNC AstNode *xr_ast_export_reexport(XrCompilerSession *session, const char *from_path,
                                        ReexportMember *members, int count, bool is_all, int line);

// Create ternary expression node
XR_FUNC AstNode *xr_ast_ternary(XrCompilerSession *session, AstNode *condition, AstNode *true_expr,
                                AstNode *false_expr, int line);

// Create optional chain node
XR_FUNC AstNode *xr_ast_optional_chain(XrCompilerSession *session, AstNode *object,
                                       const char *name, AstNode *index, int chain_type, int line);

// Create range expression node
XR_FUNC AstNode *xr_ast_range(XrCompilerSession *session, AstNode *start, AstNode *end,
                              bool inclusive_end, int line);

// Create destructure patterns (flat only)
XR_FUNC XrDestructurePattern *xr_pattern_array(XrCompilerSession *session,
                                               XrDestructurePattern **elements, int count);
XR_FUNC XrDestructurePattern *xr_pattern_tuple(XrCompilerSession *session,
                                               XrDestructurePattern **elements, int count);
XR_FUNC XrDestructurePattern *xr_pattern_object(XrCompilerSession *session, char **fields,
                                                XrDestructurePattern **patterns, int count,
                                                bool use_shorthand);
XR_FUNC XrDestructurePattern *xr_pattern_identifier(XrCompilerSession *session, const char *name,
                                                    XrTypeRef *type);
XR_FUNC XrDestructurePattern *xr_pattern_skip(XrCompilerSession *session);

// Create destructure nodes
XR_FUNC AstNode *xr_ast_destructure_decl(XrCompilerSession *session, XrDestructurePattern *pattern,
                                         AstNode *initializer, bool is_const, int line);
XR_FUNC AstNode *xr_ast_destructure_assign(XrCompilerSession *session,
                                           XrDestructurePattern *pattern, AstNode *value, int line);

// Create match expression node
XR_FUNC AstNode *xr_ast_match_expr(XrCompilerSession *session, AstNode *expr, AstNode **arms,
                                   int arm_count, int line);

// Create match arm node
XR_FUNC AstNode *xr_ast_match_arm(XrCompilerSession *session, AstNode *pattern, AstNode *guard,
                                  AstNode *body, int line);

// Create literal pattern node
XR_FUNC AstNode *xr_ast_pattern_literal(XrCompilerSession *session, AstNode *value, int line);

// Create range pattern node
XR_FUNC AstNode *xr_ast_pattern_range(XrCompilerSession *session, AstNode *start, AstNode *end,
                                      bool inclusive_end, int line);

// Create wildcard pattern node
XR_FUNC AstNode *xr_ast_pattern_wildcard(XrCompilerSession *session, int line);

// Create multi-value pattern node
XR_FUNC AstNode *xr_ast_pattern_multi(XrCompilerSession *session, AstNode **patterns, int count,
                                      int line);

// Create tuple pattern node (positional destructure inside match arms)
XR_FUNC AstNode *xr_ast_pattern_tuple(XrCompilerSession *session, AstNode **patterns, int count,
                                      int line);

// Create ADT variant destructure pattern node: Shape.Circle(r, ...)
XR_FUNC AstNode *xr_ast_pattern_adt(XrCompilerSession *session, AstNode *variant,
                                    AstNode **patterns, int count, int line);

// Create object/record match pattern node: { x, y } / { x: sub }
XR_FUNC AstNode *xr_ast_pattern_object(XrCompilerSession *session, char **field_names,
                                       AstNode **patterns, int count, int line);

// Create array match pattern node: [a, b, ..rest]
XR_FUNC AstNode *xr_ast_pattern_array(XrCompilerSession *session, AstNode **patterns, int count,
                                      bool has_rest, char *rest_name, int line);

// Create type pattern node: `is T` or `is T name`
XR_FUNC AstNode *xr_ast_pattern_type(XrCompilerSession *session, XrTypeRef *type,
                                     const char *binding_name, int line);

// Create type alias node
XR_FUNC AstNode *xr_ast_type_alias(XrCompilerSession *session, const char *name, char **field_names,
                                   XrTypeRef **field_types, bool *field_optional, int field_count,
                                   int line);

// Create go expression node (supports name and link mode)
XR_FUNC AstNode *xr_ast_go_expr(XrCompilerSession *session, AstNode *expr, const char *name,
                                uint8_t link_mode, int line);

// Create await expression node
XR_FUNC AstNode *xr_ast_await_expr(XrCompilerSession *session, AstNode *expr, AstNode *timeout,
                                   bool is_any, bool is_all, bool is_any_success, int line);

// Create Channel creation node
XR_FUNC AstNode *xr_ast_channel_new(XrCompilerSession *session, AstNode *buffer_size, int line);

// Create select case node
XR_FUNC AstNode *xr_ast_select_case(XrCompilerSession *session, const char *var_name,
                                    AstNode *channel, AstNode *value, AstNode *body, bool is_send,
                                    bool is_default, bool is_timeout, int line);

// Create select statement node
XR_FUNC AstNode *xr_ast_select_stmt(XrCompilerSession *session, AstNode **cases, int case_count,
                                    int line);

// Create defer statement node
XR_FUNC AstNode *xr_ast_defer_stmt(XrCompilerSession *session, AstNode *expr, int line);

// Create scope block node (supports scope mode)
XR_FUNC AstNode *xr_ast_scope_block(XrCompilerSession *session, AstNode *body, uint8_t scope_mode,
                                    int line);

// Create yield statement node (yield execution)
XR_FUNC AstNode *xr_ast_yield_stmt(XrCompilerSession *session, AstNode *value, int line);

// Create cancelled() expression node
XR_FUNC AstNode *xr_ast_cancelled_expr(XrCompilerSession *session, int line);

// Create move expression node (explicit ownership transfer)
XR_FUNC AstNode *xr_ast_move_expr(XrCompilerSession *session, AstNode *expr, int line, int column);
XR_FUNC AstNode *xr_ast_unsafe_expr(XrCompilerSession *session, AstNode *operand, int line,
                                    int column);

// Debug: print AST structure
XR_FUNC void xr_ast_print(AstNode *node, int indent);

// Get node type name
XR_FUNC const char *xr_ast_typename(AstNodeType type);

#endif  // XAST_API_H
