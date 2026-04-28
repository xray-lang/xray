/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lower.c - AST to typed SSA IR lowering (Braun SSA construction)
 *
 * Single-pass recursive walk over the AST, producing XiFunc with
 * on-the-fly SSA construction via the Braun algorithm.
 */

#include "xi_lower.h"
#include "xi.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/parser/xast_types.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/lexer/xlex.h"

#include <string.h>
#include <stdio.h>

/* ========== Forward Declarations ========== */

static XiValue *lower_expr(XiLower *l, AstNode *node);
static XiValue *lower_short_circuit(XiLower *l, AstNode *node);
static void lower_stmt(XiLower *l, AstNode *node);
static void lower_stmts(XiLower *l, AstNode **stmts, int count);

/* ========== Braun SSA: Variable Management ========== */

/* Find or create a variable ID for the given name. */
static int var_lookup_or_create(XiLower *l, const char *name, struct XrType *type) {
    XR_DCHECK(name != NULL, "var_lookup_or_create: name is NULL");

    for (int i = 0; i < l->var_count; i++) {
        if (strcmp(l->vars[i].name, name) == 0)
            return i;
    }

    XR_CHECK(l->var_count < XI_LOWER_MAX_VARS, "xi_lower: too many variables");
    int id = l->var_count++;
    l->vars[id].name = name;
    l->vars[id].type = type;
    return id;
}

/* Find variable ID by name. Returns -1 if not found. */
static int var_find(XiLower *l, const char *name) {
    for (int i = 0; i < l->var_count; i++) {
        if (strcmp(l->vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Write: currentDef[var][block] = value */
static void braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val) {
    XR_DCHECK(var_id >= 0 && var_id < XI_LOWER_MAX_VARS,
              "braun_write: var_id out of range");
    XR_DCHECK(blk->id < XI_LOWER_MAX_BLOCKS,
              "braun_write: block_id out of range");
    l->var_defs[var_id * XI_LOWER_MAX_BLOCKS + blk->id] = val;
}

/* Read: get currentDef[var][block], may be NULL. */
static XiValue *braun_read_local(XiLower *l, int var_id, XiBlock *blk) {
    XR_DCHECK(var_id >= 0 && var_id < XI_LOWER_MAX_VARS,
              "braun_read_local: var_id out of range");
    if (blk->id >= XI_LOWER_MAX_BLOCKS) return NULL;
    return l->var_defs[var_id * XI_LOWER_MAX_BLOCKS + blk->id];
}

/* Forward declarations */
static XiValue *braun_read_recursive(XiLower *l, int var_id, XiBlock *blk);
static XiValue *add_phi_operands(XiLower *l, int var_id, XiPhi *phi);

static XiValue *braun_read(XiLower *l, int var_id, XiBlock *blk) {
    XiValue *val = braun_read_local(l, var_id, blk);
    if (val) return val;
    return braun_read_recursive(l, var_id, blk);
}

/* Try to remove trivial phi: if all operands are the same (or self),
 * replace with that single value. */
static XiValue *try_remove_trivial_phi(XiLower *l, int var_id, XiPhi *phi) {
    XiValue *same = NULL;
    XiValue *pv = &phi->value;

    for (uint16_t i = 0; i < pv->nargs; i++) {
        XiValue *op = pv->args[i];
        if (op == same || op == pv)
            continue;  /* self-reference or same as current candidate */
        if (same != NULL)
            return pv;  /* non-trivial: two distinct operands */
        same = op;
    }

    if (same == NULL)
        return pv;  /* undefined — keep the phi */

    /* Trivial: update the def map so future reads see the simplified value */
    braun_write(l, var_id, phi->value.block, same);
    return same;
}

/*
 * Braun read recursive — the core SSA construction algorithm.
 *
 * Three cases:
 *   1. Block not sealed (loop header): create an incomplete phi, record it,
 *      and fill operands later when the block is sealed.
 *   2. Single predecessor: just recurse into that predecessor.
 *   3. Multiple predecessors (sealed): create phi, fill operands, simplify.
 */
static XiValue *braun_read_recursive(XiLower *l, int var_id, XiBlock *blk) {
    XiValue *val;
    struct XrType *type = l->vars[var_id].type;
    if (!type) type = l->type_any;

    if (!blk->sealed) {
        /* Block not sealed: create an incomplete phi placeholder.
         * Operands will be filled in braun_seal_block(). */
        XiPhi *phi = xi_phi_new(l->func, blk, type, 0);
        val = &phi->value;

        /* Record for later completion */
        XR_CHECK(l->incomplete_count < XI_LOWER_MAX_INCOMPLETE,
                 "xi_lower: too many incomplete phis");
        XiIncompletePhi *ip = &l->incomplete[l->incomplete_count++];
        ip->var_id = var_id;
        ip->block = blk;
        ip->phi = phi;
    } else if (blk->npreds == 0) {
        /* Entry block or unreachable — variable used before definition. */
        val = xi_const_null(l->func, blk, l->type_null);
    } else if (blk->npreds == 1) {
        /* Single predecessor: no phi needed, recurse. */
        val = braun_read(l, var_id, blk->preds[0]);
    } else {
        /* Multiple predecessors: insert phi, then fill operands. */
        XiPhi *phi = xi_phi_new(l->func, blk, type, blk->npreds);
        /* Write before filling to break recursive cycles */
        braun_write(l, var_id, blk, &phi->value);
        val = add_phi_operands(l, var_id, phi);
    }

    braun_write(l, var_id, blk, val);
    return val;
}

/* Fill phi operands by reading from each predecessor. */
static XiValue *add_phi_operands(XiLower *l, int var_id, XiPhi *phi) {
    XiBlock *blk = phi->value.block;
    /* Reallocate args to match current pred count */
    phi->value.nargs = blk->npreds;
    if (blk->npreds > 0) {
        phi->value.args = (XiValue **) xi_func_arena_alloc(
            l->func, blk->npreds * sizeof(XiValue *));
    }
    for (uint16_t i = 0; i < blk->npreds; i++) {
        phi->value.args[i] = braun_read(l, var_id, blk->preds[i]);
    }
    return try_remove_trivial_phi(l, var_id, phi);
}

/*
 * Seal a block: all predecessors are now known.
 * Complete any incomplete phis that were deferred.
 */
static void braun_seal_block(XiLower *l, XiBlock *blk) {
    blk->sealed = true;

    /* Complete all incomplete phis for this block */
    int kept = 0;
    for (int i = 0; i < l->incomplete_count; i++) {
        XiIncompletePhi *ip = &l->incomplete[i];
        if (ip->block == blk) {
            add_phi_operands(l, ip->var_id, ip->phi);
            /* consumed — don't keep */
        } else {
            l->incomplete[kept++] = *ip;
        }
    }
    l->incomplete_count = kept;
}

/* ========== Type Helpers ========== */

/* Get the XrType* for an AST node from the analyzer's side table.
 * Falls back to XR_KIND_UNKNOWN only as last resort. */
static struct XrType *node_type(XiLower *l, AstNode *node) {
    struct XrType *t = xa_analyzer_get_node_type(l->analyzer, node);
    return t ? t : l->type_any;
}

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
        case AST_BINARY_EQ_STRICT: return XI_EQ;
        case AST_BINARY_NE_STRICT: return XI_NE;
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

static XiValue *lower_binary(XiLower *l, AstNode *node) {
    /* Short-circuit AND/OR need special control flow */
    if (node->type == AST_BINARY_AND || node->type == AST_BINARY_OR) {
        return lower_short_circuit(l, node);
    }

    XiValue *lhs = lower_expr(l, node->as.binary.left);
    XiValue *rhs = lower_expr(l, node->as.binary.right);
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

/* Short-circuit && and || use control flow (like if-expressions). */
static XiValue *lower_short_circuit(XiLower *l, AstNode *node) {
    bool is_and = (node->type == AST_BINARY_AND);

    XiValue *lhs = lower_expr(l, node->as.binary.left);
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

    braun_seal_block(l, eval_rhs);
    braun_seal_block(l, skip);

    /* Evaluate RHS */
    l->cur_block = eval_rhs;
    XiValue *rhs = lower_expr(l, node->as.binary.right);
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip block just jumps to merge */
    xi_block_set_jump(skip, merge);

    braun_seal_block(l, merge);

    /* Merge with phi */
    l->cur_block = merge;
    XiPhi *phi = xi_phi_new(l->func, merge, l->type_bool, merge->npreds);
    if (phi) {
        /* Operand order matches preds order.
         * rhs_exit jumps first, skip second -> preds[0]=rhs_exit, preds[1]=skip */
        for (uint16_t i = 0; i < merge->npreds; i++) {
            if (merge->preds[i] == rhs_exit)
                phi->value.args[i] = rhs ? rhs : lhs;
            else
                phi->value.args[i] = lhs;
        }
    }
    return phi ? &phi->value : lhs;
}

static XiValue *lower_unary(XiLower *l, AstNode *node) {
    XiValue *operand = lower_expr(l, node->as.unary.operand);
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
    int var_id = var_find(l, name);
    if (var_id < 0) {
        /* Undeclared variable — semantic error caught earlier.
         * Return null constant as fallback. */
        return xi_const_null(l->func, l->cur_block, l->type_null);
    }
    return braun_read(l, var_id, l->cur_block);
}

static XiValue *lower_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.assignment.name;
    XiValue *val = lower_expr(l, node->as.assignment.value);
    if (!val) return NULL;

    int var_id = var_find(l, name);
    if (var_id < 0) {
        /* Create implicitly (shouldn't happen after semantic analysis) */
        var_id = var_lookup_or_create(l, name, val->type);
    }

    braun_write(l, var_id, l->cur_block, val);
    return val;
}

static XiValue *lower_compound_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.compound_assignment.name;
    int var_id = var_find(l, name);
    if (var_id < 0)
        return xi_const_null(l->func, l->cur_block, l->type_null);

    XiValue *cur = braun_read(l, var_id, l->cur_block);
    XiValue *rhs = lower_expr(l, node->as.compound_assignment.value);
    if (!cur || !rhs) return NULL;

    /* Map compound op token to Xi binary op */
    uint16_t op;
    switch (node->as.compound_assignment.op) {
        case TK_PLUS_ASSIGN:  op = XI_ADD; break;
        case TK_MINUS_ASSIGN: op = XI_SUB; break;
        case TK_MUL_ASSIGN:   op = XI_MUL; break;
        case TK_DIV_ASSIGN:   op = XI_DIV; break;
        case TK_MOD_ASSIGN:   op = XI_MOD; break;
        default: op = XI_ADD; break;
    }

    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);
    if (!result_type)
        result_type = infer_binary_type(l, node->type, cur->type, rhs->type);
    XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, rhs);
    if (result)
        braun_write(l, var_id, l->cur_block, result);
    return result;
}

static XiValue *lower_inc_dec(XiLower *l, AstNode *node) {
    const char *name = node->as.inc.name;
    int var_id = var_find(l, name);
    if (var_id < 0)
        return xi_const_null(l->func, l->cur_block, l->type_null);

    XiValue *cur = braun_read(l, var_id, l->cur_block);
    if (!cur) return NULL;

    XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
    uint16_t op = (node->type == AST_INC) ? XI_ADD : XI_SUB;
    struct XrType *result_type = cur->type ? cur->type : l->type_int;

    XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, one);
    if (result)
        braun_write(l, var_id, l->cur_block, result);
    return result;
}

static XiValue *lower_member_access(XiLower *l, AstNode *node) {
    MemberAccessNode *ma = &node->as.member_access;
    XiValue *obj = lower_expr(l, ma->object);
    if (!obj) return NULL;

    struct XrType *result_type = node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v) return NULL;
    v->args[0] = obj;
    v->aux = (void *) ma->name;  /* field name (borrowed from AST) */
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_member_set(XiLower *l, AstNode *node) {
    MemberSetNode *ms = &node->as.member_set;
    XiValue *obj = lower_expr(l, ms->object);
    XiValue *val = lower_expr(l, ms->value);
    if (!obj || !val) return NULL;

    struct XrType *result_type = val->type;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD, result_type, 2);
    if (!v) return NULL;
    v->args[0] = obj;
    v->args[1] = val;
    v->aux = (void *) ms->member;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_index_get(XiLower *l, AstNode *node) {
    IndexGetNode *ig = &node->as.index_get;
    XiValue *obj = lower_expr(l, ig->array);
    XiValue *idx = lower_expr(l, ig->index);
    if (!obj || !idx) return NULL;

    struct XrType *result_type = node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
    if (!v) return NULL;
    v->args[0] = obj;
    v->args[1] = idx;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_index_set(XiLower *l, AstNode *node) {
    IndexSetNode *is_node = &node->as.index_set;
    XiValue *obj = lower_expr(l, is_node->array);
    XiValue *idx = lower_expr(l, is_node->index);
    XiValue *val = lower_expr(l, is_node->value);
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
        elem_vals[i] = lower_expr(l, arr->elements[i]);
    }

    /* Create array: XI_ARRAY_NEW with element count as aux */
    struct XrType *result_type = node_type(l, node);
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

static XiValue *lower_call(XiLower *l, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    uint16_t nargs = (uint16_t)(call->arg_count + 1);  /* callee + args */

    /* Evaluate callee and all arguments before creating CALL */
    XiValue *callee_val = lower_expr(l, call->callee);
    XiValue *arg_vals[32];
    int n = call->arg_count > 32 ? 32 : call->arg_count;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = lower_expr(l, call->arguments[i]);
    }

    struct XrType *result_type = node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CALL, result_type, nargs);
    if (!v) return NULL;

    v->args[0] = callee_val;
    for (int i = 0; i < n; i++) {
        v->args[i + 1] = arg_vals[i];
    }
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_ternary(XiLower *l, AstNode *node) {
    XiValue *cond = lower_expr(l, node->as.ternary.condition);
    if (!cond) return NULL;

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *else_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);
    braun_seal_block(l, then_blk);
    braun_seal_block(l, else_blk);

    l->cur_block = then_blk;
    XiValue *then_val = lower_expr(l, node->as.ternary.true_expr);
    XiBlock *then_exit = l->cur_block;
    xi_block_set_jump(then_exit, merge);

    l->cur_block = else_blk;
    XiValue *else_val = lower_expr(l, node->as.ternary.false_expr);
    XiBlock *else_exit = l->cur_block;
    xi_block_set_jump(else_exit, merge);

    braun_seal_block(l, merge);
    l->cur_block = merge;
    struct XrType *result_type = node_type(l, node);
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
    XiValue *lhs = lower_expr(l, node->as.binary.left);
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
    braun_seal_block(l, eval_rhs);
    braun_seal_block(l, skip);

    /* Evaluate RHS in eval_rhs block */
    l->cur_block = eval_rhs;
    XiValue *rhs = lower_expr(l, node->as.binary.right);
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip → merge (lhs is non-null) */
    xi_block_set_jump(skip, merge);

    braun_seal_block(l, merge);
    l->cur_block = merge;

    struct XrType *result_type = node_type(l, node);
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
        key_vals[i] = lower_expr(l, map->keys[i]);
        val_vals[i] = lower_expr(l, map->values[i]);
    }

    /* Create map: XI_MAP_NEW with capacity */
    struct XrType *result_type = node_type(l, node);
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
static XiValue *lower_function_decl(XiLower *l, AstNode *node) {
    /* Recursively lower the function body into a child XiFunc */
    XiFunc *child = xi_lower_func(node, l->analyzer, l->isolate);
    if (!child) return xi_const_null(l->func, l->cur_block, l->type_null);

    /* Register as child of parent function */
    func_add_child(l->func, child);
    uint16_t child_idx = (uint16_t)(l->func->nchildren - 1);

    /* Emit CLOSURE_NEW with aux pointing to child func */
    struct XrType *fn_type = node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_CLOSURE_NEW, fn_type, 0);
    if (!v) return NULL;
    v->aux = (void *) child;
    v->aux_int = child_idx;
    v->line = (uint32_t) node->line;

    /* If named, register in SSA so the function can be called by name */
    FunctionDeclNode *fdecl = &node->as.function_decl;
    if (fdecl->name) {
        int var_id = var_lookup_or_create(l, fdecl->name, fn_type);
        braun_write(l, var_id, l->cur_block, v);
    }

    return v;
}

static XiValue *lower_new_expr(XiLower *l, AstNode *node) {
    NewExprNode *ne = &node->as.new_expr;

    /* Evaluate arguments */
    XiValue *arg_vals[32];
    int n = ne->arg_count > 32 ? 32 : ne->arg_count;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = lower_expr(l, ne->arguments[i]);
    }

    /* Allocate and call constructor */
    struct XrType *result_type = node_type(l, node);
    XiValue *obj = xi_value_new(l->func, l->cur_block, XI_ALLOC, result_type, 0);
    if (!obj) return NULL;
    obj->aux = (void *) ne->class_name;
    obj->line = (uint32_t) node->line;

    /* Constructor call: CALL_METHOD on the allocated object */
    uint16_t nargs = (uint16_t)(n + 1);
    XiValue *call = xi_value_new(l->func, l->cur_block, XI_CALL_METHOD,
                                  result_type, nargs);
    if (call) {
        call->args[0] = obj;
        for (int i = 0; i < n; i++)
            call->args[i + 1] = arg_vals[i];
        call->aux = (void *) "init";  /* constructor method name */
        call->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
        call->line = (uint32_t) node->line;
    }
    return obj;
}

static XiValue *lower_go_expr(XiLower *l, AstNode *node) {
    GoExprNode *go = &node->as.go_expr;
    XiValue *task_expr = lower_expr(l, go->expr);
    if (!task_expr) return NULL;

    struct XrType *result_type = node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_GO, result_type, 1);
    if (!v) return NULL;
    v->args[0] = task_expr;
    v->aux_int = (int64_t) go->link_mode;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_await_expr(XiLower *l, AstNode *node) {
    AwaitExprNode *aw = &node->as.await_expr;
    XiValue *task = lower_expr(l, aw->expr);
    if (!task) return NULL;

    /* Optional timeout argument */
    XiValue *timeout = aw->timeout ? lower_expr(l, aw->timeout) : NULL;
    uint16_t nargs = timeout ? 2 : 1;

    struct XrType *result_type = node_type(l, node);
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
    XiValue *buf_size = ch->buffer_size ? lower_expr(l, ch->buffer_size) : NULL;
    uint16_t nargs = buf_size ? 1 : 0;

    struct XrType *result_type = node_type(l, node);
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
        parts[i] = lower_expr(l, ts->parts[i]);
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
        elem_vals[i] = lower_expr(l, sl->elements[i]);
    }

    struct XrType *result_type = node_type(l, node);
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

static void lower_defer(XiLower *l, AstNode *node) {
    DeferStmtNode *d = &node->as.defer_stmt;
    XiValue *expr = lower_expr(l, d->expr);
    if (!expr || !l->cur_block) return;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_DEFER, l->type_void, 1);
    if (!v) return;
    v->args[0] = expr;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
}

static XiValue *lower_is_expr(XiLower *l, AstNode *node) {
    IsExprNode *is = &node->as.is_expr;
    XiValue *val = lower_expr(l, is->expr);
    if (!val) return NULL;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_IS, l->type_bool, 1);
    if (!v) return NULL;
    v->args[0] = val;
    v->aux = (void *) is->type;  /* target type for runtime check */
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_as_expr(XiLower *l, AstNode *node) {
    AsExprNode *as = &node->as.as_expr;
    XiValue *val = lower_expr(l, as->expr);
    if (!val) return NULL;

    struct XrType *target = as->type ? as->type : l->type_any;
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_AS, target, 1);
    if (!v) return NULL;
    v->args[0] = val;
    v->aux = (void *) as->type;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_slice_expr(XiLower *l, AstNode *node) {
    SliceExprNode *sl = &node->as.slice_expr;
    XiValue *src = lower_expr(l, sl->source);
    XiValue *start = sl->start ? lower_expr(l, sl->start)
                                : xi_const_int(l->func, l->cur_block, 0, l->type_int);
    XiValue *end = sl->end ? lower_expr(l, sl->end) : NULL;
    if (!src) return NULL;

    uint16_t nargs = end ? 3 : 2;
    struct XrType *result_type = node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_SLICE, result_type, nargs);
    if (!v) return NULL;
    v->args[0] = src;
    v->args[1] = start;
    if (end) v->args[2] = end;
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_range_expr(XiLower *l, AstNode *node) {
    RangeNode *rn = &node->as.range;
    XiValue *start = lower_expr(l, rn->start);
    XiValue *end = lower_expr(l, rn->end);
    if (!start || !end) return NULL;

    struct XrType *result_type = node_type(l, node);
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
        val_vals[i] = lower_expr(l, sl->field_values[i]);
    }

    struct XrType *result_type = node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *obj = xi_value_new(l->func, l->cur_block, XI_ALLOC, result_type, 1);
    if (!obj) return NULL;
    obj->args[0] = cap;
    obj->aux = (void *) sl->struct_name;
    obj->line = (uint32_t) node->line;

    for (int i = 0; i < n; i++) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD,
                                     l->type_void, 2);
        if (!set) break;
        set->args[0] = obj;
        set->args[1] = val_vals[i];
        if (sl->field_names[i])
            set->aux = (void *) sl->field_names[i];
        set->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return obj;
}

/*
 * Optional chain: obj?.name or obj?[idx]
 * Short-circuits to null if obj is null.
 */
static XiValue *lower_optional_chain(XiLower *l, AstNode *node) {
    OptionalChainNode *oc = &node->as.optional_chain;
    XiValue *obj = lower_expr(l, oc->object);
    if (!obj) return NULL;

    /* Check if obj is null */
    XiValue *is_null = xi_value_new(l->func, l->cur_block, XI_ISNULL, l->type_bool, 1);
    if (!is_null) return obj;
    is_null->args[0] = obj;

    XiBlock *access_blk = xi_block_new(l->func);
    XiBlock *null_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);

    xi_block_set_if(l->cur_block, is_null, null_blk, access_blk);
    braun_seal_block(l, access_blk);
    braun_seal_block(l, null_blk);

    /* Null path → produce null */
    l->cur_block = null_blk;
    XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
    xi_block_set_jump(l->cur_block, merge);

    /* Access path → perform member access or index */
    l->cur_block = access_blk;
    struct XrType *result_type = node_type(l, node);
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
        XiValue *idx = lower_expr(l, oc->index);
        access_val = xi_value_new(l->func, l->cur_block, XI_INDEX_GET, result_type, 2);
        if (access_val) {
            access_val->args[0] = obj;
            access_val->args[1] = idx;
        }
    }
    XiBlock *access_exit = l->cur_block;
    xi_block_set_jump(access_exit, merge);

    braun_seal_block(l, merge);
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

static XiValue *lower_object_literal(XiLower *l, AstNode *node) {
    ObjectLiteralNode *obj = &node->as.object_literal;
    int count = obj->count;

    /* Evaluate all values first */
    XiValue *val_vals[32];
    int n = count > 32 ? 32 : count;
    for (int i = 0; i < n; i++) {
        val_vals[i] = lower_expr(l, obj->values[i]);
    }

    /* Allocate object */
    struct XrType *result_type = node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_ALLOC, result_type, 1);
    if (!obj_val) return NULL;
    obj_val->args[0] = cap;
    obj_val->line = (uint32_t) node->line;

    /* STORE_FIELD for each property */
    for (int i = 0; i < n; i++) {
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_STORE_FIELD,
                                     l->type_void, 2);
        if (!set) break;
        set->args[0] = obj_val;
        set->args[1] = val_vals[i];
        /* Key is either a literal string or computed — use aux for name */
        if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING)
            set->aux = (void *) obj->keys[i]->as.literal.raw_value.string_val;
        set->flags |= XI_FLAG_SIDE_EFFECT;
    }
    return obj_val;
}

/*
 * Lower pattern comparison for a match arm.
 * Returns a bool XiValue representing whether the pattern matches.
 */
static XiValue *lower_pattern_test(XiLower *l, XiValue *subject, AstNode *pattern) {
    if (!pattern || !subject) return NULL;

    switch (pattern->type) {
        case AST_PATTERN_WILDCARD:
            /* _ always matches */
            return xi_const_bool(l->func, l->cur_block, true, l->type_bool);

        case AST_PATTERN_LITERAL: {
            /* Compare subject == literal value */
            XiValue *lit = lower_expr(l, pattern->as.pattern_literal.value);
            if (!lit) return NULL;
            return xi_binary(l->func, l->cur_block, XI_EQ, l->type_bool, subject, lit);
        }

        case AST_PATTERN_RANGE: {
            /* subject >= start && subject <= end */
            XiValue *start = lower_expr(l, pattern->as.pattern_range.start);
            XiValue *end = lower_expr(l, pattern->as.pattern_range.end);
            if (!start || !end) return NULL;
            XiValue *ge = xi_binary(l->func, l->cur_block, XI_GE,
                                     l->type_bool, subject, start);
            XiValue *le = xi_binary(l->func, l->cur_block, XI_LE,
                                     l->type_bool, subject, end);
            return xi_binary(l->func, l->cur_block, XI_BAND, l->type_bool, ge, le);
        }

        case AST_PATTERN_MULTI: {
            /* pattern1 | pattern2 | ... → OR chain */
            PatternMultiNode *mp = &pattern->as.pattern_multi;
            XiValue *result = NULL;
            for (int i = 0; i < mp->count; i++) {
                XiValue *test = lower_pattern_test(l, subject, mp->patterns[i]);
                if (!test) continue;
                if (!result)
                    result = test;
                else
                    result = xi_binary(l->func, l->cur_block, XI_BOR,
                                        l->type_bool, result, test);
            }
            return result ? result : xi_const_bool(l->func, l->cur_block,
                                                     false, l->type_bool);
        }

        default:
            /* Unknown pattern — treat as no match */
            return xi_const_bool(l->func, l->cur_block, false, l->type_bool);
    }
}

/*
 * match expr { arm1, arm2, ..., armN }
 *
 * Lowered as a chain of conditional blocks:
 *   test arm0 → body0 | test arm1 → body1 | ... | default
 * All body blocks jump to a single merge block with a PHI for the result.
 */
static XiValue *lower_match(XiLower *l, AstNode *node) {
    MatchExprNode *m = &node->as.match_expr;
    XiValue *subject = lower_expr(l, m->expr);
    if (!subject) return NULL;

    struct XrType *result_type = node_type(l, node);
    XiBlock *merge = xi_block_new(l->func);
    int arm_count = m->arm_count;

    /* Track body exit blocks and their result values for the PHI */
    XiBlock *body_exits[32];
    XiValue *body_vals[32];
    int exit_count = 0;
    int max_arms = arm_count > 32 ? 32 : arm_count;

    for (int i = 0; i < max_arms; i++) {
        AstNode *arm_node = m->arms[i];
        MatchArmNode *arm = &arm_node->as.match_arm;

        /* Generate pattern test in current block */
        XiValue *test = lower_pattern_test(l, subject, arm->pattern);

        /* Apply guard: test = test && guard */
        if (arm->guard && test) {
            XiValue *guard = lower_expr(l, arm->guard);
            if (guard)
                test = xi_binary(l->func, l->cur_block, XI_BAND,
                                  l->type_bool, test, guard);
        }

        /* Last arm: no condition check needed (fallthrough / default) */
        bool is_last = (i == max_arms - 1);

        if (is_last || !test) {
            /* Unconditional: lower body directly */
            XiValue *val = lower_expr(l, arm->body);
            if (l->cur_block) {
                if (exit_count < 32) {
                    body_exits[exit_count] = l->cur_block;
                    body_vals[exit_count] = val;
                    exit_count++;
                }
                xi_block_set_jump(l->cur_block, merge);
            }
        } else {
            /* Conditional: create body and next-test blocks */
            XiBlock *body_blk = xi_block_new(l->func);
            XiBlock *next_blk = xi_block_new(l->func);
            xi_block_set_if(l->cur_block, test, body_blk, next_blk);
            braun_seal_block(l, body_blk);
            braun_seal_block(l, next_blk);

            /* Lower arm body */
            l->cur_block = body_blk;
            XiValue *val = lower_expr(l, arm->body);
            if (l->cur_block) {
                if (exit_count < 32) {
                    body_exits[exit_count] = l->cur_block;
                    body_vals[exit_count] = val;
                    exit_count++;
                }
                xi_block_set_jump(l->cur_block, merge);
            }

            /* Continue testing in next block */
            l->cur_block = next_blk;
        }
    }

    /* If we fell through all arms without matching, jump to merge with null */
    if (l->cur_block && l->cur_block != merge) {
        if (exit_count < 32) {
            body_exits[exit_count] = l->cur_block;
            body_vals[exit_count] = xi_const_null(l->func, l->cur_block, l->type_null);
            exit_count++;
        }
        xi_block_set_jump(l->cur_block, merge);
    }

    braun_seal_block(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
    if (!l->cur_block) return NULL;

    /* Create PHI for the match result */
    if (merge->npreds == 1) {
        /* Single predecessor: no PHI needed */
        return (exit_count > 0) ? body_vals[0] : NULL;
    }

    XiPhi *phi = xi_phi_new(l->func, merge, result_type, merge->npreds);
    if (!phi) return NULL;
    for (uint16_t p = 0; p < merge->npreds; p++) {
        phi->value.args[p] = xi_const_null(l->func, merge, l->type_null);
        for (int j = 0; j < exit_count; j++) {
            if (merge->preds[p] == body_exits[j]) {
                phi->value.args[p] = body_vals[j] ? body_vals[j]
                    : xi_const_null(l->func, merge, l->type_null);
                break;
            }
        }
    }
    return &phi->value;
}

/* Main expression dispatcher */
static XiValue *lower_expr(XiLower *l, AstNode *node) {
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
            return lower_expr(l, node->as.grouping);

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
            return lower_match(l, node);

        /* Function / closure */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return lower_function_decl(l, node);

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

        default:
            /* Unsupported expression: emit null placeholder */
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

/* ========== Statement Lowering ========== */

static void lower_var_decl(XiLower *l, AstNode *node) {
    const char *name = node->as.var_decl.name;
    struct XrType *type = node_type(l, node);

    int var_id = var_lookup_or_create(l, name, type);

    if (node->as.var_decl.initializer) {
        XiValue *init = lower_expr(l, node->as.var_decl.initializer);
        if (init) braun_write(l, var_id, l->cur_block, init);
    } else {
        /* Uninitialized: default to null */
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        braun_write(l, var_id, l->cur_block, null_val);
    }
}

static void lower_print(XiLower *l, AstNode *node) {
    PrintNode *p = &node->as.print_stmt;
    uint16_t nargs = (uint16_t) p->expr_count;

    /* Evaluate all arguments first so they appear before PRINT in the block */
    XiValue *arg_vals[16];
    int n = nargs > 16 ? 16 : nargs;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = lower_expr(l, p->exprs[i]);
    }

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT,
                               l->type_void, nargs);
    if (!v) return;

    for (int i = 0; i < n; i++) {
        v->args[i] = arg_vals[i];
    }
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
}

static void lower_throw(XiLower *l, AstNode *node) {
    ThrowStmtNode *t = &node->as.throw_stmt;
    XiValue *val = lower_expr(l, t->expression);
    if (!val) return;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_THROW, l->type_void, 1);
    if (!v) return;
    v->args[0] = val;
    v->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    v->line = (uint32_t) node->line;

    /* Throw terminates the block — no successors */
    l->cur_block->kind = XI_BLOCK_UNREACHABLE;
    l->cur_block->control = val;
    l->cur_block = NULL;
}

static void lower_return(XiLower *l, AstNode *node) {
    ReturnStmtNode *ret = &node->as.return_stmt;
    XiValue *val = NULL;

    if (ret->value_count > 0 && ret->values[0]) {
        val = lower_expr(l, ret->values[0]);
    }

    xi_block_set_return(l->cur_block, val);
    l->cur_block = NULL;  /* mark as terminated — subsequent stmts are dead */
}

static void lower_block(XiLower *l, AstNode *node) {
    lower_stmts(l, node->as.block.statements, node->as.block.count);
}

static void lower_if(XiLower *l, AstNode *node) {
    IfStmtNode *s = &node->as.if_stmt;

    XiValue *cond = lower_expr(l, s->condition);
    if (!cond || !l->cur_block) return;

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *else_blk = s->else_branch ? xi_block_new(l->func) : merge;

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);

    /* then_blk has 1 pred (cur_block) — seal immediately */
    braun_seal_block(l, then_blk);
    if (s->else_branch)
        braun_seal_block(l, else_blk);

    /* Then branch */
    l->cur_block = then_blk;
    lower_stmt(l, s->then_branch);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    /* Else branch */
    if (s->else_branch) {
        l->cur_block = else_blk;
        lower_stmt(l, s->else_branch);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* merge preds now fully known — seal and continue */
    braun_seal_block(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

static void lower_while(XiLower *l, AstNode *node) {
    WhileStmtNode *s = &node->as.while_stmt;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* Jump to condition — cond_blk is a loop header (unsealed) */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition: cond_blk NOT sealed yet (back edge pending) */
    l->cur_block = cond_blk;
    XiValue *cond = lower_expr(l, s->condition);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    /* body_blk has 1 pred (cond_blk) — seal immediately */
    braun_seal_block(l, body_blk);

    /* Body */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = cond_blk;

    l->cur_block = body_blk;
    lower_stmt(l, s->body);
    if (l->cur_block)  /* back edge */
        xi_block_set_jump(l->cur_block, cond_blk);

    /* All preds of cond_blk now known (entry + back edge) — seal */
    braun_seal_block(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    braun_seal_block(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

static void lower_for(XiLower *l, AstNode *node) {
    ForStmtNode *s = &node->as.for_stmt;

    /* Initializer in current block */
    if (s->initializer)
        lower_stmt(l, s->initializer);
    if (!l->cur_block) return;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *incr_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* cond_blk is a loop header — do NOT seal yet */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition */
    l->cur_block = cond_blk;
    if (s->condition) {
        XiValue *cond = lower_expr(l, s->condition);
        if (cond)
            xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);
    } else {
        xi_block_set_jump(l->cur_block, body_blk);
    }

    braun_seal_block(l, body_blk);

    /* Body */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = incr_blk;

    l->cur_block = body_blk;
    lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    braun_seal_block(l, incr_blk);

    /* Increment */
    l->cur_block = incr_blk;
    if (s->increment) {
        if (incr_blk->npreds > 0)
            lower_expr(l, s->increment);
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    /* cond_blk back edge now added — seal */
    braun_seal_block(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    braun_seal_block(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/*
 * for (item in collection) { body }
 *
 * Lowered to:
 *   iter = ITER_NEW collection
 *   jmp cond_blk
 * cond_blk:             ; loop header (unsealed)
 *   valid = ITER_VALID iter
 *   if valid → body_blk, exit_blk
 * body_blk:
 *   item = ITER_NEXT iter
 *   ... body ...
 *   jmp cond_blk        ; back edge
 * exit_blk:
 *   ...
 */
static void lower_for_in(XiLower *l, AstNode *node) {
    ForInStmtNode *s = &node->as.for_in_stmt;

    /* Evaluate collection and create iterator */
    XiValue *coll = lower_expr(l, s->collection);
    if (!coll || !l->cur_block) return;

    struct XrType *iter_type = l->type_any;  /* opaque iterator */
    XiValue *iter = xi_value_new(l->func, l->cur_block, XI_ITER_NEW, iter_type, 1);
    if (!iter) return;
    iter->args[0] = coll;
    iter->line = (uint32_t) node->line;

    XiBlock *cond_blk = xi_block_new(l->func);
    XiBlock *body_blk = xi_block_new(l->func);
    XiBlock *exit_blk = xi_block_new(l->func);

    /* cond_blk is a loop header — do NOT seal yet */
    xi_block_set_jump(l->cur_block, cond_blk);

    /* Condition: ITER_VALID */
    l->cur_block = cond_blk;
    XiValue *valid = xi_value_new(l->func, l->cur_block, XI_ITER_VALID, l->type_bool, 1);
    if (valid) {
        valid->args[0] = iter;
        xi_block_set_if(l->cur_block, valid, body_blk, exit_blk);
    }

    braun_seal_block(l, body_blk);

    /* Body: get current item via ITER_NEXT */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = cond_blk;

    l->cur_block = body_blk;
    struct XrType *item_type = s->item_type ? s->item_type : l->type_any;
    XiValue *item = xi_value_new(l->func, l->cur_block, XI_ITER_NEXT, item_type, 1);
    if (item) {
        item->args[0] = iter;
        item->line = (uint32_t) node->line;
    }

    /* Register loop variable */
    int var_id = var_lookup_or_create(l, s->item_name, item_type);
    if (item) braun_write(l, var_id, l->cur_block, item);

    lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, cond_blk);

    /* All preds of cond_blk now known — seal */
    braun_seal_block(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    braun_seal_block(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

static void lower_break(XiLower *l) {
    if (l->break_target && l->cur_block) {
        xi_block_set_jump(l->cur_block, l->break_target);
        l->cur_block = NULL;
    }
}

static void lower_continue(XiLower *l) {
    if (l->continue_target && l->cur_block) {
        xi_block_set_jump(l->cur_block, l->continue_target);
        l->cur_block = NULL;
    }
}

/*
 * try { ... } catch (e) { ... } finally { ... }
 *
 * Lowered structurally (exception dispatch is runtime):
 *   try_blk:  body instructions
 *   catch_blk: catch variable bound to exception value
 *   finally_blk: always executed
 *   merge: continuation
 */
static void lower_try_catch(XiLower *l, AstNode *node) {
    TryCatchNode *tc = &node->as.try_catch;

    XiBlock *try_blk = xi_block_new(l->func);
    XiBlock *catch_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *finally_blk = tc->finally_body ? xi_block_new(l->func) : NULL;
    XiBlock *normal_target = finally_blk ? finally_blk : merge;

    /* Jump into try block */
    xi_block_set_jump(l->cur_block, try_blk);
    braun_seal_block(l, try_blk);

    /* Lower try body */
    l->cur_block = try_blk;
    lower_stmt(l, tc->try_body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, normal_target);

    /* Seal catch block (1 pred: implicit exception edge) */
    braun_seal_block(l, catch_blk);
    l->cur_block = catch_blk;

    /* Bind catch variable to a PARAM-like placeholder */
    if (tc->catch_var) {
        struct XrType *err_type = l->type_any;
        XiValue *exc_val = xi_value_new(l->func, l->cur_block, XI_PARAM, err_type, 0);
        if (exc_val) {
            exc_val->aux_int = -1;  /* sentinel: catch param, not function param */
            exc_val->line = (uint32_t) tc->catch_var_line;
        }
        int var_id = var_lookup_or_create(l, tc->catch_var, err_type);
        if (exc_val) braun_write(l, var_id, l->cur_block, exc_val);
    }

    lower_stmt(l, tc->catch_body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, normal_target);

    /* Finally block (optional) */
    if (finally_blk) {
        braun_seal_block(l, finally_blk);
        l->cur_block = finally_blk;
        lower_stmt(l, tc->finally_body);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    braun_seal_block(l, merge);
    l->cur_block = (merge->npreds > 0) ? merge : NULL;
}

/* Main statement dispatcher */
static void lower_stmt(XiLower *l, AstNode *node) {
    if (!node) return;
    if (!l->cur_block) return;  /* dead code */

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            lower_var_decl(l, node);
            break;

        case AST_EXPR_STMT:
            lower_expr(l, node->as.expr_stmt);
            break;

        case AST_PRINT_STMT:
            lower_print(l, node);
            break;

        case AST_RETURN_STMT:
            lower_return(l, node);
            break;

        case AST_BLOCK:
            lower_block(l, node);
            break;

        case AST_IF_STMT:
            lower_if(l, node);
            break;

        case AST_WHILE_STMT:
            lower_while(l, node);
            break;

        case AST_FOR_STMT:
            lower_for(l, node);
            break;

        case AST_FOR_IN_STMT:
            lower_for_in(l, node);
            break;

        case AST_BREAK_STMT:
            lower_break(l);
            break;

        case AST_CONTINUE_STMT:
            lower_continue(l);
            break;

        case AST_THROW_STMT:
            lower_throw(l, node);
            break;

        case AST_TRY_CATCH:
            lower_try_catch(l, node);
            break;

        /* Function declaration as statement */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            lower_function_decl(l, node);
            break;

        case AST_DEFER_STMT:
            lower_defer(l, node);
            break;

        /* Expressions that appear as statements (assignment, call, etc.) */
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_CALL_EXPR:
        case AST_INC:
        case AST_DEC:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
        case AST_GO_EXPR:
        case AST_AWAIT_EXPR:
            lower_expr(l, node);
            break;

        default:
            /* Unsupported statement: skip */
            break;
    }
}

static void lower_stmts(XiLower *l, AstNode **stmts, int count) {
    for (int i = 0; i < count; i++) {
        if (!l->cur_block) break;  /* dead code after return/break */
        lower_stmt(l, stmts[i]);
    }
}

/* ========== Context Initialization ========== */

static void lower_init(XiLower *l, struct XaAnalyzer *analyzer,
                        struct XrayIsolate *isolate) {
    memset(l, 0, sizeof(XiLower));
    l->analyzer = analyzer;
    l->isolate = isolate;

    /* Heap-allocate the 2D def map (256*256 pointers = 512KB) */
    size_t def_map_size = (size_t)XI_LOWER_MAX_VARS * XI_LOWER_MAX_BLOCKS;
    l->var_defs = (XiValue **) xr_calloc(def_map_size, sizeof(XiValue *));
    XR_CHECK(l->var_defs != NULL, "xi_lower: failed to allocate var_defs");

    /* Cache singleton types */
    l->type_int = xr_type_new_int(isolate);
    l->type_float = xr_type_new_float(isolate);
    l->type_bool = xr_type_new_bool(isolate);
    l->type_string = xr_type_new_string(isolate);
    l->type_null = xr_type_new_null(isolate);
    l->type_void = xr_type_new_void(isolate);
    l->type_any = xr_type_new_unknown(isolate);
}

static void lower_cleanup(XiLower *l) {
    if (l->var_defs) {
        xr_free(l->var_defs);
        l->var_defs = NULL;
    }
}

/* ========== Public API ========== */

XiFunc *xi_lower_func(AstNode *func_node, struct XaAnalyzer *analyzer,
                       struct XrayIsolate *isolate) {
    XR_CHECK(func_node != NULL, "xi_lower_func: func_node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_func: analyzer is NULL");
    XR_CHECK(func_node->type == AST_FUNCTION_DECL ||
             func_node->type == AST_FUNCTION_EXPR,
             "xi_lower_func: not a function node");

    FunctionDeclNode *fdecl = &func_node->as.function_decl;

    XiLower l;
    lower_init(&l, analyzer, isolate);

    /* Determine return type */
    struct XrType *ret_type = fdecl->return_type;
    if (!ret_type) ret_type = l.type_void;

    l.func = xi_func_new(fdecl->name ? fdecl->name : "<anonymous>", ret_type);
    if (!l.func) { lower_cleanup(&l); return NULL; }
    l.func->analyzer = analyzer;

    /* Entry block (no predecessors — seal immediately) */
    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* Lower parameters */
    l.func->nparams = (uint16_t) fdecl->param_count;
    if (fdecl->param_count > 0) {
        l.func->params = (XiValue **) xr_calloc(
            fdecl->param_count, sizeof(XiValue *));
        if (!l.func->params) { xi_func_free(l.func); lower_cleanup(&l); return NULL; }
    }

    for (int i = 0; i < fdecl->param_count; i++) {
        XrParamNode *p = fdecl->params[i];
        struct XrType *ptype = p->type ? p->type : l.type_any;

        XiValue *param_val = xi_param(l.func, entry, (uint16_t) i, ptype);
        l.func->params[i] = param_val;

        /* Register parameter in Braun SSA */
        int var_id = var_lookup_or_create(&l, p->name, ptype);
        braun_write(&l, var_id, entry, param_val);
    }

    /* Lower function body */
    if (fdecl->body) {
        lower_stmt(&l, fdecl->body);
    }

    /* If last block not terminated, add implicit void return */
    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    lower_cleanup(&l);
    return l.func;
}

XiFunc *xi_lower_program(AstNode *program_node, struct XaAnalyzer *analyzer,
                          struct XrayIsolate *isolate) {
    XR_CHECK(program_node != NULL, "xi_lower_program: node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_program: analyzer is NULL");

    XiLower l;
    lower_init(&l, analyzer, isolate);

    l.func = xi_func_new("<main>", l.type_void);
    if (!l.func) { lower_cleanup(&l); return NULL; }
    l.func->analyzer = analyzer;
    l.func->nparams = 0;
    l.func->params = NULL;

    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* Lower all top-level statements */
    if (program_node->type == AST_BLOCK) {
        lower_stmts(&l, program_node->as.block.statements,
                     program_node->as.block.count);
    } else {
        /* ProgramNode */
        lower_stmts(&l, program_node->as.program.statements,
                     program_node->as.program.count);
    }

    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    lower_cleanup(&l);
    return l.func;
}
