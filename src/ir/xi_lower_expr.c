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
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/lexer/xlex.h"

#include "../runtime/class/xenum.h"
#include "../runtime/object/xstring.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"
#include "../frontend/parser/xtype_ref.h"

#include <string.h>
#include <stdio.h>

/* Copy a string into the XiFunc arena so it survives AST destruction. */
static const char *arena_strdup(XiFunc *f, const char *s) {
    if (!s) return NULL;
    uint32_t len = (uint32_t)strlen(s);
    char *copy = (char *)xi_func_arena_alloc(f, len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

/* ========== Forward Declarations ========== */

static XiValue *lower_short_circuit(XiLower *l, AstNode *node);

/* Local type inference for binary ops when side table has no entry.
 * Mirrors the analyzer's xa_visit_binary() rules. */
static struct XrType *infer_binary_type(XiLower *l, AstNodeType ast_type,
                                         struct XrType *left, struct XrType *right) {
    /* Comparison → bool */
    if (ast_type >= AST_BINARY_EQ && ast_type <= AST_BINARY_GE)
        return l->type_bool;
    if (ast_type == AST_BINARY_EQ_STRICT || ast_type == AST_BINARY_NE_STRICT)
        return l->type_bool;
    /* Logical → bool */
    if (ast_type == AST_BINARY_AND || ast_type == AST_BINARY_OR)
        return l->type_bool;
    /* Bitwise → int */
    if (ast_type >= AST_BINARY_BAND && ast_type <= AST_BINARY_RSHIFT)
        return l->type_int;
    /* Arithmetic: promote float, otherwise preserve int */
    if (left && left->kind == XR_KIND_FLOAT) return l->type_float;
    if (right && right->kind == XR_KIND_FLOAT) return l->type_float;
    if (left && left->kind == XR_KIND_INT &&
        right && right->kind == XR_KIND_INT)
        return l->type_int;
    /* string + string → string */
    if (left && left->kind == XR_KIND_STRING &&
        right && right->kind == XR_KIND_STRING)
        return l->type_string;
    /* Fallback: use side table or operand type */
    return left ? left : l->type_any;
}

/* Local type inference for unary ops. */
static struct XrType *infer_unary_type(XiLower *l, AstNodeType ast_type,
                                        struct XrType *operand) {
    switch (ast_type) {
        case AST_UNARY_NEG:  return operand ? operand : l->type_int;
        case AST_UNARY_NOT:  return l->type_bool;
        case AST_UNARY_BNOT: return l->type_int;
        default: return operand ? operand : l->type_any;
    }
}

/* Map AST binary node type to Xi op. */
static uint16_t binary_ast_to_xi_op(AstNodeType ast_type) {
    switch (ast_type) {
        case AST_BINARY_ADD:    return XI_ADD;
        case AST_BINARY_SUB:    return XI_SUB;
        case AST_BINARY_MUL:    return XI_MUL;
        case AST_BINARY_DIV:    return XI_DIV;
        case AST_BINARY_MOD:    return XI_MOD;
        case AST_BINARY_BAND:   return XI_BAND;
        case AST_BINARY_BOR:    return XI_BOR;
        case AST_BINARY_BXOR:   return XI_BXOR;
        case AST_BINARY_LSHIFT: return XI_SHL;
        case AST_BINARY_RSHIFT: return XI_SHR;
        case AST_BINARY_EQ:     return XI_EQ;
        case AST_BINARY_NE:     return XI_NE;
        case AST_BINARY_EQ_STRICT: return XI_EQ_STRICT;
        case AST_BINARY_NE_STRICT: return XI_NE_STRICT;
        case AST_BINARY_LT:    return XI_LT;
        case AST_BINARY_LE:    return XI_LE;
        case AST_BINARY_GT:    return XI_GT;
        case AST_BINARY_GE:    return XI_GE;
        default:                return XI_ADD;  /* fallback */
    }
}

/* ========== Expression Lowering ========== */

static XiValue *lower_literal(XiLower *l, AstNode *node) {
    switch (node->type) {
        case AST_LITERAL_INT:
            return xi_const_int(l->func, l->cur_block,
                                node->as.literal.raw_value.int_val,
                                l->type_int);
        case AST_LITERAL_FLOAT:
            return xi_const_float(l->func, l->cur_block,
                                  node->as.literal.raw_value.float_val,
                                  l->type_float);
        case AST_LITERAL_TRUE:
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);
        case AST_LITERAL_FALSE:
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
        case AST_LITERAL_NULL:
            return xi_const_null(l->func, l->cur_block, l->type_null);
        case AST_LITERAL_STRING:
            return xi_const_str(l->func, l->cur_block,
                                node->as.literal.raw_value.string_val,
                                l->type_string);
        default:
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

/* Collect leaf operands from a string ADD chain (left-recursive flatten). */
static int collect_str_concat_leaves(XiLower *l, AstNode *node,
                                     AstNode **leaves, int max) {
    if (!node) return 0;
    if (node->type == AST_BINARY_ADD) {
        /* Check if this ADD node has string result type */
        struct XrType *t = xa_analyzer_get_node_type(l->analyzer, node);
        if (t && t->kind == XR_KIND_STRING) {
            int n = collect_str_concat_leaves(l, node->as.binary.left, leaves, max);
            n += collect_str_concat_leaves(l, node->as.binary.right,
                                           leaves + n, max - n);
            return n;
        }
    }
    /* Leaf node */
    if (max <= 0) return 0;
    leaves[0] = node;
    return 1;
}

static XiValue *lower_binary(XiLower *l, AstNode *node) {
    /* Short-circuit AND/OR need special control flow */
    if (node->type == AST_BINARY_AND || node->type == AST_BINARY_OR) {
        return lower_short_circuit(l, node);
    }

    /* String concat optimization: flatten ADD chain → XI_STR_CONCAT
     * which emits STRBUF_NEW/APPEND/FINISH (no intermediate allocs). */
    if (node->type == AST_BINARY_ADD) {
        struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
        if (result_type && result_type->kind == XR_KIND_STRING) {
            AstNode *leaves[64];
            int nleaves = collect_str_concat_leaves(l, node, leaves, 64);
            if (nleaves >= 2) {
                XiValue *parts[64];
                for (int i = 0; i < nleaves; i++) {
                    parts[i] = xi_lower_expr(l, leaves[i]);
                    if (!parts[i]) return NULL;
                }
                XiValue *v = xi_value_new(l->func, l->cur_block,
                                          XI_STR_CONCAT, l->type_string,
                                          (uint16_t) nleaves);
                if (!v) return NULL;
                for (int i = 0; i < nleaves; i++)
                    v->args[i] = parts[i];
                v->line = (uint32_t) node->line;
                return v;
            }
        }
    }

    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    if (!lhs || !rhs) return NULL;

    /* Prefer analyzer side table; fall back to local inference from operands */
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type) {
        result_type = infer_binary_type(l, node->type,
                                         lhs->type, rhs->type);
    }
    uint16_t op = binary_ast_to_xi_op(node->type);

    return xi_binary(l->func, l->cur_block, op, result_type, lhs, rhs);
}

/* Short-circuit && and || use control flow (like if-expressions).
 * Always produces a bool result (xray semantics: && / || never leak
 * raw operand values — they return true/false). */
static XiValue *lower_short_circuit(XiLower *l, AstNode *node) {
    bool is_and = (node->type == AST_BINARY_AND);

    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    if (!lhs) return NULL;

    XiBlock *eval_rhs = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *skip = xi_block_new(l->func);

    /* AND: if lhs then eval_rhs else skip (result = false)
     * OR:  if lhs then skip (result = true) else eval_rhs */
    if (is_and)
        xi_block_set_if(l->cur_block, lhs, eval_rhs, skip);
    else
        xi_block_set_if(l->cur_block, lhs, skip, eval_rhs);

    xi_lower_braun_seal(l, eval_rhs);
    xi_lower_braun_seal(l, skip);

    /* Evaluate RHS and coerce to bool if not already bool-typed */
    l->cur_block = eval_rhs;
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    if (rhs && !(rhs->type && rhs->type->kind == XR_KIND_BOOL)) {
        XiValue *not1 = xi_value_new(l->func, l->cur_block, XI_NOT,
                                      l->type_bool, 1);
        if (not1) {
            not1->args[0] = rhs;
            XiValue *not2 = xi_value_new(l->func, l->cur_block, XI_NOT,
                                          l->type_bool, 1);
            if (not2) {
                not2->args[0] = not1;
                rhs = not2;
            }
        }
    }
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip block: short-circuit result is a known boolean constant.
     * AND skips when LHS is falsy → result = false.
     * OR  skips when LHS is truthy → result = true. */
    l->cur_block = skip;
    XiValue *skip_val = xi_const_bool(l->func, skip, !is_and, l->type_bool);
    xi_block_set_jump(skip, merge);

    xi_lower_braun_seal(l, merge);

    /* Merge with phi */
    l->cur_block = merge;
    XiPhi *phi = xi_phi_new(l->func, merge, l->type_bool, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == rhs_exit)
                phi->value.args[i] = rhs ? rhs : skip_val;
            else
                phi->value.args[i] = skip_val;
        }
    }
    return phi ? &phi->value : lhs;
}

static XiValue *lower_unary(XiLower *l, AstNode *node) {
    XiValue *operand = xi_lower_expr(l, node->as.unary.operand);
    if (!operand) return NULL;

    /* Prefer analyzer side table; fall back to local inference */
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type) {
        result_type = infer_unary_type(l, node->type, operand->type);
    }
    uint16_t op;

    switch (node->type) {
        case AST_UNARY_NEG:  op = XI_NEG; break;
        case AST_UNARY_NOT:  op = XI_NOT; break;
        case AST_UNARY_BNOT: op = XI_BNOT; break;
        default: op = XI_NEG; break;
    }

    return xi_unary(l->func, l->cur_block, op, result_type, operand);
}

static XiValue *lower_variable(XiLower *l, AstNode *node) {
    const char *name = node->as.variable.name;
    uint32_t sid = node->as.variable.symbol_id;
    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        /* Program-level shared variables must be read from the shared array
         * because called functions can modify them via XI_SET_SHARED,
         * which bypasses the local SSA and leaves it stale. */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            struct XrType *type = l->vars[var_id].type;
            if (!type) type = l->type_any;
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                                       type, 0);
            if (v) v->aux_int = l->shared_map[var_id];
            return v;
        }
        return xi_lower_braun_read(l, var_id, l->cur_block);
    }

    /* Check for program-level shared variable (supports forward references) */
    struct XrType *shared_type = NULL;
    int shared_idx = xi_lower_find_shared(l, sid, name, &shared_type);
    if (shared_idx >= 0) {
        if (!shared_type) shared_type = l->type_any;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                                   shared_type, 0);
        if (v) v->aux_int = shared_idx;
        return v;
    }

    /* Not found locally — try upvalue capture from enclosing scope */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type) upval_type = l->type_any;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                                   upval_type, 0);
        if (v) v->aux_int = upval_idx;
        return v;
    }

    /* Undeclared variable — semantic error caught earlier */
    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *lower_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.assignment.name;
    uint32_t sid = node->as.assignment.symbol_id;
    XiValue *val = xi_lower_expr(l, node->as.assignment.value);
    if (!val) return NULL;

    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        /* When assigning from a different variable (e.g. x = i), insert
         * an explicit copy so the target gets its own SSA value.  Without
         * this, braun_write stores the source variable's value directly,
         * and the shared SSA value causes two variables to coalesce to
         * the same physical register — corrupting loop-carried values
         * when the source variable is subsequently modified. */
        if (val->var_id != 0xFF && val->var_id != (uint8_t)var_id) {
            XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY,
                                          val->type, 1);
            if (copy) {
                copy->args[0] = val;
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
            if (!child) continue;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                if (child->captures[ci].source == XI_CAPTURE_SRC_REG &&
                    child->captures[ci].name && name &&
                    strcmp(child->captures[ci].name, name) == 0) {
                    child->captures[ci].needs_cell = true;
                    if (var_id < l->var_count)
                        l->vars[var_id].captured_by_child = true;
                    val->flags |= XI_FLAG_SIDE_EFFECT;
                }
            }
        }

        /* If this is a program-level shared variable, also update shared array */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiValue *store = xi_value_new(l->func, l->cur_block,
                                           XI_SET_SHARED, l->type_void, 1);
            if (store) {
                store->args[0] = val;
                store->aux_int = l->shared_map[var_id];
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
        return val;
    }

    /* Check for program-level shared variable from nested scope */
    int shared_idx = xi_lower_find_shared(l, sid, name, NULL);
    if (shared_idx >= 0) {
        XiValue *store = xi_value_new(l->func, l->cur_block,
                                       XI_SET_SHARED, l->type_void, 1);
        if (store) {
            store->args[0] = val;
            store->aux_int = shared_idx;
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
        return val;
    }

    /* Try upvalue store for captured mutable variable */
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, NULL);
    if (upval_idx >= 0) {
        /* Mark the capture as needing cell indirection because the child
         * mutates the captured variable.  The emit stage uses this to
         * emit CELL_NEW in the parent and CELL_GET/CELL_SET in the child. */
        XR_DCHECK(upval_idx < (int)l->func->ncaptures,
                  "upval_idx out of range for needs_cell");
        l->func->captures[upval_idx].needs_cell = true;

        XiValue *store = xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL,
                                       l->type_void, 1);
        if (store) {
            store->args[0] = val;
            store->aux_int = upval_idx;
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
        return val;
    }

    /* Create implicitly (shouldn't happen after semantic analysis) */
    var_id = xi_lower_var_create(l, sid, name, val->type);
    xi_lower_braun_write(l, var_id, l->cur_block, val);
    return val;
}

/* Map compound assignment operator token to Xi binary op */
static uint16_t compound_op_to_xi(int tok) {
    switch (tok) {
        case TK_PLUS_ASSIGN:   return XI_ADD;
        case TK_MINUS_ASSIGN:  return XI_SUB;
        case TK_MUL_ASSIGN:    return XI_MUL;
        case TK_DIV_ASSIGN:    return XI_DIV;
        case TK_MOD_ASSIGN:    return XI_MOD;
        case TK_AND_ASSIGN:    return XI_BAND;
        case TK_OR_ASSIGN:     return XI_BOR;
        case TK_XOR_ASSIGN:    return XI_BXOR;
        case TK_LSHIFT_ASSIGN: return XI_SHL;
        case TK_RSHIFT_ASSIGN: return XI_SHR;
        default:               return XI_ADD;
    }
}

static XiValue *lower_compound_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.compound_assignment.name;
    uint32_t sid = node->as.compound_assignment.symbol_id;
    XiValue *rhs = xi_lower_expr(l, node->as.compound_assignment.value);
    if (!rhs) return NULL;
    uint16_t op = compound_op_to_xi(node->as.compound_assignment.op);
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);

    /* Member compound assignment: obj.field op= rhs */
    if (node->as.compound_assignment.object) {
        XiValue *obj = xi_lower_expr(l, node->as.compound_assignment.object);
        if (!obj) return NULL;

        /* Load current field value */
        XiValue *cur = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD,
                                     result_type ? result_type : l->type_any, 1);
        if (!cur) return NULL;
        cur->args[0] = obj;
        cur->aux = (void *)arena_strdup(l->func, name);
        cur->line = (uint32_t)node->line;

        if (!result_type)
            result_type = infer_binary_type(l, node->type, cur->type, rhs->type);

        /* Compute new value */
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, rhs);
        if (!result) return NULL;

        /* Store back to field */
        XiValue *store = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD,
                                       result_type, 2);
        if (!store) return NULL;
        store->args[0] = obj;
        store->args[1] = result;
        store->aux = (void *)arena_strdup(l->func, name);
        store->flags |= XI_FLAG_SIDE_EFFECT;
        store->line = (uint32_t)node->line;
        return result;
    }

    /* Local variable */
    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        XiValue *cur = xi_lower_braun_read(l, var_id, l->cur_block);
        if (!cur) return NULL;
        if (!result_type)
            result_type = infer_binary_type(l, node->type, cur->type, rhs->type);
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, rhs);
        if (result) {
            xi_lower_braun_write(l, var_id, l->cur_block, result);

            /* Retroactive cell indirection for captured variables */
            for (uint16_t ci_fn = 0; ci_fn < l->func->nchildren; ci_fn++) {
                XiFunc *child = l->func->children[ci_fn];
                if (!child) continue;
                for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                    if (child->captures[ci].source == XI_CAPTURE_SRC_REG &&
                        child->captures[ci].name && name &&
                        strcmp(child->captures[ci].name, name) == 0) {
                        child->captures[ci].needs_cell = true;
                        if (var_id < l->var_count)
                            l->vars[var_id].captured_by_child = true;
                        result->flags |= XI_FLAG_SIDE_EFFECT;
                    }
                }
            }

            /* If program-level shared variable, also update shared array */
            if (l->is_program && l->shared_map[var_id] >= 0) {
                XiValue *store = xi_value_new(l->func, l->cur_block,
                                               XI_SET_SHARED, l->type_void, 1);
                if (store) {
                    store->args[0] = result;
                    store->aux_int = l->shared_map[var_id];
                    store->flags |= XI_FLAG_SIDE_EFFECT;
                }
            }
        }
        return result;
    }

    /* Shared variable from nested scope */
    int shared_idx = xi_lower_find_shared(l, sid, name, NULL);
    if (shared_idx >= 0) {
        XiValue *cur = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                                     l->type_any, 0);
        if (!cur) return NULL;
        cur->aux_int = shared_idx;

        if (!result_type)
            result_type = infer_binary_type(l, node->type, cur->type, rhs->type);
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, rhs);
        if (result) {
            XiValue *store = xi_value_new(l->func, l->cur_block,
                                           XI_SET_SHARED, l->type_void, 1);
            if (store) {
                store->args[0] = result;
                store->aux_int = shared_idx;
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
        return result;
    }

    /* Upvalue: read via LOAD_UPVAL, compute, write via STORE_UPVAL */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type) upval_type = l->type_any;
        XiValue *cur = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                                     upval_type, 0);
        if (!cur) return NULL;
        cur->aux_int = upval_idx;

        if (!result_type)
            result_type = infer_binary_type(l, node->type, cur->type, rhs->type);
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, rhs);
        if (result) {
            XR_DCHECK(upval_idx < (int)l->func->ncaptures,
                      "upval_idx out of range for needs_cell");
            l->func->captures[upval_idx].needs_cell = true;

            XiValue *store = xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL,
                                           l->type_void, 1);
            if (store) {
                store->args[0] = result;
                store->aux_int = upval_idx;
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
        return result;
    }

    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *lower_inc_dec(XiLower *l, AstNode *node) {
    const char *name = node->as.inc.name;
    uint32_t sid = node->as.inc.symbol_id;
    uint16_t op = (node->type == AST_INC) ? XI_ADD : XI_SUB;

    /* Local variable */
    int var_id = xi_lower_var_find(l, sid, name);
    if (var_id >= 0) {
        XiValue *cur = xi_lower_braun_read(l, var_id, l->cur_block);
        if (!cur) return NULL;
        XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        struct XrType *result_type = cur->type ? cur->type : l->type_int;
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, one);
        if (result) {
            xi_lower_braun_write(l, var_id, l->cur_block, result);
            /* If program-level shared variable, also update shared array */
            if (l->is_program && l->shared_map[var_id] >= 0) {
                XiValue *store = xi_value_new(l->func, l->cur_block,
                                               XI_SET_SHARED, l->type_void, 1);
                if (store) {
                    store->args[0] = result;
                    store->aux_int = l->shared_map[var_id];
                    store->flags |= XI_FLAG_SIDE_EFFECT;
                }
            }
        }
        return result;
    }

    /* Shared variable from nested scope */
    int shared_idx = xi_lower_find_shared(l, sid, name, NULL);
    if (shared_idx >= 0) {
        XiValue *cur = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                                     l->type_any, 0);
        if (!cur) return NULL;
        cur->aux_int = shared_idx;
        XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        struct XrType *result_type = cur->type ? cur->type : l->type_int;
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, one);
        if (result) {
            XiValue *store = xi_value_new(l->func, l->cur_block,
                                           XI_SET_SHARED, l->type_void, 1);
            if (store) {
                store->args[0] = result;
                store->aux_int = shared_idx;
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
        return result;
    }

    /* Upvalue: read via LOAD_UPVAL, compute, write via STORE_UPVAL */
    struct XrType *upval_type = NULL;
    int upval_idx = xi_lower_resolve_upvalue(l, sid, name, &upval_type);
    if (upval_idx >= 0) {
        if (!upval_type) upval_type = l->type_any;
        XiValue *cur = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                                     upval_type, 0);
        if (!cur) return NULL;
        cur->aux_int = upval_idx;
        XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
        struct XrType *result_type = cur->type ? cur->type : l->type_int;
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, one);
        if (result) {
            XR_DCHECK(upval_idx < (int)l->func->ncaptures,
                      "upval_idx out of range for needs_cell");
            l->func->captures[upval_idx].needs_cell = true;
            XiValue *store = xi_value_new(l->func, l->cur_block, XI_STORE_UPVAL,
                                           l->type_void, 1);
            if (store) {
                store->args[0] = result;
                store->aux_int = upval_idx;
                store->flags |= XI_FLAG_SIDE_EFFECT;
            }
        }
        return result;
    }

    return xi_const_null(l->func, l->cur_block, l->type_null);
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

/* Map Type.<member> names to XrTypeId constants at compile time.
 * Returns -1 if the name is not a known Type member. */
static int type_member_to_tid(const char *name) {
    if (!name) return -1;
    struct { const char *n; int tid; } table[] = {
        {"null",      0},  /* XR_TID_NULL */
        {"bool",      1},  /* XR_TID_BOOL */
        {"int8",      2},  /* XR_TID_INT8 */
        {"uint8",     3},  /* XR_TID_UINT8 */
        {"int16",     4},  /* XR_TID_INT16 */
        {"uint16",    5},  /* XR_TID_UINT16 */
        {"int32",     6},  /* XR_TID_INT32 */
        {"uint32",    7},  /* XR_TID_UINT32 */
        {"int",       8},  /* XR_TID_INT */
        {"uint64",    9},  /* XR_TID_UINT64 */
        {"float32",  10},  /* XR_TID_FLOAT32 */
        {"float",    11},  /* XR_TID_FLOAT */
        {"string",   12},  /* XR_TID_STRING */
        {"function", 13},  /* XR_TID_FUNCTION */
        {"Array",    14},  /* XR_TID_ARRAY */
        {"Set",      15},  /* XR_TID_SET */
        {"Map",      16},  /* XR_TID_MAP */
        {"object",   17},  /* XR_TID_INSTANCE */
        {"Json",     18},  /* XR_TID_JSON */
        {"BigInt",   19},  /* XR_TID_BIGINT */
        {"Channel",  21},  /* XR_TID_CHANNEL */
        {"Regex",    22},  /* XR_TID_REGEX */
        {"DateTime", 23},  /* XR_TID_DATETIME */
        {"Bytes",    33},  /* XR_TID_BYTES */
        {"TypedArray",34}, /* XR_TID_TYPED_ARRAY */
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].n) == 0)
            return table[i].tid;
    }
    return -1;
}

static XiValue *lower_member_access(XiLower *l, AstNode *node) {
    MemberAccessNode *ma = &node->as.member_access;

    /* Type.<member> → compile-time XrTypeId constant (int) */
    if (ma->object && ma->object->type == AST_VARIABLE &&
        ma->object->as.variable.name &&
        strcmp(ma->object->as.variable.name, "Type") == 0 && ma->name) {
        int tid = type_member_to_tid(ma->name);
        if (tid >= 0) {
            XiValue *v = xi_const_int(l->func, l->cur_block, (int64_t)tid,
                                       l->type_int);
            if (v) v->line = (uint32_t)node->line;
            return v;
        }
    }

    XiValue *obj = xi_lower_expr(l, ma->object);
    if (!obj) return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);

    /* Sealed Json with known field → direct indexed access */
    int fidx = json_field_index(obj->type, ma->name);
    if (fidx >= 0) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_GET_F, result_type, 1);
        if (!v) return NULL;
        v->args[0] = obj;
        v->aux_int = fidx;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v) return NULL;
    v->args[0] = obj;
    v->aux = (void *)arena_strdup(l->func, ma->name);
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_member_set(XiLower *l, AstNode *node) {
    MemberSetNode *ms = &node->as.member_set;
    XiValue *obj = xi_lower_expr(l, ms->object);
    XiValue *val = xi_lower_expr(l, ms->value);
    if (!obj || !val) return NULL;

    struct XrType *result_type = val->type;

    /* Sealed Json with known field → direct indexed store */
    int fidx = json_field_index(obj->type, ms->member);
    if (fidx >= 0) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_SET_F, result_type, 2);
        if (!v) return NULL;
        v->args[0] = obj;
        v->args[1] = val;
        v->aux_int = fidx;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, result_type, 2);
    if (!v) return NULL;
    v->args[0] = obj;
    v->args[1] = val;
    v->aux = (void *)arena_strdup(l->func, ms->member);
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_index_get(XiLower *l, AstNode *node) {
    IndexGetNode *ig = &node->as.index_get;
    XiValue *obj = xi_lower_expr(l, ig->array);
    XiValue *idx = xi_lower_expr(l, ig->index);
    if (!obj || !idx) return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
    if (!v) return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_index_set(XiLower *l, AstNode *node) {
    IndexSetNode *is_node = &node->as.index_set;
    XiValue *obj = xi_lower_expr(l, is_node->array);
    XiValue *idx = xi_lower_expr(l, is_node->index);
    XiValue *val = xi_lower_expr(l, is_node->value);
    if (!obj || !idx || !val) return NULL;

    struct XrType *result_type = val->type;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_SET, result_type, 3);
    if (!v) return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->args[2] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_array_literal(XiLower *l, AstNode *node) {
    ArrayLiteralNode *arr = &node->as.array_literal;
    int count = arr->count;

    /* Evaluate all elements first */
    XiValue *elem_vals[64];
    int n = count > 64 ? 64 : count;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, arr->elements[i]);
    }

    /* Create array: XI_ARRAY_NEW with element count as aux */
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *arr_val = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW, result_type, 1);
    if (!arr_val) return NULL;
    arr_val->args[0] = cap;
    arr_val->line = (uint32_t) node->line;

    /* Populate: INDEX_SET for each element */
    for (int i = 0; i < n; i++) {
        XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET,
                                     l->type_void, 3);
        if (!set) break;
        set->args[0] = arr_val;
        set->args[1] = idx;
        set->args[2] = elem_vals[i];
        set->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return arr_val;
}

/* Generate a location string constant for assert diagnostics.
 * Format: "line <N>" using the AST node's line number. */
static const char *make_assert_loc(XiLower *l, int line) {
    char buf[64];
    snprintf(buf, sizeof(buf), "line %d", line);
    /* Intern the string so it survives in the constant pool.
     * Use the isolate's compile-time interning. */
    XrString *s = xr_string_new(l->isolate, buf, strlen(buf));
    return s ? s->data : "?";
}

/* Intercept known compile-time builtin function calls.
 * Returns non-NULL XiValue if handled, NULL to fall through to generic CALL. */
static XiValue *lower_builtin_call(XiLower *l, AstNode *node,
                                    const char *fname, CallExprNode *call) {
    struct XrType *rtype = xi_lower_node_type(l, node);
    int line = node->line;

    /* assert(cond) / assert(cond, msg) → XI_ASSERT */
    if (strcmp(fname, "assert") == 0 && (call->arg_count == 1 || call->arg_count == 2)) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_void, 1);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *)make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 0;  /* 0 = assert_true */
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_true(cond) → XI_ASSERT aux_int=0 */
    if (strcmp(fname, "assert_true") == 0 && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_void, 1);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *)make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 0;
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_false(cond) → XI_ASSERT aux_int=1 */
    if (strcmp(fname, "assert_false") == 0 && call->arg_count == 1) {
        XiValue *cond = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT, l->type_void, 1);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = cond;
        v->aux = (void *)make_assert_loc(l, call->arguments[0]->line);
        v->aux_int = 1;  /* 1 = assert_false */
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_eq(actual, expected) → XI_ASSERT_EQ */
    if (strcmp(fname, "assert_eq") == 0 && call->arg_count == 2) {
        XiValue *actual = xi_lower_expr(l, call->arguments[0]);
        XiValue *expected = xi_lower_expr(l, call->arguments[1]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_EQ, l->type_void, 2);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = actual;
        v->args[1] = expected;
        v->aux = (void *)make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_ne(actual, unexpected) → XI_ASSERT_NE */
    if (strcmp(fname, "assert_ne") == 0 && call->arg_count == 2) {
        XiValue *actual = xi_lower_expr(l, call->arguments[0]);
        XiValue *unexpected = xi_lower_expr(l, call->arguments[1]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_NE, l->type_void, 2);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = actual;
        v->args[1] = unexpected;
        v->aux = (void *)make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* assert_throws(fn) → XI_ASSERT_THROWS */
    if (strcmp(fname, "assert_throws") == 0 && call->arg_count == 1) {
        XiValue *fn_val = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_ASSERT_THROWS,
                                   l->type_void, 1);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = fn_val;
        v->aux = (void *)make_assert_loc(l, call->arguments[0]->line);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* typeof(x) → XI_TYPEOF */
    if (strcmp(fname, "typeof") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_TYPEOF, l->type_string, 1);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        v->line = (uint32_t)line;
        return v;
    }
    /* typename(x) → XI_TYPEOF aux_int=1 (typename variant) */
    if (strcmp(fname, "typename") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_TYPEOF, l->type_string, 1);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        v->aux_int = 1;  /* typename variant */
        v->line = (uint32_t)line;
        return v;
    }
    /* dump(x) / dump(x, indent) → XI_CALL_BUILTIN aux="dump" → OP_DUMP */
    if (strcmp(fname, "dump") == 0 &&
        (call->arg_count == 1 || call->arg_count == 2)) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        int nargs = (int)call->arg_count;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN,
                                   l->type_void, (uint16_t)nargs);
        if (!v) return xi_const_null(l->func, l->cur_block, l->type_null);
        v->args[0] = arg;
        if (nargs == 2)
            v->args[1] = xi_lower_expr(l, call->arguments[1]);
        v->aux = (void *)"dump";
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t)line;
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    /* copy(x) → XI_CALL_BUILTIN aux="copy" → OP_COPY */
    if (strcmp(fname, "copy") == 0 && call->arg_count == 1) {
        XiValue *arg = xi_lower_expr(l, call->arguments[0]);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN,
                                   rtype ? rtype : l->type_any, 1);
        if (!v) return NULL;
        v->args[0] = arg;
        v->aux = (void *)"copy";
        v->line = (uint32_t)line;
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
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, rtype,
                                   (uint16_t)n);
        if (!v) return NULL;
        for (int i = 0; i < n; i++)
            v->args[i] = arg_vals[i];
        v->aux = (void *)"Bytes";
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t)line;
        return v;
    }

    /* Type conversion builtins: string(x), int(x), float(x), bool(x).
     * Each emits XI_CONVERT with the target type set on the value. */
    if (call->arg_count == 1) {
        struct XrType *target = NULL;
        if (strcmp(fname, "string") == 0) target = l->type_string;
        else if (strcmp(fname, "int") == 0) target = l->type_int;
        else if (strcmp(fname, "float") == 0) target = l->type_float;
        else if (strcmp(fname, "bool") == 0) target = l->type_bool;

        if (target) {
            XiValue *arg = xi_lower_expr(l, call->arguments[0]);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_CONVERT,
                                       target, 1);
            if (!v) return NULL;
            v->args[0] = arg;
            v->line = (uint32_t)line;
            return v;
        }
    }

    (void)rtype;
    return NULL;  /* not a builtin — fall through to generic CALL */
}

static XiValue *lower_call(XiLower *l, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;

    /* Method call: callee is obj.method — emit XI_CALL_METHOD (→ OP_INVOKE).
     * This is required for builtin methods (set.size, array.push, etc.)
     * which rely on OP_INVOKE dispatch rather than GETPROP + CALL. */
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *ma = &call->callee->as.member_access;

        /* Json.decode<T>(data) → XI_JSON_DECODE with compile-time field info.
         * The analyzer already validated T is a sealed Json type with fields
         * and stored the result type as T? in the node table. */
        if (ma->name && strcmp(ma->name, "decode") == 0 &&
            ma->object && ma->object->type == AST_VARIABLE &&
            strcmp(ma->object->as.variable.name, "Json") == 0 &&
            call->type_arg_count == 1 && call->arg_count == 1) {

            struct XrType *result_type = xi_lower_node_type(l, node);
            if (result_type && XR_TYPE_IS_JSON(result_type) &&
                result_type->object.is_sealed && result_type->object.field_count > 0) {

                int fc = result_type->object.field_count;
                XiValue *data_val = xi_lower_expr(l, call->arguments[0]);
                if (!data_val) return NULL;

                /* Arena-copy field names so they survive AST destruction */
                const char **names = (const char **)xi_func_arena_alloc(
                    l->func, (uint32_t)(fc * (int)sizeof(const char *)));
                XR_DCHECK(names != NULL, "json_decode: arena alloc failed");
                for (int i = 0; i < fc; i++) {
                    names[i] = arena_strdup(l->func, result_type->object.field_names[i]);
                }

                XiValue *v = xi_value_new(l->func, l->cur_block, XI_JSON_DECODE,
                                           result_type, 1);
                if (!v) return NULL;
                v->args[0] = data_val;
                v->aux = (void *)names;
                v->aux_int = fc;
                v->flags |= XI_FLAG_SIDE_EFFECT;
                v->line = (uint32_t)node->line;
                return v;
            }
        }

        XiValue *recv = xi_lower_expr(l, ma->object);
        if (!recv) return NULL;

        XiValue *arg_vals[32];
        int n = call->arg_count > 32 ? 32 : call->arg_count;
        for (int i = 0; i < n; i++) {
            arg_vals[i] = xi_lower_expr(l, call->arguments[i]);
        }

        struct XrType *result_type = xi_lower_node_type(l, node);
        uint16_t nargs = (uint16_t)(n + 1);  /* receiver + args */
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                   result_type, nargs);
        if (!v) return NULL;
        v->args[0] = recv;
        for (int i = 0; i < n; i++)
            v->args[i + 1] = arg_vals[i];
        v->aux = (void *)arena_strdup(l->func, ma->name);
        v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        v->line = (uint32_t)node->line;
        return v;
    }

    /* Compile-time builtin interception: detect calls to known builtins
     * and emit specialized Xi ops instead of generic XI_CALL. */
    if (call->callee && call->callee->type == AST_VARIABLE) {
        const char *fname = call->callee->as.variable.name;
        XiValue *bi = lower_builtin_call(l, node, fname, call);
        if (bi) return bi;
    }

    uint16_t nargs = (uint16_t)(call->arg_count + 1);  /* callee + args */

    /* Evaluate callee and all arguments before creating CALL */
    XiValue *callee_val = xi_lower_expr(l, call->callee);
    XiValue *arg_vals[32];
    int n = call->arg_count > 32 ? 32 : call->arg_count;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = xi_lower_expr(l, call->arguments[i]);
    }

    /* Detect self-call: callee resolves to the self-reference dummy.
     * Mark with aux_int=1 so xi_emit produces OP_CALLSELF. */
    bool is_self_call = (l->self_value != NULL && callee_val == l->self_value);

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, nargs);
    if (!v) return NULL;

    v->args[0] = callee_val;
    for (int i = 0; i < n; i++) {
        v->args[i + 1] = arg_vals[i];
    }
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    v->line = (uint32_t) node->line;
    if (is_self_call) v->aux_int = 1;
    return v;
}

static XiValue *lower_ternary(XiLower *l, AstNode *node) {
    XiValue *cond = xi_lower_expr(l, node->as.ternary.condition);
    if (!cond) return NULL;

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *else_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);
    xi_lower_braun_seal(l, then_blk);
    xi_lower_braun_seal(l, else_blk);

    l->cur_block = then_blk;
    XiValue *then_val = xi_lower_expr(l, node->as.ternary.true_expr);
    XiBlock *then_exit = l->cur_block;
    xi_block_set_jump(then_exit, merge);

    l->cur_block = else_blk;
    XiValue *else_val = xi_lower_expr(l, node->as.ternary.false_expr);
    XiBlock *else_exit = l->cur_block;
    xi_block_set_jump(else_exit, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;
    struct XrType *result_type = xi_lower_node_type(l, node);
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
    XiValue *lhs = xi_lower_expr(l, node->as.binary.left);
    if (!lhs) return NULL;

    XiBlock *eval_rhs = xi_block_new(l->func);
    XiBlock *skip = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    /* Test: is lhs null? */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null) return lhs;
    is_null->args[0] = lhs;

    /* If null → eval rhs; otherwise → skip (use lhs) */
    xi_block_set_if(l->cur_block, is_null, eval_rhs, skip);
    xi_lower_braun_seal(l, eval_rhs);
    xi_lower_braun_seal(l, skip);

    /* Evaluate RHS in eval_rhs block */
    l->cur_block = eval_rhs;
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip → merge (lhs is non-null) */
    xi_block_set_jump(skip, merge);

    xi_lower_braun_seal(l, merge);
    l->cur_block = merge;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (phi) {
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == rhs_exit)
                phi->value.args[i] = rhs ? rhs : lhs;
            else
                phi->value.args[i] = lhs;
        }
    }
    return phi ? &phi->value : lhs;
}

static XiValue *lower_map_literal(XiLower *l, AstNode *node) {
    MapLiteralNode *map = &node->as.map_literal;
    int count = map->count;

    /* Evaluate all keys and values first */
    XiValue *key_vals[32], *val_vals[32];
    int n = count > 32 ? 32 : count;
    for (int i = 0; i < n; i++) {
        key_vals[i] = xi_lower_expr(l, map->keys[i]);
        val_vals[i] = xi_lower_expr(l, map->values[i]);
    }

    /* Create map: XI_MAP_NEW with capacity */
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *map_val = xi_value_new(l->func, l->cur_block, XI_MAP_NEW, result_type, 1);
    if (!map_val) return NULL;
    map_val->args[0] = cap;
    map_val->line = (uint32_t) node->line;

    /* Populate: INDEX_SET for each key-value pair */
    for (int i = 0; i < n; i++) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET,
                                     l->type_void, 3);
        if (!set) break;
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
        XiFunc **tmp = (XiFunc **) xr_realloc(parent->children,
                                                new_cap * sizeof(XiFunc *));
        if (!tmp) return;
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
    if (!child) return xi_const_null(l->func, l->cur_block, l->type_null);

    /* Register as child of parent function */
    func_add_child(l->func, child);
    uint16_t child_idx = (uint16_t)(l->func->nchildren - 1);

    /* Emit CLOSURE_NEW with captured values as args.  Listing them as
     * args ensures liveness analysis keeps their registers alive until
     * the closure instruction executes (prevents premature recycling). */
    struct XrType *fn_type = xi_lower_node_type(l, node);
    uint16_t ncap = child->ncaptures;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, fn_type, ncap);
    if (!v) return NULL;
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &child->captures[ci];
        v->args[ci] = (cap->source == XI_CAPTURE_SRC_REG && cap->value)
                       ? cap->value : NULL;
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

        /* For program-level named functions, also store into shared array
         * so nested functions can access via XI_GET_SHARED (forward refs). */
        if (l->is_program && l->shared_map[var_id] >= 0) {
            XiValue *store = xi_value_new(l->func, l->cur_block,
                                           XI_SET_SHARED, l->type_void, 1);
            if (store) {
                store->args[0] = v;
                store->aux_int = l->shared_map[var_id];
                store->flags |= XI_FLAG_SIDE_EFFECT;
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
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_MAP_NEW,
                                       result_type, 1);
            if (!v) return NULL;
            v->args[0] = cap;
            v->aux_int = 0;
            v->line = (uint32_t)node->line;
            return v;
        }
        if (strcmp(cname, "WeakMap") == 0 && ne->arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_MAP_NEW,
                                       result_type, 1);
            if (!v) return NULL;
            v->args[0] = cap;
            v->aux_int = 0x02;  /* weak flag in C field bit 1 */
            v->line = (uint32_t)node->line;
            return v;
        }
        if (strcmp(cname, "Array") == 0 && ne->arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_ARRAY_NEW,
                                       result_type, 1);
            if (!v) return NULL;
            v->args[0] = cap;
            v->line = (uint32_t)node->line;
            return v;
        }
        if (strcmp(cname, "Set") == 0 && ne->arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SET_NEW,
                                       result_type, 1);
            if (!v) return NULL;
            v->args[0] = cap;
            v->aux_int = 0;
            v->line = (uint32_t)node->line;
            return v;
        }
        if (strcmp(cname, "WeakSet") == 0 && ne->arg_count == 0) {
            XiValue *cap = xi_const_int(l->func, l->cur_block, 0, l->type_int);
            XiValue *v = xi_value_new(l->func, l->cur_block, XI_SET_NEW,
                                       result_type, 1);
            if (!v) return NULL;
            v->args[0] = cap;
            v->aux_int = 0x02;  /* weak flag in B field bit 1 */
            v->line = (uint32_t)node->line;
            return v;
        }
    }

    /* Generic class: resolve class name and invoke constructor */
    XiValue *arg_vals[32];
    int n = ne->arg_count > 32 ? 32 : ne->arg_count;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = xi_lower_expr(l, ne->arguments[i]);
    }

    /* Resolve class name using the same chain as lower_variable:
     * local variable → program-level shared → upvalue capture. */
    XiValue *cls = NULL;
    int var_id = xi_lower_var_find(l, 0, cname);
    if (var_id >= 0) {
        if (l->is_program && l->shared_map[var_id] >= 0) {
            cls = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                               l->type_any, 0);
            if (cls) cls->aux_int = l->shared_map[var_id];
        } else {
            cls = xi_lower_braun_read(l, var_id, l->cur_block);
        }
    }
    if (!cls) {
        struct XrType *shared_type = NULL;
        int shared_idx = xi_lower_find_shared(l, 0, cname, &shared_type);
        if (shared_idx >= 0) {
            cls = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                               l->type_any, 0);
            if (cls) cls->aux_int = shared_idx;
        }
    }
    if (!cls) {
        struct XrType *upval_type = NULL;
        int upval_idx = xi_lower_resolve_upvalue(l, 0, cname, &upval_type);
        if (upval_idx >= 0) {
            cls = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                               l->type_any, 0);
            if (cls) cls->aux_int = upval_idx;
        }
    }
    if (!cls) {
        cls = xi_const_null(l->func, l->cur_block, l->type_null);
    }

    uint16_t nargs = (uint16_t)(n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                  result_type, nargs);
    if (!call) return NULL;
    call->args[0] = cls;
    for (int i = 0; i < n; i++)
        call->args[i + 1] = arg_vals[i];
    call->aux = (void *)"constructor";
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    call->line = (uint32_t)node->line;
    return call;
}

static XiValue *lower_go_expr(XiLower *l, AstNode *node) {
    GoExprNode *go = &node->as.go_expr;
    AstNode *expr = go->expr;
    struct XrType *result_type = xi_lower_node_type(l, node);

    if (expr->type == AST_CALL_EXPR) {
        /* go fn(args): extract callee + args, don't execute the call.
         * XI_GO args[0]=callee, args[1..n]=params (same as OP_GO layout). */
        CallExprNode *call = &expr->as.call_expr;
        XiValue *callee = xi_lower_expr(l, call->callee);
        if (!callee) return NULL;
        uint16_t nargs = (uint16_t)(1 + call->arg_count);
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_GO, result_type, nargs);
        if (!v) return NULL;
        v->args[0] = callee;
        for (int i = 0; i < call->arg_count; i++) {
            XiValue *arg = xi_lower_expr(l, call->arguments[i]);
            if (!arg) return NULL;
            v->args[1 + i] = arg;
        }
        v->aux_int = (int64_t) go->link_mode;
        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
        return v;
    }

    /* go fn — closure with no arguments */
    XiValue *callee = xi_lower_expr(l, expr);
    if (!callee) return NULL;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GO, result_type, 1);
    if (!v) return NULL;
    v->args[0] = callee;
    v->aux_int = (int64_t) go->link_mode;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_await_expr(XiLower *l, AstNode *node) {
    AwaitExprNode *aw = &node->as.await_expr;
    XiValue *task = xi_lower_expr(l, aw->expr);
    if (!task) return NULL;

    /* Optional timeout argument */
    XiValue *timeout = aw->timeout ? xi_lower_expr(l, aw->timeout) : NULL;
    uint16_t nargs = timeout ? 2 : 1;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AWAIT, result_type, nargs);
    if (!v) return NULL;
    v->args[0] = task;
    if (timeout) v->args[1] = timeout;
    /* Encode await variant flags: is_any, is_all, is_any_success */
    v->aux_int = (aw->is_any ? 1 : 0) | (aw->is_all ? 2 : 0) |
                 (aw->is_any_success ? 4 : 0);
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_channel_new(XiLower *l, AstNode *node) {
    ChannelNewNode *ch = &node->as.channel_new;
    XiValue *buf_size = ch->buffer_size ? xi_lower_expr(l, ch->buffer_size) : NULL;
    uint16_t nargs = buf_size ? 1 : 0;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CHAN_NEW, result_type, nargs);
    if (!v) return NULL;
    if (buf_size) v->args[0] = buf_size;
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

    /* Evaluate all parts */
    XiValue *parts[64];
    int n = count > 64 ? 64 : count;
    for (int i = 0; i < n; i++) {
        parts[i] = xi_lower_expr(l, ts->parts[i]);
    }

    struct XrType *result_type = l->type_string;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STR_CONCAT,
                               result_type, (uint16_t) n);
    if (!v) return NULL;
    for (int i = 0; i < n; i++) {
        v->args[i] = parts[i];
    }
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_set_literal(XiLower *l, AstNode *node) {
    SetLiteralNode *sl = &node->as.set_literal;
    int count = sl->count;

    XiValue *elem_vals[64];
    int n = count > 64 ? 64 : count;
    for (int i = 0; i < n; i++) {
        elem_vals[i] = xi_lower_expr(l, sl->elements[i]);
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *set_val = xi_value_new(l->func, l->cur_block, XI_SET_NEW, result_type, 1);
    if (!set_val) return NULL;
    set_val->args[0] = cap;
    set_val->line = (uint32_t) node->line;

    /* Populate: CALL_METHOD("add") for each element */
    for (int i = 0; i < n; i++) {
        XiValue *add = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                     l->type_void, 2);
        if (!add) break;
        add->args[0] = set_val;
        add->args[1] = elem_vals[i];
        add->aux = (void *) "add";
        add->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return set_val;
}

static XiValue *lower_is_expr(XiLower *l, AstNode *node) {
    IsExprNode *is = &node->as.is_expr;
    XiValue *val = xi_lower_expr(l, is->expr);
    if (!val) return NULL;

    /* Resolve the target type to a runtime value so the VM can use it
     * directly from a register:
     *   - Primitive types → XI_CONST with XrTypeId
     *   - Named types (classes) → scope-resolved class value */
    XiValue *type_val = NULL;
    XrTypeRef *tref = is->type;
    if (tref) {
        int tid = -1;
        switch (tref->kind) {
            case XR_TREF_INT:         tid = 8;  break;  /* XR_TID_INT */
            case XR_TREF_FLOAT:       tid = 11; break;  /* XR_TID_FLOAT */
            case XR_TREF_STRING:      tid = 12; break;  /* XR_TID_STRING */
            case XR_TREF_BOOL:        tid = 1;  break;  /* XR_TID_BOOL */
            case XR_TREF_NULL:        tid = 0;  break;  /* XR_TID_NULL */
            default: break;
        }
        /* Generic containers: Array<T> → XR_TID_ARRAY, Map<K,V> → XR_TID_MAP, etc. */
        if (tid < 0 && tref->kind == XR_TREF_GENERIC && tref->name) {
            if (strcmp(tref->name, "Array") == 0)      tid = 14; /* XR_TID_ARRAY */
            else if (strcmp(tref->name, "Map") == 0)   tid = 16; /* XR_TID_MAP */
            else if (strcmp(tref->name, "Set") == 0)   tid = 15; /* XR_TID_SET */
        }
        /* Bare container names without generic args (Array, Map, etc.) */
        if (tid < 0 && tref->kind == XR_TREF_NAMED && tref->name) {
            if (strcmp(tref->name, "Array") == 0)      tid = 14;
            else if (strcmp(tref->name, "Map") == 0)   tid = 16;
            else if (strcmp(tref->name, "Set") == 0)   tid = 15;
            else if (strcmp(tref->name, "Json") == 0)  tid = 18;
        }
        if (tid >= 0) {
            type_val = xi_value_new(l->func, l->cur_block,
                                     XI_CONST, l->type_int, 0);
            if (type_val) type_val->aux_int = tid;
        } else if (tref->kind == XR_TREF_NAMED && tref->name) {
            /* Resolve class from scope chain */
            int var = xi_lower_var_find(l, 0, tref->name);
            if (var >= 0) {
                if (l->is_program && l->shared_map &&
                    var < l->var_count && l->shared_map[var] >= 0) {
                    type_val = xi_value_new(l->func, l->cur_block,
                                             XI_GET_SHARED, l->type_any, 0);
                    if (type_val) type_val->aux_int = l->shared_map[var];
                } else {
                    type_val = xi_lower_braun_read(l, var, l->cur_block);
                }
            }
            if (!type_val) {
                struct XrType *stype = NULL;
                int sidx = xi_lower_find_shared(l, 0, tref->name, &stype);
                if (sidx >= 0) {
                    type_val = xi_value_new(l->func, l->cur_block,
                                             XI_GET_SHARED, l->type_any, 0);
                    if (type_val) type_val->aux_int = sidx;
                }
            }
        }
    }

    uint16_t nargs = (type_val != NULL) ? 2 : 1;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_IS, l->type_bool, nargs);
    if (!v) return NULL;
    v->args[0] = val;
    if (type_val) v->args[1] = type_val;
    v->aux = (void *) is->type;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_as_expr(XiLower *l, AstNode *node) {
    AsExprNode *as = &node->as.as_expr;
    XiValue *val = xi_lower_expr(l, as->expr);
    if (!val) return NULL;

    struct XrType *target = as->type ? as->type : l->type_any;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, target, 1);
    if (!v) return NULL;
    v->args[0] = val;
    v->aux = (void *) as->type;     /* target XrType* for TID lookup */
    v->aux_int = as->is_safe ? 1 : 0; /* 0=unsafe (throw), 1=safe (null) */
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_slice_expr(XiLower *l, AstNode *node) {
    SliceExprNode *sl = &node->as.slice_expr;
    XiValue *src = xi_lower_expr(l, sl->source);
    XiValue *start = sl->start ? xi_lower_expr(l, sl->start)
                                : xi_const_int(l->func, l->cur_block, 0, l->type_int);
    /* Omitted end → sentinel -1 (VM interprets as "up to length") */
    XiValue *end = sl->end ? xi_lower_expr(l, sl->end)
                           : xi_const_int(l->func, l->cur_block, -1, l->type_int);
    if (!src) return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_SLICE, result_type, 3);
    if (!v) return NULL;
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
    if (!start || !end) return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_RANGE, result_type, 2);
    if (!v) return NULL;
    v->args[0] = start;
    v->args[1] = end;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_struct_literal(XiLower *l, AstNode *node) {
    StructLiteralNode *sl = &node->as.struct_literal;
    int count = sl->field_count;

    XiValue *val_vals[32];
    int n = count > 32 ? 32 : count;
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, sl->field_values[i]);
    }

    /* Arena-copy field names so they survive AST destruction */
    const char **names_copy = (const char **)xi_func_arena_alloc(
        l->func, (uint32_t)(sizeof(const char *) * n));
    if (!names_copy) return NULL;
    for (int i = 0; i < n; i++) {
        names_copy[i] = sl->field_names[i];
    }

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *obj = xi_value_new(l->func, l->cur_block, XI_JSON_NEW, result_type, 0);
    if (!obj) return NULL;
    obj->aux_int = n;
    obj->aux = (void *)names_copy;
    obj->line = (uint32_t) node->line;

    for (int i = 0; i < n; i++) {
        XiValue *init = xi_value_new(l->func, l->cur_block, XI_JSON_INIT_F,
                                      l->type_void, 2);
        if (!init) break;
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
    if (!obj) return NULL;

    /* Check if obj is null */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null) return obj;
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

/* expr! — force unwrap nullable; runtime null-check then pass-through */
static XiValue *lower_force_unwrap(XiLower *l, AstNode *node) {
    XiValue *val = xi_lower_expr(l, node->as.unary.operand);
    if (!val) return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    /* Emit a null-check that throws on null, otherwise returns val */
    XiValue *chk = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!chk) return val;
    chk->args[0] = val;

    XiBlock *ok_blk = xi_block_new(l->func);
    XiBlock *throw_blk = xi_block_new(l->func);
    xi_block_set_if(l->cur_block, chk, throw_blk, ok_blk);
    xi_lower_braun_seal(l, throw_blk);
    xi_lower_braun_seal(l, ok_blk);

    /* Throw path */
    l->cur_block = throw_blk;
    XiValue *msg = xi_const_str(l->func, l->cur_block,
                                 "force unwrap of null value", l->type_string);
    XiValue *thr = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_void, 1);
    if (thr) {
        thr->args[0] = msg;
        thr->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    }
    l->cur_block->kind = XI_BLOCK_UNREACHABLE;

    /* Ok path */
    l->cur_block = ok_blk;
    XiValue *copy = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (copy) copy->args[0] = val;
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
        if (!upval_type) upval_type = this_type;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_UPVAL,
                                   upval_type, 0);
        if (v) v->aux_int = upval_idx;
        return v;
    }

    /* No 'this' in scope (e.g. top-level code) — return null */
    return xi_const_null(l->func, l->cur_block, l->type_null);
}

static XiValue *lower_super_call(XiLower *l, AstNode *node) {
    SuperCallNode *sc = &node->as.super_call;
    XiValue *arg_vals[32];
    int n = sc->arg_count > 32 ? 32 : sc->arg_count;
    for (int i = 0; i < n; i++)
        arg_vals[i] = xi_lower_expr(l, sc->arguments[i]);

    /* 'this' is receiver for super call */
    struct XrType *this_type = l->type_any;
    int var_id = xi_lower_var_create(l, 0, "this", this_type);
    XiValue *this_val = xi_lower_braun_read(l, var_id, l->cur_block);

    struct XrType *result_type = xi_lower_node_type(l, node);
    uint16_t nargs = (uint16_t)(n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                  result_type, nargs);
    if (!call) return NULL;
    call->args[0] = this_val ? this_val : xi_const_null(l->func, l->cur_block, l->type_null);
    for (int i = 0; i < n; i++)
        call->args[i + 1] = arg_vals[i];
    call->aux = (void *)(sc->method_name ? sc->method_name : "constructor");
    call->aux_int = 1;  /* super call → emit OP_SUPERINVOKE */
    call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    call->line = (uint32_t) node->line;
    return call;
}

static XiValue *lower_enum_access(XiLower *l, AstNode *node) {
    EnumAccessNode *ea = &node->as.enum_access;
    XR_DCHECK(ea->enum_name != NULL, "enum access must have enum name");

    /* Resolve the enum type variable, then GETPROP for the member */
    int var_id = xi_lower_var_create(l, 0, ea->enum_name, l->type_any);
    XiValue *enum_val = xi_lower_braun_read(l, var_id, l->cur_block);

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v) return NULL;
    v->args[0] = enum_val;
    v->aux = (void *)arena_strdup(l->func, ea->member_name);
    v->line = (uint32_t) node->line;
    return v;
}

/* Evaluate compile-time constant for enum member values. */
static XrValue enum_eval_const(XiLower *l, AstNode *expr) {
    if (!expr) return xr_null();
    switch (expr->type) {
        case AST_LITERAL_INT:    return xr_int(expr->as.literal.raw_value.int_val);
        case AST_LITERAL_FLOAT:  return xr_float(expr->as.literal.raw_value.float_val);
        case AST_LITERAL_STRING: {
            const char *s = expr->as.literal.raw_value.string_val;
            XrString *xs = xr_compile_time_intern(l->isolate, s, strlen(s));
            return xr_string_value(xs);
        }
        case AST_LITERAL_TRUE:   return xr_bool(true);
        case AST_LITERAL_FALSE:  return xr_bool(false);
        default:                 return xr_null();
    }
}

/* Lower AST_ENUM_DECL: create XrEnumType at compile time, store as
 * shared variable so enum member access can find it. */
XR_FUNC void xi_lower_enum_decl(XiLower *l, AstNode *node) {
    EnumDeclNode *ed = &node->as.enum_decl;
    XR_DCHECK(ed->name != NULL, "enum name must not be NULL");
    XR_DCHECK(l->isolate != NULL, "isolate required for enum creation");

    int n = ed->member_count;
    char **names = (char **)xr_malloc(sizeof(char *) * n);
    XrValue *values = (XrValue *)xr_malloc(sizeof(XrValue) * n);
    if (!names || !values) {
        xr_free(names); xr_free(values);
        return;
    }

    int64_t auto_val = 0;
    int detected_base = XR_TINT;  // default for auto-increment enums
    for (int i = 0; i < n; i++) {
        EnumMemberNode *m = &ed->members[i]->as.enum_member;
        names[i] = strdup(m->name);
        if (m->value) {
            values[i] = enum_eval_const(l, m->value);
            if (XR_IS_INT(values[i])) {
                auto_val = XR_TO_INT(values[i]) + 1;
            } else if (XR_IS_STRING(values[i])) {
                detected_base = XR_TSTRING;
            } else if (XR_IS_FLOAT(values[i])) {
                detected_base = XR_TFLOAT;
            } else if (XR_IS_BOOL(values[i])) {
                detected_base = XR_TBOOL;
            }
        } else {
            values[i] = xr_int(auto_val);
            auto_val++;
        }
    }

    XrEnumType *et = xr_enum_type_new(l->isolate, ed->name, detected_base,
                                       names, values, n);
    /* names/values ownership transferred to xr_enum_type_new */

    /* Store as XI_CONST with type_any (emitter handles via LOADK) */
    XiValue *cv = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_any, 0);
    if (!cv) return;
    cv->aux = (void *)et;
    cv->line = (uint32_t)node->line;

    /* Write to shared variable so enum access resolves correctly */
    int var_id = xi_lower_var_create(l, 0, ed->name, l->type_any);
    xi_lower_braun_write(l, var_id, l->cur_block, cv);

    if (l->is_program && var_id < l->var_count
        && l->shared_map[var_id] >= 0) {
        XiValue *ss = xi_value_new(l->func, l->cur_block,
                                    XI_SET_SHARED, l->type_void, 1);
        if (ss) {
            ss->args[0] = cv;
            ss->aux_int = l->shared_map[var_id];
            ss->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }
}

static XiValue *lower_enum_convert(XiLower *l, AstNode *node) {
    EnumConvertNode *ec = &node->as.enum_convert;
    XiValue *val = xi_lower_expr(l, ec->value_expr);
    if (!val) return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, result_type, 1);
    if (!v) return NULL;
    v->args[0] = val;
    v->aux = (void *)arena_strdup(l->func, ec->enum_name);
    v->line = (uint32_t) node->line;
    return v;
}

/* ch.send / ch.recv are parsed as method calls, not dedicated AST nodes.
 * AST_CHAN_SEND / AST_CHAN_RECV are reserved types, handled here for
 * completeness if ever emitted. */

static XiValue *lower_cancelled_expr(XiLower *l, AstNode *node) {
    /* cancelled() returns bool */
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL_BUILTIN, l->type_bool, 0);
    if (!v) return NULL;
    v->aux_int = 0;  /* builtin id for 'cancelled' */
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_move_expr(XiLower *l, AstNode *node) {
    /* move var — transfer ownership; semantically same as reading the var */
    MoveExprNode *me = &node->as.move_expr;
    XiValue *val = xi_lower_expr(l, me->expr);
    if (!val) return NULL;
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_COPY, result_type, 1);
    if (!v) return val;
    v->args[0] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_object_literal(XiLower *l, AstNode *node) {
    ObjectLiteralNode *obj = &node->as.object_literal;
    int count = obj->count;

    /* Evaluate all values first */
    XiValue *val_vals[32];
    int n = count > 32 ? 32 : count;
    for (int i = 0; i < n; i++) {
        val_vals[i] = xi_lower_expr(l, obj->values[i]);
    }

    /* Collect string key names for Shape construction (arena-allocated) */
    const char **key_names = (const char **)xi_func_arena_alloc(
        l->func, (uint32_t)(sizeof(const char *) * n));
    if (!key_names) return NULL;
    for (int i = 0; i < n; i++) {
        if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING) {
            key_names[i] = obj->keys[i]->as.literal.raw_value.string_val;
        } else {
            key_names[i] = "?";
        }
    }

    /* Create Json object with known shape */
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_JSON_NEW, result_type, 0);
    if (!obj_val) return NULL;
    obj_val->aux_int = n;
    obj_val->aux = (void *)key_names;
    obj_val->line = (uint32_t) node->line;

    /* Init each field by index */
    for (int i = 0; i < n; i++) {
        XiValue *init = xi_value_new(l->func, l->cur_block, XI_JSON_INIT_F,
                                      l->type_void, 2);
        if (!init) break;
        init->args[0] = obj_val;
        init->args[1] = val_vals[i];
        init->aux_int = i;
        init->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return obj_val;
}

/* lower_pattern_test, lower_match → xi_lower_stmt.c */

/* Main expression dispatcher */
XR_FUNC XiValue *xi_lower_expr(XiLower *l, AstNode *node) {
    if (!node) return NULL;
    if (!l->cur_block) return NULL;  /* dead code after return/break */

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
        case AST_BINARY_ADD:  case AST_BINARY_SUB:
        case AST_BINARY_MUL:  case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND: case AST_BINARY_BOR:
        case AST_BINARY_BXOR: case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:   case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT: case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:   case AST_BINARY_LE:
        case AST_BINARY_GT:   case AST_BINARY_GE:
        case AST_BINARY_AND:  case AST_BINARY_OR:
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
            return lower_compound_assignment(l, node);
        case AST_INC:
        case AST_DEC:
            return lower_inc_dec(l, node);

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
        case AST_MAP_LITERAL:
            return lower_map_literal(l, node);

        case AST_OBJECT_LITERAL:
            return lower_object_literal(l, node);

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
            return lower_enum_access(l, node);
        case AST_ENUM_CONVERT:
            return lower_enum_convert(l, node);
        case AST_ENUM_INDEX:
            return lower_enum_access(l, node);  /* same pattern: load field */

        /* Coroutine expressions */
        case AST_AWAIT_ALL_EXPR:
        case AST_AWAIT_ANY_EXPR:
            return lower_await_expr(l, node);  /* reuse: flags distinguish */
        case AST_CANCELLED_EXPR:
            return lower_cancelled_expr(l, node);
        case AST_MOVE_EXPR:
            return lower_move_expr(l, node);

        /* Scope block in expression context: lower as statement, return null */
        case AST_SCOPE_BLOCK:
            xi_lower_scope_block(l, node);
            return xi_const_null(l->func, l->cur_block, l->type_null);

        /* BigInt: lowered as a BigInt constant (string digits + BigInt type) */
        case AST_LITERAL_BIGINT:
            return xi_const_bigint(l->func, l->cur_block,
                                   node->as.literal.raw_value.bigint_val ?
                                   node->as.literal.raw_value.bigint_val : "0",
                                   l->type_bigint);
        case AST_LITERAL_REGEX:
            return xi_const_str(l->func, l->cur_block,
                                node->as.literal.raw_value.string_val ?
                                node->as.literal.raw_value.string_val : "",
                                l->type_string);

        /* Expression statement wrapper: unwrap */
        case AST_EXPR_STMT:
            return xi_lower_expr(l, node->as.expr_stmt);

        default:
            /* Every analyzer-accepted AST node must be lowerable.
             * Reaching here indicates a compiler bug, not a user error. */
            XR_DCHECK_FMT(false, "unsupported expr AST kind %d in lowering",
                          (int)node->type);
            l->had_error = true;
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

/* Class declaration lowering (method compilation + XI_CLASS_CREATE).
 * Factored into .inc.c to keep individual files under the 3000-line limit. */
#include "xi_lower_class.inc.c"
