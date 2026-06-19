/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_slp.c - SLP (Superword Level Parallelism) vectorization
 *
 * Identifies groups of isomorphic adjacent-memory operations in a
 * single basic block and packs them into vector ops (XI_VEC_*).
 *
 * Current scope:
 *   - Straight-line code within a single block
 *   - Identifies INDEX_SET patterns with consecutive constant indices
 *   - Packs into VEC_LOAD + VEC_ADD/SUB/MUL + VEC_STORE when profitable
 *   - Default VF (vector factor) = 4 (maps to 128-bit SIMD)
 */

#include "xi_opt_slp.h"
#include "xi.h"

#define SLP_DEFAULT_VF 4

/* Check if two index operations are adjacent (indices differ by 1). */
static bool is_adjacent_index(const XiValue *a, const XiValue *b) {
    if (!a || !b)
        return false;
    if (a->op != b->op)
        return false;
    if (a->nargs < 2 || b->nargs < 2)
        return false;
    /* Same base array. */
    if (a->args[0] != b->args[0])
        return false;
    /* Indices must be constants differing by 1. */
    if (a->nargs < 2 || b->nargs < 2)
        return false;
    XiValue *idx_a = a->args[1];
    XiValue *idx_b = b->args[1];
    if (!idx_a || !idx_b)
        return false;
    if (idx_a->op != XI_CONST || idx_b->op != XI_CONST)
        return false;
    return (idx_b->aux_int == idx_a->aux_int + 1);
}

/* Find a group of VF adjacent INDEX_SET ops in a block, starting from
 * position `start`. Returns the number of ops in the group (0 if none). */
static uint32_t find_adjacent_group(XiBlock *blk, uint32_t start, uint32_t vf) {
    if (start >= blk->nvalues)
        return 0;

    XiValue *first = blk->values[start];
    if (!first || first->op != XI_INDEX_SET)
        return 0;
    if (first->nargs < 3)
        return 0;

    /* The stored values must come from the same op type (e.g., all XI_ADD). */
    XiValue *stored_first = first->args[2];
    if (!stored_first)
        return 0;

    uint32_t count = 1;
    for (uint32_t i = start + 1; i < blk->nvalues && count < vf; i++) {
        XiValue *cur = blk->values[i];
        if (!cur || cur->op != XI_INDEX_SET)
            break;
        if (!is_adjacent_index(blk->values[i - 1], cur))
            break;
        if (cur->nargs < 3 || !cur->args[2])
            break;
        /* Stored values must be isomorphic (same op). */
        if (cur->args[2]->op != stored_first->op)
            break;
        count++;
    }

    return count >= vf ? count : 0;
}

/* Convert a group of INDEX_SET(array, idx, ADD(LOAD(b,i), LOAD(c,i)))
 * into VEC_LOAD + VEC_ADD + VEC_STORE. Returns true if converted. */
static bool vectorize_group(XiFunc *f, XiBlock *blk, uint32_t start, uint32_t vf) {
    /* Validate that the stored values are binary ops on index loads. */
    XiValue *first_store = blk->values[start];
    XiValue *first_op = first_store->args[2];

    uint16_t arith_op = first_op->op;
    uint16_t vec_op;
    switch (arith_op) {
        case XI_ADD:
            vec_op = XI_VEC_ADD;
            break;
        case XI_SUB:
            vec_op = XI_VEC_SUB;
            break;
        case XI_MUL:
            vec_op = XI_VEC_MUL;
            break;
        default:
            return false;
    }

    /* Check that all arithmetic ops have INDEX_GET sources from
     * the same base arrays. */
    if (first_op->nargs < 2 || !first_op->args[0] || !first_op->args[1])
        return false;
    if (first_op->args[0]->op != XI_INDEX_GET || first_op->args[1]->op != XI_INDEX_GET)
        return false;

    XiValue *base_b = first_op->args[0]->args[0]; /* source array B */
    XiValue *base_c = first_op->args[1]->args[0]; /* source array C */
    XiValue *base_a = first_store->args[0];       /* destination array A */

    if (!base_a || !base_b || !base_c)
        return false;

    XiValue *start_idx = first_store->args[1];
    if (!start_idx)
        return false;

    /* Create VEC_LOAD for sources. */
    struct XrType *elem_type = first_op->type;
    XiValue *vec_b = xi_value_new(f, blk, XI_VEC_LOAD, elem_type, 2);
    if (!vec_b)
        return false;
    vec_b->args[0] = base_b;
    vec_b->args[1] = first_op->args[0]->args[1];
    vec_b->aux_int = (int64_t) vf;

    XiValue *vec_c = xi_value_new(f, blk, XI_VEC_LOAD, elem_type, 2);
    if (!vec_c)
        return false;
    vec_c->args[0] = base_c;
    vec_c->args[1] = first_op->args[1]->args[1];
    vec_c->aux_int = (int64_t) vf;

    /* Create VEC_OP. */
    XiValue *vec_result = xi_value_new(f, blk, vec_op, elem_type, 2);
    if (!vec_result)
        return false;
    vec_result->args[0] = vec_b;
    vec_result->args[1] = vec_c;
    vec_result->aux_int = (int64_t) vf;

    /* Create VEC_STORE. */
    XiValue *vec_store = xi_value_new(f, blk, XI_VEC_STORE, elem_type, 3);
    if (!vec_store)
        return false;
    vec_store->args[0] = base_a;
    vec_store->args[1] = vec_result;
    vec_store->args[2] = start_idx;
    vec_store->aux_int = (int64_t) vf;

    /* Mark original scalar stores as dead by converting to XI_COPY of
     * vec_store result.  DCE will clean up the now-unused scalar ops. */
    for (uint32_t i = start; i < start + vf; i++) {
        XiValue *v = blk->values[i];
        if (v) {
            v->op = XI_COPY;
            v->args[0] = vec_store;
            v->nargs = 1;
            v->aux_int = XI_COPY_KIND_IDENTITY;
            v->aux = NULL;
        }
    }

    return true;
}

XR_FUNC XiPassChange xi_opt_slp(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    uint32_t vf = SLP_DEFAULT_VF;
    uint32_t n_vec = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues;) {
            uint32_t group_len = find_adjacent_group(blk, vi, vf);
            if (group_len >= vf && vectorize_group(f, blk, vi, vf)) {
                n_vec++;
                vi += group_len;
            } else {
                vi++;
            }
        }
    }

    if (n_vec == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = false,
        .n_removed = n_vec * SLP_DEFAULT_VF,
        .n_added = n_vec * 4, /* VEC_LOAD × 2 + VEC_OP + VEC_STORE */
    };
}
