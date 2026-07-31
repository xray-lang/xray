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
#include "xanalyzer_capability.h"
#include "xanalyzer_incremental.h"
#include "../../base/xstorage.h"
#include "../../base/xmalloc.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xdefs.h"

// Main dispatch (defined in xanalyzer_visitor.c)
XR_FUNC XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XaSymbol *xa_lookup_visible_symbol(XaInferContext *ctx, const char *name);

// Utility functions (defined in xanalyzer_visitor.c)
XR_FUNC bool xa_check_null_safety(XaAnalyzer *analyzer, XrType *target, XrType *source,
                                  const char *context_msg, XrLocation *loc);
XR_FUNC XrType *xa_infer_type_param_from_arg(XrType *param_type, XrType *arg_type,
                                             const char *tp_name, int depth);
XR_FUNC XrType *xa_substitute_generic_call(XaInferContext *ctx, XaSymbolLinks *links,
                                           XrType *callee_type, XrType *return_type,
                                           CallExprNode *call, int arg_count,
                                           XrType **effective_arg_types, bool writeback_inferred);
// task-221 gap C: record inferred generic type arguments on a call node so
// monomorphization/AOT cgen specialize inferred generic construction and
// generic-function calls. Defined in xanalyzer_visitor_call.c.
void xa_writeback_inferred_type_args(XrCompilerSession *session, CallExprNode *call,
                                     XrType **inferred, int type_param_count);
XR_FUNC XrType *xa_infer_function_return_type(XaInferContext *ctx, AstNode *body);
XR_FUNC bool xa_body_has_return_expr(AstNode *node);
XR_FUNC bool xa_type_is_default_initializable(XaInferContext *ctx, XrType *type);
/* Strict-null gate (E0379), defined in xanalyzer_visitor_expr.c. Every position
 * that would dereference the value at runtime must route through this, so the
 * rule has one implementation and one wording. Pass optional_chain_applies only
 * where `?.` is actually accepted by the grammar. */
XR_FUNC bool xa_check_nullable_use(XaInferContext *ctx, AstNode *node, XrType *value_type,
                                   const char *use_desc, bool optional_chain_applies);

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
XR_FUNC XaSymbol *xa_visit_bind_parameter_symbol(XaInferContext *ctx, XrParamNode *param,
                                                 int fallback_line);
XR_FUNC bool xa_propagate_receiver_mutations_for_ast(XaAnalyzer *analyzer, AstNode *node);
XR_FUNC bool xa_propagate_param_escape_summaries_for_ast(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_validate_interface_throw_effects(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_apply_param_storage_requirements_to_scope(XaInferContext *ctx,
                                                          XaSymbolLinks *links);
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
XR_FUNC void xa_parallel_capture_check(XaInferContext *ctx, AstNode *loc_node, XaSymbol *sym,
                                       bool is_write);
XR_FUNC void xa_parallel_callback_effect_check(XaInferContext *ctx, AstNode *body);
XR_FUNC bool xa_statement_can_fall_through(AstNode *node);
XR_FUNC bool xa_type_is_concurrency_handle(const XrType *type);
XR_FUNC const char *xa_concurrency_handle_label(const XrType *type);
XR_FUNC bool xa_expr_is_sys_thread_spawn_call(AstNode *expr);
XR_FUNC bool xa_freestanding_profile_enabled(XaAnalyzer *analyzer);
XR_FUNC bool xa_freestanding_stdlib_module_known(const char *module_name);
XR_FUNC bool xa_freestanding_stdlib_module_allowed(const char *module_name);
XR_FUNC bool xa_freestanding_stdlib_member_allowed(const char *module_name,
                                                   const char *member_name);
XR_FUNC const char *xa_freestanding_stdlib_member_reject_suggestion(const char *module_name);
XR_FUNC void xa_freestanding_report_unavailable(XaInferContext *ctx, AstNode *node,
                                                const char *feature, const char *suggestion);
XR_FUNC void xa_report_unknown_stdlib_member(XaInferContext *ctx, AstNode *node,
                                             const char *module_name, const char *member_name);

/* Canonical resolver for a variable reference: prefers the symbol id recorded
 * when the reference was resolved, so the answer does not depend on which scope
 * happens to be current when the question is asked. */
XR_FUNC XaSymbol *xa_resolve_variable_symbol(XaInferContext *ctx, AstNode *node);

/* Canonical recognizer for a call to a member of a VM-intrinsic built-in module
 * (`Coro.yield()`, `Coro.Local<T>()`, `CoroPool.submit()`, ...).  The receiver is
 * resolved through the symbol table and must be the built-in module symbol; a
 * user declaration that merely spells the same name resolves to that declaration
 * and is rejected here.  Every site that needs this question must call this
 * function -- recognizing an intrinsic by comparing the source spelling makes the
 * same semantic decision twice with two different answers. */
XR_FUNC bool xa_call_is_builtin_module_member(XaInferContext *ctx, const CallExprNode *call,
                                              const char *module_name, const char *member_name);

/* Task 216: a function item can carry a precise inferred effect, but storing it
 * in an unannotated variable/field creates a merge point whose type defaults to
 * MAY_THROW. Copy before widening so the declaration symbol's precise function
 * type remains unchanged. A compiler-owned effect constraint may bypass this
 * helper by using its canonical declared type. */
static inline XrType *xa_function_value_storage_type(XaInferContext *ctx, XrType *inferred) {
    if (!ctx || !ctx->analyzer || !inferred || inferred->kind != XR_KIND_FUNCTION)
        return inferred;
    XrType *stored = xr_type_copy(ctx->analyzer->isolate, inferred);
    if (!stored)
        return xr_type_new_error(ctx->analyzer->isolate);
    xr_type_function_set_throw_effect(stored, XR_FN_EFFECT_MAY_THROW);
    return stored;
}

static inline bool xa_freestanding_type_requires_tagged_value(const XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_JSON:
        case XR_KIND_RECORD:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_FUNCTION:
            return true;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (xa_freestanding_type_requires_tagged_value(type->union_type.members[i]))
                    return true;
            }
            return false;
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++) {
                if (xa_freestanding_type_requires_tagged_value(type->tuple.element_types[i]))
                    return true;
            }
            return false;
        case XR_KIND_FIXED_ARRAY:
            return xa_freestanding_type_requires_tagged_value(type->fixed_array.element_type);
        case XR_KIND_SLICE:
        case XR_KIND_POINTER:
            return xa_freestanding_type_requires_tagged_value(type->container.element_type);
        default:
            return false;
    }
}

static inline bool xa_type_has_fixed_layout_data_object(const XrType *type) {
    return type && (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
           type->instance.class_ref && type->instance.class_ref->struct_layout;
}

/* Types whose freestanding top-level const representation is materialized as
 * stable native data. Addressability and declaration validation must share
 * this predicate so addressability checks agree with backend storage guarantees. */
static inline bool xa_type_supports_const_static_data_object(const XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_STRING:
        case XR_KIND_NULL:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_TUPLE:
            return true;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return xa_type_has_fixed_layout_data_object(type);
        default:
            return false;
    }
}

static inline void xa_freestanding_report_tagged_type_unavailable(XaInferContext *ctx,
                                                                  AstNode *node, const XrType *type,
                                                                  const char *context) {
    if (!ctx || !node || !xa_freestanding_type_requires_tagged_value(type))
        return;
    char feature[192];
    if (context && context[0]) {
        snprintf(feature, sizeof(feature), "tagged/dynamic value type in %s", context);
    } else {
        snprintf(feature, sizeof(feature), "tagged/dynamic value type");
    }
    xa_freestanding_report_unavailable(ctx, node, feature,
                                       "use fixed-layout structs, fixed arrays, "
                                       "Slice<T>/Slice<byte>, enum value-structs, Buffer, or "
                                       "raw pointers instead");
}

// Expression visitors (defined in xanalyzer_visitor_expr.c)
XR_FUNC XrType *xa_visit_struct_literal(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC bool xa_pattern_has_binding(AstNode *pattern);
XR_FUNC void xa_register_pattern_bindings(XaInferContext *ctx, AstNode *pattern, XrType *slot_type);
XR_FUNC XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_force_unwrap(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_as_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC bool xa_boundary_transfer_type_needs_explicit(const XrType *type);
XR_FUNC bool xa_expr_creates_fresh_root(XaInferContext *ctx, AstNode *value);
XR_FUNC bool xa_boundary_arg_is_explicit_copy(AstNode *arg_node);
XR_FUNC bool xa_boundary_arg_is_shared(XaInferContext *ctx, AstNode *arg_node);
XR_FUNC XaSymbol *xa_boundary_move_source_symbol(XaInferContext *ctx, AstNode *arg_node);
XR_FUNC bool xa_symbol_has_shared_provenance(const XaSymbol *sym);
XR_FUNC bool xa_expr_yields_shared_provenance(XaInferContext *ctx, AstNode *expr, XrType *type);
XR_FUNC void xa_check_boundary_transfer_arg(XaInferContext *ctx, AstNode *boundary_node,
                                            AstNode *arg_node, XrType *arg_type,
                                            const char *boundary_label);
XR_FUNC void xa_check_arg_access_authorization(XaInferContext *ctx, AstNode *call_node,
                                               AstNode *arg_node, XrCallArgAccess access, int slot,
                                               XrParamMode param_mode);

// Unified function body visitor (collect + direct traversal)
XR_FUNC void xa_visit_function_body_unified(XaInferContext *ctx, AstNode *body);

// Shared between expr and stmt visitors
XR_FUNC const char *get_typeof_arg_name(AstNode *node);
XR_FUNC void xa_assign_check_type(XaInferContext *ctx, AstNode *node, XrType *target_type,
                                  XrType *value_type, const char *target_name,
                                  const char *target_kind);
XR_FUNC XaSymbol *xa_root_variable_symbol_for_expr(XaInferContext *ctx, AstNode *expr);
XR_FUNC XaSymbol *xa_read_param_symbol_for_expr(XaInferContext *ctx, AstNode *expr);
XR_FUNC bool xa_type_needs_borrow_escape_guard(XrType *type);
XR_FUNC bool xa_type_has_movable_root(XrType *type);
XR_FUNC XaSymbol *xa_borrowed_param_root_symbol(XaInferContext *ctx, AstNode *expr);
XR_FUNC bool xa_type_contains_span_view(XrType *type);
XR_FUNC void xa_check_span_generic_class_type_args(XaInferContext *ctx, AstNode *loc_node,
                                                   const char *class_name, XrType **type_args,
                                                   int type_arg_count);
XR_FUNC bool xa_expr_has_stable_borrow_owner(AstNode *expr);
XR_FUNC bool xa_type_can_own_span_view(XrType *type);
XR_FUNC XaSymbol *xa_span_borrow_owner_receiver_symbol(XaInferContext *ctx, AstNode *expr,
                                                       XrType *receiver_type);
XR_FUNC void xa_report_view_expr_requires_target(XaInferContext *ctx, AstNode *node,
                                                 const char *kind);
XR_FUNC void xa_register_active_span_borrow(XaInferContext *ctx, XaSymbol *view_sym, AstNode *value,
                                            XrType *value_type);
XR_FUNC void xa_clear_active_span_borrow_for_view(XaInferContext *ctx, XaSymbol *view_sym);
XR_FUNC void xa_clear_active_span_borrows_in_scope(XaInferContext *ctx, XaScope *scope);
XR_FUNC void xa_check_active_span_borrow_owner_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                        XaSymbol *owner_sym, const char *operation);
/* Return a live local strong alias of move_sym's root, if one is used after
 * the current statement. analysis_failed is set on evidence-allocation
 * failure so the caller can fail closed. */
XR_FUNC XaSymbol *xa_find_live_strong_alias_after_current(XaInferContext *ctx, XaSymbol *move_sym,
                                                          bool *analysis_failed);
XR_FUNC void xa_mark_root_alias_state(XaInferContext *ctx, XaRootId root, XaRootAliasState state);
XR_FUNC void xa_check_active_span_borrow_owner_path_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                             XaSymbol *owner_sym,
                                                             const char *owner_path,
                                                             const char *operation);
XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_expr(XaInferContext *ctx, AstNode *expr,
                                                     char *path_buf, size_t path_buf_size);
XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_owner_expr(XaInferContext *ctx, AstNode *expr,
                                                           char *path_buf, size_t path_buf_size);
XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_member_write(XaInferContext *ctx, AstNode *object,
                                                             const char *member,
                                                             XrType *member_type, char *path_buf,
                                                             size_t path_buf_size);
XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_index_write(XaInferContext *ctx, AstNode *array,
                                                            AstNode *index, XrType *element_type,
                                                            char *path_buf, size_t path_buf_size);
XR_FUNC void xa_visit_inline_statement_sequence_with_cursor(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_check_span_value_escape(XaInferContext *ctx, AstNode *loc_node, XrType *value_type,
                                        const char *escape_context);
XR_FUNC void xa_check_span_borrow_source_stable(XaInferContext *ctx, AstNode *loc_node,
                                                AstNode *source, const char *operation);
XR_FUNC bool xa_type_contains_float(XrType *type);
XR_FUNC void xa_report_float_modulo_error(XaInferContext *ctx, AstNode *node, XrType *left,
                                          XrType *right);
XR_FUNC void xa_check_condition_type(XaInferContext *ctx, AstNode *node, XrType *cond_type);
/* Reject `is` / `as` targets the runtime carries no identity for. Without
 * this the lowering emits a type test with no target operand, which is
 * malformed Xi IR rather than a user-facing diagnostic. */
XR_FUNC void xa_check_runtime_testable_type(XaInferContext *ctx, AstNode *node, XrTypeRef *tref,
                                            const char *op_name);
XR_FUNC void xa_check_logical_operand_type(XaInferContext *ctx, AstNode *node, XrType *type);
struct XrClassInfo;
XR_FUNC void xa_check_member_visibility(XaInferContext *ctx, AstNode *node, XaSymbol *member,
                                        struct XrClassInfo *owner);
XR_FUNC void xa_check_constructor_visibility(XaInferContext *ctx, AstNode *node,
                                             struct XrClassInfo *owner);

// Module graph export-symbol lookup (defined in xanalyzer_visitor.c).
// Resolves an import specifier to the target module's semantic export-symbol hashmap.
struct XrHashMap;
XR_FUNC struct XrHashMap *resolve_graph_export_symbols(XaAnalyzer *analyzer,
                                                       const char *module_name, bool is_quoted);

#endif  // XANALYZER_VISITOR_INTERNAL_H
