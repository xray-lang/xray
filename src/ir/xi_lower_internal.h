/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_internal.h - Shared declarations between xi_lower.c and xi_lower_stmt.c
 *
 * Internal header: do not include from outside the xi_lower_* translation units.
 */

#ifndef XI_LOWER_INTERNAL_H
#define XI_LOWER_INTERNAL_H

#include "xi_lower.h"
#include "xi.h"
#include "../base/xdefs.h"

#include <string.h>

struct AstNode;
struct XrType;
typedef struct MethodDeclNode MethodDeclNode;
typedef struct ClassDeclNode ClassDeclNode;

/* Copy a string into the XiFunc arena so it survives AST destruction. */
static inline const char *arena_strdup(XiFunc *f, const char *s) {
    if (!s)
        return NULL;
    uint32_t len = (uint32_t) strlen(s);
    char *copy = (char *) xi_func_arena_alloc(f, len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

/* ========== Braun SSA Primitives ========== */

XR_FUNC int xi_lower_var_create(XiLower *l, uint32_t symbol_id, const char *name,
                                struct XrType *type);
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val);
XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk);
XR_FUNC void xi_lower_braun_seal(XiLower *l, XiBlock *blk);
XR_FUNC void xi_lower_capture_source_vars(XiLower *l);

/* ========== Variable / Scope Lookup (xi_lower.c) ========== */

XR_FUNC int xi_lower_var_find(XiLower *l, uint32_t symbol_id, const char *name);
XR_FUNC int xi_lower_resolve_upvalue(XiLower *l, uint32_t symbol_id, const char *name,
                                     struct XrType **out_type);

/* ========== Top-level Binding Helpers (xi_lower.c) ========== */

/* A resolved program-level binding.  Carries both the slot index
 * (used by XI_GET/SET_SHARED in compiled module mode) and the name
 * (used by XI_GET/SET_GLOBAL in REPL mode), so the emit helper can
 * pick the right opcode without callers branching on repl_mode.
 *
 * slot < 0 / name == NULL means the lookup missed.  When the binding
 * is valid, slot is always >= 0 and name points into the program-
 * scope XiLower's arena-owned variable name. */
typedef struct XiTopBinding {
    int slot;
    const char *name;
    struct XrType *type;
} XiTopBinding;

/* Walk the parent chain to find a program-level top binding by
 * symbol_id (preferred) or by name fallback.  Returns an invalid
 * binding (slot=-1, name=NULL) on miss.  Mode-agnostic: the caller
 * never branches on repl_mode. */
XR_FUNC XiTopBinding xi_lower_find_top_binding(XiLower *l, uint32_t symbol_id, const char *name);

/* Emit a load for the given top binding.  Picks XI_GET_GLOBAL in
 * REPL mode (name-keyed globals dict) and XI_GET_SHARED otherwise
 * (slot-indexed shared array).  type may be NULL; the helper falls
 * back to binding.type and finally to l->type_any.
 * Caller must pass a valid binding (xi_top_binding_valid). */
XR_FUNC XiValue *xi_lower_emit_top_load(XiLower *l, XiTopBinding binding, struct XrType *type);

/* Emit a store of `val` to the given top binding.  Mirrors
 * xi_lower_emit_top_load on the opcode choice and sets
 * XI_FLAG_SIDE_EFFECT so the SSA value is not DCE'd.
 * Returns the store XiValue, or NULL on allocation failure. */
XR_FUNC XiValue *xi_lower_emit_top_store(XiLower *l, XiTopBinding binding, XiValue *val);

/* True iff the binding is a successful lookup result. */
static inline bool xi_top_binding_valid(XiTopBinding b) {
    return b.slot >= 0 && b.name != NULL;
}

/* ========== Context Init / Cleanup (xi_lower.c) ========== */

XR_FUNC void xi_lower_init(XiLower *l, struct XaAnalyzer *analyzer, struct XrVMRuntime *isolate);
XR_FUNC void xi_lower_cleanup(XiLower *l);

/* ========== Function Lowering (xi_lower.c) ========== */

XR_FUNC XiFunc *xi_lower_func_impl(struct AstNode *func_node, struct XaAnalyzer *analyzer,
                                   struct XrVMRuntime *isolate, XiLower *parent_ctx);
XR_FUNC void xi_lower_func_add_child(XiFunc *parent, XiFunc *child);

/* Rewrite direct calls to generator functions into XI_GEN_CALL across the whole
 * function tree. Call once on the program/function root after lowering. */
XR_FUNC void xi_lower_rewrite_generator_calls(XiFunc *root);

/* ========== AST Lowering Primitives ========== */

XR_FUNC XiValue *xi_lower_expr(XiLower *l, struct AstNode *node);
XR_FUNC bool xi_lower_boundary_transfer_arg(XiLower *l, struct AstNode *child, XiValue **out_value,
                                            uint8_t *out_mode);
XR_FUNC void xi_lower_stmt(XiLower *l, struct AstNode *node);
XR_FUNC struct XrType *xi_lower_node_type(XiLower *l, struct AstNode *node);

/* ========== Cross-boundary helpers (xi_lower_expr.c, called from stmt) ========== */

XR_FUNC XiValue *xi_lower_function_decl(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_enum_decl(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_class_decl(XiLower *l, struct AstNode *node);
XR_FUNC XiFunc *xi_lower_method_as_func(XiLower *l, MethodDeclNode *m, bool is_inst,
                                        ClassDeclNode *cd, struct XrType *receiver_type);
XR_FUNC const char *xi_lower_enum_method_hidden_name(XiFunc *arena, const char *enum_name,
                                                     const char *method_name, bool is_static);

/* ========== Method Symbol Resolution ========== */

/* Resolve a method name to a global SymbolId through the isolate's symbol
 * table.  Runs during lowering (main thread), so the isolate is always
 * available.  Returns 0 on failure. */
XR_FUNC int32_t xi_lower_method_symbol(XiLower *l, const char *method_name);

XR_FUNC XiValue *xi_lower_bool_condition(XiLower *l, XiValue *cond);

/* ========== Compound Statement Lowering (xi_lower_stmt.c) ========== */

XR_FUNC void xi_lower_select(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_scope_block(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_for_in(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_try_catch(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_match(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_pattern_test(XiLower *l, XiValue *subject, struct AstNode *pattern);
XR_FUNC void xi_lower_defer_scope_push(XiLower *l);
XR_FUNC void xi_lower_defer_scope_pop_normal(XiLower *l, int line);
XR_FUNC void xi_lower_defer_run_to_depth(XiLower *l, int target_depth, int line);
XR_FUNC bool xi_lower_defer_has_active_mark(XiLower *l);

/* Emit XI_IS test against the given XrTypeRef on a pre-lowered value. */
struct XrTypeRef;
XR_FUNC XiValue *xi_lower_is_test(XiLower *l, XiValue *val, struct XrTypeRef *tref, int line);
XR_FUNC XiValue *xi_lower_checktype_for_type(XiLower *l, struct AstNode *node, XiValue *val,
                                             struct XrType *target_type);

/* ========== Error Propagation (xi_lower_misc.c) ========== */

/* Insert error channel check after a fallible call.  If inside a try
 * block, jumps to the current catch target.  Otherwise propagates by
 * writing error + returning from the function. */
XR_FUNC void xi_lower_insert_err_check(XiLower *l, struct AstNode *node);

/* Re-propagate a materialized error value through the value channel,
 * running any finally blocks it escapes (see xi_lower_stmt.c). */
XR_FUNC void xi_lower_reprop_error(XiLower *l, XiValue *val, struct AstNode *node);

/* ========== Misc Expression Lowering (xi_lower_misc.c) ========== */

XR_FUNC XiValue *xi_lower_enum_access(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_enum_convert(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_cancelled_expr(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_move_expr(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_object_literal(XiLower *l, struct AstNode *node);

#endif  // XI_LOWER_INTERNAL_H
