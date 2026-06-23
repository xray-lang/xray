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
    while (v && ((v->op == XI_COPY && !xi_copy_is_value_clone(v)) || v->op == XI_MOVE) &&
           v->nargs >= 1)
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

static XiFunc *lower_resolve_static_callee_func_in_scope(XiFunc *scope, XiValue *callee) {
    while (callee && callee->op == XI_COPY && !xi_copy_is_value_clone(callee) && callee->nargs >= 1)
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
                if (xi_own_use_is_consuming(user->op, a))
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
    /* &&/|| are canonicalized to ternary before lowering */
    XR_DCHECK(node->type != AST_BINARY_AND && node->type != AST_BINARY_OR,
              "lower_binary: &&/|| must be canonicalized to ternary");

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
        return xi_lower_braun_read(l, var_id, l->cur_block);
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
            {"Exception", XR_GLOBAL_VAR_EXCEPTION},
            {"Range", XR_GLOBAL_VAR_RANGE},
            {"DateTime", XR_GLOBAL_VAR_DATETIME},
            {"Atomic", XR_GLOBAL_VAR_ATOMIC},
            {"Ordering", XR_GLOBAL_VAR_ORDERING},
            {"Recv", XR_GLOBAL_VAR_RECV},
            {"SendResult", XR_GLOBAL_VAR_SEND_RESULT},
            {"TaskResult", XR_GLOBAL_VAR_TASK_RESULT},
            {"TaskStatus", XR_GLOBAL_VAR_TASK_STATUS},
            {"WorkQueue", XR_GLOBAL_VAR_WORKQUEUE},
            {"ResultGroup", XR_GLOBAL_VAR_RESULTGROUP},
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
         * per script by xray_isolate_set_script_info: `process` is the Process
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
    if (!type || type->kind != XR_KIND_JSON || !type->object.is_sealed)
        return -1;
    for (int i = 0; i < type->object.field_count; i++) {
        if (type->object.field_names && type->object.field_names[i] &&
            strcmp(type->object.field_names[i], name) == 0)
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

    /* Sealed Json with known field → direct indexed access */
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
    while (obj && obj->op == XI_COPY && !xi_copy_is_value_clone(obj) && obj->nargs >= 1 &&
           obj->args[0])
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

/* Build the scaled address `ptr + idx * sizeof(pointee)` as an integer SSA
 * value. Used by raw-pointer subscript and offset(). */
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
        XiValue *addr = xi_lower_ptr_scaled_addr(l, node, obj, idx, obj->type, l->type_int);
        if (!addr)
            return NULL;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, result_type, 1);
        if (!v)
            return NULL;
        v->args[0] = addr;
        v->aux_int = (int64_t) xi_pointer_pointee_ffi(obj->type);
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
        XiValue *addr = xi_lower_ptr_scaled_addr(l, node, obj, idx, obj->type, l->type_int);
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

static XiValue *lower_array_literal(XiLower *l, AstNode *node) {
    ArrayLiteralNode *arr = &node->as.array_literal;
    int count = arr->count;

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

/* Lower Coro.method(args...) → XI_CORO_OP.
 * Returns NULL for unrecognized methods. */
static XiValue *lower_coro_method(XiLower *l, AstNode *node, const char *method,
                                  CallExprNode *call) {
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

static XiValue *lower_call(XiLower *l, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;

    /* Method call: callee is obj.method — emit XI_CALL_METHOD (→ OP_INVOKE).
     * This is required for builtin methods (set.size, array.push, etc.)
     * which rely on OP_INVOKE dispatch rather than GETPROP + CALL. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;

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
         * The analyzer already validated T is a sealed Json type with fields
         * and stored the result type as T? in the node table. */
        if (ma->name && strcmp(ma->name, "decode") == 0 && ma->object &&
            ma->object->type == AST_VARIABLE && strcmp(ma->object->as.variable.name, "Json") == 0 &&
            call->type_arg_count == 1 && call->arg_count == 1) {
            struct XrType *result_type = xi_lower_node_type(l, node);
            if (result_type && XR_TYPE_IS_JSON(result_type) && result_type->object.is_sealed &&
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

        /* FFI raw pointer methods: deref()/offset(i)/isNull(). */
        if (recv->type && XR_TYPE_IS_POINTER(recv->type) && ma->name) {
            if (strcmp(ma->name, "deref") == 0 && n == 0) {
                XiValue *v = xi_value_new(l->func, l->cur_block, XI_PTR_LOAD, result_type, 1);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                v->aux_int = (int64_t) xi_pointer_pointee_ffi(recv->type);
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
            if (strcmp(ma->name, "loadU32LE") == 0 && n == 1) {
                bytes_op = XI_BYTES_LOAD_U32_LE;
                expected_args = 2;
            } else if (strcmp(ma->name, "loadU64LE") == 0 && n == 1) {
                bytes_op = XI_BYTES_LOAD_U64_LE;
                expected_args = 2;
            } else if (strcmp(ma->name, "copyWithin") == 0 && n == 3) {
                bytes_op = XI_BYTES_COPY_WITHIN;
                expected_args = 4;
            } else if (strcmp(ma->name, "copyFrom") == 0 && n == 4) {
                bytes_op = XI_BYTES_COPY_FROM;
                expected_args = 5;
            } else if (strcmp(ma->name, "repeatFrom") == 0 && n == 3) {
                bytes_op = XI_BYTES_REPEAT_FROM;
                expected_args = 4;
            }
            if (bytes_op) {
                XiValue *v =
                    xi_value_new(l->func, l->cur_block, bytes_op, result_type, expected_args);
                if (!v)
                    return NULL;
                v->args[0] = recv;
                for (int i = 0; i < n; i++)
                    v->args[i + 1] = arg_vals[i];
                v->line = (uint32_t) node->line;
                return v;
            }
        }

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

static void func_add_child(XiFunc *parent, XiFunc *child) {
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
    func_add_child(l->func, child);
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

static XiValue *lower_new_expr(XiLower *l, AstNode *node) {
    NewExprNode *ne = &node->as.new_expr;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XR_DCHECK(ne->class_name != NULL, "new expr must have class name");
    const char *cname = ne->class_name;

    /* Built-in collection types: emit specialized ops (no constructor call) */
    if (ne->module_name == NULL) {
        if (strcmp(cname, "Map") == 0 && ne->arg_count == 0) {
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
        if (strcmp(cname, "WeakMap") == 0 && ne->arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            v->aux_int = 0x02; /* weak flag in C field bit 1 */
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && ne->arg_count == 0) {
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
        if (strcmp(cname, "Array") == 0 && ne->arg_count == 2) {
            XiValue *count = xi_lower_expr(l, ne->arguments[0]);
            XiValue *fill = xi_lower_expr(l, ne->arguments[1]);
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
        if (strcmp(cname, "Set") == 0 && ne->arg_count == 0) {
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
        if (strcmp(cname, "WeakSet") == 0 && ne->arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
            if (!v)
                return NULL;
            v->args[0] = cap;
            v->aux_int = 0x02; /* weak flag in B field bit 1 */
            v->line = (uint32_t) node->line;
            return v;
        }
        if (strcmp(cname, "StringBuilder") == 0 && ne->arg_count == 0) {
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, result_type, 0);
            if (!v)
                return NULL;
            v->aux = (void *) "StringBuilder";
            v->flags |= XI_FLAG_SIDE_EFFECT;
            v->line = (uint32_t) node->line;
            return v;
        }
        /* Exception: no special handling needed — it is a regular class with a
         * primitive constructor registered in core->exceptionClass. Falls through
         * to the generic class-instantiation path below. */
        /* new Bytes() / new Bytes(n) / new Bytes(n, fill) */
        if (strcmp(cname, "Bytes") == 0 && ne->arg_count <= 2) {
            int n = (int) ne->arg_count;
            XiValue *arg_vals[2];
            for (int i = 0; i < n; i++)
                arg_vals[i] = xi_lower_expr(l, ne->arguments[i]);
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
        if (strcmp(cname, "Channel") == 0 && ne->arg_count <= 1) {
            XiValue *buf_size = ne->arg_count == 1 ? xi_lower_expr(l, ne->arguments[0]) : NULL;
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
    for (int i = 0; i < ne->arg_count; i++) {
        XiValue *arg = xi_lower_expr(l, ne->arguments[i]);
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
            {"Exception", XR_GLOBAL_VAR_EXCEPTION},
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
    if (ne->arg_count == 0 && ne->module_name == NULL && l->analyzer) {
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

static XiValue *lower_go_expr(XiLower *l, AstNode *node) {
    GoExprNode *go = &node->as.go_expr;
    AstNode *expr = go->expr;
    struct XrType *result_type = xi_lower_node_type(l, node);

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
        XiLowerArgList args;
        xi_lower_arg_list_init(&args, stack_args, XI_LOWER_CALL_ARG_STACK_CAP);
        if (!lower_call_args_expand_spread(l, call, &args, XI_LOWER_MAX_CALL_ARGS, NULL, 0,
                                           (int) node->line))
            return NULL;
        XiValue **arg_vals = args.items;
        int n = args.count;
        uint16_t nargs = (uint16_t) (1 + n);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_GO, result_type, nargs);
        if (!v)
            return NULL;
        v->args[0] = callee;
        for (int i = 0; i < n; i++) {
            v->args[1 + i] = arg_vals[i];
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
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GO, result_type, 1);
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
                              !aw->is_all && !aw->is_any_success;
    XiValue *task = xi_lower_expr(l, aw->expr);
    if (!task)
        return NULL;

    /* Optional timeout argument */
    XiValue *timeout = aw->timeout ? xi_lower_expr(l, aw->timeout) : NULL;
    uint16_t nargs = timeout ? 2 : 1;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AWAIT, result_type, nargs);
    if (!v)
        return NULL;
    v->args[0] = task;
    if (timeout)
        v->args[1] = timeout;
    /* Encode await variant flags. */
    v->aux_int = (aw->is_any ? XI_AWAIT_AUX_ANY : 0) | (aw->is_all ? XI_AWAIT_AUX_ALL : 0) |
                 (aw->is_any_success ? XI_AWAIT_AUX_ANY_SUCCESS : 0) |
                 (direct_one_shot_go ? XI_AWAIT_AUX_ONE_SHOT_GO : 0);
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
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
            else if (strcmp(tref->name, "Exception") == 0)
                tid = 24; /* XR_TID_EXCEPTION */
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
    struct XrType *exception_type = xr_type_new_class(NULL, "Exception");
    XiValue *cls = xi_value_new(l->func, l->cur_block, XI_GET_BUILTIN, exception_type, 0);
    if (!cls) {
        l->cur_block->kind = XI_BLOCK_UNREACHABLE;
        l->cur_block = ok_blk;
        return val;
    }
    cls->aux_int = XR_GLOBAL_VAR_EXCEPTION;
    cls->aux = (void *) "Exception";

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

        /* Scope block in expression context: supervisor returns errors[] */
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
