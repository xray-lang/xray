/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_internal.h - Internal shared declarations for analyzer visitor
 */

#ifndef XANALYZER_VISITOR_INTERNAL_H
#define XANALYZER_VISITOR_INTERNAL_H

#include "xanalyzer_visitor.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_incremental.h"
#include "../../base/xmalloc.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xdefs.h"

// Main dispatch (defined in xanalyzer_visitor.c)
XR_FUNC XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node);

// Utility functions (defined in xanalyzer_visitor.c)
XR_FUNC bool xa_check_null_safety(XaAnalyzer *analyzer, XrType *target, XrType *source,
                                  const char *context_msg, XrLocation *loc);
XR_FUNC XrType *xa_infer_type_param_from_arg(XrType *param_type, XrType *arg_type,
                                             const char *tp_name, int depth);
XR_FUNC XrType *xa_substitute_generic_call(XaInferContext *ctx, XaSymbolLinks *links,
                                           XrType *callee_type, XrType *return_type,
                                           CallExprNode *call, int arg_count,
                                           XrType **effective_arg_types);
XR_FUNC XrType *xa_infer_function_return_type(XaInferContext *ctx, AstNode *body);
XR_FUNC bool xa_body_has_return_expr(AstNode *node);
XR_FUNC bool xa_type_is_default_initializable(XaInferContext *ctx, XrType *type);

// Cross-TU helpers between xanalyzer_visitor.c (the dispatch / hoisting
// / infer entry points) and xanalyzer_visitor_decl.c (the bulk of
// decl-shaped collect logic). Not exported via xanalyzer_visitor.h
// because no caller outside src/frontend/analyzer/
// needs them.
XR_FUNC void xa_visit_collect_function_decl_only(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_function_body(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_statements_with_hoisting(XaInferContext *ctx, AstNode **stmts,
                                                       int count);
XR_FUNC void xa_visit_add_symbol_checked(XaInferContext *ctx, XaSymbol *symbol, int line);
XR_FUNC bool xa_propagate_receiver_mutations_for_ast(XaAnalyzer *analyzer, AstNode *node);
XR_FUNC bool xa_propagate_param_escape_summaries_for_ast(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *resolve_class_to_type_param(XrVMRuntime *X, XrType *type, const char **tp_names,
                                            int tp_count);
XR_FUNC void xa_set_function_type_params_from_ast(XaInferContext *ctx, XrType *fn_type,
                                                  XrGenericParam **type_params, int count);
XR_FUNC void xa_loop_scope_push(XaInferContext *ctx, XaLoopScope *scope, const char *label,
                                AstNode *node);
XR_FUNC void xa_loop_scope_pop(XaInferContext *ctx, XaLoopScope *scope);
XR_FUNC void xa_validate_loop_control(XaInferContext *ctx, AstNode *node, const char *label,
                                      bool is_continue);
XR_FUNC void xa_validate_hashable_key_type(XaInferContext *ctx, XrType *type,
                                           XaSymbolLinks *generic_links, const char *context,
                                           XrLocation *loc);
XR_FUNC void xa_validate_hashable_contract_for_class(XaInferContext *ctx, AstNode *node,
                                                     XrClassInfo *info);

// Expression visitors (defined in xanalyzer_visitor_expr.c)
XR_FUNC XrType *xa_visit_struct_literal(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_force_unwrap(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_as_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC void check_closure_capture(XaInferContext *ctx, AstNode *node, int line);
XR_FUNC void check_coro_capture(XaInferContext *ctx, AstNode *node, int line);
XR_FUNC bool xa_boundary_transfer_type_needs_explicit(const XrType *type);
XR_FUNC bool xa_boundary_arg_is_explicit_copy(AstNode *arg_node);
XR_FUNC bool xa_boundary_arg_is_shared_const(XaInferContext *ctx, AstNode *arg_node);
XR_FUNC void xa_check_boundary_transfer_arg(XaInferContext *ctx, AstNode *boundary_node,
                                            AstNode *arg_node, XrType *arg_type,
                                            const char *boundary_label);

// Unified function body visitor (collect + direct traversal)
XR_FUNC void xa_visit_function_body_unified(XaInferContext *ctx, AstNode *body);

// Shared between expr and stmt visitors
XR_FUNC const char *get_typeof_arg_name(AstNode *node);
XR_FUNC void xa_assign_check_type(XaInferContext *ctx, AstNode *node, XrType *target_type,
                                  XrType *value_type, const char *target_name,
                                  const char *target_kind);
XR_FUNC XaSymbol *xa_in_param_symbol_for_expr(XaInferContext *ctx, AstNode *expr);
XR_FUNC bool xa_type_needs_borrow_escape_guard(XrType *type);
XR_FUNC XaSymbol *xa_borrowed_param_root_symbol(XaInferContext *ctx, AstNode *expr);
XR_FUNC bool xa_method_name_mutates_receiver(const char *name);
XR_FUNC bool xa_type_contains_float(XrType *type);
XR_FUNC void xa_report_float_modulo_error(XaInferContext *ctx, AstNode *node, XrType *left,
                                          XrType *right);
XR_FUNC void xa_check_condition_type(XaInferContext *ctx, AstNode *node, XrType *cond_type);
XR_FUNC void xa_check_logical_operand_type(XaInferContext *ctx, AstNode *node, XrType *type);
struct XrClassInfo;
XR_FUNC void xa_check_member_visibility(XaInferContext *ctx, AstNode *node, XaSymbol *member,
                                        struct XrClassInfo *owner);

// Module graph exports lookup (defined in xanalyzer_visitor.c).
// Resolves an import specifier to the target module's exports hashmap.
struct XrHashMap;
XR_FUNC struct XrHashMap *resolve_graph_exports(XaAnalyzer *analyzer, const char *module_name,
                                                bool is_quoted);

#endif  // XANALYZER_VISITOR_INTERNAL_H
