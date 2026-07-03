/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower_expr.c - Expression lowering (extracted from xi_lower.c)
 *
 * Contains: type inference helpers, all lower_* expression functions,
 * and the xi_lower_expr() dispatch switch.
 */

#include "xi_lower_internal.h"
#include "xi.h"
#include "xi_effect.h"
#include "xi_lower_expr_helpers.h"
#include "xi_own.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/parser/xtype_ref.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xa_selection.h"
#include "../frontend/lexer/xlex.h"
#include "../runtime/class/xclass_system.h"

#include "../runtime/class/xenum.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/object/xstring.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"
#include "../base/xglobal_indices.h"
#include "../base/xconstants.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/value/xffi_sig.h"
#include "../shared/xr_encoding_core.h"

#include <string.h>
#include <stdio.h>
#include <float.h>
#include <math.h>

/* ========== Forward Declarations ========== */

static XiValue *lower_try_construct_call(XiLower *l, AstNode *node, CallExprNode *call);

static int pack_go_aux(int link_mode) {
    return link_mode & XI_GO_AUX_LINK_MASK;
}

static XaSymbol *xi_lower_lookup_class_symbol(XiLower *l, const char *name) {
    if (!l || !l->analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(l->analyzer, name);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_in_scope(l->analyzer, name, l->analyzer->global_scope);
    if (sym && sym->kind == XA_SYM_CLASS)
        return sym;
    sym = xa_analyzer_lookup_deep(l->analyzer, name);
    return (sym && sym->kind == XA_SYM_CLASS) ? sym : NULL;
}

static XrStructLayout *xi_lower_lookup_struct_layout(XiLower *l, const char *name) {
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return (links && links->class_info) ? links->class_info->struct_layout : NULL;
}

static XrStructLayout *xi_lower_type_struct_layout(XiLower *l, struct XrType *type) {
    XrStructLayout *layout = xi_lower_struct_layout_of(type);
    if (layout)
        return layout;
    if (!type)
        return NULL;
    const char *class_name = xr_type_get_class_name(type);
    return class_name ? xi_lower_lookup_struct_layout(l, class_name) : NULL;
}

static bool xi_lower_type_is_named_instance(const XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_lower_type_is_named_instance(type->union_type.members[i], name))
                return true;
        }
    }
    return false;
}

static bool xi_lower_method_may_suspend(const XrType *receiver_type, const char *method,
                                        int nargs) {
    if (!receiver_type || !method)
        return false;
    if (xi_lower_type_is_named_instance(receiver_type, "WorkQueue"))
        return strcmp(method, "pop") == 0 && (nargs == 0 || nargs == 1);
    if (xi_lower_type_is_named_instance(receiver_type, "ResultGroup"))
        return strcmp(method, "recv") == 0 && nargs == 0;
    return false;
}

static XrStructLayout *xi_lower_value_struct_layout(XiLower *l, XiValue *v) {
    XrStructLayout *layout = xi_lower_type_struct_layout(l, v ? v->type : NULL);
    if (layout)
        return layout;
    while (v && (xi_copy_is_identity_alias(v) || v->op == XI_MOVE) && v->nargs >= 1)
        v = v->args[0];
    layout = xi_lower_type_struct_layout(l, v ? v->type : NULL);
    if (layout)
        return layout;
    if (!v || v->op != XI_STRUCT_GET)
        return NULL;
    XrStructLayout *parent = (XrStructLayout *) v->aux;
    if (!parent || v->aux_int < 0 || v->aux_int >= parent->field_count)
        return NULL;
    XrStructFieldLayout *field = &parent->fields[v->aux_int];
    return field->native_type == XR_NATIVE_STRUCT ? field->sub_layout : NULL;
}

static bool xi_lower_type_needs_value_clone(XiLower *l, struct XrType *type) {
    return type && (type->is_value_type || xi_lower_type_struct_layout(l, type) != NULL);
}

static bool xi_lower_value_needs_value_clone(XiLower *l, XiValue *v) {
    return v && xi_lower_type_needs_value_clone(l, v->type);
}

static bool xi_lower_value_is_fresh_value_struct(XiValue *v) {
    return v && v->op == XI_STRUCT_NEW && !xi_var_id_is_valid(v->var_id);
}

static void xi_lower_mark_value_clone_copy(XiValue *v) {
    if (v && v->op == XI_COPY)
        v->aux_int = XI_COPY_KIND_VALUE_CLONE;
}

static XiValue *xi_lower_apply_primitive_type_view(XiLower *l, AstNode *node, XiValue *val,
                                                   struct XrType *target_type) {
    if (!l || !node || !val || !val->type || !target_type || xr_type_equals(val->type, target_type))
        return val;
    if (XR_TYPE_IS_FLOAT(val->type) && XR_TYPE_IS_FLOAT(target_type) &&
        target_type->native_width == XR_NATIVE_F32) {
        XiValue *n = xi_value_new(l->func, l->cur_block, XI_NARROW_F32, target_type, 1);
        if (!n)
            return val;
        n->args[0] = val;
        n->line = (uint32_t) node->line;
        return n;
    }
    if (!((XR_TYPE_IS_INT(val->type) && XR_TYPE_IS_INT(target_type)) ||
          (XR_TYPE_IS_FLOAT(val->type) && XR_TYPE_IS_FLOAT(target_type))))
        return val;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, target_type, 1);
    if (!copy)
        return val;
    copy->args[0] = val;
    copy->line = (uint32_t) node->line;
    return copy;
}

XR_FUNC XiValue *xi_lower_checktype_for_type(XiLower *l, AstNode *node, XiValue *val,
                                             struct XrType *target_type) {
    if (!l || !l->func || !node || !val || !target_type || XR_TYPE_IS_UNKNOWN(target_type))
        return val;
    if (val->type && xr_type_assignable(target_type, val->type))
        return xi_lower_apply_primitive_type_view(l, node, val, target_type);

    XrType *check_type = target_type;
    bool allow_null = target_type->is_nullable ||
                      xr_type_intrinsically_includes_null(target_type) ||
                      XR_TYPE_IS_NULL(target_type);
    if (target_type->is_nullable)
        check_type = xr_type_non_nullable(l->isolate, target_type);
    if (!check_type || XR_TYPE_IS_UNKNOWN(check_type) || XR_TYPE_IS_UNION(check_type))
        return val;

    uint8_t tid = xr_type_to_tid(check_type);
    if (tid == XR_TID_NULL && !XR_TYPE_IS_NULL(check_type))
        return val;

    XiValue *check = xi_value_new(l->func, l->cur_block, XI_CHECKTYPE, target_type, 1);
    if (!check)
        return val;
    check->args[0] = val;
    check->aux_int = ((int64_t) tid << 1) | (allow_null ? 1 : 0);
    check->line = (uint32_t) node->line;
    return check;
}

static XiFunc *lower_resolve_static_callee_func_in_scope(XiFunc *scope, XiValue *callee) {
    while (callee && xi_copy_is_identity_alias(callee) && callee->nargs >= 1)
        callee = callee->args[0];
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (XiFunc *) callee->aux;
    if (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW && callee->aux)
        return (XiFunc *) callee->aux;
    if (callee->op == XI_GET_SHARED) {
        int64_t slot = callee->aux_int;
        for (XiFunc *fn = scope; fn; fn = fn->parent_func) {
            if (fn->shared_slot_funcs && slot >= 0 && slot < (int64_t) fn->shared_slot_func_count &&
                fn->shared_slot_funcs[slot])
                return fn->shared_slot_funcs[slot];
        }
    }
    return NULL;
}

static XiFunc *lower_resolve_static_callee_func(XiLower *l, XiValue *callee) {
    return lower_resolve_static_callee_func_in_scope(l ? l->func : NULL, callee);
}

/* Post-lowering rewrite: a direct call to a generator function does not run the
 * body — it constructs a coroutine-backed iterator. Rewrite XI_CALL -> XI_GEN_CALL
 * for every call whose static callee is a generator (entry_type == 2). This runs
 * after the whole function tree is lowered (so every callee's entry_type is set,
 * including forward/nested references) and before escape/ownership analysis (so
 * the generator call's coroutine-capture escape semantics are honored). The call
 * result type is already Iterator<T> (the generator's declared return type), so
 * only the opcode changes. */
static void xi_lower_rewrite_generator_calls_in(XiFunc *f) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_CALL || v->nargs < 1)
                continue;
            XiFunc *callee = lower_resolve_static_callee_func_in_scope(f, v->args[0]);
            if (callee && callee->entry_type == 2 /* XR_ENTRY_GENERATOR */)
                v->op = XI_GEN_CALL;
        }
    }
    for (uint16_t c = 0; c < f->nchildren; c++)
        xi_lower_rewrite_generator_calls_in(f->children[c]);
}

XR_FUNC void xi_lower_rewrite_generator_calls(XiFunc *root) {
    xi_lower_rewrite_generator_calls_in(root);
}

static bool lower_value_param_is_readonly_depth(XiFunc *callee, uint16_t pidx, int depth);

static bool lower_call_arg_is_readonly_forward(XiFunc *scope, XiValue *call, uint16_t arg_idx,
                                               int depth) {
    if (!scope || !call || call->op != XI_CALL || arg_idx == 0 || arg_idx >= call->nargs)
        return false;
    XiFunc *target = lower_resolve_static_callee_func_in_scope(scope, call->args[0]);
    if (!target)
        return false;
    return lower_value_param_is_readonly_depth(target, (uint16_t) (arg_idx - 1), depth + 1);
}

static bool lower_value_param_is_readonly_depth(XiFunc *callee, uint16_t pidx, int depth) {
    if (!callee || pidx >= callee->nparams)
        return false;
    if (depth > 32)
        return false;
    XiValue *param = callee->params[pidx];
    if (!param)
        return false;

    for (uint32_t b = 0; b < callee->nblocks; b++) {
        XiBlock *blk = callee->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *user = blk->values[i];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != param)
                    continue;
                if (lower_call_arg_is_readonly_forward(callee, user, a, depth))
                    continue;
                if (xi_own_value_arg_is_consuming(user, a))
                    return false;
                if ((xi_op_default_effects(user->op) | user->flags) & XI_FLAG_WRITES_MEM)
                    return false;
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == param)
                    return false;
            }
        }
        if (blk->kind == XI_BLOCK_RETURN && blk->control == param)
            return false;
    }
    return true;
}

static bool lower_value_param_is_readonly(XiFunc *callee, uint16_t pidx) {
    return lower_value_param_is_readonly_depth(callee, pidx, 0);
}

static void lower_apply_auto_borrow_param_modes(XiLower *l, XiFunc *callee,
                                                const uint8_t *explicit_modes, int explicit_count,
                                                uint8_t *out_modes, int mode_count) {
    if (!out_modes || mode_count <= 0)
        return;
    for (int i = 0; i < mode_count; i++) {
        out_modes[i] = (explicit_modes && i < explicit_count) ? explicit_modes[i] : XR_PARAM_VALUE;
    }
    if (!l || !callee || callee->nparams == 0)
        return;

    int n = callee->nparams < (uint16_t) mode_count ? (int) callee->nparams : mode_count;
    for (int i = 0; i < n; i++) {
        if (out_modes[i] == XR_PARAM_VALUE && lower_value_param_is_readonly(callee, (uint16_t) i))
            out_modes[i] = XR_PARAM_IN;
    }
}

static XiValue *xi_lower_narrow_for_native_field(XiLower *l, AstNode *node, XiValue *val,
                                                 uint8_t native_type);
static struct XrType *xi_lower_struct_field_type(XiLower *l, struct XrType *fallback,
                                                 XrStructLayout *layout, int field_index);

#define XI_LOWER_VALUE_LIST_STACK_CAP 32
#define XI_LOWER_MAX_VARIADIC_VALUES ((int) UINT16_MAX)

typedef struct XiLowerValueList {
    XiValue **items;
    int count;
    int cap;
} XiLowerValueList;

static void xi_lower_value_list_init(XiLowerValueList *list, XiValue **stack_items, int stack_cap) {
    list->items = stack_items;
    list->count = 0;
    list->cap = stack_cap;
}

static bool xi_lower_value_list_grow(XiLower *l, XiLowerValueList *list, int max_items) {
    int next_cap = list->cap > 0 ? list->cap * 2 : 1;
    if (next_cap <= list->count)
        next_cap = list->count + 1;
    if (next_cap > max_items)
        next_cap = max_items;
    if (next_cap <= list->cap)
        return false;

    XiValue **items = (XiValue **) xi_func_arena_alloc(
        l->func, (uint32_t) ((size_t) next_cap * sizeof(XiValue *)));
    if (!items) {
        l->had_error = true;
        return false;
    }
    if (list->count > 0)
        memcpy(items, list->items, (size_t) list->count * sizeof(XiValue *));
    list->items = items;
    list->cap = next_cap;
    return true;
}

static bool xi_lower_value_list_push(XiLower *l, XiLowerValueList *list, XiValue *value,
                                     int max_items, const char *what, int line) {
    if (list->count >= max_items) {
        fprintf(stderr, "[LOWER] %s exceeds %d at line %d\n", what ? what : "value count",
                max_items, line);
        l->had_error = true;
        return false;
    }
    if (list->count >= list->cap && !xi_lower_value_list_grow(l, list, max_items))
        return false;
    list->items[list->count++] = value;
    return true;
}

/* Propagate needs_cell along the transitive upvalue capture chain.
 * When an inner closure mutates a captured variable through SRC_UPVAL,
 * every intermediate level up to the defining SRC_REG capture needs
 * needs_cell=true so the emitter generates OP_CELL_NEW at the origin
 * and OP_CELL_GET/OP_CELL_SET at each forwarding level. */
static void propagate_needs_cell(XiLower *l, int upval_idx) {
    if (upval_idx < 0 || upval_idx >= (int) l->func->ncaptures)
        return;
    XiCapture *cap = &l->func->captures[upval_idx];
    if (cap->needs_cell)
        return; /* already propagated */
    cap->needs_cell = true;

    /* Propagate upward through the transitive capture chain */
    if (cap->source == XI_CAPTURE_SRC_UPVAL && l->parent) {
        propagate_needs_cell(l->parent, (int) cap->index);
    } else if (cap->source == XI_CAPTURE_SRC_REG && l->parent && cap->name) {
        /* Mark the defining scope's variable so definitions survive DCE
         * and the emitter redirects writes through CELL_SET. */
        int parent_var = xi_lower_var_find(l->parent, 0, cap->name);
        if (parent_var >= 0 && parent_var < l->parent->var_count)
            l->parent->vars[parent_var].captured_by_child = true;
    }

    /* Propagate downward: child closures that already captured this
     * upvalue via SRC_UPVAL may have inherited needs_cell=false at
     * creation time.  Update them so the emitter generates CELL_GET. */
    for (uint16_t ci_fn = 0; ci_fn < l->func->nchildren; ci_fn++) {
        XiFunc *child = l->func->children[ci_fn];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
            if (child->captures[ci].source == XI_CAPTURE_SRC_UPVAL &&
                (int) child->captures[ci].index == upval_idx && !child->captures[ci].needs_cell) {
                child->captures[ci].needs_cell = true;
            }
        }
    }
}

/* ========== Expression Lowering ========== */

static XiValue *lower_literal(XiLower *l, AstNode *node) {
    switch (node->type) {
        case AST_LITERAL_INT:
            return xi_const_int(l->func, l->cur_block, node->as.literal.raw_value.int_val,
                                l->type_int);
        case AST_LITERAL_FLOAT:
            return xi_const_float(l->func, l->cur_block, node->as.literal.raw_value.float_val,
                                  l->type_float);
        case AST_LITERAL_TRUE:
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);
        case AST_LITERAL_FALSE:
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        case AST_LITERAL_CHAR:
            return xi_const_char(l->func, l->cur_block, node->as.literal.raw_value.char_val,
                                 l->type_char);
        case AST_LITERAL_NULL:
            return xi_const_null(l->func, l->cur_block, l->type_null);
        case AST_LITERAL_STRING:
            return xi_const_str(l->func, l->cur_block, node->as.literal.raw_value.string_val,
                                l->type_string);
        default:
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

static uint16_t xi_narrow_op_for_native_type(uint8_t native_type);

static bool xi_binary_needs_wrap(uint16_t op) {
    switch (op) {
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
            return true;
        default:
            return false;
    }
}

static XiValue *xi_lower_wrap_if_needed(XiLower *l, AstNode *node, XiValue *value,
                                        struct XrType *result_type, uint16_t source_op) {
    if (!value || !result_type || result_type->kind != XR_KIND_INT ||
        !xi_binary_needs_wrap(source_op))
        return value;
    uint16_t narrow_op = xi_narrow_op_for_native_type(result_type->native_width);
    if (!narrow_op)
        return value;
    XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, result_type, 1);
    if (!n)
        return value;
    n->args[0] = value;
    n->line = (uint32_t) node->line;
    return n;
}

/* float32 precision boundaries: AOT narrows each operand to its static C type
 * at use (f32->float, f64->double, int->int64) and computes via C's usual
 * arithmetic conversions. The helpers below mirror that in the shared IR so the
 * VM and AOT stay bit-identical on mixed-precision arithmetic. */

/* Promote an int operand to float so a single-precision opcode sees two float
 * operands (C computes `float * int` in float). */
static XiValue *xi_lower_promote_int_to_float(XiLower *l, AstNode *node, XiValue *v) {
    if (!v || !v->type || !XR_TYPE_IS_INT(v->type))
        return v;
    XiValue *conv = xi_value_new(l->func, l->cur_block, XI_CONVERT, l->type_float, 1);
    if (!conv)
        return v;
    conv->args[0] = v;
    conv->line = (uint32_t) node->line;
    return conv;
}

/* Narrow a float32 operand to single precision before a wider (float64) op,
 * mirroring AOT's `(float)operand` at use. */
static XiValue *xi_lower_narrow_f32_operand(XiLower *l, AstNode *node, XiValue *v) {
    if (!v || !v->type || v->type->kind != XR_KIND_FLOAT || v->type->native_width != XR_NATIVE_F32)
        return v;
    XiValue *n = xi_value_new(l->func, l->cur_block, XI_NARROW_F32, v->type, 1);
    if (!n)
        return v;
    n->args[0] = v;
    n->line = (uint32_t) node->line;
    return n;
}

static bool lower_str_concat_part(XiLower *l, AstNode *node, XiLowerValueList *parts, int line) {
    if (!node)
        return true;
    if (node->type == AST_BINARY_ADD) {
        /* Check if this ADD node has string result type */
        struct XrType *t = xa_analyzer_get_node_type(l->analyzer, node);
        if (t && t->kind == XR_KIND_STRING) {
            return lower_str_concat_part(l, node->as.binary.left, parts, line) &&
                   lower_str_concat_part(l, node->as.binary.right, parts, line);
        }
    }

    XiValue *part = xi_lower_expr(l, node);
    if (!part)
        return false;
    return xi_lower_value_list_push(l, parts, part, XI_LOWER_MAX_VARIADIC_VALUES,
                                    "string concat part count", line);
}

static XiValue *lower_binary(XiLower *l, AstNode *node) {
    /* Most &&/|| nodes are canonicalized to ternary to preserve short-circuit
     * semantics. Speculation-safe bool chains may intentionally remain here and
     * lower to XI_BAND/XI_BOR to avoid hot-path CFG/phi expansion. */

    /* String concat optimization: flatten ADD chain → XI_STR_CONCAT
     * which emits STRBUF_NEW/APPEND/FINISH (no intermediate allocs). */
    if (node->type == AST_BINARY_ADD) {
        struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
        if (result_type && result_type->kind == XR_KIND_STRING) {
            XiValue *stack_parts[XI_LOWER_VALUE_LIST_STACK_CAP];
            XiLowerValueList parts;
            xi_lower_value_list_init(&parts, stack_parts, XI_LOWER_VALUE_LIST_STACK_CAP);
            if (!lower_str_concat_part(l, node, &parts, node->line))
                return NULL;
            if (parts.count >= 2) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_STR_CONCAT, l->type_string,
                                          (uint16_t) parts.count);
                if (!v)
                    return NULL;
                for (int i = 0; i < parts.count; i++)
                    v->args[i] = parts.items[i];
                v->line = (uint32_t) node->line;
                return v;
            }
            if (parts.count == 1)
                return parts.items[0];
        }
    }

    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    if (!lhs || !rhs)
        return NULL;

    /* Prefer analyzer side table; fall back to local inference from operands */
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type) {
        result_type = xi_lower_infer_binary_type(l, node->type, lhs->type, rhs->type);
    }
    uint16_t op = xi_lower_binary_ast_to_xi_op(node->type);

    // float32 precision boundaries: keep the VM and AOT bit-identical on mixed
    // operands by mirroring AOT's per-operand narrowing in the shared IR.
    if (op == XI_ADD || op == XI_SUB || op == XI_MUL || op == XI_DIV) {
        if (result_type && result_type->kind == XR_KIND_FLOAT &&
            result_type->native_width == XR_NATIVE_F32) {
            // float32 result: promote int operands to float so the
            // single-precision opcode operates on two float values.
            lhs = xi_lower_promote_int_to_float(l, node, lhs);
            rhs = xi_lower_promote_int_to_float(l, node, rhs);
        } else if (result_type && result_type->kind == XR_KIND_FLOAT) {
            // float64 result with a float32 operand: narrow the float32 side to
            // single precision before the double op.
            lhs = xi_lower_narrow_f32_operand(l, node, lhs);
            rhs = xi_lower_narrow_f32_operand(l, node, rhs);
        }
    } else if (op == XI_EQ || op == XI_NE || op == XI_LT || op == XI_LE || op == XI_GT ||
               op == XI_GE) {
        // Comparisons: AOT narrows each f32 operand to float at use, so mirror
        // that for bit-identical results (e.g. (float)a == 0.1 is false even
        // though the stored double equals 0.1).
        lhs = xi_lower_narrow_f32_operand(l, node, lhs);
        rhs = xi_lower_narrow_f32_operand(l, node, rhs);
    }

    XiValue *raw = xi_binary(l->func, l->cur_block, op, result_type, lhs, rhs);
    return xi_lower_wrap_if_needed(l, node, raw, result_type, op);
}

static XiValue *lower_unary(XiLower *l, AstNode *node) {
    XiValue *operand = xi_lower_expr(l, node->as.unary.operand);
    if (!operand)
        return NULL;

    /* Prefer analyzer side table; fall back to local inference */
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type) {
        result_type = xi_lower_infer_unary_type(l, node->type, operand->type);
    }
    uint16_t op;

    switch (node->type) {
        case AST_UNARY_NEG:
            op = XI_NEG;
            break;
        case AST_UNARY_NOT:
            op = XI_NOT;
            break;
        case AST_UNARY_BNOT:
            op = XI_BNOT;
            break;
        default:
            op = XI_NEG;
            break;
    }

    XiValue *raw = xi_unary(l->func, l->cur_block, op, result_type, operand);
    if (op == XI_BNOT)
        return xi_lower_wrap_if_needed(l, node, raw, result_type, op);
    return raw;
}

static XiValue *lower_variable(XiLower *l, AstNode *node) {
    const char *name = node->as.variable.name;
    uint32_t sid = node->as.variable.symbol_id;
    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        /* Program-level top-level variables must be read from the
         * backing store because called functions can modify them,
         * which bypasses the local SSA and leaves it stale. */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            return xi_lower_emit_top_load(l, b, NULL);
        }
        XiValue *cur = xi_lower_braun_read(l, var_id, l->cur_block);
        if (cur && var_id < l->var_count && l->vars[var_id].captured_by_child) {
            XiValue *load = xi_value_new(l->func, l->cur_block, XI_COPY, cur->type, 1);
            if (load) {
                load->args[0] = cur;
                load->aux_int = XI_COPY_KIND_CELL_READ;
                load->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM;
                load->line = (uint32_t) node->line;
                return load;
            }
        }
        return cur;
    }

    /* Check for program-level variable from a nested scope */
    XiTopBinding tb = xi_lower_find_top_binding(l, sid, name);
    if (xi_top_binding_valid(tb)) {
        return xi_lower_emit_top_load(l, tb, NULL);
    }

    /* Not found locally — try upvalue capture from enclosing scope */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type)
            upval_type = l->type_any;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, upval_type, 0);
        if (v)
            v->aux_int = upval_idx;
        return v;
    }

    /* Builtin class names (PascalCase) resolve to runtime class globals.
     * Used as namespaces for static method dispatch like Json.parse(s). */
    if (name) {
        static const struct {
            const char *name;
            int index;
        } builtin_classes[] = {
            {"Reflect", XR_GLOBAL_VAR_REFLECT},
            {"Array", XR_GLOBAL_VAR_ARRAY},
            {"Set", XR_GLOBAL_VAR_SET},
            {"Map", XR_GLOBAL_VAR_MAP},
            {"String", XR_GLOBAL_VAR_STRING},
            {"Json", XR_GLOBAL_VAR_JSON},
            {"Bytes", XR_GLOBAL_VAR_BYTES},
            {"Process", XR_GLOBAL_VAR_PROCESS},
            {"PanicInfo", XR_GLOBAL_VAR_PANIC_INFO},
            {"Range", XR_GLOBAL_VAR_RANGE},
            {"DateTime", XR_GLOBAL_VAR_DATETIME},
            {"Atomic", XR_GLOBAL_VAR_ATOMIC},
            {"Ordering", XR_GLOBAL_VAR_ORDERING},
            {"Recv", XR_GLOBAL_VAR_RECV},
            {"SendResult", XR_GLOBAL_VAR_SEND_RESULT},
            {"TaskResult", XR_GLOBAL_VAR_TASK_RESULT},
            {"TaskOutcome", XR_GLOBAL_VAR_TASK_OUTCOME},
            {"TaskStatus", XR_GLOBAL_VAR_TASK_STATUS},
            {"WorkQueue", XR_GLOBAL_VAR_WORKQUEUE},
            {"ResultGroup", XR_GLOBAL_VAR_RESULTGROUP},
            {"CountdownLatch", XR_GLOBAL_VAR_COUNTDOWNLATCH},
            {"Semaphore", XR_GLOBAL_VAR_SEMAPHORE},
            {"EventCount", XR_GLOBAL_VAR_EVENTCOUNT},
        };
        for (int i = 0; i < (int) (sizeof(builtin_classes) / sizeof(builtin_classes[0])); i++) {
            if (strcmp(name, builtin_classes[i].name) == 0) {
                struct XrType *cls_type = xr_type_new_class(NULL, name);
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, cls_type, 0);
                if (v) {
                    v->aux_int = builtin_classes[i].index;
                    v->aux = (void *) name;
                    v->line = (uint32_t) node->line;
                }
                return v;
            }
        }

        /* Builtin instance / value globals (camelCase / dunder) are populated
         * per script by xray_vm_set_script_info: `process` is the Process
         * instance carrying argv/cwd/argv0, `__file__` / `__dir__` are the
         * current module's source path and directory. */
        static const struct {
            const char *name;
            int index;
        } builtin_vars[] = {
            {"process", XR_GLOBAL_VAR_PROCESS},
            {"__file__", XR_GLOBAL_VAR_FILE},
            {"__dir__", XR_GLOBAL_VAR_DIR},
        };
        for (int i = 0; i < (int) (sizeof(builtin_vars) / sizeof(builtin_vars[0])); i++) {
            if (strcmp(name, builtin_vars[i].name) == 0) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, l->type_any, 0);
                if (v) {
                    v->aux_int = builtin_vars[i].index;
                    v->aux = (void *) builtin_vars[i].name;
                    v->line = (uint32_t) node->line;
                }
                return v;
            }
        }
    }

    /* Unresolved variable is a compiler bug: the analyzer must resolve
     * all variable references before lowering.  Hard-fail so the bug
     * surfaces immediately instead of hiding behind a runtime null. */
    fprintf(stderr, "[LOWER] unresolved variable '%s' (symbol_id=%u) at line %d\n",
            name ? name : "<null>", sid, (int) node->line);
    l->had_error = true;
    return NULL;
}

static XiValue *lower_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.assignment.name;
    uint32_t sid = node->as.assignment.symbol_id;
    XiValue *val = xi_lower_expr(l, node->as.assignment.value);
    if (!val)
        return NULL;

    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        /* Implicit int→float promotion on assignment to a float variable */
        struct XrType *var_type = l->vars[var_id].type;
        if (var_type && XR_TYPE_IS_FLOAT(var_type) && val->type && XR_TYPE_IS_INT(val->type)) {
            XiValue *conv = xi_value_new(l->func, l->cur_block, XI_CONVERT, l->type_float, 1);
            if (conv) {
                conv->args[0] = val;
                conv->line = (uint32_t) node->line;
                val = conv;
            }
        }
        if (var_type && var_type->kind == XR_KIND_INT && var_type->native_width != 0 && val->type &&
            XR_TYPE_IS_INT(val->type)) {
            uint16_t narrow_op = xi_narrow_op_for_native_type(var_type->native_width);
            if (narrow_op) {
                XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, var_type, 1);
                if (n) {
                    n->args[0] = val;
                    n->line = (uint32_t) node->line;
                    val = n;
                }
            }
        }
        val = xi_lower_apply_primitive_type_view(l, node, val, var_type);
        /* When assigning from a different variable (e.g. x = i), insert
         * an explicit copy so the target gets its own SSA value.  Without
         * this, braun_write stores the source variable's value directly,
         * and the shared SSA value causes two variables to coalesce to
         * the same physical register — corrupting loop-carried values
         * when the source variable is subsequently modified. */
        bool need_copy = (xi_var_id_is_valid(val->var_id) && val->var_id != (XiVarId) var_id);
        /* Value types (structs) need independent storage on assignment. */
        bool value_clone_copy =
            xi_lower_value_needs_value_clone(l, val) && !xi_lower_value_is_fresh_value_struct(val);
        if (!need_copy && value_clone_copy)
            need_copy = true;
        if (need_copy) {
            XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, val->type, 1);
            if (copy) {
                copy->args[0] = val;
                if (value_clone_copy)
                    xi_lower_mark_value_clone_copy(copy);
                val = copy;
            }
        }
        xi_lower_braun_write(l, var_id, l->cur_block, val);

        /* If a child closure already captured this variable, retroactively
         * enable cell indirection so the closure sees the updated value.
         * Also mark captured_by_child so the new SSA value survives DCE
         * (the emitter redirects it through CELL_SET at emit time). */
        for (uint16_t ci_fn = 0; ci_fn < l->func->nchildren; ci_fn++) {
            XiFunc *child = l->func->children[ci_fn];
            if (!child)
                continue;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                if (child->captures[ci].source == XI_CAPTURE_SRC_REG && child->captures[ci].name &&
                    name && strcmp(child->captures[ci].name, name) == 0) {
                    child->captures[ci].needs_cell = true;
                    if (var_id < l->var_count)
                        l->vars[var_id].captured_by_child = true;
                    val->flags |= XI_FLAG_SIDE_EFFECT;
                }
            }
        }

        /* If this is a program-level variable, also update backing store */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, val);
        }
        return val;
    }

    /* Check for program-level variable from nested scope */
    XiTopBinding tb = xi_lower_find_top_binding(l, sid, name);
    if (xi_top_binding_valid(tb)) {
        if (tb.type && tb.type->kind == XR_KIND_INT && tb.type->native_width != 0 && val->type &&
            XR_TYPE_IS_INT(val->type)) {
            uint16_t narrow_op = xi_narrow_op_for_native_type(tb.type->native_width);
            if (narrow_op) {
                XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, tb.type, 1);
                if (n) {
                    n->args[0] = val;
                    n->line = (uint32_t) node->line;
                    val = n;
                }
            }
        }
        val = xi_lower_apply_primitive_type_view(l, node, val, tb.type);
        xi_lower_emit_top_store(l, tb, val);
        return val;
    }

    /* Try upvalue store for captured mutable variable */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (upval_type && upval_type->kind == XR_KIND_INT && upval_type->native_width != 0 &&
            val->type && XR_TYPE_IS_INT(val->type)) {
            uint16_t narrow_op = xi_narrow_op_for_native_type(upval_type->native_width);
            if (narrow_op) {
                XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, upval_type, 1);
                if (n) {
                    n->args[0] = val;
                    n->line = (uint32_t) node->line;
                    val = n;
                }
            }
        }
        val = xi_lower_apply_primitive_type_view(l, node, val, upval_type);
        /* Mark the capture as needing cell indirection because the child
         * mutates the captured variable.  The emit stage uses this to
         * emit CELL_NEW in the parent and CELL_GET/CELL_SET in the child. */
        XR_DCHECK(upval_idx < (int) l->func->ncaptures, "upval_idx out of range for needs_cell");
        propagate_needs_cell(l, upval_idx);

        XiValue *store = xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL, l->type_unit, 1);
        if (store) {
            store->args[0] = val;
            store->aux_int = upval_idx;
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
        return val;
    }

    /* Unresolved assignment target is a compiler bug: the analyzer must
     * bind all assignment targets before lowering. */
    fprintf(stderr, "[LOWER] unresolved assignment target '%s' (symbol_id=%u) at line %d\n",
            name ? name : "<null>", sid, (int) node->line);
    l->had_error = true;
    return NULL;
}

static int json_field_index(struct XrType *type, const char *name) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !type->object.field_names || !name)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (!type->object.field_names[i])
            return -1;
    }
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names[i] && strcmp(type->object.field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static const XiImportRef *lower_import_ref_from_value(XiLower *l, const XiValue *v) {
    while (v &&
           (v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_BOX || v->op == XI_UNBOX ||
            v->op == XI_RETAIN || v->op == XI_RELEASE) &&
           v->nargs >= 1) {
        v = v->args[0];
    }
    if (!v)
        return NULL;
    if (v->op == XI_IMPORT_REF && v->aux)
        return (const XiImportRef *) v->aux;
    if (v->op != XI_GET_SHARED || v->aux_int < 0)
        return NULL;
    int slot = (int) v->aux_int;
    for (XiLower *p = l; p; p = p->parent) {
        if (p->is_program && slot < p->var_cap)
            return p->shared_slot_imports ? p->shared_slot_imports[slot] : NULL;
    }
    return NULL;
}

static bool lower_value_is_whole_module_import(XiLower *l, const XiValue *v,
                                               const char *module_name) {
    const XiImportRef *ref = lower_import_ref_from_value(l, v);
    return ref && ref->module_path && strcmp(ref->module_path, module_name) == 0 &&
           ref->member_name == NULL;
}

static bool lower_math_constant(XiLower *l, const char *name, XiValue **out) {
    if (!out)
        return false;
    if (out)
        *out = NULL;
    if (!name)
        return false;

    if (strcmp(name, "PI") == 0)
        *out = xi_const_float(l->func, l->cur_block, 3.14159265358979323846, l->type_float);
    else if (strcmp(name, "E") == 0)
        *out = xi_const_float(l->func, l->cur_block, 2.71828182845904523536, l->type_float);
    else if (strcmp(name, "TAU") == 0)
        *out = xi_const_float(l->func, l->cur_block, 6.28318530717958647692, l->type_float);
    else if (strcmp(name, "SQRT2") == 0)
        *out = xi_const_float(l->func, l->cur_block, 1.41421356237309504880, l->type_float);
    else if (strcmp(name, "LN2") == 0)
        *out = xi_const_float(l->func, l->cur_block, 0.69314718055994530942, l->type_float);
    else if (strcmp(name, "LN10") == 0)
        *out = xi_const_float(l->func, l->cur_block, 2.30258509299404568402, l->type_float);
    else if (strcmp(name, "LOG2E") == 0)
        *out = xi_const_float(l->func, l->cur_block, 1.44269504088896340736, l->type_float);
    else if (strcmp(name, "LOG10E") == 0)
        *out = xi_const_float(l->func, l->cur_block, 0.43429448190325182765, l->type_float);
    else if (strcmp(name, "EPSILON") == 0)
        *out = xi_const_float(l->func, l->cur_block, DBL_EPSILON, l->type_float);
    else if (strcmp(name, "MAX_INT") == 0)
        *out = xi_const_int(l->func, l->cur_block, INT64_MAX, l->type_int);
    else if (strcmp(name, "MIN_INT") == 0)
        *out = xi_const_int(l->func, l->cur_block, INT64_MIN, l->type_int);
    else if (strcmp(name, "MAX_FLOAT") == 0)
        *out = xi_const_float(l->func, l->cur_block, DBL_MAX, l->type_float);
    else if (strcmp(name, "INF") == 0)
        *out = xi_const_float(l->func, l->cur_block, INFINITY, l->type_float);
    else if (strcmp(name, "NAN") == 0)
        *out = xi_const_float(l->func, l->cur_block, NAN, l->type_float);
    else
        return false;

    return *out != NULL;
}

static bool lower_encoding_constant(XiLower *l, const char *name, XiValue **out) {
    if (!out)
        return false;
    *out = NULL;
    if (!name)
        return false;

    if (strcmp(name, "LE") == 0)
        *out = xi_const_int(l->func, l->cur_block, XR_ENCODING_UTF16_LE, l->type_int);
    else if (strcmp(name, "BE") == 0)
        *out = xi_const_int(l->func, l->cur_block, XR_ENCODING_UTF16_BE, l->type_int);
    else
        return false;

    return *out != NULL;
}

static bool lower_math_call_arity_ok(const char *name, int nargs) {
    if (!name || nargs < 0)
        return false;
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
        return true;
    if (strcmp(name, "pow") == 0 || strcmp(name, "atan2") == 0 || strcmp(name, "hypot") == 0 ||
        strcmp(name, "fmod") == 0)
        return nargs == 2;
    if (strcmp(name, "clamp") == 0 || strcmp(name, "lerp") == 0)
        return nargs == 3;
    static const char *unary[] = {
        "abs",  "floor", "ceil",  "round", "sqrt",     "sin",      "cos",  "tan",   "asin",
        "acos", "atan",  "log",   "log10", "log2",     "exp",      "sinh", "cosh",  "tanh",
        "cbrt", "trunc", "log1p", "expm1", "degToRad", "radToDeg", "sign", "isNaN", "isFinite",
    };
    for (int i = 0; i < (int) (sizeof(unary) / sizeof(unary[0])); i++) {
        if (strcmp(name, unary[i]) == 0)
            return nargs == 1;
    }
    return false;
}

static bool lower_math_args_all_int(XiValue **arg_vals, int arg_count) {
    if (!arg_vals || arg_count <= 0)
        return false;
    for (int i = 0; i < arg_count; i++) {
        if (!arg_vals[i] || !arg_vals[i]->type || !XR_TYPE_IS_INT(arg_vals[i]->type))
            return false;
    }
    return true;
}

static AstNode *expr_unwrap_grouping(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node;
}

static bool lower_math_preserves_int_arg_shape(const char *member) {
    return member && (strcmp(member, "abs") == 0 || strcmp(member, "min") == 0 ||
                      strcmp(member, "max") == 0 || strcmp(member, "clamp") == 0);
}

static struct XrType *lower_math_call_result_type(XiLower *l, const char *member,
                                                  XiValue **arg_vals, int arg_count) {
    if (strcmp(member, "abs") == 0 && arg_count == 1 &&
        lower_math_args_all_int(arg_vals, arg_count))
        return l->type_any;
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) && arg_count == 0)
        return l->type_any;
    if ((strcmp(member, "min") == 0 || strcmp(member, "max") == 0) &&
        lower_math_args_all_int(arg_vals, arg_count))
        return l->type_int;
    if (strcmp(member, "clamp") == 0 && arg_count == 3 &&
        lower_math_args_all_int(arg_vals, arg_count))
        return l->type_int;
    if (strcmp(member, "floor") == 0 || strcmp(member, "ceil") == 0 ||
        strcmp(member, "round") == 0 || strcmp(member, "trunc") == 0 || strcmp(member, "sign") == 0)
        return l->type_int;
    if (strcmp(member, "isNaN") == 0 || strcmp(member, "isFinite") == 0)
        return l->type_bool;
    return l->type_float;
}

static XiValue *lower_member_access(XiLower *l, AstNode *node) {
    MemberAccessNode *ma = &node->as.member_access;

    XiValue *obj = xi_lower_expr(l, ma->object);
    if (!obj)
        return NULL;

    if (lower_value_is_whole_module_import(l, obj, "math")) {
        XiValue *constant = NULL;
        if (lower_math_constant(l, ma->name, &constant))
            return constant;
    }
    if (lower_value_is_whole_module_import(l, obj, "encoding")) {
        XiValue *constant = NULL;
        if (lower_encoding_constant(l, ma->name, &constant))
            return constant;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);

    /* Struct with compile-time layout → XI_STRUCT_GET (emitter decides
     * whether to stack-allocate or fall back to OP_GETPROP) */
    XrStructLayout *slayout = xi_lower_value_struct_layout(l, obj);
    if (slayout) {
        int sidx = xi_lower_struct_field_index(slayout, ma->name);
        if (sidx >= 0) {
            result_type = xi_lower_struct_field_type(l, result_type, slayout, sidx);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_STRUCT_GET, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->aux = (void *) slayout;
            v->aux_int = sidx;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    /* Json with a complete compile-time field table → direct indexed access.
     * Non-sealed object literals may still grow dynamically, but their
     * declared field indices remain stable. Computed-key object literals
     * have NULL holes in the analyzer field table and use name lookup
     * because codegen compacts only the static named fields. */
    int fidx = json_field_index(obj->type, ma->name);
    if (fidx >= 0) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_GET_F, result_type, 1);
        if (!v)
            return NULL;
        v->args[0] = obj;
        v->aux_int = fidx;
        v->line = (uint32_t) node->line;
        return v;
    }

    /* Tuple `.N` → XI_TUPLE_GET (analyzer has already bounds-checked N).
     * The member name is always a digit run for tuples; if it's not we
     * leave the access alone and let LOAD_FIELD's runtime guard handle
     * the bad code (it can't actually reach here after analyzer rules
     * are enforced, but stays robust if a later refactor introduces an
     * unverified path). */
    if (obj->type && obj->type->kind == XR_KIND_TUPLE && ma->name) {
        bool digits_only = (ma->name[0] != '\0');
        for (const char *p = ma->name; *p && digits_only; p++) {
            if (*p < '0' || *p > '9')
                digits_only = false;
        }
        if (digits_only) {
            long idx = strtol(ma->name, NULL, 10);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->aux_int = idx;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->aux = (void *) arena_strdup(l->func, ma->name);
    v->aux_int = xi_lower_method_symbol(l, ma->name);
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_member_set_target(XiValue *obj) {
    while (obj && xi_copy_is_identity_alias(obj) && obj->nargs >= 1 && obj->args[0])
        obj = obj->args[0];
    return obj;
}

static XiValue *lower_member_set(XiLower *l, AstNode *node) {
    MemberSetNode *ms = &node->as.member_set;
    XiValue *obj = xi_lower_expr(l, ms->object);
    XiValue *val = xi_lower_expr(l, ms->value);
    if (!obj || !val)
        return NULL;
    obj = lower_member_set_target(obj);

    struct XrType *result_type = val->type;

    /* Struct with compile-time layout → XI_STRUCT_SET */
    XrStructLayout *slayout = xi_lower_value_struct_layout(l, obj);
    if (slayout) {
        int sidx = xi_lower_struct_field_index(slayout, ms->member);
        if (sidx >= 0) {
            val = xi_lower_narrow_for_native_field(l, node, val, slayout->fields[sidx].native_type);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_STRUCT_SET, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = obj;
            v->args[1] = val;
            v->aux = (void *) slayout;
            v->aux_int = sidx;
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    /* Sealed Json with known field → direct indexed store */
    int fidx = json_field_index(obj->type, ms->member);
    if (fidx >= 0) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_SET_F, result_type, 2);
        if (!v)
            return NULL;
        v->args[0] = obj;
        v->args[1] = val;
        v->aux_int = fidx;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, result_type, 2);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->args[1] = val;
    v->aux = (void *) arena_strdup(l->func, ms->member);
    v->aux_int = xi_lower_method_symbol(l, ms->member);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

#include "xi_lower_native_width.inc.c"

static bool xi_lower_type_is_unknown(struct XrType *type) {
    return !type || XR_TYPE_IS_UNKNOWN(type);
}

static struct XrType *xi_lower_type_for_native_layout(XiLower *l, struct XrType *fallback,
                                                      uint8_t native_type) {
    if (!l)
        return fallback;
    switch (native_type) {
        case XR_NATIVE_I64:
            return l->type_int ? l->type_int : fallback;
        case XR_NATIVE_U64:
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32: {
            struct XrType *type = xr_type_new_int_width(l->isolate, native_type);
            return type ? type : fallback;
        }
        case XR_NATIVE_F64:
            return l->type_float ? l->type_float : fallback;
        case XR_NATIVE_F32: {
            struct XrType *type = xr_type_new_float_width(l->isolate, native_type);
            return type ? type : fallback;
        }
        case XR_NATIVE_BOOL:
            return l->type_bool ? l->type_bool : fallback;
        case XR_NATIVE_STRING:
            return l->type_string ? l->type_string : fallback;
        default:
            return fallback;
    }
}

static struct XrType *xi_lower_widened_elem_type(XiLower *l, struct XrType *fallback,
                                                 struct XrType *elem_type) {
    if (!elem_type)
        return fallback;
    switch (elem_type->native_width) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
            return l && l->type_int ? l->type_int : fallback;
        case XR_NATIVE_F32:
            return l && l->type_float ? l->type_float : fallback;
        default:
            return elem_type;
    }
}

static struct XrType *xi_lower_struct_field_type(XiLower *l, struct XrType *fallback,
                                                 XrStructLayout *layout, int field_index) {
    if (!layout || field_index < 0 || field_index >= layout->field_count)
        return fallback;
    XrStructFieldLayout *field = &layout->fields[field_index];
    if (field->native_type == XR_NATIVE_ARRAY) {
        struct XrType *elem_type =
            xi_lower_type_for_native_layout(l, NULL, field->elem_native_type);
        if (!elem_type || !l || !l->isolate || field->elem_count == 0)
            return fallback;
        struct XrType *array_type =
            xr_type_new_fixed_array(l->isolate, elem_type, (int) field->elem_count);
        return array_type ? array_type : fallback;
    }
    return xi_lower_type_for_native_layout(l, fallback, field->native_type);
}

/* Byte size of a raw pointer's pointee, for scaling p[i] / p.offset(i). C
 * pointer arithmetic on RawPtr<T> advances by sizeof(T), matching `T*`. */
static int64_t xi_pointer_pointee_size(struct XrType *ptr_type) {
    if (!ptr_type || !XR_TYPE_IS_POINTER(ptr_type))
        return 1;
    struct XrType *pointee = ptr_type->container.element_type;
    if (!pointee)
        return 1;
    if (pointee->native_width != 0)
        return (int64_t) xr_native_type_size(pointee->native_width);
    switch (pointee->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_POINTER:
            return 8;
        case XR_KIND_BOOL:
            return 1;
        default:
            return 1;
    }
}

/* XrFFIType width code of a raw pointer's pointee (carried on PTR_LOAD/STORE). */
static uint8_t xi_pointer_pointee_ffi(struct XrType *ptr_type) {
    struct XrType *pointee =
        (ptr_type && XR_TYPE_IS_POINTER(ptr_type)) ? ptr_type->container.element_type : NULL;
    return xr_ffi_type_from_xrtype(pointee, false);
}

static bool xi_pointer_pointee_is_u8(struct XrType *ptr_type) {
    struct XrType *pointee =
        (ptr_type && XR_TYPE_IS_POINTER(ptr_type)) ? ptr_type->container.element_type : NULL;
    return pointee && XR_TYPE_IS_INT(pointee) && pointee->native_width == XR_NATIVE_U8;
}

/* Build the scaled address `ptr + idx * sizeof(pointee)` as a raw-pointer SSA
 * value. VM/tagged boundaries still encode the address as an integer, but AOT
 * keeps the local as a native pointer. */
static XiValue *xi_lower_ptr_scaled_addr(XiLower *l, AstNode *node, XiValue *ptr, XiValue *idx,
                                         struct XrType *ptr_type, struct XrType *addr_type) {
    XiValue *scaled = idx;
    int64_t size = xi_pointer_pointee_size(ptr_type);
    if (size != 1) {
        XiValue *sz = xi_const_int(l->func, l->cur_block, size, l->type_int);
        XiValue *mul = xi_value_new(l->func, l->cur_block, XI_MUL, l->type_int, 2);
        if (!mul)
            return NULL;
        mul->args[0] = idx;
        mul->args[1] = sz;
        mul->line = (uint32_t) node->line;
        scaled = mul;
    }
    XiValue *add = xi_value_new(l->func, l->cur_block, XI_ADD, addr_type, 2);
    if (!add)
        return NULL;
    add->args[0] = ptr;
    add->args[1] = scaled;
    add->line = (uint32_t) node->line;
    return add;
}

/* Lower `unsafe { stmt* }`: run the statements; the value is the trailing
 * expression statement (or null). unsafe is otherwise codegen-transparent. */
static XiValue *lower_unsafe_expr(XiLower *l, AstNode *node) {
    AstNode *body = node->as.unsafe_expr.operand;
    if (!body)
        return xi_const_null(l->func, l->cur_block, l->type_null);
    if (body->type != AST_BLOCK)
        return xi_lower_expr(l, body);
    BlockNode *blk = &body->as.block;
    XiValue *value = NULL;
    for (int i = 0; i < blk->count; i++) {
        AstNode *stmt = blk->statements[i];
        if (!stmt)
            continue;
        bool is_last = (i == blk->count - 1);
        if (is_last && stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
            value = xi_lower_expr(l, stmt->as.expr_stmt);
            if (!l->cur_block)
                return NULL;
        } else {
            xi_lower_stmt(l, stmt);
            if (!l->cur_block)
                return NULL;
        }
    }
    if (!value)
        value = xi_const_null(l->func, l->cur_block, l->type_null);
    return value;
}

static XiValue *lower_index_get(XiLower *l, AstNode *node) {
    IndexGetNode *ig = &node->as.index_get;
    XiValue *obj = xi_lower_expr(l, ig->array);
    XiValue *idx = xi_lower_expr(l, ig->index);
    if (!obj || !idx)
        return NULL;

    /* FFI raw pointer subscript p[i] => XI_PTR_LOAD(p + i*sizeof(T)). */
    if (obj->type && XR_TYPE_IS_POINTER(obj->type)) {
        struct XrType *result_type = xi_lower_node_type(l, node);
        XiValue *addr = xi_lower_ptr_scaled_addr(l, node, obj, idx, obj->type, obj->type);
        if (!addr)
            return NULL;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, result_type, 1);
        if (!v)
            return NULL;
        v->args[0] = addr;
        v->aux_int = (int64_t) xr_ffi_ptr_aux(xi_pointer_pointee_ffi(obj->type), false);
        v->flags |= XI_FLAG_READS_MEM;
        v->line = (uint32_t) node->line;
        return v;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (obj->type && XR_TYPE_IS_MAP(obj->type))
        idx = xi_lower_narrow_for_static_type(l, node, idx, obj->type->map.key_type);
    struct XrType *elem_type = xi_get_container_elem_type(obj->type);
    struct XrType *index_type =
        xi_lower_type_is_unknown(result_type) && elem_type ? elem_type : result_type;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, index_type, 2);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->line = (uint32_t) node->line;

    /* Insert XI_WIDEN after reading from a sub-width typed array */
    uint16_t widen_op = xi_widen_op_for_elem(elem_type);
    if (widen_op) {
        struct XrType *widen_type = xi_lower_type_is_unknown(result_type)
                                        ? xi_lower_widened_elem_type(l, result_type, elem_type)
                                        : result_type;
        XiValue *w = xi_value_new(l->func, l->cur_block, widen_op, widen_type, 1);
        if (!w)
            return v;
        w->args[0] = v;
        w->line = (uint32_t) node->line;
        return w;
    }
    return v;
}

static XiValue *lower_index_set(XiLower *l, AstNode *node) {
    IndexSetNode *is_node = &node->as.index_set;
    XiValue *obj = xi_lower_expr(l, is_node->array);
    XiValue *idx = xi_lower_expr(l, is_node->index);
    XiValue *val = xi_lower_expr(l, is_node->value);
    if (!obj || !idx || !val)
        return NULL;

    /* FFI raw pointer store p[i] = v => XI_PTR_STORE(p + i*sizeof(T), v). */
    if (obj->type && XR_TYPE_IS_POINTER(obj->type)) {
        XiValue *addr = xi_lower_ptr_scaled_addr(l, node, obj, idx, obj->type, obj->type);
        if (!addr)
            return NULL;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_STORE, l->type_unit, 2);
        if (!v)
            return NULL;
        v->args[0] = addr;
        v->args[1] = val;
        v->aux_int = (int64_t) xi_pointer_pointee_ffi(obj->type);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
        v->line = (uint32_t) node->line;
        return v;
    }

    if (obj->type && XR_TYPE_IS_MAP(obj->type)) {
        idx = xi_lower_narrow_for_static_type(l, node, idx, obj->type->map.key_type);
        val = xi_lower_narrow_for_static_type(l, node, val, obj->type->map.value_type);
    } else {
        /* Insert XI_NARROW before writing to a sub-width typed array */
        struct XrType *elem_type = xi_get_container_elem_type(obj->type);
        uint16_t narrow_op = xi_narrow_op_for_elem(elem_type);
        if (narrow_op) {
            XiValue *n = xi_value_new(l->func, l->cur_block, narrow_op, val->type, 1);
            if (n) {
                n->args[0] = val;
                n->line = (uint32_t) node->line;
                val = n;
            }
        }
    }

    struct XrType *result_type = val->type;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, result_type, 3);
    if (!v)
        return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->args[2] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_tuple_literal(XiLower *l, AstNode *node) {
    TupleLiteralNode *tup = &node->as.tuple_literal;
    struct XrType *result_type = xi_lower_node_type(l, node);

    /* First pass: evaluate every element value, expanding spreads into
     * one TUPLE_GET per source slot. The flat list `elem_vals[]`
     * mirrors the final tuple's element layout exactly. */
    XiValue *elem_vals[64];
    uint16_t slot = 0;
    for (int i = 0; i < tup->count && slot < 64; i++) {
        AstNode *child = tup->elements[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return NULL;
            int arity = src->type ? xr_type_tuple_count(src->type) : 0;
            for (int j = 0; j < arity && slot < 64; j++) {
                struct XrType *et = xr_type_tuple_get(src->type, j);
                XiValue *get =
                    xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et ? et : l->type_any, 1);
                if (!get)
                    return NULL;
                get->args[0] = src;
                get->aux_int = j;
                elem_vals[slot++] = get;
            }
            continue;
        }

        elem_vals[slot] = xi_lower_expr(l, child);
        if (!elem_vals[slot])
            return NULL;
        slot++;
    }
    uint16_t safe_n = slot;

    XiValue *tup_val = xi_value_new(l->func, l->cur_block, XI_TUPLE_NEW, result_type, safe_n);
    if (!tup_val)
        return NULL;
    for (uint16_t i = 0; i < safe_n; i++)
        tup_val->args[i] = elem_vals[i];
    tup_val->aux_int = safe_n;
    tup_val->line = (uint32_t) node->line;
    return tup_val;
}

/* Array literal with `...spread` elements: `[...a, x, ...b]`.
 * Built dynamically because spread sources have runtime length — a fresh
 * array is allocated (heap; it grows), singletons are appended with
 * XI_ARRAY_PUSH and each spread source is spliced with XI_ARRAY_EXTEND.
 * Runtime cost is O(total elements). The no-spread path keeps the static
 * pre-sized ARRAY_NEW + INDEX_SET fast path. */
static XiValue *lower_array_literal_spread(XiLower *l, AstNode *node, struct XrType *result_type) {
    ArrayLiteralNode *arr = &node->as.array_literal;
    int count = arr->count;

    /* XI_ARRAY_NEW's argument is the initial LENGTH (both backends preset
     * length and fill slots; the static literal path overwrites them via
     * INDEX_SET). The spread path appends everything with PUSH/EXTEND, so
     * it must start from an empty array — a non-zero count here would leave
     * phantom leading null/zero elements. */
    XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
    if (!arr_val)
        return NULL;
    arr_val->args[0] = cap;
    arr_val->line = (uint32_t) node->line;

    for (int i = 0; i < count; i++) {
        AstNode *child = arr->elements[i];
        if (!child)
            continue;
        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return NULL;
            XiValue *ext = xi_value_new(l->func, l->cur_block, XI_ARRAY_EXTEND, l->type_unit, 2);
            if (!ext)
                return NULL;
            ext->args[0] = arr_val;
            ext->args[1] = src;
            ext->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
            ext->line = (uint32_t) node->line;
        } else {
            XiValue *elem = xi_lower_expr(l, child);
            if (!elem)
                return NULL;
            XiValue *push = xi_value_new(l->func, l->cur_block, XI_ARRAY_PUSH, l->type_unit, 2);
            if (!push)
                return NULL;
            push->args[0] = arr_val;
            push->args[1] = elem;
            push->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            push->line = (uint32_t) node->line;
        }
    }
    return arr_val;
}

static XiValue *lower_array_literal(XiLower *l, AstNode *node) {
    ArrayLiteralNode *arr = &node->as.array_literal;
    int count = arr->count;

    /* Spread elements force the dynamic build path. */
    for (int i = 0; i < count; i++) {
        if (arr->elements[i] && arr->elements[i]->type == AST_SPREAD_EXPR)
            return lower_array_literal_spread(l, node, xi_lower_node_type(l, node));
    }

    /* Evaluate all elements first */
    int n = count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, arr->elements[i]);
        if (!elem_vals[i])
            return NULL;
    }

    /* Create array: XI_ARRAY_NEW with element count as aux */
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
    if (!arr_val)
        return NULL;
    arr_val->args[0] = cap;
    arr_val->line = (uint32_t) node->line;

    /* Populate: INDEX_SET for each element */
    struct XrType *elem_type = xi_get_container_elem_type(result_type);
    uint16_t narrow_op = xi_narrow_op_for_elem(elem_type);
    for (int i = 0; i < n; i++) {
        XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
        XiValue *elem = elem_vals[i];
        if (narrow_op) {
            XiValue *narrow = xi_value_new(l->func, l->cur_block, narrow_op, elem->type, 1);
            if (narrow) {
                narrow->args[0] = elem;
                narrow->line = (uint32_t) node->line;
                elem = narrow;
            }
        }
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
        if (!set)
            break;
        set->args[0] = arr_val;
        set->args[1] = idx;
        set->args[2] = elem;
        set->flags |= XI_FLAG_SIDE_EFFECT;
        set->line = (uint32_t) node->line;
    }
    return arr_val;
}

/* Generate a location string constant for assert diagnostics.
 * Format: "line <N>" using the AST node's line number. */
static const char *make_assert_loc(XiLower *l, int line) {
    char buf[64];
    snprintf(buf, sizeof(buf), "line %d", line);
    return arena_strdup(l->func, buf);
}

/* Intercept known compile-time builtin function calls.
 * Returns non-NULL XiValue if handled, NULL to fall through to generic CALL. */
static XiValue *lower_builtin_call(XiLower *l, AstNode *node, const char *fname,
                                   CallExprNode *call) {
    struct XrType *rtype = xi_lower_node_type(l, node);
    int line = node->line;

    /* assert(cond) / assert(cond, msg) → XI_ASSERT */
    if (strcmp(fname, "assert") == 0 && (call->arg_count == 1 || call->arg_count == 2)) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 0; /* 0 = assert_true */
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_true(cond) → XI_ASSERT aux_int=0 */
    if (strcmp(fname, "assert_true") == 0 && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 0;
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_false(cond) → XI_ASSERT aux_int=1 */
    if (strcmp(fname, "assert_false") == 0 && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 1; /* 1 = assert_false */
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* likely(cond) / unlikely(cond) are semantic identity over bool.
     * AOT consumes the copy kind when this value controls a branch; VM sees
     * an ordinary copy, so functionality stays aligned. */
    if ((strcmp(fname, "likely") == 0 || strcmp(fname, "unlikely") == 0) && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_COPY, l->type_bool, 1);
        if (!v)
            return NULL;
        v->args[0] = cond;
        v->aux_int = (strcmp(fname, "likely") == 0) ? XI_COPY_KIND_LIKELY : XI_COPY_KIND_UNLIKELY;
        v->line = (uint32_t) line;
        return v;
    }
    /* assert_eq(actual, expected) → XI_ASSERT_EQ */
    if (strcmp(fname, "assert_eq") == 0 && call->arg_count == 2) {
        XiValue *actual = xi_lower_expr(l, call->arguments[0]);
        XiValue *expected = xi_lower_expr(l, call->arguments[1]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_EQ, l->type_unit, 2);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = actual;
        v->args[1] = expected;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_ne(actual, unexpected) → XI_ASSERT_NE */
    if (strcmp(fname, "assert_ne") == 0 && call->arg_count == 2) {
        XiValue *actual = xi_lower_expr(l, call->arguments[0]);
        XiValue *unexpected = xi_lower_expr(l, call->arguments[1]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_NE, l->type_unit, 2);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = actual;
        v->args[1] = unexpected;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_throws(fn) → XI_ASSERT_THROWS */
    if (strcmp(fname, "assert_throws") == 0 && call->arg_count == 1) {
        XiValue *fn_val = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_THROWS, l->type_unit, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = fn_val;
        v->aux = (void *) make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* typeof(x) → XI_TYPEOF aux_int=1 (returns string).
     * Returns the runtime type name as a string. For class/enum
     * instances the concrete name is returned. */
    if (strcmp(fname, "typeof") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_TYPEOF, l->type_string, 1);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        v->aux_int = 1; /* emit OP_TYPENAME: returns string */
        v->line = (uint32_t) line;
        return v;
    }
    /* dump(x) / dump(x, indent) → XI_CALL_BUILTIN aux="dump" → OP_DUMP */
    if (strcmp(fname, "dump") == 0 && (call->arg_count == 1 || call->arg_count == 2)) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        int nargs = (int) call->arg_count;
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, l->type_unit, (uint16_t) nargs);
        if (!v)
            return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        if (nargs == 2)
            v->args[1] = xi_lower_expr(l, call->arguments[1]);
        v->aux = (void *) "dump";
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* copy(x) → XI_CALL_BUILTIN aux="copy" → OP_COPY */
    if (strcmp(fname, "copy") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, rtype ? rtype : l->type_any, 1);
        if (!v)
            return NULL;
        v->args[0] = arg;
        v->aux = (void *) "copy";
        v->line = (uint32_t) line;
        return v;
    }
    /* chr(x) → XI_CALL_BUILTIN aux="chr" → OP_CHR */
    if (strcmp(fname, "chr") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, l->type_string, 1);
        if (!v)
            return NULL;
        v->args[0] = arg;
        v->aux = (void *) "chr";
        v->line = (uint32_t) line;
        return v;
    }
    /* Bytes(n) / Bytes(n, fill) → XI_CALL_BUILTIN with aux_int encoding
     * the opcode OP_BYTES_NEW so the emitter produces the right instruction. */
    if (strcmp(fname, "Bytes") == 0 && call->arg_count >= 1 && call->arg_count <= 2) {
        /* Evaluate arguments BEFORE creating CALL_BUILTIN to ensure
         * argument values appear before the call in the block. */
        int n = call->arg_count;
        XiValue *arg_vals[2];
        for (int i = 0; i < n; i++)
            arg_vals[i] = xi_lower_expr(l, call->arguments[i]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, rtype, (uint16_t) n);
        if (!v)
            return NULL;
        for (int i = 0; i < n; i++)
            v->args[i] = arg_vals[i];
        v->aux = (void *) "Bytes";
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) line;
        return v;
    }

    /* print(...) in expression context (e.g. match arm body).
     * Statement-level print is handled by AST_PRINT_STMT → lower_print(),
     * but expression-level calls (AST_CALL_EXPR on variable "print") arrive
     * here.  Emit XI_PRINT instructions with the same encoding. */
    if (strcmp(fname, "print") == 0) {
        int n = (int) call->arg_count;
        XiValue *stack_args[XI_LOWER_VALUE_LIST_STACK_CAP];
        XiLowerValueList args;
        xi_lower_value_list_init(&args, stack_args, XI_LOWER_VALUE_LIST_STACK_CAP);
        for (int i = 0; i < n; i++) {
            XiValue *arg = xi_lower_expr(l, call->arguments[i]);
            if (!arg)
                return xi_const_null(l->func, l->cur_block, l->type_null);
            if (!xi_lower_value_list_push(l, &args, arg, XI_LOWER_MAX_VARIADIC_VALUES,
                                          "print argument count", line))
                return xi_const_null(l->func, l->cur_block, l->type_null);
        }
        for (int i = 0; i < args.count; i++) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT, l->type_unit, 1);
            if (!v)
                return xi_const_null(l->func, l->cur_block, l->type_null);
            v->args[0] = args.items[i];
            int add_space = (i > 0) ? 1 : 0;
            int newline = (i == args.count - 1) ? 1 : 0;
            v->aux_int = add_space | (newline << 1);
            v->flags = xi_op_default_effects(XI_PRINT);
            v->line = (uint32_t) line;
        }
        if (args.count == 0) {
            /* print() with no args → emit newline */
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT, l->type_unit, 1);
            if (!v)
                return xi_const_null(l->func, l->cur_block, l->type_null);
            v->args[0] = xi_const_null(l->func, l->cur_block, l->type_null);
            v->aux_int = (1 << 1) | (1 << 4); /* newline + skip_null */
            v->flags = xi_op_default_effects(XI_PRINT);
            v->line = (uint32_t) line;
        }
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }

    /* Type conversion builtins: string(x), int(x), float(x), bool(x).
     * Each emits XI_CONVERT with the target type set on the value. */
    if (call->arg_count == 1) {
        struct XrType *target = NULL;
        if (strcmp(fname, "string") == 0)
            target = l->type_string;
        else if (strcmp(fname, "int") == 0)
            target = l->type_int;
        else if (strcmp(fname, "float") == 0)
            target = l->type_float;
        else if (strcmp(fname, "bool") == 0)
            target = l->type_bool;
        else if (strcmp(fname, "char") == 0)
            target = l->type_char;

        if (target) {
            XiValue *arg = xi_lower_expr(l, call->arguments[0]);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CONVERT, target, 1);
            if (!v)
                return NULL;
            v->args[0] = arg;
            v->line = (uint32_t) line;
            return v;
        }
    }

    (void) rtype;
    return NULL; /* not a builtin — fall through to generic CALL */
}

/* Map Coro.method() names to XI_CORO_OP sub-type constants.
 * Returns -1 for unknown methods. */
static int coro_method_sub_type(const char *method) {
    XR_DCHECK(method != NULL, "coro_method_sub_type: NULL method");
    /* Dedicated opcodes */
    if (strcmp(method, "setLocal") == 0)
        return XI_CORO_SUB_SET_LOCAL;
    if (strcmp(method, "getLocal") == 0)
        return XI_CORO_SUB_GET_LOCAL;
    if (strcmp(method, "lockThread") == 0)
        return XI_CORO_SUB_LOCK_THREAD;
    if (strcmp(method, "unlockThread") == 0)
        return XI_CORO_SUB_UNLOCK_THREAD;
    /* OP_CORO_CTRL sub-opcodes (CORO_CTRL_* values from xchunk.h) */
    if (strcmp(method, "stats") == 0)
        return XI_CORO_SUB_CTRL_BASE + 0;
    if (strcmp(method, "list") == 0)
        return XI_CORO_SUB_CTRL_BASE + 1;
    if (strcmp(method, "dump") == 0)
        return XI_CORO_SUB_CTRL_BASE + 3;
    if (strcmp(method, "stalled") == 0)
        return XI_CORO_SUB_CTRL_BASE + 4;
    if (strcmp(method, "deadlocks") == 0)
        return XI_CORO_SUB_CTRL_BASE + 5;
    if (strcmp(method, "top") == 0)
        return XI_CORO_SUB_CTRL_BASE + 6;
    if (strcmp(method, "groupBy") == 0)
        return XI_CORO_SUB_CTRL_BASE + 7;
    if (strcmp(method, "whereis") == 0)
        return XI_CORO_SUB_CTRL_BASE + 8;
    if (strcmp(method, "monitor") == 0)
        return XI_CORO_SUB_CTRL_BASE + 9;
    if (strcmp(method, "demonitor") == 0)
        return XI_CORO_SUB_CTRL_BASE + 10;
    if (strcmp(method, "self") == 0)
        return XI_CORO_SUB_CTRL_BASE + 11;
    if (strcmp(method, "kill") == 0)
        return XI_CORO_SUB_CTRL_BASE + 12;
    return -1;
}

#define XI_LOWER_CALL_ARG_STACK_CAP 32
#define XI_LOWER_MAX_CALL_ARGS ((int) UINT16_MAX - 1)

typedef struct XiLowerArgList {
    XiValue **items;
    int count;
    int cap;
} XiLowerArgList;

typedef struct XiLowerGoArgList {
    XiValue **items;
    uint8_t *modes;
    int count;
    int cap;
} XiLowerGoArgList;

static void xi_lower_arg_list_init(XiLowerArgList *list, XiValue **stack_items, int stack_cap) {
    list->items = stack_items;
    list->count = 0;
    list->cap = stack_cap;
}

static bool xi_lower_arg_list_grow(XiLower *l, XiLowerArgList *list, int max_args) {
    int next_cap = list->cap > 0 ? list->cap * 2 : 1;
    if (next_cap <= list->count)
        next_cap = list->count + 1;
    if (next_cap > max_args)
        next_cap = max_args;
    if (next_cap <= list->cap)
        return false;

    XiValue **items =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (next_cap * (int) sizeof(XiValue *)));
    if (!items) {
        l->had_error = true;
        return false;
    }
    if (list->count > 0)
        memcpy(items, list->items, (size_t) list->count * sizeof(XiValue *));
    list->items = items;
    list->cap = next_cap;
    return true;
}

static bool xi_lower_arg_list_push(XiLower *l, XiLowerArgList *list, XiValue *value, int max_args,
                                   int line) {
    if (list->count >= max_args) {
        fprintf(stderr, "[LOWER] call argument count exceeds %d at line %d\n", max_args, line);
        l->had_error = true;
        return false;
    }
    if (list->count >= list->cap && !xi_lower_arg_list_grow(l, list, max_args)) {
        l->had_error = true;
        return false;
    }
    list->items[list->count++] = value;
    return true;
}

static void xi_lower_go_arg_list_init(XiLowerGoArgList *list, XiValue **stack_items,
                                      uint8_t *stack_modes, int stack_cap) {
    list->items = stack_items;
    list->modes = stack_modes;
    list->count = 0;
    list->cap = stack_cap;
}

static bool xi_lower_go_arg_list_grow(XiLower *l, XiLowerGoArgList *list, int max_args) {
    int next_cap = list->cap > 0 ? list->cap * 2 : 1;
    if (next_cap <= list->count)
        next_cap = list->count + 1;
    if (next_cap > max_args)
        next_cap = max_args;
    if (next_cap <= list->cap)
        return false;

    XiValue **items =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (next_cap * (int) sizeof(XiValue *)));
    uint8_t *modes =
        (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) (next_cap * (int) sizeof(uint8_t)));
    if (!items || !modes) {
        l->had_error = true;
        return false;
    }
    if (list->count > 0) {
        memcpy(items, list->items, (size_t) list->count * sizeof(XiValue *));
        memcpy(modes, list->modes, (size_t) list->count * sizeof(uint8_t));
    }
    list->items = items;
    list->modes = modes;
    list->cap = next_cap;
    return true;
}

static bool xi_lower_go_arg_list_push(XiLower *l, XiLowerGoArgList *list, XiValue *value,
                                      uint8_t mode, int max_args, int line) {
    if (list->count >= max_args) {
        fprintf(stderr, "[LOWER] go argument count exceeds %d at line %d\n", max_args, line);
        l->had_error = true;
        return false;
    }
    if (list->count >= list->cap && !xi_lower_go_arg_list_grow(l, list, max_args)) {
        l->had_error = true;
        return false;
    }
    list->items[list->count] = value;
    list->modes[list->count] = mode;
    list->count++;
    return true;
}

static bool lower_expr_is_copy_call(AstNode *node, AstNode **inner_out) {
    if (inner_out)
        *inner_out = NULL;
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count != 1 || !call->callee || call->callee->type != AST_VARIABLE)
        return false;
    const char *name = call->callee->as.variable.name;
    if (!name || strcmp(name, "copy") != 0)
        return false;
    if (inner_out)
        *inner_out = call->arguments[0];
    return true;
}

static bool lower_go_call_args(XiLower *l, CallExprNode *call, XiLowerGoArgList *args, int max_args,
                               int line) {
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *child = call->arguments[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return false;
            int arity = src->type ? xr_type_tuple_count(src->type) : 0;
            for (int j = 0; j < arity; j++) {
                struct XrType *et = xr_type_tuple_get(src->type, j);
                XiValue *get =
                    xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et ? et : l->type_any, 1);
                if (!get)
                    return false;
                get->args[0] = src;
                get->aux_int = j;
                if (!xi_lower_go_arg_list_push(l, args, get, XR_TRANSFER_SHARE, max_args, line))
                    return false;
            }
            continue;
        }

        uint8_t mode = XR_TRANSFER_SHARE;
        AstNode *value_node = child;
        if (child->type == AST_MOVE_EXPR) {
            mode = XR_TRANSFER_MOVE;
        } else {
            AstNode *copy_inner = NULL;
            if (lower_expr_is_copy_call(child, &copy_inner) && copy_inner) {
                mode = XR_TRANSFER_COPY;
                value_node = copy_inner;
            }
        }

        XiValue *a = xi_lower_expr(l, value_node);
        if (!a)
            return false;
        if (!xi_lower_go_arg_list_push(l, args, a, mode, max_args, line))
            return false;
    }
    return true;
}

XR_FUNC bool xi_lower_boundary_transfer_arg(XiLower *l, AstNode *child, XiValue **out_value,
                                            uint8_t *out_mode) {
    if (out_value)
        *out_value = NULL;
    if (out_mode)
        *out_mode = XR_TRANSFER_SHARE;
    if (!child || !out_value || !out_mode)
        return false;

    uint8_t mode = XR_TRANSFER_SHARE;
    AstNode *value_node = child;
    if (child->type == AST_MOVE_EXPR) {
        mode = XR_TRANSFER_MOVE;
    } else {
        AstNode *copy_inner = NULL;
        if (lower_expr_is_copy_call(child, &copy_inner) && copy_inner) {
            mode = XR_TRANSFER_COPY;
            value_node = copy_inner;
        }
    }

    XiValue *value = xi_lower_expr(l, value_node);
    if (!value)
        return false;
    *out_value = value;
    *out_mode = mode;
    return true;
}

static bool lower_call_is_sys_thread_spawn(const CallExprNode *call) {
    if (!call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *spawn = &call->callee->as.member_access;
    if (!spawn->name || strcmp(spawn->name, "spawn") != 0 || !spawn->object ||
        spawn->object->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *thread = &spawn->object->as.member_access;
    if (!thread->name || strcmp(thread->name, "Thread") != 0 || !thread->object ||
        thread->object->type != AST_VARIABLE)
        return false;
    const char *module_name = thread->object->as.variable.name;
    return module_name && strcmp(module_name, "sys") == 0;
}

static AstNode *lower_thread_spawn_body_arg(CallExprNode *call) {
    if (!call)
        return NULL;
    if (call->arg_count == 1)
        return call->arguments[0];
    if (call->arg_count == 2)
        return call->arguments[1];
    return NULL;
}

static XiValue *lower_thread_spawn_expr(XiLower *l, AstNode *node, AstNode *expr) {
    struct XrType *result_type = xi_lower_node_type(l, node);
    if (!expr)
        return NULL;

    if (expr->type == AST_CALL_EXPR) {
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee)
            return NULL;
        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        uint8_t stack_modes[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerGoArgList args;
        xi_lower_go_arg_list_init(&args, stack_args, stack_modes, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_go_call_args(l, call, &args, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
        int n = args.count;
        XiValue *v =
            xi_value_new(l->func, l->cur_block, XI_THREAD_SPAWN, result_type, (uint16_t) (1 + n));
        if (!v)
            return NULL;
        v->args[0] = callee;
        for (int i = 0; i < n; i++)
            v->args[1 + i] = args.items[i];
        if (n > 0) {
            uint8_t *modes =
                (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) n * sizeof(uint8_t)));
            if (!modes)
                return NULL;
            memcpy(modes, args.modes, (size_t) n * sizeof(uint8_t));
            v->aux = modes;
        }
        v->aux_int = (int64_t) pack_go_aux(XR_LINK_NONE);
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *callee = xi_lower_expr(l, expr);
    if (!callee)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_THREAD_SPAWN, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = callee;
    v->aux_int = (int64_t) pack_go_aux(XR_LINK_NONE);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_thread_spawn_call(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!call)
        return NULL;
    if (call->arg_count == 2 && call->arguments[0]) {
        if (!xi_lower_expr(l, call->arguments[0]))
            return NULL;
    }
    return lower_thread_spawn_expr(l, node, lower_thread_spawn_body_arg(call));
}

/* Lower Coro.method(args...) → XI_CORO_OP.
 * Returns NULL for unrecognized methods. */
static XiValue *lower_coro_method(XiLower *l, AstNode *node, const char *method,
                                  CallExprNode *call) {
    /* Coro.yield(): cooperative CPU yield (Gosched). Lowers to an immediate
     * XI_YIELD suspend point — the same primitive the former bare `yield`
     * statement used. `yield expr` is reserved for generator value production. */
    if (strcmp(method, "yield") == 0) {
        if (call->arg_count != 0) {
            fprintf(stderr, "[LOWER] Coro.yield() takes no arguments at line %d\n",
                    (int) node->line);
            l->had_error = true;
            return NULL;
        }
        XiValue *y = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_unit, 0);
        if (!y)
            return NULL;
        y->aux_int = XI_YIELD_AUX_IMMEDIATE;
        y->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
        y->line = (uint32_t) node->line;
        return y;
    }
    int sub = coro_method_sub_type(method);
    if (sub < 0)
        return NULL;

    int n = call->arg_count;
    if (n < 0 || n > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] Coro.%s argument count exceeds %d at line %d\n", method,
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }
    XiValue *stack_args[16];
    XiValue **arg_vals = stack_args;
    if (n > 16) {
        arg_vals =
            (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) n * sizeof(XiValue *)));
        if (!arg_vals)
            return NULL;
    }
    for (int i = 0; i < n; i++) {
        arg_vals[i] = xi_lower_expr(l, call->arguments[i]);
        if (!arg_vals[i])
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CORO_OP, result_type, (uint16_t) n);
    if (!v)
        return NULL;
    for (int i = 0; i < n; i++)
        v->args[i] = arg_vals[i];
    v->aux_int = sub;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

/* Lower the argument list of a call, expanding any AST_SPREAD_EXPR
 * `...t` into one TUPLE_GET per static element of the source tuple.
 * Returns the effective argument count written into `args`. The
 * caller-supplied `pmodes`/`pcount` apply XR_PARAM_VALUE deep-copy
 * semantics to value-type slots; spread-expanded slots are not copied
 * (the source tuple already owns the element). */
static bool lower_call_args_expand_spread(XiLower *l, CallExprNode *call, XiLowerArgList *args,
                                          int max_args, const uint8_t *pmodes, int pcount,
                                          int line) {
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *child = call->arguments[i];
        if (!child)
            continue;

        if (child->type == AST_SPREAD_EXPR) {
            XiValue *src = xi_lower_expr(l, child->as.spread_expr.expr);
            if (!src)
                return false;
            int arity = src->type ? xr_type_tuple_count(src->type) : 0;
            for (int j = 0; j < arity; j++) {
                struct XrType *et = xr_type_tuple_get(src->type, j);
                XiValue *get =
                    xi_value_new(l->func, l->cur_block, XI_TUPLE_GET, et ? et : l->type_any, 1);
                if (!get)
                    return false;
                get->args[0] = src;
                get->aux_int = j;
                if (!xi_lower_arg_list_push(l, args, get, max_args, line))
                    return false;
            }
            continue;
        }

        XiValue *a = xi_lower_expr(l, child);
        if (!a)
            return false;
        uint8_t mode = (pmodes && args->count < pcount) ? pmodes[args->count] : XR_PARAM_VALUE;
        if (xi_lower_value_needs_value_clone(l, a) && !xi_lower_value_is_fresh_value_struct(a) &&
            mode == XR_PARAM_VALUE) {
            XiValue *cpy = xi_value_new(l->func, l->cur_block, XI_COPY, a->type, 1);
            if (cpy) {
                cpy->args[0] = a;
                xi_lower_mark_value_clone_copy(cpy);
                a = cpy;
            }
        }
        if (!xi_lower_arg_list_push(l, args, a, max_args, line))
            return false;
    }
    return true;
}

static XiValue *lower_emit_function_call(XiLower *l, AstNode *node, CallExprNode *call,
                                         XiValue *callee_val, struct XrType *callee_type) {
    if (!callee_val)
        return NULL;
    callee_type = xr_type_non_nullable(l->isolate, callee_type);

    const uint8_t *pmodes = NULL;
    int pcount = 0;
    if (callee_type && callee_type->kind == XR_KIND_FUNCTION) {
        pmodes = callee_type->function.param_passing_modes;
        pcount = callee_type->function.param_count;
    }

    XiFunc *static_callee = lower_resolve_static_callee_func(l, callee_val);
    uint8_t stack_auto_modes[64];
    const uint8_t *effective_pmodes = pmodes;
    int effective_pcount = pcount;
    int auto_count = pcount > 0 ? pcount : (static_callee ? (int) static_callee->nparams : 0);
    if (static_callee && auto_count > 0) {
        uint8_t *auto_modes =
            auto_count <= (int) (sizeof(stack_auto_modes) / sizeof(stack_auto_modes[0]))
                ? stack_auto_modes
                : (uint8_t *) xi_func_arena_alloc(
                      l->func, (uint32_t) ((size_t) auto_count * sizeof(uint8_t)));
        if (auto_modes) {
            lower_apply_auto_borrow_param_modes(l, static_callee, pmodes, pcount, auto_modes,
                                                auto_count);
            effective_pmodes = auto_modes;
            effective_pcount = auto_count;
        }
    }

    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, effective_pmodes,
                                       effective_pcount, (int) node->line))
        return NULL;
    XiValue **arg_vals = args.items;
    int n = args.count;

    const XiImportRef *callee_import = lower_import_ref_from_value(l, callee_val);
    const char *math_callee_member =
        (callee_import && callee_import->module_path &&
         strcmp(callee_import->module_path, "math") == 0 && callee_import->member_name)
            ? callee_import->member_name
            : NULL;

    if (callee_type && callee_type->kind == XR_KIND_FUNCTION && callee_type->function.param_types) {
        int pc = callee_type->function.param_count;
        for (int i = 0; i < n && i < pc; i++) {
            struct XrType *pt = callee_type->function.param_types[i];
            if (!pt || !arg_vals[i] || !arg_vals[i]->type)
                continue;
            if (pt->kind == XR_KIND_TYPE_PARAM && call->type_arg_count > 0 &&
                callee_type->function.type_param_names) {
                const char *tp_name = pt->type_param.name;
                for (int ti = 0; ti < callee_type->function.type_param_count; ti++) {
                    if (callee_type->function.type_param_names[ti] && tp_name &&
                        strcmp(callee_type->function.type_param_names[ti], tp_name) == 0 &&
                        ti < call->type_arg_count && call->type_args[ti]) {
                        pt = xr_tref_resolve(l->isolate, call->type_args[ti]);
                        break;
                    }
                }
            }
            if (pt && XR_TYPE_IS_FLOAT(pt) && XR_TYPE_IS_INT(arg_vals[i]->type) &&
                !lower_math_preserves_int_arg_shape(math_callee_member)) {
                XiValue *conv = xi_value_new(l->func, l->cur_block, XI_CONVERT, l->type_float, 1);
                if (conv) {
                    conv->args[0] = arg_vals[i];
                    conv->line = (uint32_t) node->line;
                    arg_vals[i] = conv;
                }
            }
            /* Strict dynamic→concrete argument boundary: a Json/dynamic value passed
             * into a concrete parameter is verified at runtime via OP_CHECKTYPE,
             * exactly like the let-binding / return / map-key boundaries. This keeps
             * VM and AOT raising the same TypeError on mismatch instead of silently
             * coercing (e.g. Json int 1 into a `bool` parameter). */
            if (xr_is_json_coercion(pt, arg_vals[i]->type))
                arg_vals[i] = xi_lower_checktype_for_type(l, node, arg_vals[i], pt);
        }
    }

    bool is_self_call = (l->self_var_id >= 0 && (uint32_t) l->self_var_id <= XI_MAX_VAR_ID &&
                         callee_val->var_id == (XiVarId) l->self_var_id);

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (math_callee_member && lower_math_call_arity_ok(math_callee_member, n))
        result_type = lower_math_call_result_type(l, math_callee_member, arg_vals, n);

    uint16_t nargs = (uint16_t) (n + 1);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = callee_val;
    for (int i = 0; i < n; i++)
        v->args[i + 1] = arg_vals[i];
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    if (is_self_call)
        v->aux_int = 1;

    xi_lower_insert_err_check(l, node);
    return v;
}

static XiValue *lower_enum_method_callee(XiLower *l, const XaSelection *sel, int line) {
    if (!l || !sel || !sel->target_symbol || !sel->receiver_type ||
        sel->receiver_type->kind != XR_KIND_ENUM)
        return NULL;
    const char *enum_name = sel->receiver_type->enum_type.enum_name;
    const char *method_name = sel->target_symbol->name;
    const char *hidden = xi_lower_enum_method_hidden_name(l->func, enum_name, method_name,
                                                          sel->target_symbol->is_static);
    uint32_t sid = sel->target_symbol->id;

    int var_id = xi_lower_var_find(l, sid, hidden);
    if (var_id >= 0) {
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            XiValue *load = xi_lower_emit_top_load(l, b, sel->result_type);
            if (load)
                load->line = (uint32_t) line;
            return load;
        }
        return xi_lower_braun_read(l, var_id, l->cur_block);
    }

    XiTopBinding tb = xi_lower_find_top_binding(l, sid, hidden);
    if (xi_top_binding_valid(tb)) {
        XiValue *load = xi_lower_emit_top_load(l, tb, sel->result_type);
        if (load)
            load->line = (uint32_t) line;
        return load;
    }

    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, hidden, &upval_type);
    if (upval_idx >= 0) {
        XiValue *load = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                                     upval_type ? upval_type : sel->result_type, 0);
        if (load) {
            load->aux_int = upval_idx;
            load->line = (uint32_t) line;
        }
        return load;
    }
    return NULL;
}

static XiValue *lower_enum_method_direct_call(XiLower *l, AstNode *node, CallExprNode *call,
                                              MemberAccessNode *ma) {
    const XaSelection *sel = xa_analyzer_get_selection(l->analyzer, call->callee);
    if (!sel || !sel->target_symbol || sel->target_symbol->kind != XA_SYM_METHOD ||
        !sel->receiver_type || sel->receiver_type->kind != XR_KIND_ENUM)
        return NULL;
    if (sel->kind != XA_SEL_STATIC_MEMBER && sel->kind != XA_SEL_METHOD)
        return NULL;

    bool is_static = sel->target_symbol->is_static || sel->kind == XA_SEL_STATIC_MEMBER;
    XiValue *callee = lower_enum_method_callee(l, sel, (int) node->line);
    if (!callee)
        return NULL;

    XiValue *recv = NULL;
    if (!is_static) {
        recv = xi_lower_expr(l, ma->object);
        if (!recv)
            return NULL;
    }

    const uint8_t *pmodes = NULL;
    int pcount = 0;
    if (sel->result_type && sel->result_type->kind == XR_KIND_FUNCTION) {
        pmodes = sel->result_type->function.param_passing_modes;
        pcount = sel->result_type->function.param_count;
    }

    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, pmodes, pcount,
                                       (int) node->line))
        return NULL;

    int extra = is_static ? 0 : 1;
    int n = args.count + extra;
    uint16_t nargs = (uint16_t) (n + 1);
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = callee;
    int out = 1;
    if (!is_static)
        v->args[out++] = recv;
    for (int i = 0; i < args.count; i++)
        v->args[out + i] = args.items[i];
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    xi_lower_insert_err_check(l, node);
    return v;
}

static bool lower_is_direct_arg(AstNode *arg) {
    return arg && arg->type != AST_SPREAD_EXPR;
}

static bool lower_is_channel_send_boundary_method(const char *method) {
    return method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0 ||
                      strcmp(method, "sendTimeout") == 0);
}

static XrType *xi_raw_pointer_type_namespace(XiLower *l, AstNode *object) {
    if (!l || !object || object->type != AST_NEW_EXPR)
        return NULL;
    NewExprNode *ne = &object->as.new_expr;
    if (ne->module_name || !ne->class_name || ne->type_arg_count != 1 || !ne->type_args ||
        !ne->type_args[0])
        return NULL;
    bool is_mut = false;
    if (strcmp(ne->class_name, "RawPtr") == 0) {
        is_mut = false;
    } else if (strcmp(ne->class_name, "RawMut") == 0) {
        is_mut = true;
    } else {
        return NULL;
    }
    XrType *pointee = xr_tref_resolve(l->isolate, ne->type_args[0]);
    if (!pointee)
        pointee = xr_type_new_unknown(l->isolate);
    return xr_type_new_pointer(l->isolate, pointee, is_mut);
}

static XiValue *lower_raw_pointer_static_call(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!l || !node || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &call->callee->as.member_access;
    if (!ma->name || strcmp(ma->name, "null") != 0 || call->arg_count != 0)
        return NULL;
    XrType *ptr_type = xi_raw_pointer_type_namespace(l, ma->object);
    if (!ptr_type)
        return NULL;
    XrType *result_type = xi_lower_node_type(l, node);
    if (!result_type || xi_lower_type_is_unknown(result_type))
        result_type = ptr_type;
    XiValue *v = xi_const_int(l->func, l->cur_block, 0, result_type);
    if (v)
        v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_channel_send_boundary_call(XiLower *l, AstNode *node, CallExprNode *call,
                                                 const char *method, XiValue *recv) {
    if (!recv || !recv->type || recv->type->kind != XR_KIND_CHANNEL ||
        !lower_is_channel_send_boundary_method(method))
        return NULL;
    int want_args = strcmp(method, "sendTimeout") == 0 ? 2 : 1;
    if (call->arg_count != want_args || !lower_is_direct_arg(call->arguments[0]))
        return NULL;
    if (want_args == 2 && !lower_is_direct_arg(call->arguments[1]))
        return NULL;

    XiValue *payload = NULL;
    uint8_t transfer_mode = XR_TRANSFER_SHARE;
    if (!xi_lower_boundary_transfer_arg(l, call->arguments[0], &payload, &transfer_mode))
        return NULL;
    XiValue *timeout = NULL;
    if (want_args == 2) {
        timeout = xi_lower_expr(l, call->arguments[1]);
        if (!timeout)
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (strcmp(method, "send") == 0) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_SEND, l->type_unit, 2);
        if (!v)
            return NULL;
        v->args[0] = recv;
        v->args[1] = payload;
        xi_chan_send_set_transfer_mode(v, transfer_mode);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND;
        v->line = (uint32_t) node->line;
        return v;
    }

    uint16_t nargs = (uint16_t) (want_args + 1);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = recv;
    v->args[1] = payload;
    if (want_args == 2) {
        v->args[2] = timeout;
    }
    v->aux = (void *) arena_strdup(l->func, method);
    v->aux_int = (int64_t) xi_lower_method_symbol(l, method) << 1;
    xi_chan_send_set_transfer_mode(v, transfer_mode);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    if (xi_lower_method_may_suspend(recv->type, method, want_args))
        v->flags |= XI_FLAG_MAY_SUSPEND;
    v->line = (uint32_t) node->line;
    xi_lower_insert_err_check(l, node);
    return v;
}

static XiValue *lower_call(XiLower *l, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;

    if (lower_call_is_sys_thread_spawn(call))
        return lower_thread_spawn_call(l, node, call);

    XiValue *raw_pointer_static = lower_raw_pointer_static_call(l, node, call);
    if (raw_pointer_static)
        return raw_pointer_static;

    /* Method call: callee is obj.method — emit XI_CALL_METHOD (→ OP_INVOKE).
     * This is required for builtin methods (set.size, array.push, etc.)
     * which rely on OP_INVOKE dispatch rather than GETPROP + CALL. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;

        XiValue *enum_direct = lower_enum_method_direct_call(l, node, call, ma);
        if (enum_direct)
            return enum_direct;

        if (ma->object && ma->object->type == AST_VARIABLE && ma->name &&
            strcmp(ma->name, "withCapacity") == 0 && call->arg_count == 1 &&
            (strcmp(ma->object->as.variable.name, "Array") == 0 ||
             strcmp(ma->object->as.variable.name, "Bytes") == 0)) {
            XiValue *cap = xi_lower_expr(l, call->arguments[0]);
            if (!cap)
                return NULL;
            struct XrType *result_type = xi_lower_node_type(l, node);
            if (!result_type && strcmp(ma->object->as.variable.name, "Bytes") == 0)
                result_type = xr_type_new_bytes(NULL);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            v->aux = (void *) "array_with_capacity";
            v->aux_int = xi_array_cfield_from_type(result_type);
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }

        /* Json.decode<T>(data) → XI_JSON_DECODE with compile-time field info.
         * The analyzer already validated T is a sealed Record type with fields
         * and stored the result type as T? in the node table. */
        if (ma->name && strcmp(ma->name, "decode") == 0 && ma->object &&
            ma->object->type == AST_VARIABLE && strcmp(ma->object->as.variable.name, "Json") == 0 &&
            call->type_arg_count == 1 && call->arg_count == 1) {
            struct XrType *result_type = xi_lower_node_type(l, node);
            if (result_type && XR_TYPE_IS_RECORD(result_type) && result_type->object.is_sealed &&
                result_type->object.field_count > 0) {
                int fc = result_type->object.field_count;
                XiValue *data_val = xi_lower_expr(l, call->arguments[0]);
                if (!data_val)
                    return NULL;

                /* Arena-copy field names so they survive AST destruction */
                const char **names = (const char **) xi_func_arena_alloc(
                    l->func, (uint32_t) (fc * (int) sizeof(const char *)));
                XR_DCHECK(names != NULL, "json_decode: arena alloc failed");
                for (int i = 0; i < fc; i++) {
                    names[i] = arena_strdup(l->func, result_type->object.field_names[i]);
                }

                XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_DECODE, result_type, 1);
                if (!v)
                    return NULL;
                v->args[0] = data_val;
                v->aux = (void *) names;
                v->aux_int = fc;
                v->flags |= XI_FLAG_SIDE_EFFECT;
                v->line = (uint32_t) node->line;
                return v;
            }
        }

        /* Coro.method() → XI_CORO_OP with sub-type encoding.
         * Coro is a built-in module with dedicated VM opcodes; it has
         * no runtime object, so the generic XI_CALL_METHOD path would
         * fail because lower_variable("Coro") cannot resolve. */
        if (ma->object && ma->object->type == AST_VARIABLE && ma->name &&
            strcmp(ma->object->as.variable.name, "Coro") == 0) {
            XiValue *coro_op = lower_coro_method(l, node, ma->name, call);
            if (coro_op)
                return coro_op;
            /* Unknown Coro method — fall through to generic path which
             * will report "unresolved variable" for Coro. */
        }

        XiValue *recv = xi_lower_expr(l, ma->object);
        if (!recv)
            return NULL;

        XiValue *chan_send = lower_channel_send_boundary_call(l, node, call, ma->name, recv);
        if (chan_send)
            return chan_send;

        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerArgList args;
        xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, NULL, 0,
                                           (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;

        struct XrType *result_type = xi_lower_node_type(l, node);

        if (lower_value_is_whole_module_import(l, recv, "math") &&
            lower_math_call_arity_ok(ma->name, n))
            result_type = lower_math_call_result_type(l, ma->name, arg_vals, n);

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "clear") == 0 && n == 0) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->aux = (void *) "array_clear";
            v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "reserve") == 0 && n == 1) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->aux = (void *) "array_reserve";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "resize") == 0 &&
            (n == 2 || (n == 1 && xi_type_is_bytes(recv->type)))) {
            XiValue *fill =
                n == 2 ? arg_vals[1] : xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 3);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->args[2] = fill;
            v->aux = (void *) "array_resize";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "getUnchecked") == 0 && n == 1) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->aux_int = 1;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && XR_TYPE_IS_ARRAY(recv->type) && ma->name &&
            strcmp(ma->name, "setUnchecked") == 0 && n == 2) {
            struct XrType *elem_type = xi_get_container_elem_type(recv->type);
            arg_vals[1] = xi_lower_narrow_for_static_type(l, node, arg_vals[1], elem_type);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, result_type, 3);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->args[2] = arg_vals[1];
            v->aux_int = 1;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && XR_TYPE_IS_SPAN(recv->type) && ma->name &&
            strcmp(ma->name, "getUnchecked") == 0 && n == 1) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->aux_int = 1;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && (XR_TYPE_IS_ARRAY(recv->type) || XR_TYPE_IS_SPAN(recv->type)) &&
            ma->name && n == 0 &&
            (strcmp(ma->name, "dataPtrUnchecked") == 0 ||
             strcmp(ma->name, "dataMutPtrUnchecked") == 0)) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_ARRAY_DATA_PTR, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->line = (uint32_t) node->line;
            return v;
        }

        /* FFI raw pointer methods: deref()/offset(i)/loadLEUnchecked<T>(i)/isNull(). */
        if (recv->type && XR_TYPE_IS_POINTER(recv->type) && ma->name) {
            if (strcmp(ma->name, "loadLEUnchecked") == 0 && n == 1 &&
                xi_pointer_pointee_is_u8(recv->type) && call->type_arg_count == 1 &&
                call->type_args && call->type_args[0]) {
                XrType *target = xr_tref_resolve(l->isolate, call->type_args[0]);
                uint8_t code = xr_ffi_type_from_xrtype(target, false);
                if (code == XR_FFI_T_U16 || code == XR_FFI_T_U32 || code == XR_FFI_T_U64) {
                    XiValue *addr = xi_lower_ptr_scaled_addr(l, node, recv, arg_vals[0], recv->type,
                                                             recv->type);
                    if (!addr)
                        return NULL;
                    struct XrType *load_type =
                        xi_lower_type_is_unknown(result_type) ? target : result_type;
                    XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, load_type, 1);
                    if (!v)
                        return NULL;
                    v->args[0] = addr;
                    v->aux_int = (int64_t) xr_ffi_ptr_aux(code, true);
                    v->flags |= XI_FLAG_READS_MEM;
                    v->line = (uint32_t) node->line;
                    return v;
                }
            }
            if (strcmp(ma->name, "deref") == 0 && n == 0) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, result_type, 1);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->aux_int = (int64_t) xr_ffi_ptr_aux(xi_pointer_pointee_ffi(recv->type), false);
                v->flags |= XI_FLAG_READS_MEM;
                v->line = (uint32_t) node->line;
                return v;
            }
            if (strcmp(ma->name, "offset") == 0 && n == 1) {
                XiValue *v =
                    xi_lower_ptr_scaled_addr(l, node, recv, arg_vals[0], recv->type, recv->type);
                if (!v)
                    return NULL;
                return v;
            }
            if (strcmp(ma->name, "copyFromNonOverlappingUnchecked") == 0 && n == 2) {
                XiValue *byte_count = arg_vals[1];
                int64_t size = xi_pointer_pointee_size(recv->type);
                if (size != 1) {
                    XiValue *sz = xi_const_int(l->func, l->cur_block, size, l->type_int);
                    XiValue *mul = xi_value_new(l->func, l->cur_block, XI_MUL, l->type_int, 2);
                    if (!mul)
                        return NULL;
                    mul->args[0] = byte_count;
                    mul->args[1] = sz;
                    mul->line = (uint32_t) node->line;
                    byte_count = mul;
                }
                XiValue *v =
                    xi_value_new(l->func, l->cur_block, XI_PTR_COPY_NONOVERLAP, result_type, 3);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->args[1] = arg_vals[0];
                v->args[2] = byte_count;
                v->line = (uint32_t) node->line;
                return v;
            }
            if (strcmp(ma->name, "isNull") == 0 && n == 0) {
                XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_EQ, l->type_bool, 2);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->args[1] = zero;
                v->line = (uint32_t) node->line;
                return v;
            }
        }

        if (xi_type_is_bytes(recv->type) && ma->name) {
            uint16_t bytes_op = 0;
            uint16_t expected_args = 0;
            bool load_le_unchecked = strcmp(ma->name, "loadLEUnchecked") == 0;
            if ((strcmp(ma->name, "loadLE") == 0 || load_le_unchecked) && n == 1 &&
                call->type_arg_count == 1 && call->type_args && call->type_args[0]) {
                XrType *target = xr_tref_resolve(l->isolate, call->type_args[0]);
                if (target && XR_TYPE_IS_INT(target) && target->native_width == XR_NATIVE_U16) {
                    bytes_op = XI_BYTES_LOAD_U16_LE;
                    expected_args = 2;
                } else if (target && XR_TYPE_IS_INT(target) &&
                           target->native_width == XR_NATIVE_U32) {
                    bytes_op = XI_BYTES_LOAD_U32_LE;
                    expected_args = 2;
                } else if (target && XR_TYPE_IS_INT(target) &&
                           target->native_width == XR_NATIVE_U64) {
                    bytes_op = XI_BYTES_LOAD_U64_LE;
                    expected_args = 2;
                }
            }
            if (bytes_op) {
                /* Strict dynamic→int boundary on Bytes intrinsic offsets/counts:
                 * a Json/dynamic argument is verified at runtime via OP_CHECKTYPE
                 * so VM and AOT raise the same TypeError instead of silently
                 * coercing. */
                for (int i = 0; i < n; i++) {
                    if (arg_vals[i] && arg_vals[i]->type &&
                        xr_is_json_coercion(l->type_int, arg_vals[i]->type))
                        arg_vals[i] =
                            xi_lower_checktype_for_type(l, node, arg_vals[i], l->type_int);
                }
                XiValue *v =
                    xi_value_new(l->func, l->cur_block, bytes_op, result_type, expected_args);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                for (int i = 0; i < n; i++)
                    v->args[i + 1] = arg_vals[i];
                if (load_le_unchecked)
                    v->aux_int = 1;
                v->line = (uint32_t) node->line;
                return v;
            }
        }

        xi_lower_check_map_method_args(l, node, ma->name, recv, arg_vals, n);
        xi_lower_check_set_method_args(l, node, ma->name, recv, arg_vals, n);
        xi_lower_narrow_map_method_args(l, node, ma->name, recv, arg_vals, n);
        xi_lower_narrow_set_method_args(l, node, ma->name, recv, arg_vals, n);

        if (recv->type && recv->type->kind == XR_KIND_CHANNEL && ma->name &&
            strcmp(ma->name, "send") == 0 && n == 1) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_SEND, l->type_unit, 2);
            if (!v)
                return NULL;
            v->args[0] = recv;
            v->args[1] = arg_vals[0];
            v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND;
            v->line = (uint32_t) node->line;
            return v;
        }

        if (recv->type && recv->type->kind == XR_KIND_CHANNEL && ma->name &&
            strcmp(ma->name, "recvOr") == 0 && n == 1) {
            /* ch.recvOr(fallback): blocking recv returning the received value,
             * or `fallback` when the channel is closed and drained. Reuses the
             * raw XI_CHAN_RECV / XI_CHAN_RECV_STATUS fast path (same as the recv
             * match lowering), so no Recv<T> enum is materialized and VM / AOT
             * agree. Equivalent to
             *   match ch.recv() { Recv.Value(v) -> v; _ -> fallback }. */
            struct XrType *payload_type = recv->type->container.element_type
                                              ? recv->type->container.element_type
                                              : result_type;
            XiValue *chan_recv = xi_value_new(l->func, l->cur_block, XI_CHAN_RECV, payload_type, 1);
            if (!chan_recv)
                return NULL;
            chan_recv->args[0] = recv;
            chan_recv->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_SUSPEND;
            chan_recv->line = (uint32_t) node->line;

            XiValue *status =
                xi_value_new(l->func, l->cur_block, XI_CHAN_RECV_STATUS, l->type_bool, 1);
            if (!status)
                return NULL;
            status->args[0] = chan_recv;
            status->line = (uint32_t) node->line;

            XiBlock *value_blk = xi_block_new(l->func);
            XiBlock *fallback_blk = xi_block_new(l->func);
            XiBlock *merge = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, status, value_blk, fallback_blk);
            xi_lower_braun_seal(l, value_blk);
            xi_lower_braun_seal(l, fallback_blk);

            xi_block_set_jump(value_blk, merge);
            xi_block_set_jump(fallback_blk, merge);

            xi_lower_braun_seal(l, merge);
            l->cur_block = merge;
            XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
            if (phi) {
                for (uint16_t i = 0; i < merge->npreds; i++) {
                    if (merge->preds[i] == value_blk)
                        phi->value.args[i] = chan_recv;
                    else
                        phi->value.args[i] = arg_vals[0];
                }
            }
            return phi ? &phi->value : chan_recv;
        }

        uint16_t nargs = (uint16_t) (n + 1); /* receiver + args */
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
        if (!v)
            return NULL;
        v->args[0] = recv;
        for (int i = 0; i < n; i++)
            v->args[i + 1] = arg_vals[i];
        v->aux = (void *) arena_strdup(l->func, ma->name);
        v->aux_int = (int64_t) xi_lower_method_symbol(l, ma->name) << 1;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        if (xi_lower_method_may_suspend(recv->type, ma->name, n))
            v->flags |= XI_FLAG_MAY_SUSPEND;
        v->line = (uint32_t) node->line;

        xi_lower_insert_err_check(l, node);
        return v;
    }

    /* Optional chain method call: obj?.method(args) — null short-circuit
     * with XI_CALL_METHOD on the non-null path. chain_type==2 signals
     * the parser detected a call immediately after the optional chain. */
    if (call->callee && call->callee->type == AST_OPTIONAL_CHAIN &&
        call->callee->as.optional_chain.name && call->callee->as.optional_chain.chain_type == 2) {
        OptionalChainNode *oc = &call->callee->as.optional_chain;
        XiValue *obj = xi_lower_expr(l, oc->object);
        if (!obj)
            return NULL;

        XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
        if (!is_null)
            return obj;
        is_null->args[0] = obj;

        XiBlock *call_blk = xi_block_new(l->func);
        XiBlock *null_blk = xi_block_new(l->func);
        XiBlock *merge = xi_block_new(l->func);

        xi_block_set_if(l->cur_block, is_null, null_blk, call_blk);
        xi_lower_braun_seal(l, call_blk);
        xi_lower_braun_seal(l, null_blk);

        /* Null path */
        l->cur_block = null_blk;
        struct XrType *result_type = xi_lower_node_type(l, node);
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        xi_block_set_jump(l->cur_block, merge);

        /* Non-null path: emit XI_CALL_METHOD */
        l->cur_block = call_blk;
        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerArgList args;
        xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, NULL, 0,
                                           (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;

        xi_lower_check_map_method_args(l, node, oc->name, obj, arg_vals, n);
        xi_lower_check_set_method_args(l, node, oc->name, obj, arg_vals, n);
        xi_lower_narrow_map_method_args(l, node, oc->name, obj, arg_vals, n);
        xi_lower_narrow_set_method_args(l, node, oc->name, obj, arg_vals, n);

        uint16_t nargs = (uint16_t) (n + 1);
        XiValue *mcall = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
        if (!mcall)
            return NULL;
        mcall->args[0] = obj;
        for (int i = 0; i < n; i++)
            mcall->args[i + 1] = arg_vals[i];
        mcall->aux = (void *) arena_strdup(l->func, oc->name);
        mcall->aux_int = (int64_t) xi_lower_method_symbol(l, oc->name) << 1;
        mcall->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        mcall->line = (uint32_t) node->line;
        XiBlock *call_exit = l->cur_block;
        xi_block_set_jump(call_exit, merge);

        /* Merge: PHI(null, method_result) */
        xi_lower_braun_seal(l, merge);
        l->cur_block = merge;
        XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
        if (phi) {
            for (uint16_t i = 0; i < merge->npreds; i++) {
                if (merge->preds[i] == null_blk)
                    phi->value.args[i] = null_val;
                else
                    phi->value.args[i] = mcall;
            }
        }
        return phi ? &phi->value : null_val;
    }

    /* Optional function call: func?.(args) — evaluate args only on the non-null
     * path and lower to a normal XI_CALL there. */
    if (call->callee && call->callee->type == AST_OPTIONAL_CHAIN &&
        call->callee->as.optional_chain.chain_type == 3) {
        OptionalChainNode *oc = &call->callee->as.optional_chain;
        XiValue *callee_val = xi_lower_expr(l, oc->object);
        if (!callee_val)
            return NULL;

        XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
        if (!is_null)
            return callee_val;
        is_null->args[0] = callee_val;

        XiBlock *call_blk = xi_block_new(l->func);
        XiBlock *null_blk = xi_block_new(l->func);
        XiBlock *merge = xi_block_new(l->func);

        xi_block_set_if(l->cur_block, is_null, null_blk, call_blk);
        xi_lower_braun_seal(l, call_blk);
        xi_lower_braun_seal(l, null_blk);

        l->cur_block = null_blk;
        struct XrType *result_type = xi_lower_node_type(l, node);
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        xi_block_set_jump(l->cur_block, merge);

        l->cur_block = call_blk;
        XiValue *call_val =
            lower_emit_function_call(l, node, call, callee_val, xi_lower_node_type(l, oc->object));
        if (!call_val)
            return NULL;
        XiBlock *call_exit = l->cur_block;
        xi_block_set_jump(call_exit, merge);

        xi_lower_braun_seal(l, merge);
        l->cur_block = merge;
        XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
        if (phi) {
            for (uint16_t i = 0; i < merge->npreds; i++) {
                if (merge->preds[i] == null_blk)
                    phi->value.args[i] = null_val;
                else
                    phi->value.args[i] = call_val;
            }
        }
        return phi ? &phi->value : null_val;
    }

    /* Compile-time builtin interception: detect calls to known builtins
     * and emit specialized Xi ops instead of generic XI_CALL. */
    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *fname = call->callee->as.variable.name;
        XiValue *bi = lower_builtin_call(l, node, fname, call);
        if (bi)
            return bi;
    }

    /* `T(args)` where T is a class name constructs an instance (no `new`).
     * Unified with new-expr so nested/builtin/Exception classes all work. */
    XiValue *constructed = lower_try_construct_call(l, node, call);
    if (constructed)
        return constructed;

    /* Evaluate callee and all arguments before creating CALL */
    XiValue *callee_val = xi_lower_expr(l, call->callee);
    if (!callee_val)
        return NULL;

    return lower_emit_function_call(l, node, call, callee_val, xi_lower_node_type(l, call->callee));
}

static XiValue *xi_lower_narrow_select_arm(XiLower *l, AstNode *node, XiValue *val,
                                           struct XrType *result_type) {
    if (!val || !result_type || !val->type || xr_type_assignable(result_type, val->type))
        return val;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (!copy)
        return val;
    copy->args[0] = val;
    copy->line = (uint32_t) node->line;
    return copy;
}

static XiValue *lower_ternary(XiLower *l, AstNode *node) {
    XiValue *cond = xi_lower_expr(l, node->as.ternary.condition);
    if (!cond)
        return NULL;
    cond = xi_lower_bool_condition(l, cond);

    struct XrType *result_type = xi_lower_node_type(l, node);

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *else_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);
    xi_lower_braun_seal(l, then_blk);
    xi_lower_braun_seal(l, else_blk);

    l->cur_block = then_blk;
    XiValue *then_val = xi_lower_expr(l, node->as.ternary.true_expr);
    then_val = xi_lower_narrow_select_arm(l, node, then_val, result_type);
    XiBlock *then_exit = l->cur_block;
    xi_block_set_jump(then_exit, merge);

    l->cur_block = else_blk;
    XiValue *else_val = xi_lower_expr(l, node->as.ternary.false_expr);
    else_val = xi_lower_narrow_select_arm(l, node, else_val, result_type);
    XiBlock *else_exit = l->cur_block;
    xi_block_set_jump(else_exit, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;
    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == then_exit)
                phi->value.args[i] = then_val;
            else
                phi->value.args[i] = else_val;
        }
    }
    return phi ? &phi->value : then_val;
}

/*
 * Nullish coalesce (a ?? b): if a is null, evaluate b; otherwise use a.
 * Similar to short-circuit OR but checks null instead of falsy.
 */
static XiValue *lower_nullish_coalesce(XiLower *l, AstNode *node) {
    /* Partially dead after canonicalization: simple LHS is canonicalized
     * to ternary, but complex LHS still falls through to here. */
    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    if (!lhs)
        return NULL;

    XiBlock *eval_rhs = xi_block_new(l->func);
    XiBlock *skip = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    /* Test: is lhs null? */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null)
        return lhs;
    is_null->args[0] = lhs;

    /* If null → eval rhs; otherwise → skip (use lhs) */
    xi_block_set_if(l->cur_block, is_null, eval_rhs, skip);
    xi_lower_braun_seal(l, eval_rhs);
    xi_lower_braun_seal(l, skip);

    struct XrType *result_type = xi_lower_node_type(l, node);

    /* Evaluate RHS in eval_rhs block */
    l->cur_block = eval_rhs;
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip → merge (lhs is non-null) */
    l->cur_block = skip;
    XiValue *skip_val = xi_lower_narrow_select_arm(l, node, lhs, result_type);
    xi_block_set_jump(skip, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == rhs_exit)
                phi->value.args[i] = rhs ? rhs : lhs;
            else
                phi->value.args[i] = skip_val;
        }
    }
    return phi ? &phi->value : lhs;
}

static XiValue *lower_map_literal(XiLower *l, AstNode *node) {
    MapLiteralNode *map = &node->as.map_literal;
    int count = map->count;

    /* Evaluate all keys and values first */
    int n = count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **key_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    XiValue **val_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!key_vals || !val_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        key_vals[i] = xi_lower_expr(l, map->keys[i]);
        val_vals[i] = xi_lower_expr(l, map->values[i]);
        if (!key_vals[i] || !val_vals[i])
            return NULL;
    }

    /* Create map: XI_MAP_NEW with capacity */
    struct XrType *result_type = xi_lower_node_type(l, node);
    if (result_type && XR_TYPE_IS_MAP(result_type)) {
        for (int i = 0; i < n; i++) {
            key_vals[i] =
                xi_lower_narrow_for_static_type(l, node, key_vals[i], result_type->map.key_type);
            val_vals[i] =
                xi_lower_narrow_for_static_type(l, node, val_vals[i], result_type->map.value_type);
        }
    }
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *map_val = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
    if (!map_val)
        return NULL;
    map_val->args[0] = cap;
    map_val->line = (uint32_t) node->line;

    /* Populate: INDEX_SET for each key-value pair */
    for (int i = 0; i < n; i++) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, l->type_unit, 3);
        if (!set)
            break;
        set->args[0] = map_val;
        set->args[1] = key_vals[i];
        set->args[2] = val_vals[i];
        set->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return map_val;
}

XR_FUNC void xi_lower_func_add_child(XiFunc *parent, XiFunc *child) {
    if (parent->nchildren >= parent->children_cap) {
        uint16_t new_cap = parent->children_cap ? parent->children_cap * 2 : 4;
        XiFunc **tmp = (XiFunc **) xr_realloc(parent->children, new_cap * sizeof(XiFunc *));
        if (!tmp)
            return;
        parent->children = tmp;
        parent->children_cap = new_cap;
    }
    parent->children[parent->nchildren++] = child;
}

/*
 * Lower a function declaration / function expression.
 * Recursively lowers the function body into a child XiFunc,
 * then emits XI_CLOSURE_NEW in the parent to produce a callable value.
 */
/* xi_lower_func_impl declared in xi_lower_internal.h */

XR_FUNC XiValue *xi_lower_function_decl(XiLower *l, AstNode *node) {
    /* Recursively lower the function body into a child XiFunc,
     * passing 'l' as parent so the child can resolve upvalue captures. */
    XiFunc *child = xi_lower_func_impl(node, l->analyzer, l->isolate, l);
    if (!child) {
        l->had_error = true;
        return NULL;
    }

    /* Register as child of parent function */
    xi_lower_func_add_child(l->func, child);
    uint16_t child_idx = (uint16_t) (l->func->nchildren - 1);

    /* Emit CLOSURE_NEW with captured values as args.  Listing them as
     * args ensures liveness analysis keeps their registers alive until
     * the closure instruction executes (prevents premature recycling). */
    struct XrType *fn_type = xi_lower_node_type(l, node);
    uint16_t ncap = child->ncaptures;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, fn_type, ncap);
    if (!v)
        return NULL;
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &child->captures[ci];
        v->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value) ? cap->value : NULL;
    }
    v->aux = (void *) child;
    v->aux_int = child_idx;
    v->line = (uint32_t) node->line;

    /* If named, register in SSA so the function can be called by name */
    FunctionDeclNode *fdecl = &node->as.function_decl;
    if (fdecl->name) {
        int var_id = xi_lower_var_create(l, fdecl->symbol_id, fdecl->name, fn_type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

        /* Hoisted closures must survive DCE: they are stored into cells
         * at emit time for mutable upvalue capture by sibling functions. */
        if (var_id >= 0 && var_id < l->var_count && l->vars[var_id].hoisted)
            v->flags |= XI_FLAG_SIDE_EFFECT;

        /* For program-level named functions, also store into backing
         * store so nested functions can access (forward refs). */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            int slot = l->shared_map[var_id];
            XiTopBinding b;
            b.slot = slot;
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            xi_lower_emit_top_store(l, b, v);
            /* Track function → shared slot for module export metadata */
            if (slot >= 0 && slot < l->var_cap) {
                l->shared_slot_funcs[slot] = child;
                if (l->func->shared_slot_funcs && slot < (int) l->func->shared_slot_func_count)
                    l->func->shared_slot_funcs[slot] = child;
            }
        }
    }

    return v;
}

/* Shared construction lowering used by both `T(args)` calls (lower_call) and
 * the legacy new-expr node. Builds built-in collection ops or a class
 * constructor invocation. result_type is the resolved instance/container type
 * from the node table. */
static XiValue *lower_construct(XiLower *l, AstNode *node, struct XrType *result_type,
                                const char *module_name, const char *cname, AstNode **arguments,
                                int arg_count) {
    XR_DCHECK(cname != NULL, "construct must have class name");

    /* Built-in collection types: emit specialized ops (no constructor call) */
    if (module_name == NULL) {
        if (strcmp(cname, "Map") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            /* Encode key_kind and value_tid: C = (key_kind<<7)|(vtid<<2)|flags */
            if (XR_TYPE_IS_MAP(result_type)) {
                uint8_t vtid = 0, key_kind = 0;
                if (result_type->map.value_type)
                    vtid = xr_type_to_tid(result_type->map.value_type);
                if (result_type->map.key_type) {
                    uint8_t ktid = xr_type_to_tid(result_type->map.key_type);
                    if (ktid == XR_TID_STRING)
                        key_kind = 1;
                    else if (ktid == XR_TID_INT)
                        key_kind = 2;
                }
                v->aux_int = (int64_t) ((key_kind << 7) | ((vtid & 0x1F) << 2));
            } else {
                v->aux_int = 0;
            }
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "WeakMap") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            v->aux_int = 0x02; /* weak flag in C field bit 1 */
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            /* Encode elem_tid from explicit type param: C = (tid<<2)|mode */
            if (XR_TYPE_IS_ARRAY(result_type) && result_type->container.element_type) {
                uint8_t tid = xr_type_to_tid(result_type->container.element_type);
                v->aux_int = (int64_t) (tid << 2);
            }
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && arg_count == 2) {
            XiValue *count = xi_lower_expr(l, arguments[0]);
            XiValue *fill = xi_lower_expr(l, arguments[1]);
            if (!count || !fill)
                return NULL;
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 2);
            if (!v)
                return NULL;
            v->args[0] = count;
            v->args[1] = fill;
            v->aux = (void *) "array_filled_new";
            v->aux_int = xi_array_cfield_from_type(result_type);
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Set") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            /* Encode elem_tid from explicit type param: B = (tid<<2)|flags */
            if (result_type->kind == XR_KIND_SET && result_type->container.element_type) {
                uint8_t tid = xr_type_to_tid(result_type->container.element_type);
                v->aux_int = (int64_t) ((tid & 0x1F) << 2);
            } else {
                v->aux_int = 0;
            }
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "WeakSet") == 0 && arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            v->aux_int = 0x02; /* weak flag in B field bit 1 */
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "StringBuilder") == 0 && arg_count == 0) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 0);
            if (!v)
                return NULL;
            v->aux = (void *) "StringBuilder";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        /* Exception: no special handling needed — it is a regular class with a
         * primitive constructor registered in core->panicInfoClass. Falls through
         * to the generic class-instantiation path below. */
        /* new Bytes() / new Bytes(n) / new Bytes(n, fill) */
        if (strcmp(cname, "Bytes") == 0 && arg_count <= 2) {
            int n = (int) arg_count;
            XiValue *arg_vals[2];
            for (int i = 0; i < n; i++)
                arg_vals[i] = xi_lower_expr(l, arguments[i]);
            XiValue *v =
                xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, (uint16_t) n);
            if (!v)
                return NULL;
            for (int i = 0; i < n; i++)
                v->args[i] = arg_vals[i];
            v->aux = (void *) "Bytes";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        /* new Channel() / new Channel(bufferSize) */
        if (strcmp(cname, "Channel") == 0 && arg_count <= 1) {
            XiValue *buf_size = arg_count == 1 ? xi_lower_expr(l, arguments[0]) : NULL;
            uint8_t elem_tid = 0;
            if (result_type && result_type->kind == XR_KIND_CHANNEL &&
                result_type->container.element_type) {
                elem_tid = xr_type_to_tid(result_type->container.element_type);
                if (!buf_size)
                    buf_size = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            }
            uint16_t nch = buf_size ? 1 : 0;
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_NEW, result_type, nch);
            if (!v)
                return NULL;
            if (buf_size)
                v->args[0] = buf_size;
            v->aux_int = elem_tid;
            v->line = (uint32_t) node->line;
            return v;
        }
    }

    /* Generic class: resolve class name and invoke constructor */
    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    for (int i = 0; i < arg_count; i++) {
        XiValue *arg = xi_lower_expr(l, arguments[i]);
        if (!arg)
            return NULL;
        if (!xi_lower_arg_list_push(l, &args, arg, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
    }
    XiValue **arg_vals = args.items;
    int n = args.count;

    XiValue *cls = NULL;
    int var_id = xi_lower_var_find(l, 0, cname);
    if (var_id >= 0) {
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiTopBinding b;
            b.slot = l->shared_map[var_id];
            b.name = l->vars[var_id].name;
            b.type = l->vars[var_id].type;
            cls = xi_lower_emit_top_load(l, b, l->type_any);
        } else {
            cls = xi_lower_braun_read(l, var_id, l->cur_block);
        }
    }
    if (!cls) {
        XiTopBinding tb = xi_lower_find_top_binding(l, 0, cname);
        if (xi_top_binding_valid(tb))
            cls = xi_lower_emit_top_load(l, tb, l->type_any);
    }
    if (!cls) {
        struct XrType *upval_type = NULL;
        int upval_idx = xi_lower_resolve_upvalue(l, 0, cname, &upval_type);
        if (upval_idx >= 0) {
            cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, l->type_any, 0);
            if (cls)
                cls->aux_int = upval_idx;
        }
    }
    /* Built-in unified-class names (Exception, Range, DateTime, etc.)
     * are populated into the VM builtins array by the prelude module
     * loader at fixed XR_GLOBAL_VAR_* indices. Resolve them via
     * XI_GET_BUILTIN before falling back to null. */
    if (!cls && cname) {
        static const struct {
            const char *name;
            int index;
        } builtin_class_globals[] = {
            {"PanicInfo", XR_GLOBAL_VAR_PANIC_INFO},
            {"Range", XR_GLOBAL_VAR_RANGE},
            {"DateTime", XR_GLOBAL_VAR_DATETIME},
        };
        for (size_t bi = 0; bi < sizeof(builtin_class_globals) / sizeof(builtin_class_globals[0]);
             bi++) {
            if (strcmp(cname, builtin_class_globals[bi].name) == 0) {
                struct XrType *cls_type = xr_type_new_class(NULL, cname);
                cls = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, cls_type, 0);
                if (cls) {
                    cls->aux_int = builtin_class_globals[bi].index;
                    cls->aux = (void *) builtin_class_globals[bi].name;
                }
                break;
            }
        }
    }
    if (!cls) {
        cls = xi_const_null(l->func, l->cur_block, l->type_null);
    }

    /* Zero-arg struct with compile-time layout → XI_STRUCT_NEW.
     * The emitter decides stack vs heap via struct_can_stack_alloc. */
    if (arg_count == 0 && module_name == NULL && l->analyzer) {
        XrStructLayout *slayout = xi_lower_lookup_struct_layout(l, cname);
        if (slayout) {
            XiValue *inst = xi_value_new(l->func, l->cur_block, XI_STRUCT_NEW, result_type, 1);
            if (!inst)
                return NULL;
            inst->args[0] = cls;
            inst->aux = (void *) slayout;
            inst->flags |= XI_FLAG_SIDE_EFFECT;
            inst->line = (uint32_t) node->line;
            return inst;
        }
    }

    uint16_t nargs = (uint16_t) (n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
    if (!call)
        return NULL;
    call->args[0] = cls;
    for (int i = 0; i < n; i++)
        call->args[i + 1] = arg_vals[i];
    call->aux = (void *) "constructor";
    call->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    call->line = (uint32_t) node->line;
    return call;
}

static XiValue *lower_new_expr(XiLower *l, AstNode *node) {
    NewExprNode *ne = &node->as.new_expr;
    return lower_construct(l, node, xi_lower_node_type(l, node), ne->module_name, ne->class_name,
                           ne->arguments, ne->arg_count);
}

/* True if the named class is `Exception` or derives from it. Exception is a
 * built-in primitive class; constructing it (or a subclass) must go through the
 * new-expr construction path, not the normal class-binding call path. */
static bool lower_class_is_exception_kind(XiLower *l, const char *name) {
    if (!name)
        return false;
    if (strcmp(name, "PanicInfo") == 0)
        return true;
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym || !l->analyzer)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    XrClassInfo *info = links ? links->class_info : NULL;
    for (XrClassInfo *c = info; c; c = c->base) {
        if (c->base_name && strcmp(c->base_name, "PanicInfo") == 0)
            return true;
        if (c->name && strcmp(c->name, "PanicInfo") == 0)
            return true;
    }
    return false;
}

/* True if the named class declares type parameters (generic). Generic classes
 * must construct through the new-expr path (monomorphization-aware), which the
 * normal class-binding call path does not handle for AOT. */
static bool lower_class_is_generic(XiLower *l, const char *name) {
    if (!name || !l->analyzer)
        return false;
    XaSymbol *sym = xi_lower_lookup_class_symbol(l, name);
    if (!sym)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(l->analyzer, sym);
    return links && xa_symbol_links_get_type_param_count(links) > 0;
}

/* A bare `T(args)` call constructs through the new-expr construction path when
 * the normal class-binding call path cannot handle it: Exception (built-in
 * primitive class) and its subclasses, and generic classes (monomorphization).
 * Plain non-generic user classes (top-level and nested) construct correctly via
 * the normal call lowering and are left alone (preserves cross-module dispatch).
 * Returns NULL when the normal call path should be used. */
static XiValue *lower_try_construct_call(XiLower *l, AstNode *node, CallExprNode *call) {
    if (!call->callee || call->callee->type != AST_VARIABLE)
        return NULL;
    const char *name = call->callee->as.variable.name;
    if (!name)
        return NULL;
    if (!lower_class_is_exception_kind(l, name) && !lower_class_is_generic(l, name))
        return NULL;
    return lower_construct(l, node, xi_lower_node_type(l, node), NULL, name, call->arguments,
                           call->arg_count);
}

static int xi_func_append_synthetic_capture(XiFunc *func, XiValue *value, struct XrType *type);
static bool lower_parallel_expr_bind_locals(XiLower *child_l, XrParallelLocalBinding *locals,
                                            int local_count, XiValue **parent_sources,
                                            XiValue *worker, int line);

static bool parallel_expr_has_local_initializers(XrParallelLocalBinding *locals, int local_count);
static XiFunc *lower_parallel_reduce_combine_func(XiLower *parent, AstNode *combine,
                                                  struct XrType *acc_type,
                                                  XiNativeCallbackKind callback_kind, int line);

static XiValue *lower_parallel_reduce_item_value_once(XiLower *child_l, ParallelReduceExprNode *pr,
                                                      struct XrType *acc_type, int line) {
    if (!child_l || !child_l->cur_block || !pr || !pr->body || pr->body->type != AST_BLOCK)
        return NULL;
    BlockNode *blk = &pr->body->as.block;
    if (blk->count <= 0)
        return NULL;
    for (int si = 0; si < blk->count && child_l->cur_block; si++) {
        AstNode *stmt = blk->statements[si];
        bool is_last = (si == blk->count - 1);
        if (is_last && stmt && stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
            XiValue *ret = xi_lower_expr(child_l, stmt->as.expr_stmt);
            ret = xi_lower_checktype_for_type(child_l, stmt->as.expr_stmt, ret,
                                              acc_type ? acc_type : child_l->type_int);
            return ret;
        }
        xi_lower_stmt(child_l, stmt);
    }
    (void) line;
    return NULL;
}

static XiValue *lower_parallel_reduce_make_static_combine_closure(XiLower *child_l,
                                                                  XiFunc *combine_func,
                                                                  uint16_t combine_child_idx,
                                                                  int line) {
    if (!child_l || !child_l->func || !child_l->cur_block || !combine_func)
        return NULL;
    uint16_t ncap = combine_func->ncaptures;
    XiValue *closure =
        xi_value_new(child_l->func, child_l->cur_block, XI_CLOSURE_NEW, child_l->type_any, ncap);
    if (!closure)
        return NULL;
    closure->aux = (void *) combine_func;
    closure->aux_int = combine_child_idx;
    closure->line = (uint32_t) line;
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &combine_func->captures[ci];
        closure->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value) ? cap->value : NULL;
    }
    return closure;
}

static XiValue *lower_parallel_reduce_emit_combine_call(XiLower *child_l, XiValue *combine_closure,
                                                        struct XrType *acc_type, XiValue *acc,
                                                        XiValue *item, int line) {
    if (!child_l || !child_l->cur_block || !combine_closure || !acc || !item)
        return NULL;
    XiValue *call = xi_value_new(child_l->func, child_l->cur_block, XI_CALL,
                                 acc_type ? acc_type : child_l->type_int, 3);
    if (!call)
        return NULL;
    call->args[0] = combine_closure;
    call->args[1] = acc;
    call->args[2] = item;
    call->flags |= XI_FLAG_SIDE_EFFECT;
    call->line = (uint32_t) line;
    return call;
}

static bool lower_parallel_reduce_item_range_loop(XiLower *child_l, ParallelReduceExprNode *pr,
                                                  int item_var, XiValue *end,
                                                  struct XrType *acc_type, XiFunc *combine_func,
                                                  uint16_t combine_child_idx, int line,
                                                  int *out_acc_var) {
    if (!child_l || !child_l->func || !child_l->cur_block || !pr || item_var < 0 || !end ||
        !combine_func)
        return false;

    XiValue *combine_closure = lower_parallel_reduce_make_static_combine_closure(
        child_l, combine_func, combine_child_idx, line);
    if (!combine_closure)
        return false;

    XiValue *first = lower_parallel_reduce_item_value_once(child_l, pr, acc_type, line);
    if (!first || !child_l->cur_block || child_l->had_error)
        return false;

    int acc_var = xi_lower_var_create(child_l, 0, "$parallel_reduce_local_acc",
                                      acc_type ? acc_type : child_l->type_int);
    if (acc_var < 0)
        return false;
    if (out_acc_var)
        *out_acc_var = acc_var;
    xi_lower_braun_write(child_l, acc_var, child_l->cur_block, first);

    XiValue *first_item = xi_lower_braun_read(child_l, item_var, child_l->cur_block);
    XiValue *one = xi_const_int(child_l->func, child_l->cur_block, 1, child_l->type_int);
    XiValue *next_item = (first_item && one) ? xi_binary(child_l->func, child_l->cur_block, XI_ADD,
                                                         child_l->type_int, first_item, one)
                                             : NULL;
    if (!next_item)
        return false;
    next_item->line = (uint32_t) line;
    xi_lower_braun_write(child_l, item_var, child_l->cur_block, next_item);

    XiBlock *cond_blk = xi_block_new(child_l->func);
    XiBlock *body_blk = xi_block_new(child_l->func);
    XiBlock *incr_blk = xi_block_new(child_l->func);
    XiBlock *exit_blk = xi_block_new(child_l->func);
    if (!cond_blk || !body_blk || !incr_blk || !exit_blk)
        return false;

    xi_block_set_jump(child_l->cur_block, cond_blk);

    child_l->cur_block = cond_blk;
    XiValue *cur_item = xi_lower_braun_read(child_l, item_var, child_l->cur_block);
    if (!cur_item)
        return false;
    XiValue *cond =
        xi_binary(child_l->func, child_l->cur_block, XI_LT, child_l->type_bool, cur_item, end);
    if (!cond)
        return false;
    cond->line = (uint32_t) line;
    xi_block_set_if(child_l->cur_block, cond, body_blk, exit_blk);

    xi_lower_braun_seal(child_l, body_blk);
    child_l->cur_block = body_blk;
    XiValue *item_result = lower_parallel_reduce_item_value_once(child_l, pr, acc_type, line);
    if (!item_result || !child_l->cur_block || child_l->had_error)
        return false;
    XiValue *acc = xi_lower_braun_read(child_l, acc_var, child_l->cur_block);
    XiValue *combined = lower_parallel_reduce_emit_combine_call(child_l, combine_closure, acc_type,
                                                                acc, item_result, line);
    if (!combined)
        return false;
    xi_lower_braun_write(child_l, acc_var, child_l->cur_block, combined);
    if (child_l->cur_block)
        xi_block_set_jump(child_l->cur_block, incr_blk);

    xi_lower_braun_seal(child_l, incr_blk);
    child_l->cur_block = incr_blk;
    if (incr_blk->npreds > 0) {
        XiValue *inc_item = xi_lower_braun_read(child_l, item_var, child_l->cur_block);
        XiValue *inc_one = xi_const_int(child_l->func, child_l->cur_block, 1, child_l->type_int);
        XiValue *after_item = (inc_item && inc_one)
                                  ? xi_binary(child_l->func, child_l->cur_block, XI_ADD,
                                              child_l->type_int, inc_item, inc_one)
                                  : NULL;
        if (!after_item)
            return false;
        after_item->line = (uint32_t) line;
        xi_lower_braun_write(child_l, item_var, child_l->cur_block, after_item);
    }
    if (child_l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(child_l->cur_block, cond_blk);

    xi_lower_braun_seal(child_l, cond_blk);
    xi_lower_braun_seal(child_l, exit_blk);
    child_l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
    return true;
}

static XiFunc *lower_parallel_reduce_body_func(XiLower *parent, ParallelReduceExprNode *pr,
                                               struct XrType *acc_type,
                                               XiNativeCallbackKind callback_kind,
                                               XiNativeCallbackKind combine_callback_kind,
                                               XiValue **local_sources, int line) {
    char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "%s$parallel_reduce_body_%d",
             parent->func && parent->func->name ? parent->func->name : "<anon>",
             parent->synthetic_id++);

    XiLower child_l;
    xi_lower_init(&child_l, parent->analyzer, parent->isolate);
    child_l.parent = parent;
    child_l.repl_mode = parent->repl_mode;

    child_l.func = xi_func_new(name_buf, acc_type ? acc_type : child_l.type_int);
    if (!child_l.func) {
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->native_callback_kind = callback_kind;
    child_l.func->parent_func = parent->func;
    child_l.func->analyzer = parent->analyzer;
    bool local_init_range_body =
        !pr->range_body && parallel_expr_has_local_initializers(pr->locals, pr->local_count);
    bool worker_range_body = pr->range_body || local_init_range_body;
    uint16_t param_count = worker_range_body ? 3 : 2;
    child_l.func->nparams = param_count;
    child_l.func->min_params = param_count;
    child_l.func->entry_type = 0;
    child_l.func->params = (XiValue **) xr_calloc(param_count, sizeof(XiValue *));
    if (!child_l.func->params) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiBlock *entry = xi_block_new(child_l.func);
    if (!entry) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    entry->sealed = true;
    child_l.cur_block = entry;

    XiValue *item = xi_param(child_l.func, entry, 0, child_l.type_int);
    XiValue *end = worker_range_body ? xi_param(child_l.func, entry, 1, child_l.type_int) : NULL;
    XiValue *worker = xi_param(child_l.func, entry, worker_range_body ? 2 : 1, child_l.type_int);
    if (!item || (worker_range_body && !end) || !worker) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->params[0] = item;
    if (worker_range_body)
        child_l.func->params[1] = end;
    child_l.func->params[worker_range_body ? 2 : 1] = worker;

    int item_var =
        xi_lower_var_create(&child_l, pr->item_symbol_id, pr->item_name, child_l.type_int);
    if (item_var < 0) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    xi_lower_braun_write(&child_l, item_var, entry, item);
    if (worker_range_body && pr->end_name) {
        int end_var =
            xi_lower_var_create(&child_l, pr->end_symbol_id, pr->end_name, child_l.type_int);
        if (end_var < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        xi_lower_braun_write(&child_l, end_var, entry, end);
    }
    if (pr->worker_name) {
        int worker_var =
            xi_lower_var_create(&child_l, pr->worker_symbol_id, pr->worker_name, child_l.type_int);
        if (worker_var < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        xi_lower_braun_write(&child_l, worker_var, entry, worker);
    }
    if (!lower_parallel_expr_bind_locals(&child_l, pr->locals, pr->local_count, local_sources,
                                         worker, line)) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiFunc *local_combine_func = NULL;
    uint16_t local_combine_child_idx = 0;
    if (local_init_range_body) {
        local_combine_func = lower_parallel_reduce_combine_func(&child_l, pr->combine, acc_type,
                                                                combine_callback_kind, line);
        if (!local_combine_func) {
            child_l.had_error = true;
        } else {
            uint16_t before_children = child_l.func->nchildren;
            xi_lower_func_add_child(child_l.func, local_combine_func);
            if (child_l.func->nchildren == before_children) {
                xi_func_free(local_combine_func);
                local_combine_func = NULL;
                child_l.had_error = true;
            } else {
                local_combine_child_idx = (uint16_t) (child_l.func->nchildren - 1);
            }
        }
    }

    xi_lower_defer_scope_push(&child_l);
    if (local_init_range_body) {
        int local_acc_var = -1;
        if (child_l.had_error || !lower_parallel_reduce_item_range_loop(
                                     &child_l, pr, item_var, end, acc_type, local_combine_func,
                                     local_combine_child_idx, line, &local_acc_var)) {
            child_l.had_error = true;
        } else if (child_l.cur_block && local_acc_var >= 0) {
            XiValue *ret = xi_lower_braun_read(&child_l, local_acc_var, child_l.cur_block);
            xi_lower_defer_scope_pop_normal(&child_l, line);
            xi_block_set_return(child_l.cur_block, ret);
            child_l.cur_block = NULL;
            goto parallel_reduce_body_done;
        }
    } else if (pr->body && pr->body->type == AST_BLOCK) {
        XiValue *ret = lower_parallel_reduce_item_value_once(&child_l, pr, acc_type, line);
        if (ret && child_l.cur_block && !child_l.had_error) {
            xi_lower_defer_scope_pop_normal(&child_l, line);
            xi_block_set_return(child_l.cur_block, ret);
            child_l.cur_block = NULL;
            goto parallel_reduce_body_done;
        }
        child_l.had_error = true;
    } else {
        child_l.had_error = true;
    }

parallel_reduce_body_done:
    if (child_l.cur_block)
        xi_lower_defer_scope_pop_normal(&child_l, line);

    XiFunc *result = child_l.had_error ? NULL : child_l.func;
    if (result) {
        result->stage = XI_STAGE_RAW;
        result->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
        xi_lower_capture_source_vars(&child_l);
    } else {
        xi_func_free(child_l.func);
    }
    xi_lower_cleanup(&child_l);
    return result;
}

static XiFunc *lower_parallel_collect_body_func(XiLower *parent, ParallelCollectExprNode *pc,
                                                struct XrType *elem_type,
                                                XiNativeCallbackKind callback_kind,
                                                XiValue **local_sources, int line) {
    char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "%s$parallel_collect_body_%d",
             parent->func && parent->func->name ? parent->func->name : "<anon>",
             parent->synthetic_id++);

    XiLower child_l;
    xi_lower_init(&child_l, parent->analyzer, parent->isolate);
    child_l.parent = parent;
    child_l.repl_mode = parent->repl_mode;

    child_l.func = xi_func_new(name_buf, elem_type ? elem_type : child_l.type_any);
    if (!child_l.func) {
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->native_callback_kind = callback_kind;
    child_l.func->parent_func = parent->func;
    child_l.func->analyzer = parent->analyzer;
    child_l.func->nparams = 2;
    child_l.func->min_params = 2;
    child_l.func->entry_type = 0;
    child_l.func->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    if (!child_l.func->params) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiBlock *entry = xi_block_new(child_l.func);
    if (!entry) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    entry->sealed = true;
    child_l.cur_block = entry;

    XiValue *item = xi_param(child_l.func, entry, 0, child_l.type_int);
    XiValue *worker = xi_param(child_l.func, entry, 1, child_l.type_int);
    if (!item || !worker) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->params[0] = item;
    child_l.func->params[1] = worker;

    int item_var =
        xi_lower_var_create(&child_l, pc->item_symbol_id, pc->item_name, child_l.type_int);
    if (item_var < 0) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    xi_lower_braun_write(&child_l, item_var, entry, item);
    if (pc->worker_name) {
        int worker_var =
            xi_lower_var_create(&child_l, pc->worker_symbol_id, pc->worker_name, child_l.type_int);
        if (worker_var < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        xi_lower_braun_write(&child_l, worker_var, entry, worker);
    }
    if (!lower_parallel_expr_bind_locals(&child_l, pc->locals, pc->local_count, local_sources,
                                         worker, line)) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    xi_lower_defer_scope_push(&child_l);
    if (pc->body && pc->body->type == AST_BLOCK) {
        BlockNode *blk = &pc->body->as.block;
        for (int si = 0; si < blk->count && child_l.cur_block; si++) {
            AstNode *stmt = blk->statements[si];
            bool is_last = (si == blk->count - 1);
            if (is_last && stmt && stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
                XiValue *ret = xi_lower_expr(&child_l, stmt->as.expr_stmt);
                ret = xi_lower_checktype_for_type(&child_l, stmt->as.expr_stmt, ret,
                                                  elem_type ? elem_type : child_l.type_any);
                xi_lower_defer_scope_pop_normal(&child_l, line);
                xi_block_set_return(child_l.cur_block, ret);
                child_l.cur_block = NULL;
                goto parallel_collect_body_done;
            }
            xi_lower_stmt(&child_l, stmt);
        }
    }
    child_l.had_error = true;

parallel_collect_body_done:
    if (child_l.cur_block)
        xi_lower_defer_scope_pop_normal(&child_l, line);

    XiFunc *result = child_l.had_error ? NULL : child_l.func;
    if (result) {
        result->stage = XI_STAGE_RAW;
        result->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
        xi_lower_capture_source_vars(&child_l);
    } else {
        xi_func_free(child_l.func);
    }
    xi_lower_cleanup(&child_l);
    return result;
}

static AstNode *parallel_collect_last_value_expr(AstNode *body) {
    body = expr_unwrap_grouping(body);
    if (!body || body->type != AST_BLOCK)
        return NULL;
    BlockNode *blk = &body->as.block;
    if (blk->count <= 0)
        return NULL;
    AstNode *last = blk->statements[blk->count - 1];
    if (!last || last->type != AST_EXPR_STMT)
        return NULL;
    return expr_unwrap_grouping(last->as.expr_stmt);
}

static TupleLiteralNode *parallel_collect_into_tuple(ParallelCollectExprNode *pc) {
    AstNode *into = pc ? expr_unwrap_grouping(pc->into) : NULL;
    if (!into || into->type != AST_TUPLE_LITERAL || into->as.tuple_literal.count <= 1)
        return NULL;
    return &into->as.tuple_literal;
}

static TupleLiteralNode *parallel_collect_body_tuple(ParallelCollectExprNode *pc) {
    AstNode *last = parallel_collect_last_value_expr(pc ? pc->body : NULL);
    if (!last || last->type != AST_TUPLE_LITERAL)
        return NULL;
    return &last->as.tuple_literal;
}

static int xi_func_append_synthetic_capture(XiFunc *func, XiValue *value, struct XrType *type) {
    if (!func || func->ncaptures >= XI_MAX_CAPTURES)
        return -1;
    int idx = func->ncaptures++;
    XiCapture *cap = &func->captures[idx];
    memset(cap, 0, sizeof(*cap));
    cap->source = XI_CAPTURE_SRC_REG;
    cap->index = 0;
    cap->name = NULL;
    cap->type = type ? type : (value ? value->type : NULL);
    cap->value = value;
    cap->cell_index = -1;
    cap->env_offset = -1;
    cap->needs_cell = false;
    cap->is_reassigned = false;
    cap->is_shared = false;
    return idx;
}

static bool lower_parallel_expr_bind_locals(XiLower *child_l, XrParallelLocalBinding *locals,
                                            int local_count, XiValue **parent_sources,
                                            XiValue *worker, int line) {
    if (!child_l || !locals || local_count <= 0)
        return true;
    for (int i = 0; i < local_count; i++) {
        if (locals[i].is_initializer) {
            XiValue *init = xi_lower_expr(child_l, locals[i].source);
            if (!init)
                return false;
            struct XrType *type = init->type ? init->type : child_l->type_any;
            int var = xi_lower_var_create(child_l, locals[i].symbol_id, locals[i].name, type);
            if (var < 0)
                return false;
            xi_lower_braun_write(child_l, var, child_l->cur_block, init);
            continue;
        }
        XiValue *source = parent_sources ? parent_sources[i] : NULL;
        struct XrType *source_type = source ? source->type : child_l->type_any;
        struct XrType *elem_type = xi_get_container_elem_type(source_type);
        if (!elem_type)
            elem_type = child_l->type_any;
        int cap = xi_func_append_synthetic_capture(child_l->func, source, source_type);
        if (cap < 0)
            return false;
        XiValue *upval =
            xi_value_new(child_l->func, child_l->cur_block, XI_LOAD_UPVAL, source_type, 0);
        if (!upval)
            return false;
        upval->aux_int = cap;
        upval->line = (uint32_t) line;

        XiValue *lane = xi_value_new(child_l->func, child_l->cur_block, XI_INDEX_GET, elem_type, 2);
        if (!lane)
            return false;
        lane->args[0] = upval;
        lane->args[1] = worker;
        lane->aux_int = 1;
        lane->line = (uint32_t) line;

        int var = xi_lower_var_create(child_l, locals[i].symbol_id, locals[i].name, elem_type);
        if (var < 0)
            return false;
        xi_lower_braun_write(child_l, var, child_l->cur_block, lane);
    }
    return true;
}

static bool parallel_expr_has_local_initializers(XrParallelLocalBinding *locals, int local_count) {
    if (!locals || local_count <= 0)
        return false;
    for (int i = 0; i < local_count; i++) {
        if (locals[i].is_initializer)
            return true;
    }
    return false;
}

static bool lower_parallel_collect_emit_lane_writes_once(XiLower *child_l,
                                                         ParallelCollectExprNode *pc, XiValue *item,
                                                         XiValue *start_upval, uint16_t lane_count,
                                                         XiValue **output_upvals,
                                                         struct XrType **lane_types, int line) {
    if (!child_l || !pc || !item || !start_upval || lane_count == 0 || !output_upvals)
        return false;

    TupleLiteralNode *body_tuple = parallel_collect_body_tuple(pc);
    AstNode *single_body_expr = parallel_collect_last_value_expr(pc ? pc->body : NULL);
    bool can_lower_lanes =
        pc->body && pc->body->type == AST_BLOCK &&
        ((lane_count == 1 && single_body_expr) ||
         (lane_count > 1 && body_tuple && body_tuple->count == (int) lane_count));
    if (!can_lower_lanes)
        return false;

    BlockNode *blk = &pc->body->as.block;
    for (int si = 0; si < blk->count - 1 && child_l->cur_block; si++)
        xi_lower_stmt(child_l, blk->statements[si]);
    if (!child_l->cur_block || child_l->had_error)
        return !child_l->had_error;

    XiValue *idx = xi_value_new(child_l->func, child_l->cur_block, XI_SUB, child_l->type_int, 2);
    if (!idx)
        return false;
    idx->args[0] = item;
    idx->args[1] = start_upval;
    idx->line = (uint32_t) line;

    for (uint16_t i = 0; i < lane_count && child_l->cur_block && !child_l->had_error; i++) {
        AstNode *expr = lane_count == 1 ? single_body_expr
                                        : (body_tuple->elements ? body_tuple->elements[i] : NULL);
        XiValue *val = xi_lower_expr(child_l, expr);
        val = xi_lower_checktype_for_type(child_l, expr, val,
                                          lane_types ? lane_types[i] : child_l->type_any);
        if (!val)
            return false;
        uint16_t narrow_op = xi_narrow_op_for_elem(lane_types ? lane_types[i] : NULL);
        if (narrow_op) {
            XiValue *n = xi_value_new(child_l->func, child_l->cur_block, narrow_op, val->type, 1);
            if (!n)
                return false;
            n->args[0] = val;
            n->line = (uint32_t) (expr ? expr->line : line);
            val = n;
        }

        XiValue *set =
            xi_value_new(child_l->func, child_l->cur_block, XI_INDEX_SET, child_l->type_unit, 3);
        if (!set)
            return false;
        set->args[0] = output_upvals[i];
        set->args[1] = idx;
        set->args[2] = val;
        set->aux_int = 1; /* result lane was resized to exact count before dispatch. */
        set->flags |= XI_FLAG_SIDE_EFFECT;
        set->line = (uint32_t) (expr ? expr->line : line);
    }
    return !child_l->had_error;
}

static bool lower_parallel_collect_item_loop(XiLower *child_l, ParallelCollectExprNode *pc,
                                             int item_var, XiValue *end, XiValue *start_upval,
                                             uint16_t lane_count, XiValue **output_upvals,
                                             struct XrType **lane_types, int line) {
    if (!child_l || !child_l->func || !child_l->cur_block || !pc || item_var < 0 || !end)
        return false;

    XiBlock *cond_blk = xi_block_new(child_l->func);
    XiBlock *body_blk = xi_block_new(child_l->func);
    XiBlock *incr_blk = xi_block_new(child_l->func);
    XiBlock *exit_blk = xi_block_new(child_l->func);
    if (!cond_blk || !body_blk || !incr_blk || !exit_blk)
        return false;

    xi_block_set_jump(child_l->cur_block, cond_blk);

    child_l->cur_block = cond_blk;
    XiValue *cur_item = xi_lower_braun_read(child_l, item_var, child_l->cur_block);
    if (!cur_item)
        return false;
    XiValue *cond =
        xi_binary(child_l->func, child_l->cur_block, XI_LT, child_l->type_bool, cur_item, end);
    if (!cond)
        return false;
    cond->line = (uint32_t) line;
    xi_block_set_if(child_l->cur_block, cond, body_blk, exit_blk);

    xi_lower_braun_seal(child_l, body_blk);

    child_l->cur_block = body_blk;
    XiValue *body_item = xi_lower_braun_read(child_l, item_var, child_l->cur_block);
    if (!body_item)
        return false;
    if (!lower_parallel_collect_emit_lane_writes_once(child_l, pc, body_item, start_upval,
                                                      lane_count, output_upvals, lane_types, line))
        return false;
    if (child_l->cur_block)
        xi_block_set_jump(child_l->cur_block, incr_blk);

    xi_lower_braun_seal(child_l, incr_blk);

    child_l->cur_block = incr_blk;
    if (incr_blk->npreds > 0) {
        XiValue *inc_item = xi_lower_braun_read(child_l, item_var, child_l->cur_block);
        XiValue *one = xi_const_int(child_l->func, child_l->cur_block, 1, child_l->type_int);
        XiValue *next_item = (inc_item && one) ? xi_binary(child_l->func, child_l->cur_block,
                                                           XI_ADD, child_l->type_int, inc_item, one)
                                               : NULL;
        if (!next_item)
            return false;
        next_item->line = (uint32_t) line;
        xi_lower_braun_write(child_l, item_var, child_l->cur_block, next_item);
    }
    if (child_l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(child_l->cur_block, cond_blk);

    xi_lower_braun_seal(child_l, cond_blk);
    xi_lower_braun_seal(child_l, exit_blk);
    child_l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
    return true;
}

static XiFunc *lower_parallel_collect_multi_body_func(XiLower *parent, ParallelCollectExprNode *pc,
                                                      uint16_t lane_count, XiValue **outputs,
                                                      struct XrType **lane_types,
                                                      XiValue *start_value, XiValue **local_sources,
                                                      int line) {
    bool worker_range_body =
        (pc && pc->final_body) ||
        parallel_expr_has_local_initializers(pc ? pc->locals : NULL, pc ? pc->local_count : 0);
    char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "%s$parallel_collect_lanes_%d",
             parent->func && parent->func->name ? parent->func->name : "<anon>",
             parent->synthetic_id++);

    XiLower child_l;
    xi_lower_init(&child_l, parent->analyzer, parent->isolate);
    child_l.parent = parent;
    child_l.repl_mode = parent->repl_mode;

    child_l.func = xi_func_new(name_buf, child_l.type_unit);
    if (!child_l.func) {
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->native_callback_kind =
        worker_range_body ? XI_NATIVE_CALLBACK_PAR_RANGE_I64 : XI_NATIVE_CALLBACK_PAR_FOR_I64;
    child_l.func->parent_func = parent->func;
    child_l.func->analyzer = parent->analyzer;
    uint16_t param_count = worker_range_body ? 3 : 2;
    child_l.func->nparams = param_count;
    child_l.func->min_params = param_count;
    child_l.func->entry_type = 0;
    child_l.func->params = (XiValue **) xr_calloc(param_count, sizeof(XiValue *));
    if (!child_l.func->params) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiBlock *entry = xi_block_new(child_l.func);
    if (!entry) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    entry->sealed = true;
    child_l.cur_block = entry;

    XiValue *item = xi_param(child_l.func, entry, 0, child_l.type_int);
    XiValue *end = NULL;
    uint16_t worker_param_index = 1;
    if (worker_range_body) {
        end = xi_param(child_l.func, entry, 1, child_l.type_int);
        if (!end) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        child_l.func->params[1] = end;
        worker_param_index = 2;
    }
    XiValue *worker = xi_param(child_l.func, entry, worker_param_index, child_l.type_int);
    if (!item || !worker) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->params[0] = item;
    child_l.func->params[worker_param_index] = worker;

    int item_var =
        xi_lower_var_create(&child_l, pc->item_symbol_id, pc->item_name, child_l.type_int);
    if (item_var < 0) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    xi_lower_braun_write(&child_l, item_var, entry, item);
    if (pc->worker_name) {
        int worker_var =
            xi_lower_var_create(&child_l, pc->worker_symbol_id, pc->worker_name, child_l.type_int);
        if (worker_var < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        xi_lower_braun_write(&child_l, worker_var, entry, worker);
    }
    if (!lower_parallel_expr_bind_locals(&child_l, pc->locals, pc->local_count, local_sources,
                                         worker, line)) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiValue *output_upvals[16];
    if (lane_count > 16) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    for (uint16_t i = 0; i < lane_count; i++) {
        int cap = xi_func_append_synthetic_capture(child_l.func, outputs[i],
                                                   outputs[i] ? outputs[i]->type : NULL);
        if (cap < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        output_upvals[i] =
            xi_value_new(child_l.func, child_l.cur_block, XI_LOAD_UPVAL,
                         outputs[i] && outputs[i]->type ? outputs[i]->type : child_l.type_any, 0);
        if (!output_upvals[i]) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        output_upvals[i]->aux_int = cap;
        output_upvals[i]->line = (uint32_t) line;
    }
    int start_cap = xi_func_append_synthetic_capture(child_l.func, start_value, child_l.type_int);
    if (start_cap < 0) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    XiValue *start_upval =
        xi_value_new(child_l.func, child_l.cur_block, XI_LOAD_UPVAL, child_l.type_int, 0);
    if (!start_upval) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    start_upval->aux_int = start_cap;
    start_upval->line = (uint32_t) line;

    xi_lower_defer_scope_push(&child_l);
    bool ok = worker_range_body
                  ? lower_parallel_collect_item_loop(&child_l, pc, item_var, end, start_upval,
                                                     lane_count, output_upvals, lane_types, line)
                  : lower_parallel_collect_emit_lane_writes_once(&child_l, pc, item, start_upval,
                                                                 lane_count, output_upvals,
                                                                 lane_types, line);
    if (!ok)
        child_l.had_error = true;
    if (pc && pc->final_body && child_l.cur_block && !child_l.had_error)
        xi_lower_stmt(&child_l, pc->final_body);
    xi_lower_defer_scope_pop_normal(&child_l, line);

    if (child_l.cur_block)
        xi_block_set_return(child_l.cur_block, NULL);

    XiFunc *result = child_l.had_error ? NULL : child_l.func;
    if (result) {
        result->stage = XI_STAGE_RAW;
        result->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
        xi_lower_capture_source_vars(&child_l);
    } else {
        xi_func_free(child_l.func);
    }
    xi_lower_cleanup(&child_l);
    return result;
}

static XiFunc *lower_parallel_reduce_combine_func(XiLower *parent, AstNode *combine,
                                                  struct XrType *acc_type,
                                                  XiNativeCallbackKind callback_kind, int line) {
    AstNode *fn_node = expr_unwrap_grouping(combine);
    if (!fn_node || (fn_node->type != AST_FUNCTION_EXPR && fn_node->type != AST_FUNCTION_DECL)) {
        parent->had_error = true;
        return NULL;
    }
    FunctionDeclNode *fn_decl = &fn_node->as.function_expr;
    if (fn_decl->param_count != 2 || !fn_decl->params || !fn_decl->params[0] ||
        !fn_decl->params[1]) {
        parent->had_error = true;
        return NULL;
    }

    char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "%s$parallel_reduce_combine_%d",
             parent->func && parent->func->name ? parent->func->name : "<anon>",
             parent->synthetic_id++);

    XiLower child_l;
    xi_lower_init(&child_l, parent->analyzer, parent->isolate);
    child_l.parent = parent;
    child_l.repl_mode = parent->repl_mode;

    child_l.func = xi_func_new(name_buf, acc_type ? acc_type : child_l.type_int);
    if (!child_l.func) {
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    child_l.func->native_callback_kind = callback_kind;
    child_l.func->parent_func = parent->func;
    child_l.func->analyzer = parent->analyzer;
    child_l.func->nparams = 2;
    child_l.func->min_params = 2;
    child_l.func->entry_type = 0;
    child_l.func->params = (XiValue **) xr_calloc(2, sizeof(XiValue *));
    if (!child_l.func->params) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }

    XiBlock *entry = xi_block_new(child_l.func);
    if (!entry) {
        xi_func_free(child_l.func);
        xi_lower_cleanup(&child_l);
        return NULL;
    }
    entry->sealed = true;
    child_l.cur_block = entry;

    for (uint16_t i = 0; i < 2; i++) {
        XrParamNode *p = fn_decl->params[i];
        XiValue *param = xi_param(child_l.func, entry, i, acc_type ? acc_type : child_l.type_int);
        if (!param) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        child_l.func->params[i] = param;
        int var_id = xi_lower_var_create(&child_l, p->symbol_id, p->name,
                                         acc_type ? acc_type : child_l.type_int);
        if (var_id < 0) {
            xi_func_free(child_l.func);
            xi_lower_cleanup(&child_l);
            return NULL;
        }
        xi_lower_braun_write(&child_l, var_id, entry, param);
    }

    xi_lower_defer_scope_push(&child_l);
    if (fn_decl->body)
        xi_lower_stmt(&child_l, fn_decl->body);
    xi_lower_defer_scope_pop_normal(&child_l, line);

    if (child_l.cur_block)
        child_l.had_error = true;

    XiFunc *result = child_l.had_error ? NULL : child_l.func;
    if (result) {
        result->stage = XI_STAGE_RAW;
        result->invariant_mask = xi_stage_invariants(XI_STAGE_RAW);
        xi_lower_capture_source_vars(&child_l);
    } else {
        xi_func_free(child_l.func);
        parent->had_error = true;
    }
    xi_lower_cleanup(&child_l);
    return result;
}

/* Register `fn` as a child of the current function and wrap it in a
 * XI_CLOSURE_NEW value carrying its captures. Frees `fn` and flags the
 * lowering error on failure. */
static XiValue *lower_par_child_closure(XiLower *l, XiFunc *fn, int line) {
    uint16_t before_children = l->func->nchildren;
    xi_lower_func_add_child(l->func, fn);
    if (l->func->nchildren == before_children) {
        xi_func_free(fn);
        l->had_error = true;
        return NULL;
    }
    uint16_t child_idx = (uint16_t) (l->func->nchildren - 1);
    uint16_t ncap = fn->ncaptures;
    XiValue *closure = xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, l->type_any, ncap);
    if (!closure) {
        l->had_error = true;
        return NULL;
    }
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &fn->captures[ci];
        closure->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value) ? cap->value : NULL;
    }
    closure->aux = (void *) fn;
    closure->aux_int = child_idx;
    closure->line = (uint32_t) line;
    return closure;
}

/* Arena-allocated XiParallelCollectData with the fields every collect
 * shape shares; callers override the per-shape lane/into fields. */
static XiParallelCollectData *par_collect_make_data(XiLower *l, ParallelCollectExprNode *pc,
                                                    XiValue *body_closure, bool inclusive_end) {
    XiParallelCollectData *data = (XiParallelCollectData *) xi_func_arena_alloc(
        l->func, (uint32_t) sizeof(XiParallelCollectData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    memset(data, 0, sizeof(*data));
    data->body_func = (XiFunc *) body_closure->aux;
    data->item_name = pc->item_name ? arena_strdup(l->func, pc->item_name) : NULL;
    data->worker_name = pc->worker_name ? arena_strdup(l->func, pc->worker_name) : NULL;
    data->item_symbol_id = pc->item_symbol_id;
    data->worker_symbol_id = pc->worker_symbol_id;
    data->body_child_index = (uint16_t) body_closure->aux_int;
    data->lane_count = 1;
    data->inclusive_end = inclusive_end;
    return data;
}

/* XI_PAR_COLLECT with the fixed start/end/workers/closure prefix, `extra`
 * values at args[4..], and the closure captures appended for liveness. */
typedef struct {
    struct XrType *op_type;
    XiValue *start;
    XiValue *end;
    XiValue *workers;
    XiValue *closure;
    XiValue **extra;
    uint16_t extra_count;
    XiParallelCollectData *data;
    int line;
} ParCollectOpSpec;

static XiValue *par_collect_make_op(XiLower *l, const ParCollectOpSpec *spec) {
    uint16_t ncap = ((const XiFunc *) spec->closure->aux)->ncaptures;
    uint16_t capture_arg_base = (uint16_t) (4u + spec->extra_count);
    XiValue *par = xi_value_new(l->func, l->cur_block, XI_PAR_COLLECT, spec->op_type,
                                (uint16_t) (capture_arg_base + ncap));
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = spec->start;
    par->args[1] = spec->end;
    par->args[2] = spec->workers;
    par->args[3] = spec->closure;
    for (uint16_t i = 0; i < spec->extra_count; i++)
        par->args[4 + i] = spec->extra[i];
    for (uint16_t ci = 0; ci < ncap; ci++)
        par->args[capture_arg_base + ci] = spec->closure->args[ci];
    par->aux = spec->data;
    par->aux_kind = XI_AUX_KIND_PAR_COLLECT;
    par->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND |
                  XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;
    par->line = (uint32_t) spec->line;
    return par;
}

static XiValue *lower_parallel_reduce_expr(XiLower *l, AstNode *node) {
    ParallelReduceExprNode *pr = &node->as.parallel_reduce_expr;
    AstNode *range = expr_unwrap_grouping(pr->range);
    if (!range || range->type != AST_RANGE) {
        l->had_error = true;
        return NULL;
    }

    XiValue *start = xi_lower_expr(l, range->as.range.start);
    XiValue *end = xi_lower_expr(l, range->as.range.end);
    XiValue *workers = pr->worker_count ? xi_lower_expr(l, pr->worker_count)
                                        : xi_const_int(l->func, l->cur_block, 0, l->type_int);
    XiValue *initial = xi_lower_expr(l, pr->initial);
    if (!start || !end || !workers || !initial) {
        l->had_error = true;
        return NULL;
    }
    struct XrType *acc_type = xi_lower_node_type(l, node);
    if (!acc_type || XR_TYPE_IS_UNKNOWN(acc_type))
        acc_type = initial->type ? initial->type : l->type_int;
    bool native_i64 = acc_type && XR_TYPE_IS_INT(acc_type);
    bool native_agg = !native_i64 && xi_lower_type_struct_layout(l, acc_type) != NULL;
    initial = xi_lower_checktype_for_type(l, pr->initial, initial, acc_type);

    XiNativeCallbackKind body_callback = XI_NATIVE_CALLBACK_NONE;
    XiNativeCallbackKind combine_callback = XI_NATIVE_CALLBACK_NONE;
    if (native_i64) {
        body_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY;
        combine_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE;
    } else if (native_agg) {
        body_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_BODY;
        combine_callback = XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE;
    }

    XiValue *local_sources[XI_MAX_CAPTURES];
    memset(local_sources, 0, sizeof(local_sources));
    if (pr->local_count > XI_MAX_CAPTURES) {
        l->had_error = true;
        return NULL;
    }
    for (int i = 0; i < pr->local_count; i++) {
        if (pr->locals[i].is_initializer)
            continue;
        local_sources[i] = xi_lower_expr(l, pr->locals[i].source);
        if (!local_sources[i]) {
            l->had_error = true;
            return NULL;
        }
    }

    XiFunc *combine =
        lower_parallel_reduce_combine_func(l, pr->combine, acc_type, combine_callback, node->line);
    if (!combine) {
        l->had_error = true;
        return NULL;
    }
    XiFunc *body = lower_parallel_reduce_body_func(l, pr, acc_type, body_callback, combine_callback,
                                                   local_sources, node->line);
    if (!body) {
        if (body)
            xi_func_free(body);
        xi_func_free(combine);
        l->had_error = true;
        return NULL;
    }

    XiValue *body_closure = lower_par_child_closure(l, body, node->line);
    if (!body_closure) {
        xi_func_free(combine);
        return NULL;
    }
    uint16_t body_child_idx = (uint16_t) body_closure->aux_int;
    uint16_t body_ncap = body->ncaptures;

    XiValue *combine_closure = lower_par_child_closure(l, combine, node->line);
    if (!combine_closure)
        return NULL;
    uint16_t combine_child_idx = (uint16_t) combine_closure->aux_int;
    uint16_t combine_ncap = combine->ncaptures;

    XiParallelReduceData *data = (XiParallelReduceData *) xi_func_arena_alloc(
        l->func, (uint32_t) sizeof(XiParallelReduceData));
    if (!data) {
        l->had_error = true;
        return NULL;
    }
    data->body_func = body;
    data->combine_func = combine;
    data->accumulator_type = acc_type;
    data->item_name = pr->item_name ? arena_strdup(l->func, pr->item_name) : NULL;
    data->end_name = pr->end_name ? arena_strdup(l->func, pr->end_name) : NULL;
    data->worker_name = pr->worker_name ? arena_strdup(l->func, pr->worker_name) : NULL;
    data->item_symbol_id = pr->item_symbol_id;
    data->end_symbol_id = pr->end_symbol_id;
    data->worker_symbol_id = pr->worker_symbol_id;
    data->body_child_index = body_child_idx;
    data->combine_child_index = combine_child_idx;
    data->inclusive_end = range->as.range.inclusive_end;
    data->range_body =
        pr->range_body || parallel_expr_has_local_initializers(pr->locals, pr->local_count);

    uint16_t nargs = (uint16_t) (6u + body_ncap + combine_ncap);
    XiValue *par = xi_value_new(l->func, l->cur_block, XI_PAR_REDUCE, acc_type, nargs);
    if (!par) {
        l->had_error = true;
        return NULL;
    }
    par->args[0] = start;
    par->args[1] = end;
    par->args[2] = workers;
    par->args[3] = initial;
    par->args[4] = body_closure;
    par->args[5] = combine_closure;
    for (uint16_t ci = 0; ci < body_ncap; ci++)
        par->args[6 + ci] = body_closure->args[ci];
    for (uint16_t ci = 0; ci < combine_ncap; ci++)
        par->args[6 + body_ncap + ci] = combine_closure->args[ci];
    par->aux = data;
    par->aux_kind = XI_AUX_KIND_PAR_REDUCE;
    par->line = (uint32_t) node->line;
    return par;
}

static bool xi_type_can_use_parallel_collect_scalar_callback(const struct XrType *type) {
    return type && !type->is_nullable &&
           (XR_TYPE_IS_INT(type) || XR_TYPE_IS_FLOAT(type) || XR_TYPE_IS_BOOL(type) ||
            XR_TYPE_IS_CHAR(type));
}

/* Shared lowering inputs for the parallel-collect shapes. */
typedef struct {
    ParallelCollectExprNode *pc;
    AstNode *node;
    AstNode *range;
    XiValue *start;
    XiValue *end;
    XiValue *workers;
    XiValue **local_sources;
} ParCollectLowerCtx;

/* `collect into (a, b, ...)` tuple form: every lane array is a direct
 * write target of the body. */
static XiValue *lower_parallel_collect_tuple_expr(XiLower *l, const ParCollectLowerCtx *cc,
                                                  TupleLiteralNode *into_tuple) {
    ParallelCollectExprNode *pc = cc->pc;
    if (into_tuple->count > 16) {
        l->had_error = true;
        return NULL;
    }
    TupleLiteralNode *body_tuple = parallel_collect_body_tuple(pc);
    if (!body_tuple || body_tuple->count != into_tuple->count) {
        l->had_error = true;
        return NULL;
    }

    uint16_t lane_count = (uint16_t) into_tuple->count;
    XiValue *outputs[16];
    struct XrType *lane_types[16];
    memset(outputs, 0, sizeof(outputs));
    memset(lane_types, 0, sizeof(lane_types));
    for (uint16_t i = 0; i < lane_count; i++) {
        outputs[i] = xi_lower_expr(l, into_tuple->elements ? into_tuple->elements[i] : NULL);
        if (!outputs[i] || !outputs[i]->type || !XR_TYPE_IS_ARRAY(outputs[i]->type)) {
            l->had_error = true;
            return NULL;
        }
        lane_types[i] = xi_get_container_elem_type(outputs[i]->type);
        if (!lane_types[i])
            lane_types[i] = l->type_any;
    }

    XiFunc *body = lower_parallel_collect_multi_body_func(
        l, pc, lane_count, outputs, lane_types, cc->start, cc->local_sources, cc->node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = lower_par_child_closure(l, body, cc->node->line);
    if (!closure)
        return NULL;

    XiParallelCollectData *data =
        par_collect_make_data(l, pc, closure, cc->range->as.range.inclusive_end);
    if (!data)
        return NULL;
    data->start_capture_index = lane_count;
    data->lane_count = lane_count;
    data->direct_lane_writes = true;
    data->into_result = true;

    ParCollectOpSpec spec = {
        .op_type = xr_type_new_unit(l->isolate),
        .start = cc->start,
        .end = cc->end,
        .workers = cc->workers,
        .closure = closure,
        .extra = outputs,
        .extra_count = lane_count,
        .data = data,
        .line = (int) cc->node->line,
    };
    return par_collect_make_op(l, &spec);
}

/* `collect into arr` with a statically array-typed target: the body writes
 * the single lane directly (no per-item collect store). */
static XiValue *lower_parallel_collect_into_array_expr(XiLower *l, const ParCollectLowerCtx *cc,
                                                       XiValue *into, struct XrType *elem_type) {
    ParallelCollectExprNode *pc = cc->pc;
    XiValue *outputs[1] = {into};
    struct XrType *lane_types[1] = {elem_type};
    XiFunc *body = lower_parallel_collect_multi_body_func(l, pc, 1, outputs, lane_types, cc->start,
                                                          cc->local_sources, cc->node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = lower_par_child_closure(l, body, cc->node->line);
    if (!closure)
        return NULL;

    XiParallelCollectData *data =
        par_collect_make_data(l, pc, closure, cc->range->as.range.inclusive_end);
    if (!data)
        return NULL;
    data->element_type = elem_type;
    data->start_capture_index = 1;
    data->direct_lane_writes = true;
    data->into_result = true;

    ParCollectOpSpec spec = {
        .op_type = xr_type_new_unit(l->isolate),
        .start = cc->start,
        .end = cc->end,
        .workers = cc->workers,
        .closure = closure,
        .extra = outputs,
        .extra_count = 1,
        .data = data,
        .line = (int) cc->node->line,
    };
    return par_collect_make_op(l, &spec);
}

/* Fresh-result collect whose body writes the lane directly (final body or
 * per-lane local initializers): allocate an empty result array up front and
 * let the lane loop resize + fill it. */
static XiValue *lower_parallel_collect_fresh_array_expr(XiLower *l, const ParCollectLowerCtx *cc,
                                                        struct XrType *result_type,
                                                        struct XrType *elem_type) {
    ParallelCollectExprNode *pc = cc->pc;
    XiValue *zero = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    XiValue *result_array = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
    if (!zero || !result_array) {
        l->had_error = true;
        return NULL;
    }
    result_array->args[0] = zero;
    result_array->line = (uint32_t) cc->node->line;

    XiValue *outputs[1] = {result_array};
    struct XrType *lane_types[1] = {elem_type};
    XiFunc *body = lower_parallel_collect_multi_body_func(l, pc, 1, outputs, lane_types, cc->start,
                                                          cc->local_sources, cc->node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = lower_par_child_closure(l, body, cc->node->line);
    if (!closure)
        return NULL;

    XiParallelCollectData *data =
        par_collect_make_data(l, pc, closure, cc->range->as.range.inclusive_end);
    if (!data)
        return NULL;
    data->element_type = elem_type;
    data->start_capture_index = 1;
    data->direct_lane_writes = true;

    ParCollectOpSpec spec = {
        .op_type = result_type,
        .start = cc->start,
        .end = cc->end,
        .workers = cc->workers,
        .closure = closure,
        .extra = outputs,
        .extra_count = 1,
        .data = data,
        .line = (int) cc->node->line,
    };
    return par_collect_make_op(l, &spec);
}

static XiValue *lower_parallel_collect_expr(XiLower *l, AstNode *node) {
    ParallelCollectExprNode *pc = &node->as.parallel_collect_expr;
    AstNode *range = expr_unwrap_grouping(pc->range);
    if (!range || range->type != AST_RANGE) {
        l->had_error = true;
        return NULL;
    }

    XiValue *start = xi_lower_expr(l, range->as.range.start);
    XiValue *end = xi_lower_expr(l, range->as.range.end);
    XiValue *workers = pc->worker_count ? xi_lower_expr(l, pc->worker_count)
                                        : xi_const_int(l->func, l->cur_block, 0, l->type_int);
    if (!start || !end || !workers) {
        l->had_error = true;
        return NULL;
    }

    XiValue *local_sources[XI_MAX_CAPTURES];
    memset(local_sources, 0, sizeof(local_sources));
    if (pc->local_count > XI_MAX_CAPTURES) {
        l->had_error = true;
        return NULL;
    }
    for (int i = 0; i < pc->local_count; i++) {
        if (pc->locals[i].is_initializer)
            continue;
        local_sources[i] = xi_lower_expr(l, pc->locals[i].source);
        if (!local_sources[i]) {
            l->had_error = true;
            return NULL;
        }
    }

    ParCollectLowerCtx cc = {
        .pc = pc,
        .node = node,
        .range = range,
        .start = start,
        .end = end,
        .workers = workers,
        .local_sources = local_sources,
    };
    TupleLiteralNode *into_tuple = parallel_collect_into_tuple(pc);
    if (into_tuple)
        return lower_parallel_collect_tuple_expr(l, &cc, into_tuple);

    XiValue *into = pc->into ? xi_lower_expr(l, pc->into) : NULL;
    if (pc->into && !into) {
        l->had_error = true;
        return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *elem_type = NULL;
    if (pc->into) {
        elem_type = (into && into->type && XR_TYPE_IS_ARRAY(into->type))
                        ? xi_get_container_elem_type(into->type)
                        : NULL;
        if (!result_type)
            result_type = xr_type_new_unit(l->isolate);
    } else if (!result_type || !XR_TYPE_IS_ARRAY(result_type)) {
        struct XrType *elem = xr_type_new_unknown(NULL);
        result_type = xr_type_new_array(l->isolate, elem);
    }
    if (!elem_type)
        elem_type = xi_get_container_elem_type(result_type);
    if (!elem_type)
        elem_type = xr_type_new_unknown(NULL);

    if (pc->into && into && into->type && XR_TYPE_IS_ARRAY(into->type))
        return lower_parallel_collect_into_array_expr(l, &cc, into, elem_type);

    if (!pc->into &&
        (pc->final_body || parallel_expr_has_local_initializers(pc->locals, pc->local_count)))
        return lower_parallel_collect_fresh_array_expr(l, &cc, result_type, elem_type);

    XiNativeCallbackKind body_callback = xi_type_can_use_parallel_collect_scalar_callback(elem_type)
                                             ? XI_NATIVE_CALLBACK_PAR_COLLECT_SCALAR_BODY
                                             : XI_NATIVE_CALLBACK_NONE;
    XiFunc *body = lower_parallel_collect_body_func(l, pc, elem_type, body_callback, local_sources,
                                                    node->line);
    if (!body) {
        l->had_error = true;
        return NULL;
    }
    XiValue *closure = lower_par_child_closure(l, body, node->line);
    if (!closure)
        return NULL;
    uint16_t ncap = body->ncaptures;

    XiParallelCollectData *data =
        par_collect_make_data(l, pc, closure, range->as.range.inclusive_end);
    if (!data)
        return NULL;
    data->element_type = elem_type;
    data->result_capture_index = ncap;
    data->start_capture_index = (uint16_t) (ncap + 1);
    data->into_result = pc->into != NULL;

    ParCollectOpSpec spec = {
        .op_type = result_type,
        .start = start,
        .end = end,
        .workers = workers,
        .closure = closure,
        .extra = pc->into ? &into : NULL,
        .extra_count = (uint16_t) (pc->into ? 1u : 0u),
        .data = data,
        .line = (int) node->line,
    };
    return par_collect_make_op(l, &spec);
}

static XiValue *lower_go_expr(XiLower *l, AstNode *node) {
    GoExprNode *go = &node->as.go_expr;
    AstNode *expr = go->expr;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiOp spawn_op = go->spawn_kind == XR_SPAWN_THREAD ? XI_THREAD_SPAWN : XI_GO;

    if (expr->type == AST_CALL_EXPR) {
        /* go fn(args): extract callee + args, don't execute the call.
         * XI_GO args[0]=callee, args[1..n]=params → emits OP_GO.
         * Lower ALL operands before creating XI_GO so they precede it
         * in the block's values array (same pattern as lower_call). */
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee)
            return NULL;
        XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
        uint8_t stack_modes[XI_LOWER_CALL_ARG_STACK_CAP];
        XiLowerGoArgList args;
        xi_lower_go_arg_list_init(&args, stack_args, stack_modes, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_go_call_args(l, call, &args, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;
        uint16_t nargs = (uint16_t) (1 + n);
        XiValue *v = xi_value_new(l->func, l->cur_block, spawn_op, result_type, nargs);
        if (!v)
            return NULL;
        v->args[0] = callee;
        for (int i = 0; i < n; i++) {
            v->args[1 + i] = arg_vals[i];
        }
        if (n > 0) {
            uint8_t *modes =
                (uint8_t *) xi_func_arena_alloc(l->func, (uint32_t) ((size_t) n * sizeof(uint8_t)));
            if (!modes)
                return NULL;
            memcpy(modes, args.modes, (size_t) n * sizeof(uint8_t));
            v->aux = modes;
        }
        v->aux_int = (int64_t) pack_go_aux((int) go->link_mode);
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    /* go fn — closure with no arguments */
    XiValue *callee = xi_lower_expr(l, expr);
    if (!callee)
        return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, spawn_op, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = callee;
    v->aux_int = (int64_t) pack_go_aux((int) go->link_mode);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_await_expr(XiLower *l, AstNode *node) {
    AwaitExprNode *aw = &node->as.await_expr;
    bool direct_one_shot_go = aw->expr && aw->expr->type == AST_GO_EXPR &&
                              aw->expr->as.go_expr.link_mode == 0 && !aw->timeout && !aw->is_any &&
                              !aw->is_all && !aw->is_any_success && !aw->into;
    XiValue *task = xi_lower_expr(l, aw->expr);
    if (!task)
        return NULL;
    XiValue *into = aw->into ? xi_lower_expr(l, aw->into) : NULL;
    if (aw->into && !into)
        return NULL;

    /* Optional timeout argument */
    XiValue *timeout = aw->timeout ? xi_lower_expr(l, aw->timeout) : NULL;
    uint16_t nargs = (uint16_t) (1 + (into ? 1 : 0) + (timeout ? 1 : 0));

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AWAIT, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = task;
    uint16_t argi = 1;
    if (into)
        v->args[argi++] = into;
    if (timeout)
        v->args[argi++] = timeout;
    /* Encode await variant flags. */
    v->aux_int = (aw->is_any ? XI_AWAIT_AUX_ANY : 0) | (aw->is_all ? XI_AWAIT_AUX_ALL : 0) |
                 (aw->is_any_success ? XI_AWAIT_AUX_ANY_SUCCESS : 0) |
                 (direct_one_shot_go ? XI_AWAIT_AUX_ONE_SHOT_GO : 0) |
                 (into ? XI_AWAIT_AUX_INTO_RESULT : 0);
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    if (into)
        v->flags |= XI_FLAG_WRITES_MEM;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_channel_new(XiLower *l, AstNode *node) {
    ChannelNewNode *ch = &node->as.channel_new;
    XiValue *buf_size = ch->buffer_size ? xi_lower_expr(l, ch->buffer_size) : NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    uint8_t elem_tid = 0;
    if (result_type && result_type->kind == XR_KIND_CHANNEL &&
        result_type->container.element_type) {
        elem_tid = xr_type_to_tid(result_type->container.element_type);
        if (!buf_size)
            buf_size = xi_const_int(l->func, l->cur_block, 0, l->type_int);
    }
    uint16_t nargs = buf_size ? 1 : 0;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_NEW, result_type, nargs);
    if (!v)
        return NULL;
    if (buf_size)
        v->args[0] = buf_size;
    v->aux_int = elem_tid;
    v->line = (uint32_t) node->line;
    return v;
}

/*
 * Template string: "hello ${name}, age ${age}"
 * parts = ["hello ", <name_expr>, ", age ", <age_expr>]
 * Lower each part, then STR_CONCAT all.
 */
static XiValue *lower_template_string(XiLower *l, AstNode *node) {
    TemplateStringNode *ts = &node->as.template_str;
    int count = ts->part_count;
    if (count < 0 || count > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] template string part count exceeds %d at line %d\n",
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }

    /* Evaluate all parts */
    XiValue *stack_parts[XI_LOWER_VALUE_LIST_STACK_CAP];
    XiLowerValueList parts;
    xi_lower_value_list_init(&parts, stack_parts, XI_LOWER_VALUE_LIST_STACK_CAP);
    int n = count;
    for (int i = 0; i < n; i++) {
        XiValue *part = xi_lower_expr(l, ts->parts[i]);
        if (!part)
            return NULL;
        if (!xi_lower_value_list_push(l, &parts, part, XI_LOWER_MAX_VARIADIC_VALUES,
                                      "template string part count", node->line))
            return NULL;
    }

    struct XrType *result_type = l->type_string;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STR_CONCAT, result_type, (uint16_t) n);
    if (!v)
        return NULL;
    for (int i = 0; i < n; i++) {
        v->args[i] = parts.items[i];
    }
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_set_literal(XiLower *l, AstNode *node) {
    SetLiteralNode *sl = &node->as.set_literal;
    int count = sl->count;

    int n = count;
    int alloc_n = n > 0 ? n : 1;
    XiValue **elem_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!elem_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, sl->elements[i]);
        if (!elem_vals[i])
            return NULL;
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    if (result_type && XR_TYPE_IS_SET(result_type)) {
        for (int i = 0; i < n; i++) {
            elem_vals[i] = xi_lower_narrow_for_static_type(l, node, elem_vals[i],
                                                           result_type->container.element_type);
        }
    }
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *set_val = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
    if (!set_val)
        return NULL;
    set_val->args[0] = cap;
    set_val->line = (uint32_t) node->line;

    /* Populate: CALL_METHOD("add") for each element */
    for (int i = 0; i < n; i++) {
        XiValue *add = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, l->type_unit, 2);
        if (!add)
            break;
        add->args[0] = set_val;
        add->args[1] = elem_vals[i];
        add->aux = (void *) "add";
        add->aux_int = (int64_t) xi_lower_method_symbol(l, "add") << 1;
        add->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return set_val;
}

/* Emit an XI_IS test against the given XrTypeRef for an existing value.
 * Used both by `expr is T` and by `is T` patterns in match arms. */
XR_FUNC XiValue *xi_lower_is_test(XiLower *l, XiValue *val, XrTypeRef *tref, int line) {
    if (!val)
        return NULL;

    /* Resolve the target type to a runtime value so the VM can use it
     * directly from a register:
     *   - Primitive types → XI_CONST with XrTypeId
     *   - Named types (classes) → scope-resolved class value */
    XiValue *type_val = NULL;
    if (tref) {
        int tid = -1;
        switch (tref->kind) {
            case XR_TREF_INT:
                tid = 8;
                break; /* XR_TID_INT */
            case XR_TREF_FLOAT:
                tid = 11;
                break; /* XR_TID_FLOAT */
            case XR_TREF_STRING:
                tid = 12;
                break; /* XR_TID_STRING */
            case XR_TREF_BOOL:
                tid = 1;
                break; /* XR_TID_BOOL */
            case XR_TREF_NULL:
                tid = 0;
                break; /* XR_TID_NULL */
            case XR_TREF_UNKNOWN:
            case XR_TREF_INT_WIDTH:
            case XR_TREF_FLOAT_WIDTH:
            case XR_TREF_NAMED:
            case XR_TREF_GENERIC:
            case XR_TREF_OPTIONAL:
            case XR_TREF_UNION:
            case XR_TREF_FUNCTION:
            case XR_TREF_TUPLE:
            case XR_TREF_OBJECT:
            case XR_TREF_FIXED_ARRAY:
            case XR_TREF_TYPE_PARAM:
                break;
        }
        /* Generic containers: Array<T> → XR_TID_ARRAY, Map<K,V> → XR_TID_MAP, etc. */
        if (tid < 0 && tref->kind == XR_TREF_GENERIC && tref->name) {
            if (strcmp(tref->name, "Array") == 0)
                tid = 14; /* XR_TID_ARRAY */
            else if (strcmp(tref->name, "Map") == 0)
                tid = 16; /* XR_TID_MAP */
            else if (strcmp(tref->name, "Set") == 0)
                tid = 15; /* XR_TID_SET */
        }
        /* Bare container names without generic args and prelude types */
        if (tid < 0 && tref->kind == XR_TREF_NAMED && tref->name) {
            if (strcmp(tref->name, "Array") == 0)
                tid = 14;
            else if (strcmp(tref->name, "Map") == 0)
                tid = 16;
            else if (strcmp(tref->name, "Set") == 0)
                tid = 15;
            else if (strcmp(tref->name, "Json") == 0)
                tid = 18;
            else if (strcmp(tref->name, "PanicInfo") == 0)
                tid = 24; /* XR_TID_PANIC_INFO */
        }
        /* Tuple type: (T1, T2, ...) → look up TupleN class by arity */
        if (tid < 0 && tref->kind == XR_TREF_TUPLE && l->isolate) {
            uint16_t arity = (uint16_t) tref->nchildren;
            XrClass *tuple_cls = xr_get_or_create_tuple_class(l->isolate, arity);
            if (tuple_cls) {
                type_val = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_any, 0);
                if (type_val)
                    type_val->aux = (void *) tuple_cls;
            }
        }
        if (tid >= 0) {
            type_val = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_int, 0);
            if (type_val)
                type_val->aux_int = tid;
        } else if (tref->kind == XR_TREF_NAMED && tref->name) {
            /* Resolve class from scope chain */
            int var = xi_lower_var_find(l, 0, tref->name);
            if (var >= 0) {
                if (l->is_program && var < l->var_count && l->shared_map[var] >= 0) {
                    XiTopBinding b;
                    b.slot = l->shared_map[var];
                    b.name = l->vars[var].name;
                    b.type = l->vars[var].type;
                    type_val = xi_lower_emit_top_load(l, b, l->type_any);
                } else {
                    type_val = xi_lower_braun_read(l, var, l->cur_block);
                }
            }
            if (!type_val) {
                XiTopBinding tb = xi_lower_find_top_binding(l, 0, tref->name);
                if (xi_top_binding_valid(tb))
                    type_val = xi_lower_emit_top_load(l, tb, l->type_any);
            }
        }
    }

    uint16_t nargs = (type_val != NULL) ? 2 : 1;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_IS, l->type_bool, nargs);
    if (!v)
        return NULL;
    v->args[0] = val;
    if (type_val)
        v->args[1] = type_val;
    v->aux = (void *) tref;
    v->line = (uint32_t) line;
    return v;
}

static XiValue *lower_is_expr(XiLower *l, AstNode *node) {
    IsExprNode *is = &node->as.is_expr;
    XiValue *val = xi_lower_expr(l, is->expr);
    if (!val)
        return NULL;
    return xi_lower_is_test(l, val, is->type, node->line);
}

static XiValue *lower_as_expr(XiLower *l, AstNode *node) {
    AsExprNode *as = &node->as.as_expr;
    XiValue *val = xi_lower_expr(l, as->expr);
    if (!val)
        return NULL;

    /* Resolve XrTypeRef kind to runtime XrTypeId.
     * AsExprNode.type is XrTypeRef*, not XrType*. */
    XrTypeRef *tref = as->type;
    int tid = -1;
    const char *tname = "unknown";
    if (tref) {
        /* Optional wrapper: unwrap to get inner type */
        XrTypeRef *inner = tref;
        if (tref->kind == XR_TREF_OPTIONAL && tref->nchildren > 0)
            inner = tref->children[0];
        switch (inner->kind) {
            case XR_TREF_INT:
                tid = 8;
                tname = "int";
                break; /* XR_TID_INT */
            case XR_TREF_FLOAT:
                tid = 11;
                tname = "float";
                break; /* XR_TID_FLOAT */
            case XR_TREF_STRING:
                tid = 12;
                tname = "string";
                break; /* XR_TID_STRING */
            case XR_TREF_BOOL:
                tid = 1;
                tname = "bool";
                break; /* XR_TID_BOOL */
            case XR_TREF_CHAR:
                tid = XR_TID_CHAR;
                tname = "char";
                break;
            case XR_TREF_NULL:
                tid = 0;
                tname = "null";
                break; /* XR_TID_NULL */
            case XR_TREF_UNKNOWN:
            case XR_TREF_INT_WIDTH:
            case XR_TREF_FLOAT_WIDTH:
            case XR_TREF_NAMED:
            case XR_TREF_GENERIC:
            case XR_TREF_OPTIONAL:
            case XR_TREF_UNION:
            case XR_TREF_FUNCTION:
            case XR_TREF_TUPLE:
            case XR_TREF_OBJECT:
            case XR_TREF_FIXED_ARRAY:
            case XR_TREF_TYPE_PARAM:
                break;
        }
        if (tid < 0 && inner->kind == XR_TREF_NAMED && inner->name) {
            if (strcmp(inner->name, "Array") == 0) {
                tid = 14;
                tname = "Array";
            } else if (strcmp(inner->name, "Map") == 0) {
                tid = 16;
                tname = "Map";
            } else if (strcmp(inner->name, "Set") == 0) {
                tid = 15;
                tname = "Set";
            } else if (strcmp(inner->name, "Json") == 0) {
                tid = 18;
                tname = "Json";
            } else
                tname = inner->name;
        }
        if (tid < 0 && inner->kind == XR_TREF_GENERIC && inner->name) {
            if (strcmp(inner->name, "Array") == 0) {
                tid = 14;
                tname = "Array";
            } else if (strcmp(inner->name, "Map") == 0) {
                tid = 16;
                tname = "Map";
            } else if (strcmp(inner->name, "Set") == 0) {
                tid = 15;
                tname = "Set";
            }
        }
    }

    bool is_safe = as->is_safe;
    struct XrType *result_type = is_safe ? l->type_any : l->type_any;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, result_type, 1);
    if (!v)
        return NULL;
    v->args[0] = val;
    /* Pack tid and is_safe into aux_int: bits[32:1]=tid as 32-bit two's
     * complement, bit[0]=is_safe. The intermediate uint32_t cast is
     * required because `tid` can be -1 (unrecognised generic name); a
     * signed left shift of a negative value is undefined behaviour and
     * would surface as a UBSan failure (linux-asan).
     * The corresponding decode in xi_emit_arith.c uses the signed shift
     * `aux_int >> 1` and reads the low 32 bits back into `int`, which
     * round-trips the sentinel on every two's-complement target. */
    v->aux_int = ((int64_t) (uint32_t) tid << 1) | (is_safe ? 1 : 0);
    v->aux = (void *) arena_strdup(l->func, tname);
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_slice_expr(XiLower *l, AstNode *node) {
    SliceExprNode *sl = &node->as.slice_expr;
    XiValue *src = xi_lower_expr(l, sl->source);
    XiValue *start = sl->start ? xi_lower_expr(l, sl->start)
                               : xi_const_int(l->func, l->cur_block, 0, l->type_int);
    /* Omitted end clamps to the container length without colliding with
     * negative slice indices. */
    XiValue *end = sl->end ? xi_lower_expr(l, sl->end)
                           : xi_const_int(l->func, l->cur_block, INT64_MAX, l->type_int);
    if (!src)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_SLICE, result_type, 3);
    if (!v)
        return NULL;
    v->args[0] = src;
    v->args[1] = start;
    v->args[2] = end;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_range_expr(XiLower *l, AstNode *node) {
    RangeNode *rn = &node->as.range;
    XiValue *start = xi_lower_expr(l, rn->start);
    XiValue *end = xi_lower_expr(l, rn->end);
    if (!start || !end)
        return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_RANGE, result_type, 2);
    if (!v)
        return NULL;
    v->args[0] = start;
    v->args[1] = end;
    v->aux_int = rn->inclusive_end ? 1 : 0;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_struct_literal(XiLower *l, AstNode *node) {
    StructLiteralNode *sl = &node->as.struct_literal;
    int count = sl->field_count;
    if (count < 0 || count > XI_LOWER_MAX_VARIADIC_VALUES) {
        fprintf(stderr, "[LOWER] struct literal field count exceeds %d at line %d\n",
                XI_LOWER_MAX_VARIADIC_VALUES, (int) node->line);
        l->had_error = true;
        return NULL;
    }
    int n = count;

    /* Evaluate field values first */
    int alloc_n = n > 0 ? n : 1;
    XiValue **val_vals =
        (XiValue **) xi_func_arena_alloc(l->func, (uint32_t) (alloc_n * (int) sizeof(XiValue *)));
    if (!val_vals)
        return NULL;
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, sl->field_values[i]);
        if (!val_vals[i])
            return NULL;
    }

    /* Resolve struct class from scope: local → shared → upvalue.
     * Struct declarations are lowered as XI_CLASS_CREATE and bound to
     * a variable with the struct name, so the lookup chain works the
     * same way as for class constructors in lower_new_expr. */
    const char *sname = sl->struct_name;
    XiValue *cls = NULL;
    if (sname) {
        int var_id = xi_lower_var_find(l, 0, sname);
        if (var_id >= 0) {
            if (l->is_program && l->shared_map[var_id] >= 0) {
                XiTopBinding b;
                b.slot = l->shared_map[var_id];
                b.name = l->vars[var_id].name;
                b.type = l->vars[var_id].type;
                cls = xi_lower_emit_top_load(l, b, l->type_any);
            } else {
                cls = xi_lower_braun_read(l, var_id, l->cur_block);
            }
        }
        if (!cls) {
            XiTopBinding tb = xi_lower_find_top_binding(l, 0, sname);
            if (xi_top_binding_valid(tb))
                cls = xi_lower_emit_top_load(l, tb, l->type_any);
        }
        if (!cls) {
            struct XrType *upval_type = NULL;
            int upval_idx = xi_lower_resolve_upvalue(l, 0, sname, &upval_type);
            if (upval_idx >= 0) {
                cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, l->type_any, 0);
                if (cls)
                    cls->aux_int = upval_idx;
            }
        }
    }

    struct XrType *result_type = xi_lower_node_type(l, node);

    /* Struct with layout: emit XI_STRUCT_NEW + XI_STRUCT_SET.
     * Emitter decides stack vs heap based on local use-scan. */
    if (cls) {
        XrStructLayout *slayout = xi_lower_lookup_struct_layout(l, sname);

        if (slayout) {
            XiValue *inst = xi_value_new(l->func, l->cur_block, XI_STRUCT_NEW, result_type, 1);
            if (!inst)
                return NULL;
            inst->args[0] = cls;
            inst->aux = (void *) slayout;
            inst->flags |= XI_FLAG_SIDE_EFFECT;
            inst->line = (uint32_t) node->line;

            for (int i = 0; i < n; i++) {
                if (!val_vals[i] || !sl->field_names[i])
                    continue;
                int fidx = xi_lower_struct_field_index(slayout, sl->field_names[i]);
                if (fidx < 0)
                    continue;
                XiValue *field_val = xi_lower_narrow_for_native_field(
                    l, node, val_vals[i], slayout->fields[fidx].native_type);
                XiValue *set = xi_value_new(l->func, l->cur_block, XI_STRUCT_SET, l->type_unit, 2);
                if (!set)
                    break;
                set->args[0] = inst;
                set->args[1] = field_val;
                set->aux = (void *) slayout;
                set->aux_int = fidx;
                set->flags |= XI_FLAG_SIDE_EFFECT;
            }
            return inst;
        }

        /* No layout (generic struct) → constructor call fallback */
        XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, 1);
        if (!call)
            return NULL;
        call->args[0] = cls;
        call->aux = (void *) "constructor";
        call->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
        call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        call->line = (uint32_t) node->line;

        for (int i = 0; i < n; i++) {
            if (!val_vals[i] || !sl->field_names[i])
                continue;
            XiValue *set = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, l->type_unit, 2);
            if (!set)
                break;
            set->args[0] = call;
            set->args[1] = val_vals[i];
            set->aux = (void *) arena_strdup(l->func, sl->field_names[i]);
            set->aux_int = xi_lower_method_symbol(l, sl->field_names[i]);
            set->flags |= XI_FLAG_SIDE_EFFECT;
        }
        return call;
    }

    /* Fallback: unresolved struct → create as Json object (legacy path) */
    const char **names_copy =
        (const char **) xi_func_arena_alloc(l->func, (uint32_t) (sizeof(const char *) * n));
    if (!names_copy)
        return NULL;
    for (int i = 0; i < n; i++) {
        names_copy[i] = sl->field_names[i];
    }

    XiValue *obj = xi_value_new(l->func, l->cur_block, XI_JSON_NEW, result_type, 0);
    if (!obj)
        return NULL;
    obj->aux_int = n;
    obj->aux = (void *) names_copy;
    obj->line = (uint32_t) node->line;

    for (int i = 0; i < n; i++) {
        XiValue *init = xi_value_new(l->func, l->cur_block, XI_JSON_INIT_F, l->type_unit, 2);
        if (!init)
            break;
        init->args[0] = obj;
        init->args[1] = val_vals[i];
        init->aux_int = i;
        init->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return obj;
}

/*
 * Optional chain: obj?.name or obj?[idx]
 * Short-circuits to null if obj is null.
 */
static XiValue *lower_optional_chain(XiLower *l, AstNode *node) {
    OptionalChainNode *oc = &node->as.optional_chain;
    XiValue *obj = xi_lower_expr(l, oc->object);
    if (!obj)
        return NULL;

    /* Check if obj is null */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null)
        return obj;
    is_null->args[0] = obj;

    XiBlock *access_blk = xi_block_new(l->func);
    XiBlock *null_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, is_null, null_blk, access_blk);
    xi_lower_braun_seal(l, access_blk);
    xi_lower_braun_seal(l, null_blk);

    /* Null path → produce null */
    l->cur_block = null_blk;
    XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
    xi_block_set_jump(l->cur_block, merge);

    /* Access path → perform member access or index */
    l->cur_block = access_blk;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *access_val = NULL;
    if (oc->name) {
        /* Property access: obj.name */
        access_val = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
        if (access_val) {
            access_val->args[0] = obj;
            access_val->aux = (void *) oc->name;
            access_val->aux_int = xi_lower_method_symbol(l, oc->name);
        }
    } else if (oc->index) {
        /* Index access: obj[idx] */
        XiValue *idx = xi_lower_expr(l, oc->index);
        access_val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
        if (access_val) {
            access_val->args[0] = obj;
            access_val->args[1] = idx;
        }
    }
    XiBlock *access_exit = l->cur_block;
    xi_block_set_jump(access_exit, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;

    /* PHI merge: null or accessed value */
    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == null_blk)
                phi->value.args[i] = null_val;
            else
                phi->value.args[i] = access_val ? access_val : null_val;
        }
    }
    return phi ? &phi->value : null_val;
}

/* expr! — force unwrap nullable; runtime null-check then pass-through.
 * Throws Exception(E0413, "Attempted to unwrap a null value") on null. */
static XiValue *lower_force_unwrap(XiLower *l, AstNode *node) {
    XiValue *val = xi_lower_expr(l, node->as.unary.operand);
    if (!val)
        return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    struct XrType *operand_type = xi_lower_node_type(l, node->as.unary.operand);
    bool operand_may_be_null = !operand_type || XR_TYPE_IS_UNKNOWN(operand_type) ||
                               operand_type->is_nullable || operand_type->kind == XR_KIND_NULL ||
                               xr_type_intrinsically_includes_null(operand_type);
    if (!operand_may_be_null) {
        XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
        if (copy)
            copy->args[0] = val;
        return copy ? copy : val;
    }

    XiValue *chk = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!chk)
        return val;
    chk->args[0] = val;

    XiBlock *ok_blk = xi_block_new(l->func);
    XiBlock *throw_blk = xi_block_new(l->func);
    xi_block_set_if(l->cur_block, chk, throw_blk, ok_blk);
    xi_lower_braun_seal(l, throw_blk);
    xi_lower_braun_seal(l, ok_blk);

    /* Throw path: construct Exception(E0413) and throw */
    l->cur_block = throw_blk;
    struct XrType *exception_type = xr_type_new_class(NULL, "PanicInfo");
    XiValue *cls = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, exception_type, 0);
    if (!cls) {
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block = ok_blk;
        return val;
    }
    cls->aux_int = XR_GLOBAL_VAR_PANIC_INFO;
    cls->aux = (void *) "PanicInfo";

    XiValue *msg = xi_const_str(l->func, l->cur_block, "E0413: Attempted to unwrap a null value",
                                l->type_string);
    XiValue *exc = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, exception_type, 2);
    if (!exc) {
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block = ok_blk;
        return val;
    }
    exc->args[0] = cls;
    exc->args[1] = msg;
    exc->aux = (void *) "constructor";
    exc->aux_int = (int64_t) xi_lower_method_symbol(l, "constructor") << 1;
    exc->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    exc->line = (uint32_t) node->line;

    if (l->try_depth > 0) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_ERR_SET, l->type_unit, 1);
        if (set) {
            set->args[0] = exc;
            set->flags |= XI_FLAG_SIDE_EFFECT;
            set->line = (uint32_t) node->line;
        }
        xi_lower_defer_run_to_depth(l, l->catch_defer_depths[l->try_depth - 1], node->line);
        XiBlock *catch_blk = l->catch_targets[l->try_depth - 1];
        xi_block_set_jump(l->cur_block, catch_blk);
        l->cur_block = NULL;
    } else {
        XiValue *thr = xi_value_new(l->func, l->cur_block, XI_ERR_RETURN, l->type_unit, 1);
        if (thr) {
            thr->args[0] = exc;
            thr->flags |= XI_FLAG_SIDE_EFFECT;
            thr->line = (uint32_t) node->line;
        }
        l->cur_block->kind = XI_BLOCK_RETURN;
        l->cur_block->control = thr;
        l->cur_block = NULL;
    }

    /* Ok path */
    l->cur_block = ok_blk;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (copy)
        copy->args[0] = val;
    return copy ? copy : val;
}

static XiValue *lower_this_expr(XiLower *l, AstNode *node) {
    (void) node;
    struct XrType *this_type = xi_lower_node_type(l, node);

    /* Try local scope first (direct method context) */
    int var_id = xi_lower_var_find(l, 0, "this");
    if (var_id >= 0)
        return xi_lower_braun_read(l, var_id, l->cur_block);

    /* Not local — capture from enclosing method via upvalue */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, 0, "this", &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type)
            upval_type = this_type;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL, upval_type, 0);
        if (v)
            v->aux_int = upval_idx;
        return v;
    }

    /* No 'this' in scope (e.g. top-level code) — return null */
    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *lower_super_call(XiLower *l, AstNode *node) {
    SuperCallNode *sc = &node->as.super_call;
    XiValue *stack_args[XI_LOWER_CALL_ARG_STACK_CAP];
    XiLowerArgList args;
    xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
    for (int i = 0; i < sc->arg_count; i++) {
        XiValue *arg = xi_lower_expr(l, sc->arguments[i]);
        if (!arg)
            return NULL;
        if (!xi_lower_arg_list_push(l, &args, arg, XI_LOWER_MAX_CALL_ARGS, (int) node->line))
            return NULL;
    }
    XiValue **arg_vals = args.items;
    int n = args.count;

    /* 'this' is receiver for super call */
    struct XrType *this_type = l->type_any;
    int var_id = xi_lower_var_create(l, 0, "this", this_type);
    XiValue *this_val = xi_lower_braun_read(l, var_id, l->cur_block);

    struct XrType *result_type = xi_lower_node_type(l, node);
    uint16_t nargs = (uint16_t) (n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD, result_type, nargs);
    if (!call)
        return NULL;
    call->args[0] = this_val ? this_val : xi_const_null(l->func, l->cur_block, l->type_null);
    for (int i = 0; i < n; i++)
        call->args[i + 1] = arg_vals[i];
    call->aux = (void *) (sc->method_name ? sc->method_name : "constructor");
    call->aux_int =
        ((int64_t) xi_lower_method_symbol(l, sc->method_name ? sc->method_name : "constructor")
         << 1) |
        1;
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    call->line = (uint32_t) node->line;
    return call;
}

/* Enum access/decl/convert, object literal, catch, cancelled, move
 * are now in xi_lower_misc.c */

/* Main expression dispatcher */
XR_FUNC XiValue *xi_lower_expr(XiLower *l, AstNode *node) {
    if (!node)
        return NULL;
    if (!l->cur_block)
        return NULL; /* dead code after return/break */

    switch (node->type) {
        /* Literals */
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_CHAR:
        case AST_LITERAL_NULL:
        case AST_LITERAL_STRING:
            return lower_literal(l, node);

        /* Binary operations */
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            return lower_binary(l, node);

        /* Unary */
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            return lower_unary(l, node);

        /* Grouping: just unwrap */
        case AST_GROUPING:
            return xi_lower_expr(l, node->as.grouping);

        /* Variables and assignment */
        case AST_VARIABLE:
            return lower_variable(l, node);
        case AST_ASSIGNMENT:
            return lower_assignment(l, node);
        case AST_COMPOUND_ASSIGNMENT:
        case AST_INC:
        case AST_DEC:
            /* Canonicalized away: compound assignment → plain assignment,
             * inc/dec → assignment with +1/-1. Must never reach here. */
            XR_DCHECK(false, "AST_COMPOUND_ASSIGNMENT / INC / DEC "
                             "must be canonicalized before lowering");
            l->had_error = true;
            return NULL;

        /* Calls */
        case AST_CALL_EXPR:
            return lower_call(l, node);

        /* Ternary */
        case AST_TERNARY:
            return lower_ternary(l, node);

        /* Member / index access */
        case AST_MEMBER_ACCESS:
            return lower_member_access(l, node);
        case AST_MEMBER_SET:
            return lower_member_set(l, node);
        case AST_INDEX_GET:
            return lower_index_get(l, node);
        case AST_INDEX_SET:
            return lower_index_set(l, node);
        case AST_ARRAY_LITERAL:
            return lower_array_literal(l, node);
        case AST_TUPLE_LITERAL:
            return lower_tuple_literal(l, node);
        case AST_MAP_LITERAL:
            return lower_map_literal(l, node);

        case AST_OBJECT_LITERAL:
            return xi_lower_object_literal(l, node);

        /* Nullish coalesce */
        case AST_NULLISH_COALESCE:
            return lower_nullish_coalesce(l, node);

        /* Match expression */
        case AST_MATCH_EXPR:
            return xi_lower_match(l, node);

        /* Function / closure */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return xi_lower_function_decl(l, node);

        /* Object creation */
        case AST_NEW_EXPR:
            return lower_new_expr(l, node);

        /* Coroutine */
        case AST_GO_EXPR:
            return lower_go_expr(l, node);
        case AST_AWAIT_EXPR:
            return lower_await_expr(l, node);
        case AST_UNSAFE_EXPR:
            /* Transparent: unsafe only constrains analysis, not codegen. The
             * body is a statement block; its trailing expression statement is
             * the value (or null when the block ends with a non-expression). */
            return lower_unsafe_expr(l, node);
        case AST_CHANNEL_NEW:
            return lower_channel_new(l, node);
        case AST_PARALLEL_COLLECT_EXPR:
            return lower_parallel_collect_expr(l, node);
        case AST_PARALLEL_REDUCE_EXPR:
            return lower_parallel_reduce_expr(l, node);

        /* Template string / set literal */
        case AST_TEMPLATE_STRING:
            return lower_template_string(l, node);
        case AST_SET_LITERAL:
            return lower_set_literal(l, node);

        /* Type operations */
        case AST_IS_EXPR:
            return lower_is_expr(l, node);
        case AST_AS_EXPR:
            return lower_as_expr(l, node);

        /* Slice / range */
        case AST_SLICE_EXPR:
            return lower_slice_expr(l, node);
        case AST_RANGE:
            return lower_range_expr(l, node);

        /* Struct literal / optional chain */
        case AST_STRUCT_LITERAL:
            return lower_struct_literal(l, node);
        case AST_OPTIONAL_CHAIN:
            return lower_optional_chain(l, node);

        /* Force unwrap: expr! */
        case AST_FORCE_UNWRAP:
            return lower_force_unwrap(l, node);

        /* OOP: this / super */
        case AST_THIS_EXPR:
            return lower_this_expr(l, node);
        case AST_SUPER_CALL:
            return lower_super_call(l, node);

        /* Enum access / convert / index */
        case AST_ENUM_ACCESS:
            return xi_lower_enum_access(l, node);
        case AST_ENUM_CONVERT:
            return xi_lower_enum_convert(l, node);
        case AST_ENUM_INDEX:
            return xi_lower_enum_access(l, node); /* same pattern: load field */

        case AST_CANCELLED_EXPR:
            return xi_lower_cancelled_expr(l, node);
        case AST_MOVE_EXPR:
            return xi_lower_move_expr(l, node);

        /* Scope block in expression context: supervisor returns Array<TaskOutcome>. */
        case AST_SCOPE_BLOCK: {
            XiValue *scope_result = xi_lower_scope_block(l, node);
            if (node->as.scope_block.scope_mode == 2 && scope_result)
                return scope_result;
            return xi_const_null(l->func, l->cur_block, l->type_null);
        }

        /* BigInt: lowered as a BigInt constant (string digits + BigInt type) */
        case AST_LITERAL_BIGINT:
            return xi_const_bigint(
                l->func, l->cur_block,
                node->as.literal.raw_value.bigint_val ? node->as.literal.raw_value.bigint_val : "0",
                l->type_bigint);
        case AST_LITERAL_REGEX: {
            const char *pattern = node->as.literal.raw_value.regex.pattern;
            const char *flags = node->as.literal.raw_value.regex.flags;
            XiValue *pat_v =
                xi_const_str(l->func, l->cur_block, pattern ? pattern : "", l->type_string);
            XiValue *flg_v =
                xi_const_str(l->func, l->cur_block, flags ? flags : "", l->type_string);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_REGEX_COMPILE, l->type_regex, 2);
            if (v) {
                v->args[0] = pat_v;
                v->args[1] = flg_v;
            }
            return v;
        }

        /* Expression statement wrapper: unwrap */
        case AST_EXPR_STMT:
            return xi_lower_expr(l, node->as.expr_stmt);

        default:
            /* Every analyzer-accepted AST node must be lowerable.
             * Reaching here indicates a compiler bug, not a user error. */
            XR_DCHECK_FMT(false, "unsupported expr AST kind %d in lowering", (int) node->type);
            l->had_error = true;
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

/* Class declaration lowering (method compilation + XI_CLASS_CREATE).
 * Factored into .inc.c to keep individual files under the 3000-line limit. */
#include "xi_lower_class.inc.c"
