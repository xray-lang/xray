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
#include "../../base/xdefs.h"

XR_FUNC AstNode *xr_ast_literal_int(XrayIsolate *X, xr_Integer value, int line);
XR_FUNC AstNode *xr_ast_literal_float(XrayIsolate *X, xr_Number value, int line);
XR_FUNC AstNode *xr_ast_literal_bigint(XrayIsolate *X, const char *value, int line);
XR_FUNC AstNode *xr_ast_literal_string(XrayIsolate *X, const char *value, int line);
XR_FUNC AstNode *xr_ast_literal_regex(XrayIsolate *X, const char *pattern, const char *flags,
                                      int line);
XR_FUNC AstNode *xr_ast_literal_null(XrayIsolate *X, int line);
XR_FUNC AstNode *xr_ast_literal_bool(XrayIsolate *X, int value, int line);

XR_FUNC AstNode *xr_ast_template_string(XrayIsolate *X, AstNode **parts, int part_count, int line);

XR_FUNC AstNode *xr_ast_binary(XrayIsolate *X, AstNodeType type, AstNode *left, AstNode *right,
                               int line);

XR_FUNC AstNode *xr_ast_unary(XrayIsolate *X, AstNodeType type, AstNode *operand, int line);

XR_FUNC AstNode *xr_ast_grouping(XrayIsolate *X, AstNode *expr, int line);
XR_FUNC AstNode *xr_ast_expr_stmt(XrayIsolate *X, AstNode *expr, int line);
XR_FUNC AstNode *xr_ast_print_stmt(XrayIsolate *X, AstNode **exprs, int expr_count, int line);
XR_FUNC AstNode *xr_ast_program(XrayIsolate *iso);
XR_FUNC void xr_ast_program_add(XrayIsolate *X, AstNode *program, AstNode *stmt);
XR_FUNC AstNode *xr_ast_block(XrayIsolate *X, int line);
XR_FUNC void xr_ast_block_add(XrayIsolate *X, AstNode *block, AstNode *stmt);
XR_FUNC AstNode *xr_ast_var_decl(XrayIsolate *X, const char *name, AstNode *initializer,
                                 bool is_const, int line);

// Create variable declaration with storage mode (shared)
XR_FUNC AstNode *xr_ast_var_decl_with_mode(XrayIsolate *X, const char *name, AstNode *initializer,
                                           bool is_const, uint8_t storage_mode, int line);

// Create variable reference node
XR_FUNC AstNode *xr_ast_variable(XrayIsolate *X, const char *name, int line);

// Create assignment node
XR_FUNC AstNode *xr_ast_assignment(XrayIsolate *X, const char *name, AstNode *value, int line);

// Create compound assignment node
XR_FUNC AstNode *xr_ast_compound_assignment(XrayIsolate *X, const char *name, XrTokenType op,
                                            AstNode *value, int line);

// Create member compound assignment node
XR_FUNC AstNode *xr_ast_member_compound_assignment(XrayIsolate *X, AstNode *object,
                                                   const char *name, XrTokenType op, AstNode *value,
                                                   int line);

// Create increment node
XR_FUNC AstNode *xr_ast_inc(XrayIsolate *X, const char *name, int line);

// Create decrement node
XR_FUNC AstNode *xr_ast_dec(XrayIsolate *X, const char *name, int line);

// Create if statement node
XR_FUNC AstNode *xr_ast_if_stmt(XrayIsolate *X, AstNode *condition, AstNode *then_branch,
                                AstNode *else_branch, int line);

// Create while loop node
XR_FUNC AstNode *xr_ast_while_stmt(XrayIsolate *X, AstNode *condition, AstNode *body, int line);

// Create for loop node
XR_FUNC AstNode *xr_ast_for_stmt(XrayIsolate *X, AstNode *initializer, AstNode *condition,
                                 AstNode *increment, AstNode *body, int line);

// Create for-in loop node
XR_FUNC AstNode *xr_ast_for_in_stmt(XrayIsolate *X, const char *item_name, XrTypeRef *item_type,
                                    AstNode *collection, AstNode *body, int line);

// Create for-in key-value loop node
XR_FUNC AstNode *xr_ast_for_in_keyvalue_stmt(XrayIsolate *X, const char *key_name,
                                             const char *value_name, XrTypeRef *item_type,
                                             AstNode *collection, AstNode *body, int line);

// Create break statement node
XR_FUNC AstNode *xr_ast_break_stmt(XrayIsolate *X, int line);

// Create continue statement node
XR_FUNC AstNode *xr_ast_continue_stmt(XrayIsolate *X, int line);

// Create parameter node
XR_FUNC XrParamNode *xr_param_node_new(XrayIsolate *X, const char *name, int line, int column);

// Create function declaration node
XR_FUNC AstNode *xr_ast_function_decl(XrayIsolate *X, const char *name, XrParamNode **params,
                                      int param_count, AstNode *body, int line);

// Create function expression node
XR_FUNC AstNode *xr_ast_function_expr(XrayIsolate *X, XrParamNode **params, int param_count,
                                      AstNode *body, int line);

// Create function call node
XR_FUNC AstNode *xr_ast_call_expr(XrayIsolate *X, AstNode *callee, AstNode **arguments,
                                  int arg_count, int line);

// Create function call node with generic type arguments
XR_FUNC AstNode *xr_ast_call_expr_generic(XrayIsolate *X, AstNode *callee, AstNode **arguments,
                                          int arg_count, XrTypeRef **type_args, int type_arg_count,
                                          int line);

// Create return statement node
XR_FUNC AstNode *xr_ast_return_stmt(XrayIsolate *X, AstNode **values, int count, int line);

// Create is expression node (runtime type check)
XR_FUNC AstNode *xr_ast_is_expr(XrayIsolate *X, AstNode *expr, XrTypeRef *type, int line);

// Create as expression node (explicit type cast)
XR_FUNC AstNode *xr_ast_as_expr(XrayIsolate *X, AstNode *expr, XrTypeRef *type, bool is_safe,
                                int line);

// Create array literal node
XR_FUNC AstNode *xr_ast_array_literal(XrayIsolate *X, AstNode **elements, int count, int line);

// Create object literal node
XR_FUNC AstNode *xr_ast_object_literal(XrayIsolate *X, AstNode **keys, AstNode **values,
                                       bool *computed, int count, int line);

// Create Map literal node
XR_FUNC AstNode *xr_ast_map_literal(XrayIsolate *X, AstNode **keys, AstNode **values, int count,
                                    int line);

// Create Set literal node
XR_FUNC AstNode *xr_ast_set_literal(XrayIsolate *X, AstNode **elements, int count, int line);

// Create index access node
XR_FUNC AstNode *xr_ast_index_get(XrayIsolate *X, AstNode *array, AstNode *index, int line);

// Create index assignment node
XR_FUNC AstNode *xr_ast_index_set(XrayIsolate *X, AstNode *array, AstNode *index, AstNode *value,
                                  int line);

// Create slice expression node
XR_FUNC AstNode *xr_ast_slice_expr(XrayIsolate *X, AstNode *source, AstNode *start, AstNode *end,
                                   int line);

// Create member access node
XR_FUNC AstNode *xr_ast_member_access(XrayIsolate *X, AstNode *object, const char *name, int line);

// Create class declaration node
XR_FUNC AstNode *xr_ast_class_decl(XrayIsolate *X, const char *name, const char *super_name,
                                   AstNode **fields, int field_count, AstNode **methods,
                                   int method_count, int line);

// Create struct declaration node (value type, reuses ClassDeclNode layout)
XR_FUNC AstNode *xr_ast_struct_decl(XrayIsolate *X, const char *name, AstNode **fields,
                                    int field_count, AstNode **methods, int method_count, int line);

// Create struct literal node: Point{x: 1.0, y: 2.0}
XR_FUNC AstNode *xr_ast_struct_literal(XrayIsolate *X, const char *name, char **field_names,
                                       AstNode **field_values, int field_count, int line);

// Create interface declaration node
XR_FUNC AstNode *xr_ast_interface_decl(XrayIsolate *X, const char *name, char **extends,
                                       int extends_count, AstNode **methods, int method_count,
                                       int line);

// Create interface method signature node
XR_FUNC AstNode *xr_ast_interface_method(XrayIsolate *X, const char *name, char **parameters,
                                         XrTypeRef **param_types, int param_count, XrTypeRef *return_type,
                                         int line);

// Create field declaration node
XR_FUNC AstNode *xr_ast_field_decl(XrayIsolate *X, const char *name, XrTypeRef *field_type,
                                   bool is_private, bool is_static, AstNode *initializer, int line);

// Create method declaration node
XR_FUNC AstNode *xr_ast_method_decl(XrayIsolate *X, const char *name, char **parameters,
                                    XrTypeRef **param_types, int param_count, XrTypeRef *return_type,
                                    AstNode *body, bool is_constructor, bool is_static,
                                    bool is_private, bool is_getter, bool is_setter, int line);

// Create new expression node (supports new module.Class() and new Box<int>() syntax)
XR_FUNC AstNode *xr_ast_new_expr(XrayIsolate *X, const char *module_name, const char *class_name,
                                 AstNode **arguments, int arg_count, XrTypeRef **type_args,
                                 int type_arg_count, int line);

// Create this expression node
XR_FUNC AstNode *xr_ast_this_expr(XrayIsolate *X, int line);

// Create super call node
XR_FUNC AstNode *xr_ast_super_call(XrayIsolate *X, const char *method_name, AstNode **arguments,
                                   int arg_count, int line);

// Create member assignment node
XR_FUNC AstNode *xr_ast_member_set(XrayIsolate *X, AstNode *object, const char *member,
                                   AstNode *value, int line);

// Create enum declaration node
XR_FUNC AstNode *xr_ast_enum_decl(XrayIsolate *X, const char *name, const char *type_hint,
                                  AstNode **members, int member_count, int line);

// Create enum member node
XR_FUNC AstNode *xr_ast_enum_member(XrayIsolate *X, const char *name, AstNode *value, int line);

// Create enum access node
XR_FUNC AstNode *xr_ast_enum_access(XrayIsolate *X, const char *enum_name, const char *member_name,
                                    int line);

// Create enum conversion node
XR_FUNC AstNode *xr_ast_enum_convert(XrayIsolate *X, const char *enum_name, AstNode *value_expr,
                                     int line);

// Create enum index node (compiler-generated for for-in desugaring)
XR_FUNC AstNode *xr_ast_enum_index(XrayIsolate *X, AstNode *collection, AstNode *index_expr,
                                   int line);

// Create try-catch-finally statement node
XR_FUNC AstNode *xr_ast_try_catch(XrayIsolate *X, AstNode *try_body, const char *catch_var,
                                  int catch_var_line, int catch_var_column, AstNode *catch_body,
                                  AstNode *finally_body, int line);

// Create throw statement node
XR_FUNC AstNode *xr_ast_throw_stmt(XrayIsolate *X, AstNode *expression, int line);

// Destroy a program AST and release its owning arena in O(1).
// Only valid for AST_PROGRAM nodes. Non-owning programs (LSP) are no-ops.
XR_FUNC void xr_program_destroy(AstNode *program);

// Create import statement node
XR_FUNC AstNode *xr_ast_import_stmt(XrayIsolate *X, const char *module_name, const char *alias,
                                    ImportType import_type, int line);

// Create import statement node (extended, supports named imports)
XR_FUNC AstNode *xr_ast_import_stmt_ex(XrayIsolate *X, const char *module_name, const char *alias,
                                       ImportType import_type, ImportMember *members,
                                       int member_count, int line);

// Create export statement node
XR_FUNC AstNode *xr_ast_export_stmt(XrayIsolate *X, AstNode *declaration, const char *export_name,
                                    int line);

// Create export list statement node
XR_FUNC AstNode *xr_ast_export_list(XrayIsolate *X, char **names, int count, int line);

// Create re-export statement node
XR_FUNC AstNode *xr_ast_export_reexport(XrayIsolate *X, const char *from_path,
                                        ReexportMember *members, int count, bool is_all, int line);

// Create ternary expression node
XR_FUNC AstNode *xr_ast_ternary(XrayIsolate *X, AstNode *condition, AstNode *true_expr,
                                AstNode *false_expr, int line);

// Create optional chain node
XR_FUNC AstNode *xr_ast_optional_chain(XrayIsolate *X, AstNode *object, const char *name,
                                       AstNode *index, int chain_type, int line);

// Create range expression node
XR_FUNC AstNode *xr_ast_range(XrayIsolate *X, AstNode *start, AstNode *end, int line);

// Create destructure patterns (flat only)
XR_FUNC XrDestructurePattern *xr_pattern_array(XrayIsolate *X, XrDestructurePattern **elements,
                                               int count);
XR_FUNC XrDestructurePattern *xr_pattern_object(XrayIsolate *X, char **fields,
                                                XrDestructurePattern **patterns, int count,
                                                bool use_shorthand);
XR_FUNC XrDestructurePattern *xr_pattern_identifier(XrayIsolate *X, const char *name, XrTypeRef *type);
XR_FUNC XrDestructurePattern *xr_pattern_skip(XrayIsolate *iso);

// Create destructure nodes
XR_FUNC AstNode *xr_ast_destructure_decl(XrayIsolate *X, XrDestructurePattern *pattern,
                                         AstNode *initializer, bool is_const, int line);
XR_FUNC AstNode *xr_ast_destructure_assign(XrayIsolate *X, XrDestructurePattern *pattern,
                                           AstNode *value, int line);

// Create multi-value declaration node
XR_FUNC AstNode *xr_ast_multi_var_decl(XrayIsolate *X, char **names, int name_count,
                                       AstNode **values, int value_count, bool is_const, int line);

// Create multi-value assignment node
XR_FUNC AstNode *xr_ast_multi_assign(XrayIsolate *X, AstNode **targets, int target_count,
                                     AstNode **values, int value_count, int line);

// Create match expression node
XR_FUNC AstNode *xr_ast_match_expr(XrayIsolate *X, AstNode *expr, AstNode **arms, int arm_count,
                                   int line);

// Create match arm node
XR_FUNC AstNode *xr_ast_match_arm(XrayIsolate *X, AstNode *pattern, AstNode *guard, AstNode *body,
                                  int line);

// Create literal pattern node
XR_FUNC AstNode *xr_ast_pattern_literal(XrayIsolate *X, AstNode *value, int line);

// Create range pattern node
XR_FUNC AstNode *xr_ast_pattern_range(XrayIsolate *X, AstNode *start, AstNode *end, int line);

// Create wildcard pattern node
XR_FUNC AstNode *xr_ast_pattern_wildcard(XrayIsolate *X, int line);

// Create multi-value pattern node
XR_FUNC AstNode *xr_ast_pattern_multi(XrayIsolate *X, AstNode **patterns, int count, int line);

// Create type alias node
XR_FUNC AstNode *xr_ast_type_alias(XrayIsolate *X, const char *name, char **field_names,
                                   XrTypeRef **field_types, bool *field_optional, int field_count,
                                   int line);

// Create go expression node (supports name, priority, and link mode)
XR_FUNC AstNode *xr_ast_go_expr(XrayIsolate *X, AstNode *expr, const char *name, AstNode *priority,
                                uint8_t link_mode, int line);

// Create await expression node
XR_FUNC AstNode *xr_ast_await_expr(XrayIsolate *X, AstNode *expr, AstNode *timeout, bool is_any,
                                   bool is_all, bool is_any_success, int line);

// Create Channel creation node
XR_FUNC AstNode *xr_ast_channel_new(XrayIsolate *X, AstNode *buffer_size, int line);

// Create select case node
XR_FUNC AstNode *xr_ast_select_case(XrayIsolate *X, const char *var_name, AstNode *channel,
                                    AstNode *value, AstNode *body, bool is_send, bool is_default,
                                    bool is_timeout, int line);

// Create select statement node
XR_FUNC AstNode *xr_ast_select_stmt(XrayIsolate *X, AstNode **cases, int case_count, int line);

// Create defer statement node
XR_FUNC AstNode *xr_ast_defer_stmt(XrayIsolate *X, AstNode *expr, int line);

// Create scope block node (supports scope mode)
XR_FUNC AstNode *xr_ast_scope_block(XrayIsolate *X, AstNode *body, uint8_t scope_mode, int line);

// Create yield statement node (yield execution)
XR_FUNC AstNode *xr_ast_yield_stmt(XrayIsolate *X, int line);

// Create cancelled() expression node
XR_FUNC AstNode *xr_ast_cancelled_expr(XrayIsolate *X, int line);

// Create move expression node (explicit ownership transfer)
XR_FUNC AstNode *xr_ast_move_expr(XrayIsolate *X, AstNode *expr, int line, int column);

// Debug: print AST structure
XR_FUNC void xr_ast_print(AstNode *node, int indent);

// Get node type name
XR_FUNC const char *xr_ast_typename(AstNodeType type);

#endif  // XAST_API_H
