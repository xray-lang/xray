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
#include "../base/xtarget_data_layout.h"
#include "../toolchain/xcompiler_session.h"

#include <string.h>

struct AstNode;
struct ImportMember;
struct XrType;
struct XaSymbol;
typedef struct MethodDeclNode MethodDeclNode;
typedef struct ClassDeclNode ClassDeclNode;

static inline const XrTargetDataLayout *xi_lower_target_data_layout(const XiLower *l) {
    XrCompilerSession *session = l ? xr_compiler_session_current_for_isolate(l->isolate) : NULL;
    const XrTargetDataLayout *layout =
        session ? xr_compiler_session_target_data_layout(session) : NULL;
    /* Standalone Xi unit tests have no compiler session and model the VM
     * backend, whose target is explicitly the host ABI. */
    return layout ? layout : xr_target_data_layout_host();
}

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

/* Apply a semantic storage-domain decision to a freshly materialized Xi
 * value. This is shared by declaration/return planning and by heap-store
 * lowering: a system-domain aggregate must not acquire an execution-local
 * child whose owner heap is unavailable when that aggregate is destroyed. */
static inline bool xi_lower_mark_storage_allocation(XiValue *v, uint8_t storage_mode) {
    if (!v)
        return false;
    switch (v->op) {
        case XI_ARRAY_NEW:
        case XI_MAP_NEW:
        case XI_SET_NEW:
            xi_value_set_allocation_storage_mode(v, storage_mode);
            return true;
        case XI_OBJECT_NEW:
            xi_object_set_storage_mode(v, storage_mode);
            return true;
        case XI_TUPLE_NEW:
            xi_tuple_set_storage_mode(v, storage_mode);
            return true;
        case XI_CALL:
        case XI_CALL_METHOD:
            if (xi_value_is_constructor_call(v)) {
                xi_value_set_allocation_storage_mode(v, storage_mode);
                return true;
            }
            return false;
        case XI_CHAN_NEW:
            return true;
        case XI_CALL_BUILTIN:
            if (v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_WITH_CAPACITY ||
                v->array_intrinsic_kind == XI_ARRAY_INTRINSIC_FILLED_NEW ||
                (v->aux && (strcmp((const char *) v->aux, "array_copy_new") == 0 ||
                           strcmp((const char *) v->aux, "StringBuilder") == 0 ||
                           strcmp((const char *) v->aux, "copy") == 0))) {
                xi_value_set_allocation_storage_mode(v, storage_mode);
                return true;
            }
            return false;
        default:
            return false;
    }
}

/* ========== Braun SSA Primitives ========== */

XR_FUNC int xi_lower_var_create(XiLower *l, uint32_t symbol_id, const char *name,
                                struct XrType *type);
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val);
XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk);
XR_FUNC void xi_lower_braun_seal(XiLower *l, XiBlock *blk);
XR_FUNC bool xi_lower_capture_source_vars(XiLower *l);

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
XR_FUNC XiValue *xi_lower_enum_namespace_value(XiLower *l, struct XaSymbol *enum_sym,
                                               const char *enum_name, int line);

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
XR_FUNC void xi_lower_inherit_evidence(XiLower *child, const XiLower *parent);
XR_FUNC void xi_lower_publish_effect_sidecars(XiFunc *func, struct XaAnalyzer *analyzer,
                                              struct XaSymbol *symbol);
XR_FUNC bool xi_lower_reject_error_type(XiLower *l, const struct XrType *type, const char *context,
                                        int line);
XR_FUNC struct XrType *xi_lower_type_or_any(XiLower *l, struct XrType *type, const char *context,
                                             int line);
XR_FUNC XiSourceSpan xi_lower_push_source_span(XiLower *l, const struct AstNode *node);
XR_FUNC void xi_lower_pop_source_span(XiLower *l, XiSourceSpan previous);
XR_FUNC uint32_t xi_lower_source_node_id(const XiLower *l, const struct AstNode *node);
XR_FUNC void xi_lower_bind_module_body_id(XiLower *l);
XR_FUNC void xi_lower_bind_function_body_id(XiLower *l, uint32_t source_node_id,
                                            uint32_t source_span_id);
XR_FUNC void xi_lower_bind_method_body_id(XiLower *l, uint32_t source_node_id);
XR_FUNC uint32_t xi_lower_next_key_access_ordinal(XiLower *l, uint32_t source_span_id,
                                                  uint8_t access_op);
XR_FUNC void xi_lower_bind_callsite_id(XiLower *l, XiValue *call, uint32_t source_node_id);
XR_FUNC void xi_lower_bind_class_field_id(XiLower *l, XiValue *access,
                                          const struct XrType *receiver_type,
                                          const char *field_name);
XR_FUNC void xi_lower_bind_json_codec_id(XiLower *l, XiValue *value, uint32_t source_node_id,
                                         uint8_t expected_kind);
XR_FUNC void xi_lower_bind_object_access_id(XiLower *l, XiValue *access, const char *field_name,
                                            uint32_t source_span_id, uint8_t access_kind);
XR_FUNC void xi_lower_bind_object_merge_id(XiLower *l, XiValue *merge, uint32_t source_node_id);
XR_FUNC void xi_lower_bind_key_access_id(XiLower *l, XiValue *access, uint32_t source_span_id,
                                         uint32_t body_ordinal, uint8_t access_op);
XR_FUNC void xi_lower_bind_map_shape_id(XiLower *l, XiValue *literal, uint32_t source_span_id,
                                        uint8_t container_kind);

typedef struct XiSequenceEvidenceIds {
    uint32_t sequence_access_id;
    uint32_t capacity_op_id;
    uint32_t bulk_op_id;
    uint32_t encoding_op_id;
} XiSequenceEvidenceIds;

typedef struct XiSequenceEvidenceKinds {
    uint8_t sequence_access_kind;
    uint8_t capacity_op_kind;
    uint8_t bulk_op_kind;
    uint8_t encoding_op_kind;
} XiSequenceEvidenceKinds;

XR_FUNC void xi_lower_take_sequence_evidence_ids(XiLower *l, uint32_t source_span_id,
                                                 XiSequenceEvidenceKinds kinds,
                                                 XiSequenceEvidenceIds *out_ids);
XR_FUNC void xi_lower_apply_sequence_evidence_ids(XiValue *value, const XiSequenceEvidenceIds *ids);

/* ========== Function Lowering (xi_lower.c) ========== */

XR_FUNC XiFunc *xi_lower_func_impl(struct AstNode *func_node, struct XaAnalyzer *analyzer,
                                   struct XrVMRuntime *isolate, XiLower *parent_ctx,
                                   const struct XaTypedProgram *typed_program);
XR_FUNC void xi_lower_func_add_child(XiFunc *parent, XiFunc *child);

/* Rewrite direct calls to generator functions into XI_GEN_CALL across the whole
 * function tree. Call once on the program/function root after lowering. */
XR_FUNC void xi_lower_rewrite_generator_calls(XiFunc *root);

/* ========== AST Lowering Primitives ========== */

XR_FUNC XiValue *xi_lower_expr(XiLower *l, struct AstNode *node);
XR_FUNC struct XrAggregateLayout *xi_lower_type_struct_layout(XiLower *l, struct XrType *type);
XR_FUNC bool xi_lower_type_uses_read_place(XiLower *l, struct XrType *type);
XR_FUNC bool xi_lower_boundary_transfer_arg(XiLower *l, struct AstNode *child, XiValue **out_value,
                                            uint8_t *out_mode);
XR_FUNC void xi_lower_stmt(XiLower *l, struct AstNode *node);
struct ImportMember;
XR_FUNC bool xi_lower_import_member_is_type_only(const XiLower *l,
                                                 const struct ImportMember *member);
XR_FUNC struct XrType *xi_lower_node_type(XiLower *l, struct AstNode *node);

/* ========== Cross-boundary helpers (xi_lower_expr.c, called from stmt) ========== */

XR_FUNC XiValue *xi_lower_function_decl(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_enum_decl(XiLower *l, struct AstNode *node);
XR_FUNC void xi_lower_class_decl(XiLower *l, struct AstNode *node);
XR_FUNC XiFunc *xi_lower_method_as_func(XiLower *l, MethodDeclNode *m, bool is_inst,
                                        ClassDeclNode *cd, bool owner_is_value_aggregate,
                                        struct XrType *receiver_type, uint32_t source_span_id);
XR_FUNC const char *xi_lower_enum_method_hidden_name(XiFunc *arena, const char *enum_name,
                                                     const char *method_name, bool is_static);
XR_FUNC XiValue *xi_lower_apply_numeric_conversion_witness(XiLower *l, struct AstNode *source_node,
                                                           XiValue *value,
                                                           struct XrType *target_type);

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
XR_FUNC void xi_lower_cleanup_scope_push(XiLower *l);
XR_FUNC void xi_lower_cleanup_scope_pop_normal(XiLower *l, int line);
XR_FUNC void xi_lower_cleanup_run_to_depth(XiLower *l, int target_depth, int line);
XR_FUNC bool xi_lower_cleanup_has_active_site(XiLower *l);
XR_FUNC void xi_lower_prepare_cleanup_places(XiLower *l, AstNode *root);
XR_FUNC bool xi_lower_cleanup_symbol_needs_place(const XiLower *l, uint32_t symbol_id);
XR_FUNC bool xi_lower_cleanup_bind_place(XiLower *l, int var_id, XiValue *initial_value, int line);
XR_FUNC XiValue *xi_lower_parallel_plan_lifecycle_call(XiLower *l, AstNode *node, XiValue *plan,
                                                       const char *method);
XR_FUNC bool xi_lower_cleanup_register_parallel_end(XiLower *l, AstNode *node, XiValue *plan);

/* Emit XI_IS test against the given XrTypeRef on a pre-lowered value. */
struct XrTypeRef;
XR_FUNC XiValue *xi_lower_is_test(XiLower *l, XiValue *val, struct XrTypeRef *tref, int line,
                                  uint32_t source_node_id);
XR_FUNC XiValue *xi_lower_checktype_for_type(XiLower *l, struct AstNode *node, XiValue *val,
                                             struct XrType *target_type);

/* ========== Error Propagation (xi_lower_misc.c) ========== */

/* Insert error channel check after a producer that may raise (task 216).
 *
 * The check is generated CONSTRUCTIVELY by callee effect: `producer_may_throw`
 * must be false only when the producer is proven NO_THROW, in which case no
 * XI_ERR_CHECK node is emitted at all (the check could never fire). When true,
 * behaves as before — inside a try block it branches to the current catch
 * target; otherwise it propagates by writing the error and returning. */
XR_FUNC void xi_lower_insert_err_check(XiLower *l, struct AstNode *node, bool producer_may_throw);

/* Re-propagate a materialized error value through the value channel,
 * running any finally blocks it escapes (see xi_lower_stmt.c). */
XR_FUNC void xi_lower_reprop_error(XiLower *l, XiValue *val, struct AstNode *node);

/* ========== Misc Expression Lowering (xi_lower_misc.c) ========== */

XR_FUNC XiValue *xi_lower_enum_access(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_cancelled_expr(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_move_expr(XiLower *l, struct AstNode *node);
XR_FUNC XiValue *xi_lower_object_literal(XiLower *l, struct AstNode *node);
XR_FUNC bool xi_lower_fill_canonical_object_field_names(XiLower *l, const struct XrType *type,
                                                        const char **names, int count,
                                                        uint32_t source_span_id);

#endif  // XI_LOWER_INTERNAL_H
