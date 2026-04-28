/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi.c - Typed SSA IR core operations
 *
 * Provides create/add/free functions for XiFunc, XiBlock, XiValue, XiPhi.
 * All IR nodes are bump-allocated from a per-function arena so destruction
 * is O(1) (free the arena chunk). Individual node frees are no-ops.
 */

#include "xi.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

#include <string.h>
#include <stdio.h>

/* ========== Arena Allocator ========== */

static void *arena_alloc(XiFunc *f, uint32_t size) {
    XR_DCHECK(f != NULL, "arena_alloc: f is NULL");
    /* Align to 8 bytes */
    size = (size + 7) & ~7u;

    if (f->arena_used + size > f->arena_cap) {
        /* Grow arena: double or at least fit the requested size */
        uint32_t new_cap = f->arena_cap * 2;
        if (new_cap < f->arena_used + size)
            new_cap = f->arena_used + size + XI_ARENA_INITIAL_SIZE;
        uint8_t *tmp = (uint8_t *) xr_realloc(f->arena, new_cap);
        if (!tmp) {
            fprintf(stderr, "xi: arena realloc failed (requested %u bytes)\n", new_cap);
            return NULL;
        }
        f->arena = tmp;
        f->arena_cap = new_cap;
    }

    void *ptr = f->arena + f->arena_used;
    memset(ptr, 0, size);
    f->arena_used += size;
    return ptr;
}

void *xi_func_arena_alloc(XiFunc *f, uint32_t size) {
    return arena_alloc(f, size);
}

/* ========== Function Lifecycle ========== */

XiFunc *xi_func_new(const char *name, struct XrType *return_type) {
    XiFunc *f = (XiFunc *) xr_calloc(1, sizeof(XiFunc));
    if (!f) return NULL;

    f->name = name;
    f->return_type = return_type;

    /* Initialize arena */
    f->arena = (uint8_t *) xr_malloc(XI_ARENA_INITIAL_SIZE);
    if (!f->arena) {
        xr_free(f);
        return NULL;
    }
    f->arena_cap = XI_ARENA_INITIAL_SIZE;
    f->arena_used = 0;

    /* Initial block capacity */
    f->blocks_cap = 16;
    f->blocks = (XiBlock **) xr_calloc(f->blocks_cap, sizeof(XiBlock *));
    if (!f->blocks) {
        xr_free(f->arena);
        xr_free(f);
        return NULL;
    }

    return f;
}

void xi_func_free(XiFunc *f) {
    if (!f) return;

    /* Arena owns all XiValues, XiPhis, arg arrays.
     * Blocks and block arrays are also arena-allocated if we switch,
     * but currently blocks[] is heap-allocated for resizability. */
    xr_free(f->arena);
    xr_free(f->blocks);
    xr_free(f->params);
    xr_free(f);
}

/* ========== Block Management ========== */

XiBlock *xi_block_new(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_block_new: f is NULL");

    XiBlock *blk = (XiBlock *) arena_alloc(f, sizeof(XiBlock));
    if (!blk) return NULL;

    blk->id = f->next_block_id++;
    blk->kind = XI_BLOCK_PLAIN;
    blk->func = f;

    /* Initial values capacity */
    blk->values_cap = 16;
    blk->values = (XiValue **) arena_alloc(f, blk->values_cap * sizeof(XiValue *));

    /* Initial preds capacity */
    blk->preds_cap = 4;
    blk->preds = (XiBlock **) arena_alloc(f, blk->preds_cap * sizeof(XiBlock *));

    /* Register in function */
    if (f->nblocks >= f->blocks_cap) {
        uint32_t new_cap = f->blocks_cap * 2;
        XiBlock **tmp = (XiBlock **) xr_realloc(f->blocks, new_cap * sizeof(XiBlock *));
        if (!tmp) return NULL;
        f->blocks = tmp;
        f->blocks_cap = new_cap;
    }
    f->blocks[f->nblocks++] = blk;

    /* First block is the entry */
    if (f->nblocks == 1)
        f->entry = blk;

    return blk;
}

void xi_block_add_pred(XiBlock *blk, XiBlock *pred) {
    XR_DCHECK(blk != NULL, "xi_block_add_pred: blk is NULL");
    XR_DCHECK(pred != NULL, "xi_block_add_pred: pred is NULL");

    if (blk->npreds >= blk->preds_cap) {
        /* Arena-allocated arrays cannot be resized in place.
         * Allocate a new larger array and copy. Old memory is wasted
         * but reclaimed when the arena is freed. */
        uint16_t new_cap = blk->preds_cap * 2;
        XiBlock **new_preds = (XiBlock **) arena_alloc(blk->func, new_cap * sizeof(XiBlock *));
        if (!new_preds) return;
        memcpy(new_preds, blk->preds, blk->npreds * sizeof(XiBlock *));
        blk->preds = new_preds;
        blk->preds_cap = new_cap;
    }
    blk->preds[blk->npreds++] = pred;
}

/* ========== Value Construction ========== */

static XiValue *value_alloc(XiFunc *f, XiBlock *blk, uint16_t op,
                             struct XrType *type, uint16_t nargs) {
    XR_DCHECK(f != NULL, "value_alloc: f is NULL");
    XR_DCHECK(blk != NULL, "value_alloc: blk is NULL");
    XR_DCHECK(type != NULL, "value_alloc: type is NULL");

    XiValue *v = (XiValue *) arena_alloc(f, sizeof(XiValue));
    if (!v) return NULL;

    v->id = f->next_value_id++;
    v->op = op;
    v->type = type;
    v->nargs = nargs;
    v->uses = -1;  /* not yet computed */
    v->block = blk;

    if (nargs > 0) {
        v->args = (XiValue **) arena_alloc(f, nargs * sizeof(XiValue *));
    }

    return v;
}

static void block_append_value(XiBlock *blk, XiValue *v) {
    XR_DCHECK(blk != NULL, "block_append_value: blk is NULL");
    XR_DCHECK(v != NULL, "block_append_value: v is NULL");

    if (blk->nvalues >= blk->values_cap) {
        uint32_t new_cap = blk->values_cap * 2;
        XiValue **new_values = (XiValue **) arena_alloc(
            blk->func, new_cap * sizeof(XiValue *));
        if (!new_values) return;
        memcpy(new_values, blk->values, blk->nvalues * sizeof(XiValue *));
        blk->values = new_values;
        blk->values_cap = new_cap;
    }
    blk->values[blk->nvalues++] = v;
}

XiValue *xi_value_new(XiFunc *f, XiBlock *blk, uint16_t op,
                       struct XrType *type, uint16_t nargs) {
    XiValue *v = value_alloc(f, blk, op, type, nargs);
    if (!v) return NULL;
    block_append_value(blk, v);
    return v;
}

/* ========== Constant Constructors ========== */

XiValue *xi_const_int(XiFunc *f, XiBlock *blk, int64_t val,
                       struct XrType *int_type) {
    XR_DCHECK(int_type != NULL, "xi_const_int: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, int_type, 0);
    if (v) v->aux_int = val;
    return v;
}

XiValue *xi_const_float(XiFunc *f, XiBlock *blk, double val,
                         struct XrType *float_type) {
    XR_DCHECK(float_type != NULL, "xi_const_float: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, float_type, 0);
    if (v) {
        /* Store double bits in aux_int to avoid union aliasing issues */
        memcpy(&v->aux_int, &val, sizeof(double));
    }
    return v;
}

XiValue *xi_const_bool(XiFunc *f, XiBlock *blk, bool val,
                        struct XrType *bool_type) {
    XR_DCHECK(bool_type != NULL, "xi_const_bool: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, bool_type, 0);
    if (v) v->aux_int = val ? 1 : 0;
    return v;
}

XiValue *xi_const_null(XiFunc *f, XiBlock *blk,
                        struct XrType *null_type) {
    XR_DCHECK(null_type != NULL, "xi_const_null: type is NULL");
    return xi_value_new(f, blk, XI_CONST, null_type, 0);
}

XiValue *xi_const_str(XiFunc *f, XiBlock *blk, const char *str,
                       struct XrType *str_type) {
    XR_DCHECK(str_type != NULL, "xi_const_str: type is NULL");
    XiValue *v = xi_value_new(f, blk, XI_CONST, str_type, 0);
    if (v) v->aux = (void *) str;
    return v;
}

/* ========== Convenience Constructors ========== */

XiValue *xi_binary(XiFunc *f, XiBlock *blk, uint16_t op,
                    struct XrType *type, XiValue *lhs, XiValue *rhs) {
    XR_DCHECK(lhs != NULL, "xi_binary: lhs is NULL");
    XR_DCHECK(rhs != NULL, "xi_binary: rhs is NULL");
    XiValue *v = xi_value_new(f, blk, op, type, 2);
    if (v) {
        v->args[0] = lhs;
        v->args[1] = rhs;
    }
    return v;
}

XiValue *xi_unary(XiFunc *f, XiBlock *blk, uint16_t op,
                   struct XrType *type, XiValue *arg) {
    XR_DCHECK(arg != NULL, "xi_unary: arg is NULL");
    XiValue *v = xi_value_new(f, blk, op, type, 1);
    if (v) v->args[0] = arg;
    return v;
}

XiValue *xi_param(XiFunc *f, XiBlock *blk, uint16_t index,
                   struct XrType *type) {
    XiValue *v = xi_value_new(f, blk, XI_PARAM, type, 0);
    if (v) v->aux_int = index;
    return v;
}

/* ========== Phi Nodes ========== */

XiPhi *xi_phi_new(XiFunc *f, XiBlock *blk, struct XrType *type,
                   uint16_t npreds) {
    XR_DCHECK(f != NULL, "xi_phi_new: f is NULL");
    XR_DCHECK(blk != NULL, "xi_phi_new: blk is NULL");
    XR_DCHECK(type != NULL, "xi_phi_new: type is NULL");

    XiPhi *phi = (XiPhi *) arena_alloc(f, sizeof(XiPhi));
    if (!phi) return NULL;

    phi->value.id = f->next_value_id++;
    phi->value.op = XI_PHI;
    phi->value.type = type;
    phi->value.nargs = npreds;
    phi->value.uses = -1;
    phi->value.block = blk;

    if (npreds > 0) {
        phi->value.args = (XiValue **) arena_alloc(f, npreds * sizeof(XiValue *));
    }

    /* Prepend to block phi list */
    phi->next = blk->phis;
    blk->phis = phi;

    return phi;
}

/* ========== Block Termination ========== */

void xi_block_set_return(XiBlock *blk, XiValue *val) {
    XR_DCHECK(blk != NULL, "xi_block_set_return: blk is NULL");
    blk->kind = XI_BLOCK_RETURN;
    blk->control = val;  /* NULL for void return */
    blk->succs[0] = NULL;
    blk->succs[1] = NULL;
}

void xi_block_set_jump(XiBlock *blk, XiBlock *target) {
    XR_DCHECK(blk != NULL, "xi_block_set_jump: blk is NULL");
    XR_DCHECK(target != NULL, "xi_block_set_jump: target is NULL");
    blk->kind = XI_BLOCK_PLAIN;
    blk->control = NULL;
    blk->succs[0] = target;
    blk->succs[1] = NULL;
    xi_block_add_pred(target, blk);
}

void xi_block_set_if(XiBlock *blk, XiValue *cond,
                      XiBlock *then_blk, XiBlock *else_blk) {
    XR_DCHECK(blk != NULL, "xi_block_set_if: blk is NULL");
    XR_DCHECK(cond != NULL, "xi_block_set_if: cond is NULL");
    XR_DCHECK(then_blk != NULL, "xi_block_set_if: then_blk is NULL");
    XR_DCHECK(else_blk != NULL, "xi_block_set_if: else_blk is NULL");
    blk->kind = XI_BLOCK_IF;
    blk->control = cond;
    blk->succs[0] = then_blk;
    blk->succs[1] = else_blk;
    xi_block_add_pred(then_blk, blk);
    xi_block_add_pred(else_blk, blk);
}
