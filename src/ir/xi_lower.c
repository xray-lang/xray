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

#include <string.h>
#include <stdio.h>

/* ========== Forward Declarations ========== */

static XiValue *lower_short_circuit(XiLower *l, AstNode *node);
static void lower_stmts(XiLower *l, AstNode **stmts, int count);

/* ========== Braun SSA: Variable Management ========== */

/* Find or create a variable ID for the given name. */
XR_FUNC int xi_lower_var_create(XiLower *l, const char *name, struct XrType *type) {
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


/* ========== Shared Variable Lookup ========== */

/* Walk the parent chain to find a program-level shared variable.
 * Returns the shared index (>=0) if found, or -1 if not.
 * Sets *out_type to the variable type when found. */
static int find_shared_var(XiLower *l, const char *name, struct XrType **out_type) {
    for (XiLower *p = l->parent; p; p = p->parent) {
        if (!p->is_program) continue;
        int var_id = var_find(p, name);
        if (var_id >= 0 && p->shared_map[var_id] >= 0) {
            if (out_type) *out_type = p->vars[var_id].type;
            return p->shared_map[var_id];
        }
    }
    return -1;
}

/* ========== Upvalue Resolution ========== */

/*
 * Resolve a variable from an enclosing scope, recording captures at each
 * level.  Returns the local upvalue index in the immediate child, or -1
 * if the variable is not found in any ancestor.
 *
 * Algorithm (same as Lua/xray flat-upvalue scheme):
 *   1. Check parent's local variables → capture as SRC_REG.
 *   2. Recursively resolve in grandparent → capture as SRC_UPVAL.
 *   3. Each intermediate level records its own capture entry.
 *
 * For program-level shared variables, the caller uses find_shared_var()
 * to emit XI_GET_SHARED directly (no upvalue capture needed).
 */
static int resolve_upvalue(XiLower *l, const char *name, struct XrType **out_type) {
    XiLower *parent = l->parent;
    if (!parent) return -1;

    /* Program-level shared variables are handled via XI_GET_SHARED
     * in lower_variable/lower_assignment, not via upvalue capture. */
    if (parent->is_program) return -1;

    /* Dedup: if this variable is already captured, return existing index */
    for (uint16_t ci = 0; ci < l->func->ncaptures; ci++) {
        if (l->func->captures[ci].name &&
            strcmp(l->func->captures[ci].name, name) == 0) {
            if (out_type) *out_type = l->func->captures[ci].type;
            return (int)ci;
        }
    }

    /* Check if the variable exists as a local in the immediate parent */
    int var_id = var_find(parent, name);
    if (var_id >= 0) {
        /* Read the current SSA value from the parent's scope.  The value's
         * register will be resolved at emit time via reg_of(). */
        XiValue *parent_val = xi_lower_braun_read(parent, var_id, parent->cur_block);
        if (l->func->ncaptures >= XI_MAX_CAPTURES) return -1;
        int idx = l->func->ncaptures;
        l->func->captures[idx].source = XI_CAPTURE_SRC_REG;
        l->func->captures[idx].index = 0;
        l->func->captures[idx].name = name;
        l->func->captures[idx].type = parent->vars[var_id].type;
        l->func->captures[idx].value = parent_val;
        l->func->ncaptures++;
        if (out_type) *out_type = parent->vars[var_id].type;
        return idx;
    }

    /* Not a local in parent — try grandparent (transitive capture) */
    int parent_upval = resolve_upvalue(parent, name, out_type);
    if (parent_upval >= 0) {
        if (l->func->ncaptures >= XI_MAX_CAPTURES) return -1;
        int idx = l->func->ncaptures;
        l->func->captures[idx].source = XI_CAPTURE_SRC_UPVAL;
        l->func->captures[idx].index = (uint8_t)parent_upval;
        l->func->captures[idx].name = name;
        l->func->captures[idx].type = out_type ? *out_type : l->type_any;
        l->func->ncaptures++;
        return idx;
    }

    return -1;
}

/* Write: currentDef[var][block] = value */
XR_FUNC void xi_lower_braun_write(XiLower *l, int var_id, XiBlock *blk, XiValue *val) {
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

XR_FUNC XiValue *xi_lower_braun_read(XiLower *l, int var_id, XiBlock *blk) {
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
    xi_lower_braun_write(l, var_id, phi->value.block, same);
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
        val = xi_lower_braun_read(l, var_id, blk->preds[0]);
    } else {
        /* Multiple predecessors: insert phi, then fill operands. */
        XiPhi *phi = xi_phi_new(l->func, blk, type, blk->npreds);
        /* Write before filling to break recursive cycles */
        xi_lower_braun_write(l, var_id, blk, &phi->value);
        val = add_phi_operands(l, var_id, phi);
    }

    xi_lower_braun_write(l, var_id, blk, val);
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
        phi->value.args[i] = xi_lower_braun_read(l, var_id, blk->preds[i]);
    }
    return try_remove_trivial_phi(l, var_id, phi);
}

/*
 * Seal a block: all predecessors are now known.
 * Complete any incomplete phis that were deferred.
 */
XR_FUNC void xi_lower_braun_seal(XiLower *l, XiBlock *blk) {
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
XR_FUNC struct XrType *xi_lower_node_type(XiLower *l, AstNode *node) {
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

/* Short-circuit && and || use control flow (like if-expressions). */
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

    /* Evaluate RHS */
    l->cur_block = eval_rhs;
    XiValue *rhs = xi_lower_expr(l, node->as.binary.right);
    XiBlock *rhs_exit = l->cur_block;
    xi_block_set_jump(rhs_exit, merge);

    /* Skip block just jumps to merge */
    xi_block_set_jump(skip, merge);

    xi_lower_braun_seal(l, merge);

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
    int var_id = var_find(l, name);
    if (var_id >= 0) {
        return xi_lower_braun_read(l, var_id, l->cur_block);
    }

    /* Check for program-level shared variable (supports forward references) */
    struct XrType *shared_type = NULL;
    int shared_idx = find_shared_var(l, name, &shared_type);
    if (shared_idx >= 0) {
        if (!shared_type) shared_type = l->type_any;
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                                   shared_type, 0);
        if (v) v->aux_int = shared_idx;
        return v;
    }

    /* Not found locally — try upvalue capture from enclosing scope */
    struct XrType *upval_type = NULL;
    int upval_idx = resolve_upvalue(l, name, &upval_type);
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
    XiValue *val = xi_lower_expr(l, node->as.assignment.value);
    if (!val) return NULL;

    int var_id = var_find(l, name);
    if (var_id >= 0) {
        xi_lower_braun_write(l, var_id, l->cur_block, val);

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
    int shared_idx = find_shared_var(l, name, NULL);
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
    int upval_idx = resolve_upvalue(l, name, NULL);
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
    var_id = xi_lower_var_create(l, name, val->type);
    xi_lower_braun_write(l, var_id, l->cur_block, val);
    return val;
}

/* Map compound assignment operator token to Xi binary op */
static uint16_t compound_op_to_xi(int tok) {
    switch (tok) {
        case TK_PLUS_ASSIGN:  return XI_ADD;
        case TK_MINUS_ASSIGN: return XI_SUB;
        case TK_MUL_ASSIGN:   return XI_MUL;
        case TK_DIV_ASSIGN:   return XI_DIV;
        case TK_MOD_ASSIGN:   return XI_MOD;
        default:              return XI_ADD;
    }
}

static XiValue *lower_compound_assignment(XiLower *l, AstNode *node) {
    const char *name = node->as.compound_assignment.name;
    XiValue *rhs = xi_lower_expr(l, node->as.compound_assignment.value);
    if (!rhs) return NULL;

    uint16_t op = compound_op_to_xi(node->as.compound_assignment.op);
    struct XrType *result_type = xa_analyzer_get_node_type(l->analyzer, node);

    /* Local variable */
    int var_id = var_find(l, name);
    if (var_id >= 0) {
        XiValue *cur = xi_lower_braun_read(l, var_id, l->cur_block);
        if (!cur) return NULL;
        if (!result_type)
            result_type = infer_binary_type(l, node->type, cur->type, rhs->type);
        XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, rhs);
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

    /* Program-level shared variable from nested scope */
    int shared_idx = find_shared_var(l, name, NULL);
    if (shared_idx >= 0) {
        /* Read current value from shared array */
        XiValue *cur = xi_value_new(l->func, l->cur_block, XI_GET_SHARED,
                                     result_type ? result_type : l->type_any, 0);
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
    int upval_idx = resolve_upvalue(l, name, &upval_type);
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
    int var_id = var_find(l, name);
    if (var_id < 0)
        return xi_const_null(l->func, l->cur_block, l->type_null);

    XiValue *cur = xi_lower_braun_read(l, var_id, l->cur_block);
    if (!cur) return NULL;

    XiValue *one = xi_const_int(l->func, l->cur_block, 1, l->type_int);
    uint16_t op = (node->type == AST_INC) ? XI_ADD : XI_SUB;
    struct XrType *result_type = cur->type ? cur->type : l->type_int;

    XiValue *result = xi_binary(l->func, l->cur_block, op, result_type, cur, one);
    if (result)
        xi_lower_braun_write(l, var_id, l->cur_block, result);
    return result;
}

static XiValue *lower_member_access(XiLower *l, AstNode *node) {
    MemberAccessNode *ma = &node->as.member_access;
    XiValue *obj = xi_lower_expr(l, ma->object);
    if (!obj) return NULL;

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v) return NULL;
    v->args[0] = obj;
    v->aux = (void *) ma->name;  /* field name (borrowed from AST) */
    v->line = (uint32_t) node->line;
    return v;
}

static XiValue *lower_member_set(XiLower *l, AstNode *node) {
    MemberSetNode *ms = &node->as.member_set;
    XiValue *obj = xi_lower_expr(l, ms->object);
    XiValue *val = xi_lower_expr(l, ms->value);
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
        v->aux = (void *)ma->name;
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
/* Internal: lower a function with optional parent context for upvalue capture */
static XiFunc *lower_func_impl(AstNode *func_node, struct XaAnalyzer *analyzer,
                                struct XrayIsolate *isolate, XiLower *parent);

static XiValue *lower_function_decl(XiLower *l, AstNode *node) {
    /* Recursively lower the function body into a child XiFunc,
     * passing 'l' as parent so the child can resolve upvalue captures. */
    XiFunc *child = lower_func_impl(node, l->analyzer, l->isolate, l);
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
        int var_id = xi_lower_var_create(l, fdecl->name, fn_type);
        xi_lower_braun_write(l, var_id, l->cur_block, v);

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

    int var_id = xi_lower_var_create(l, cname, l->type_any);
    XiValue *cls = xi_lower_braun_read(l, var_id, l->cur_block);

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

static void lower_defer(XiLower *l, AstNode *node) {
    DeferStmtNode *d = &node->as.defer_stmt;
    XiValue *expr = xi_lower_expr(l, d->expr);
    if (!expr || !l->cur_block) return;

    XiValue *v = xi_value_new(l->func, l->cur_block, XI_DEFER, l->type_void, 1);
    if (!v) return;
    v->args[0] = expr;
    v->flags |= XI_FLAG_SIDE_EFFECT;
    v->line = (uint32_t) node->line;
}

static XiValue *lower_is_expr(XiLower *l, AstNode *node) {
    IsExprNode *is = &node->as.is_expr;
    XiValue *val = xi_lower_expr(l, is->expr);
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

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *obj = xi_value_new(l->func, l->cur_block, XI_ALLOC, result_type, 1);
    if (!obj) return NULL;
    obj->args[0] = cap;
    obj->aux = (void *) sl->struct_name;
    obj->line = (uint32_t) node->line;

    for (int i = 0; i < n; i++) {
        /* Use INDEX_SET with string key for map-backed struct literals.
         * STORE_FIELD/SETPROP only works on class instances. */
        XiValue *key = xi_const_str(l->func, l->cur_block,
                                     sl->field_names[i], l->type_string);
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET,
                                     l->type_void, 3);
        if (!set) break;
        set->args[0] = obj;
        set->args[1] = key;
        set->args[2] = val_vals[i];
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
    /* 'this' is the first implicit parameter (index 0) in methods */
    struct XrType *this_type = xi_lower_node_type(l, node);
    int var_id = xi_lower_var_create(l, "this", this_type);
    return xi_lower_braun_read(l, var_id, l->cur_block);
}

static XiValue *lower_super_call(XiLower *l, AstNode *node) {
    SuperCallNode *sc = &node->as.super_call;
    XiValue *arg_vals[32];
    int n = sc->arg_count > 32 ? 32 : sc->arg_count;
    for (int i = 0; i < n; i++)
        arg_vals[i] = xi_lower_expr(l, sc->arguments[i]);

    /* 'this' is receiver for super call */
    struct XrType *this_type = l->type_any;
    int var_id = xi_lower_var_create(l, "this", this_type);
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
    int var_id = xi_lower_var_create(l, ea->enum_name, l->type_any);
    XiValue *enum_val = xi_lower_braun_read(l, var_id, l->cur_block);

    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_LOAD_FIELD, result_type, 1);
    if (!v) return NULL;
    v->args[0] = enum_val;
    v->aux = (void *) ea->member_name;
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
static void lower_enum_decl(XiLower *l, AstNode *node) {
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
    for (int i = 0; i < n; i++) {
        EnumMemberNode *m = &ed->members[i]->as.enum_member;
        names[i] = strdup(m->name);
        if (m->value) {
            values[i] = enum_eval_const(l, m->value);
            if (XR_IS_INT(values[i]))
                auto_val = XR_TO_INT(values[i]) + 1;
        } else {
            values[i] = xr_int(auto_val);
            auto_val++;
        }
    }

    XrEnumType *et = xr_enum_type_new(l->isolate, ed->name, XR_TID_INT,
                                       names, values, n);
    /* names/values ownership transferred to xr_enum_type_new */

    /* Store as XI_CONST with type_any (emitter handles via LOADK) */
    XiValue *cv = xi_value_new(l->func, l->cur_block, XI_CONST, l->type_any, 0);
    if (!cv) return;
    cv->aux = (void *)et;
    cv->line = (uint32_t)node->line;

    /* Write to shared variable so enum access resolves correctly */
    int var_id = xi_lower_var_create(l, ed->name, l->type_any);
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
    v->aux = (void *) ec->enum_name;
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

/* lower_select, lower_scope_block → xi_lower_stmt.c */

static void lower_yield_stmt(XiLower *l) {
    XiValue *v = xi_value_new(l->func, l->cur_block, XI_YIELD, l->type_void, 0);
    if (v) v->flags |= XI_FLAG_SIDE_EFFECT;
}

/*
 * Bind destructure pattern elements to extracted values from 'src'.
 * Array patterns: INDEX_GET by position.
 * Object patterns: LOAD_FIELD by field name.
 * Identifier patterns: bind directly.
 */
static void lower_destructure_bind(XiLower *l, XrDestructurePattern *pat,
                                    XiValue *src) {
    if (!pat || !src || !l->cur_block) return;

    switch (pat->type) {
        case PATTERN_ARRAY: {
            int n = pat->as.array.element_count;
            for (int i = 0; i < n; i++) {
                XrDestructurePattern *elem = pat->as.array.elements[i];
                if (!elem) continue;
                XiValue *idx = xi_const_int(l->func, l->cur_block, i, l->type_int);
                XiValue *val = xi_value_new(l->func, l->cur_block,
                                             XI_INDEX_GET, l->type_any, 2);
                if (val) { val->args[0] = src; val->args[1] = idx; }
                lower_destructure_bind(l, elem, val);
            }
            break;
        }
        case PATTERN_OBJECT: {
            int n = pat->as.object.field_count;
            for (int i = 0; i < n; i++) {
                char *fname = pat->as.object.field_names[i];
                XrDestructurePattern *sub = pat->as.object.patterns[i];
                if (!fname) continue;
                /* Use INDEX_GET with string key — works for both JSON objects
                 * and maps (Xi lowers object literals as NEWMAP). */
                XiValue *key = xi_const_str(l->func, l->cur_block,
                                             fname, l->type_string);
                XiValue *val = xi_value_new(l->func, l->cur_block,
                                             XI_INDEX_GET, l->type_any, 2);
                if (val) { val->args[0] = src; val->args[1] = key; }
                lower_destructure_bind(l, sub, val);
            }
            break;
        }
        case PATTERN_IDENTIFIER: {
            const char *name = pat->as.identifier.name;
            if (!name) break;
            int var_id = xi_lower_var_create(l, name, l->type_any);
            xi_lower_braun_write(l, var_id, l->cur_block, src);
            break;
        }
        default:
            break;
    }
}

/* Destructure declaration: let [a, b] = expr or let {x, y} = expr */
static void lower_destructure_decl(XiLower *l, AstNode *node) {
    DestructureDeclNode *dd = &node->as.destructure_decl;
    XiValue *init = xi_lower_expr(l, dd->initializer);
    if (!init || !dd->pattern) return;
    lower_destructure_bind(l, dd->pattern, init);
}

/* Destructure assignment: [a, b] = [b, a] */
static void lower_destructure_assign(XiLower *l, AstNode *node) {
    DestructureAssignNode *da = &node->as.destructure_assign;
    XiValue *rhs = xi_lower_expr(l, da->value);
    if (!rhs || !da->pattern) return;
    lower_destructure_bind(l, da->pattern, rhs);
}

/* Multi-value declaration: let a, b = foo()
 *
 * When a single call expression provides all values, the VM returns
 * multiple results to consecutive registers (OP_CALL C=nresults).
 * We encode nresults in XI_CALL's aux_int bits 8-15 and use
 * XI_EXTRACT to reference the i-th result register. */
static void lower_multi_var_decl(XiLower *l, AstNode *node) {
    MultiVarDeclNode *mv = &node->as.multi_var_decl;

    /* Single call returning multiple values: let x, y = pair() */
    if (mv->value_count == 1 && mv->name_count > 1 &&
        mv->values[0]->type == AST_CALL_EXPR) {
        XiValue *call_val = xi_lower_expr(l, mv->values[0]);
        if (!call_val) return;

        /* Encode return count in aux_int upper bits (preserving self_call flag) */
        int flags = (int)(call_val->aux_int & 0xFF);
        call_val->aux_int = flags | (mv->name_count << 8);

        /* First name binds to the call result directly */
        struct XrType *vtype = call_val->type ? call_val->type : l->type_any;
        int var0 = xi_lower_var_create(l, mv->names[0], vtype);
        xi_lower_braun_write(l, var0, l->cur_block, call_val);

        /* Remaining names extracted from consecutive result registers */
        for (int i = 1; i < mv->name_count; i++) {
            XiValue *ext = xi_value_new(l->func, l->cur_block,
                                         XI_EXTRACT, l->type_any, 1);
            if (!ext) break;
            ext->args[0] = call_val;
            ext->aux_int = i;
            int var_i = xi_lower_var_create(l, mv->names[i], l->type_any);
            xi_lower_braun_write(l, var_i, l->cur_block, ext);
        }
        return;
    }

    /* General case: evaluate each value expression */
    for (int i = 0; i < mv->name_count && i < mv->value_count; i++) {
        XiValue *val = xi_lower_expr(l, mv->values[i]);
        struct XrType *vtype = val ? val->type : l->type_any;
        int var_id = xi_lower_var_create(l, mv->names[i], vtype);
        if (val) xi_lower_braun_write(l, var_id, l->cur_block, val);
    }
    /* Names without values get null */
    for (int i = mv->value_count; i < mv->name_count; i++) {
        int var_id = xi_lower_var_create(l, mv->names[i], l->type_any);
        XiValue *null_val = xi_const_null(l->func, l->cur_block, l->type_null);
        xi_lower_braun_write(l, var_id, l->cur_block, null_val);
    }
}

/* Multi-value assignment: a, b = b, a */
static void lower_multi_assign(XiLower *l, AstNode *node) {
    MultiAssignNode *ma = &node->as.multi_assign;
    /* Evaluate all RHS first to support swaps */
    XiValue *rhs_vals[32];
    int n = ma->value_count > 32 ? 32 : ma->value_count;
    for (int i = 0; i < n; i++)
        rhs_vals[i] = xi_lower_expr(l, ma->values[i]);

    /* Assign to each target */
    for (int i = 0; i < ma->target_count && i < n; i++) {
        AstNode *tgt = ma->targets[i];
        if (tgt && tgt->type == AST_VARIABLE) {
            const char *name = tgt->as.variable.name;
            struct XrType *vtype = rhs_vals[i] ? rhs_vals[i]->type : l->type_any;
            int var_id = xi_lower_var_create(l, name, vtype);
            if (rhs_vals[i]) xi_lower_braun_write(l, var_id, l->cur_block, rhs_vals[i]);
        }
    }
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

    /* Allocate object */
    struct XrType *result_type = xi_lower_node_type(l, node);
    XiValue *cap = xi_const_int(l->func, l->cur_block, count, l->type_int);
    XiValue *obj_val = xi_value_new(l->func, l->cur_block, XI_ALLOC, result_type, 1);
    if (!obj_val) return NULL;
    obj_val->args[0] = cap;
    obj_val->line = (uint32_t) node->line;

    /* INDEX_SET for each property — NEWMAP-backed objects require index
     * access, not dot-syntax SETPROP (which the VM rejects on maps). */
    for (int i = 0; i < n; i++) {
        XiValue *key = NULL;
        if (obj->keys[i] && obj->keys[i]->type == AST_LITERAL_STRING) {
            key = xi_const_str(l->func, l->cur_block,
                               obj->keys[i]->as.literal.raw_value.string_val,
                               l->type_string);
        } else if (obj->keys[i]) {
            key = xi_lower_expr(l, obj->keys[i]);
        }
        if (!key) continue;
        XiValue *set = xi_value_new(l->func, l->cur_block, XI_INDEX_SET,
                                     l->type_void, 3);
        if (!set) break;
        set->args[0] = obj_val;
        set->args[1] = key;
        set->args[2] = val_vals[i];
        set->flags |= XI_FLAG_SIDE_EFFECT;
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

        /* BigInt / Regex: lowered as const values */
        case AST_LITERAL_BIGINT:
            return xi_const_int(l->func, l->cur_block,
                                node->as.literal.raw_value.int_val,
                                l->type_int);
        case AST_LITERAL_REGEX:
            return xi_const_str(l->func, l->cur_block,
                                node->as.literal.raw_value.string_val ?
                                node->as.literal.raw_value.string_val : "",
                                l->type_string);

        /* Expression statement wrapper: unwrap */
        case AST_EXPR_STMT:
            return xi_lower_expr(l, node->as.expr_stmt);

        default:
            /* Unsupported expression: emit null placeholder */
            return xi_const_null(l->func, l->cur_block, l->type_null);
    }
}

/* ========== Statement Lowering ========== */

static void lower_var_decl(XiLower *l, AstNode *node) {
    const char *name = node->as.var_decl.name;
    struct XrType *type = xi_lower_node_type(l, node);

    int var_id = xi_lower_var_create(l, name, type);

    XiValue *init_val;
    if (node->as.var_decl.initializer) {
        init_val = xi_lower_expr(l, node->as.var_decl.initializer);
        if (!init_val) return;
    } else {
        init_val = xi_const_null(l->func, l->cur_block, l->type_null);
    }
    xi_lower_braun_write(l, var_id, l->cur_block, init_val);

    /* For program-level shared variables, also store into shared array */
    if (l->is_program && l->shared_map[var_id] >= 0) {
        XiValue *store = xi_value_new(l->func, l->cur_block,
                                       XI_SET_SHARED, l->type_void, 1);
        if (store) {
            store->args[0] = init_val;
            store->aux_int = l->shared_map[var_id];
            store->flags |= XI_FLAG_SIDE_EFFECT;
        }
    }
}

static void lower_print(XiLower *l, AstNode *node) {
    PrintNode *p = &node->as.print_stmt;
    int nargs = (int) p->expr_count;

    /* Evaluate all arguments first so they appear before PRINT in the block */
    XiValue *arg_vals[16];
    int n = nargs > 16 ? 16 : nargs;
    for (int i = 0; i < n; i++) {
        arg_vals[i] = xi_lower_expr(l, p->exprs[i]);
    }

    /* Emit one XI_PRINT per argument with correct spacing/newline flags.
     * aux_int encoding: bit0 = add_space (maps to B field),
     *                   bit1+ = C field  (bit0 = newline after print) */
    for (int i = 0; i < n; i++) {
        XiValue *v = xi_value_new(l->func, l->cur_block, XI_PRINT,
                                   l->type_void, 1);
        if (!v) return;
        v->args[0] = arg_vals[i];

        int add_space = (i > 0) ? 1 : 0;
        int newline   = (i == n - 1) ? 1 : 0;
        v->aux_int = add_space | (newline << 1);

        v->flags |= XI_FLAG_SIDE_EFFECT;
        v->line = (uint32_t) node->line;
    }
}

static void lower_throw(XiLower *l, AstNode *node) {
    ThrowStmtNode *t = &node->as.throw_stmt;
    XiValue *val = xi_lower_expr(l, t->expression);
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

    if (ret->value_count == 1 && ret->values[0]) {
        val = xi_lower_expr(l, ret->values[0]);
    } else if (ret->value_count > 1) {
        /* Multi-value return: evaluate all expressions first, then package */
        int n = ret->value_count;
        XiValue *vals[16];
        XR_DCHECK(n <= 16, "multi-return exceeds local limit");
        for (int i = 0; i < n && i < 16; i++) {
            vals[i] = xi_lower_expr(l, ret->values[i]);
        }
        XiValue *mret = xi_value_new(l->func, l->cur_block,
                                      XI_MULTI_RET, l->type_any, (uint16_t)n);
        if (mret) {
            for (int i = 0; i < n; i++) {
                mret->args[i] = vals[i];
            }
            val = mret;
        }
    }

    xi_block_set_return(l->cur_block, val);
    l->cur_block = NULL;
}

static void lower_block(XiLower *l, AstNode *node) {
    lower_stmts(l, node->as.block.statements, node->as.block.count);
}

static void lower_if(XiLower *l, AstNode *node) {
    IfStmtNode *s = &node->as.if_stmt;

    XiValue *cond = xi_lower_expr(l, s->condition);
    if (!cond || !l->cur_block) return;

    XiBlock *then_blk = xi_block_new(l->func);
    XiBlock *merge = xi_block_new(l->func);
    XiBlock *else_blk = s->else_branch ? xi_block_new(l->func) : merge;

    xi_block_set_if(l->cur_block, cond, then_blk, else_blk);

    /* then_blk has 1 pred (cur_block) — seal immediately */
    xi_lower_braun_seal(l, then_blk);
    if (s->else_branch)
        xi_lower_braun_seal(l, else_blk);

    /* Then branch */
    l->cur_block = then_blk;
    xi_lower_stmt(l, s->then_branch);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, merge);

    /* Else branch */
    if (s->else_branch) {
        l->cur_block = else_blk;
        xi_lower_stmt(l, s->else_branch);
        if (l->cur_block)
            xi_block_set_jump(l->cur_block, merge);
    }

    /* merge preds now fully known — seal and continue */
    xi_lower_braun_seal(l, merge);
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
    XiValue *cond = xi_lower_expr(l, s->condition);
    if (cond)
        xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);

    /* body_blk has 1 pred (cond_blk) — seal immediately */
    xi_lower_braun_seal(l, body_blk);

    /* Body */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = cond_blk;

    l->cur_block = body_blk;
    xi_lower_stmt(l, s->body);
    if (l->cur_block)  /* back edge */
        xi_block_set_jump(l->cur_block, cond_blk);

    /* All preds of cond_blk now known (entry + back edge) — seal */
    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

static void lower_for(XiLower *l, AstNode *node) {
    ForStmtNode *s = &node->as.for_stmt;

    /* Initializer in current block */
    if (s->initializer)
        xi_lower_stmt(l, s->initializer);
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
        XiValue *cond = xi_lower_expr(l, s->condition);
        if (cond)
            xi_block_set_if(l->cur_block, cond, body_blk, exit_blk);
    } else {
        xi_block_set_jump(l->cur_block, body_blk);
    }

    xi_lower_braun_seal(l, body_blk);

    /* Body */
    XiBlock *prev_break = l->break_target;
    XiBlock *prev_cont = l->continue_target;
    l->break_target = exit_blk;
    l->continue_target = incr_blk;

    l->cur_block = body_blk;
    xi_lower_stmt(l, s->body);
    if (l->cur_block)
        xi_block_set_jump(l->cur_block, incr_blk);

    xi_lower_braun_seal(l, incr_blk);

    /* Increment */
    l->cur_block = incr_blk;
    if (s->increment) {
        if (incr_blk->npreds > 0)
            xi_lower_expr(l, s->increment);
    }
    if (l->cur_block && incr_blk->npreds > 0)
        xi_block_set_jump(l->cur_block, cond_blk);

    /* cond_blk back edge now added — seal */
    xi_lower_braun_seal(l, cond_blk);

    l->break_target = prev_break;
    l->continue_target = prev_cont;

    xi_lower_braun_seal(l, exit_blk);
    l->cur_block = (exit_blk->npreds > 0) ? exit_blk : NULL;
}

/* lower_for_in_loop, lower_for_in_keyvalue, lower_for_in → xi_lower_stmt.c */

/* (function bodies removed — see xi_lower_stmt.c)
 * Remaining: lower_break, lower_continue kept here as they are tiny. */

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


/* Forward declarations for class lowering helpers (defined below) */
static void lower_init(XiLower *l, struct XaAnalyzer *analyzer,
                        struct XrayIsolate *isolate);
static void lower_cleanup(XiLower *l);

/* Class declaration lowering (method compilation + XI_CLASS_CREATE).
 * Factored into .inc.c to keep xi_lower.c within the 3000-line limit. */
#include "xi_lower_class.inc.c"

/* Main statement dispatcher */
XR_FUNC void xi_lower_stmt(XiLower *l, AstNode *node) {
    if (!node) return;
    if (!l->cur_block) return;  /* dead code */

    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            lower_var_decl(l, node);
            break;

        case AST_EXPR_STMT:
            xi_lower_expr(l, node->as.expr_stmt);
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
            xi_lower_for_in(l, node);
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
            xi_lower_try_catch(l, node);
            break;

        /* Function declaration as statement */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            lower_function_decl(l, node);
            break;

        case AST_DEFER_STMT:
            lower_defer(l, node);
            break;

        /* Select statement (channel multiplexing) */
        case AST_SELECT_STMT:
            xi_lower_select(l, node);
            break;

        /* Scope block (structured concurrency) */
        case AST_SCOPE_BLOCK:
            xi_lower_scope_block(l, node);
            break;

        /* Yield execution */
        case AST_YIELD_STMT:
            lower_yield_stmt(l);
            break;

        /* Destructuring */
        case AST_DESTRUCTURE_DECL:
            lower_destructure_decl(l, node);
            break;
        case AST_DESTRUCTURE_ASSIGN:
            lower_destructure_assign(l, node);
            break;

        /* Multi-value declarations and assignments */
        case AST_MULTI_VAR_DECL:
            lower_multi_var_decl(l, node);
            break;
        case AST_MULTI_ASSIGN:
            lower_multi_assign(l, node);
            break;

        /* Module system: import/export are compile-time directives,
         * no runtime IR needed. Skip silently. */
        case AST_IMPORT_STMT:
        case AST_EXPORT_STMT:
            break;

        case AST_CLASS_DECL:
            lower_class_decl(l, node);
            break;
        case AST_STRUCT_DECL:
        case AST_INTERFACE_DECL:
        case AST_TYPE_ALIAS:
            break;
        case AST_ENUM_DECL:
            lower_enum_decl(l, node);
            break;

        /* Match expression used as statement */
        case AST_MATCH_EXPR:
            xi_lower_expr(l, node);
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
        case AST_AWAIT_ALL_EXPR:
        case AST_AWAIT_ANY_EXPR:
        case AST_NEW_EXPR:
        case AST_MOVE_EXPR:
            xi_lower_expr(l, node);
            break;

        default:
            /* Unsupported statement: skip */
            break;
    }
}

static void lower_stmts(XiLower *l, AstNode **stmts, int count) {
    for (int i = 0; i < count; i++) {
        if (!l->cur_block) break;  /* dead code after return/break */
        xi_lower_stmt(l, stmts[i]);
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

    /* Initialize shared_map to -1 (no shared index) */
    memset(l->shared_map, -1, sizeof(l->shared_map));

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

/* ========== Function Lowering Implementation ========== */

/*
 * Internal function lowering with optional parent context.
 * When parent is non-NULL, the child can resolve variable references
 * from enclosing scopes via the upvalue capture mechanism.
 */
static XiFunc *lower_func_impl(AstNode *func_node, struct XaAnalyzer *analyzer,
                                struct XrayIsolate *isolate, XiLower *parent_ctx) {
    XR_DCHECK(func_node != NULL, "lower_func_impl: func_node is NULL");
    FunctionDeclNode *fdecl = &func_node->as.function_decl;

    XiLower l;
    lower_init(&l, analyzer, isolate);
    l.parent = parent_ctx;

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

    /* Detect rest parameter and compute VM entry metadata */
    bool has_rest = false;
    for (int i = 0; i < fdecl->param_count; i++) {
        if (fdecl->params[i] && fdecl->params[i]->is_rest) {
            has_rest = true;
            break;
        }
    }
    l.func->is_vararg = has_rest;
    l.func->min_params = (uint16_t) fdecl->required_count;

    if (fdecl->is_generator) {
        l.func->entry_type = 2;  /* XR_ENTRY_GENERATOR */
    } else if (fdecl->required_count <
               (has_rest ? fdecl->param_count - 1 : fdecl->param_count)) {
        l.func->entry_type = 1;  /* XR_ENTRY_DEFAULTS */
    } else {
        l.func->entry_type = 0;  /* XR_ENTRY_NORMAL */
    }

    /* nparams excludes rest param (VM packs varargs into the rest slot) */
    if (has_rest) {
        l.func->nparams = (uint16_t)(fdecl->param_count - 1);
    }

    for (int i = 0; i < fdecl->param_count; i++) {
        XrParamNode *p = fdecl->params[i];
        struct XrType *ptype = p->type ? p->type : l.type_any;

        XiValue *param_val = xi_param(l.func, entry, (uint16_t) i, ptype);
        l.func->params[i] = param_val;

        /* Register parameter in Braun SSA */
        int var_id = xi_lower_var_create(&l, p->name, ptype);
        xi_lower_braun_write(&l, var_id, entry, param_val);
    }

    /* For named functions, register a self-reference so the body can
     * resolve recursive calls.  lower_call detects l.self_value and
     * emits a self-call (OP_CALLSELF) instead of a regular call. */
    if (fdecl->name) {
        struct XrType *fn_type = ret_type;  /* approximate; exact type unused */
        XiValue *self = xi_const_null(l.func, entry, l.type_null);
        l.self_value = self;
        int self_var = xi_lower_var_create(&l, fdecl->name, fn_type);
        xi_lower_braun_write(&l, self_var, entry, self);
    }

    /* Lower function body */
    if (fdecl->body) {
        xi_lower_stmt(&l, fdecl->body);
    }

    /* If last block not terminated, add implicit void return */
    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    lower_cleanup(&l);
    return l.func;
}

/* ========== Public API ========== */

XiFunc *xi_lower_func(AstNode *func_node, struct XaAnalyzer *analyzer,
                       struct XrayIsolate *isolate) {
    XR_CHECK(func_node != NULL, "xi_lower_func: func_node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_func: analyzer is NULL");
    XR_CHECK(func_node->type == AST_FUNCTION_DECL ||
             func_node->type == AST_FUNCTION_EXPR,
             "xi_lower_func: not a function node");
    return lower_func_impl(func_node, analyzer, isolate, NULL);
}

/*
 * Pre-scan top-level statements to assign shared variable indices.
 * Every named declaration (function, variable, const) at program level
 * gets a shared slot so inner functions can reference them via
 * GETSHARED — including forward references to not-yet-lowered functions.
 */
static void prescan_shared_vars(XiLower *l, AstNode **stmts, int count) {
    XR_DCHECK(l->is_program, "prescan_shared_vars: not a program context");
    uint16_t next_shared = 0;
    for (int i = 0; i < count; i++) {
        AstNode *s = stmts[i];
        if (!s) continue;
        const char *name = NULL;
        struct XrType *type = l->type_any;

        switch (s->type) {
            case AST_FUNCTION_DECL:
                name = s->as.function_decl.name;
                type = xi_lower_node_type(l, s);
                break;
            case AST_CLASS_DECL:
                name = s->as.class_decl.name;
                break;
            case AST_VAR_DECL:
            case AST_CONST_DECL:
                name = s->as.var_decl.name;
                type = xi_lower_node_type(l, s);
                break;
            default:
                break;
        }
        if (!name) continue;

        /* Create the Braun SSA variable entry (no definition yet) */
        int var_id = xi_lower_var_create(l, name, type);
        XR_DCHECK(var_id >= 0 && var_id < XI_LOWER_MAX_VARS,
                  "prescan_shared_vars: var_id overflow");
        l->shared_map[var_id] = (int16_t)next_shared;
        next_shared++;
    }
    l->func->nshared = next_shared;
}

XiFunc *xi_lower_program(AstNode *program_node, struct XaAnalyzer *analyzer,
                          struct XrayIsolate *isolate) {
    XR_CHECK(program_node != NULL, "xi_lower_program: node is NULL");
    XR_CHECK(analyzer != NULL, "xi_lower_program: analyzer is NULL");

    XiLower l;
    lower_init(&l, analyzer, isolate);
    l.is_program = true;

    l.func = xi_func_new("<main>", l.type_void);
    if (!l.func) { lower_cleanup(&l); return NULL; }
    l.func->analyzer = analyzer;
    l.func->nparams = 0;
    l.func->params = NULL;

    XiBlock *entry = xi_block_new(l.func);
    entry->sealed = true;
    l.cur_block = entry;

    /* Pre-scan: assign shared indices to all top-level declarations */
    AstNode **stmts;
    int count;
    if (program_node->type == AST_BLOCK) {
        stmts = program_node->as.block.statements;
        count = program_node->as.block.count;
    } else {
        stmts = program_node->as.program.statements;
        count = program_node->as.program.count;
    }
    prescan_shared_vars(&l, stmts, count);

    /* Lower all top-level statements */
    lower_stmts(&l, stmts, count);

    if (l.cur_block) {
        xi_block_set_return(l.cur_block, NULL);
    }

    lower_cleanup(&l);
    return l.func;
}
