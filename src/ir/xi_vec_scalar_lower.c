/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_vec_scalar_lower.c - Scalar expansion for XI_VEC_* before backend codegen
 */

#include "xi_vec_scalar_lower.h"
#include "xi_effect.h"
#include "../base/xchecks.h"

static XiValue *make_const_index(XiFunc *f, XiBlock *blk, const XiValue *start, int64_t offset) {
    struct XrType *idx_type = start ? start->type : NULL;
    XiValue *idx = xi_value_new(f, blk, XI_CONST, idx_type, 0);
    if (!idx)
        return NULL;
    if (start && start->op == XI_CONST)
        idx->aux_int = start->aux_int + offset;
    else
        idx->aux_int = offset;
    return idx;
}

static XiValue *make_index_get(XiFunc *f, XiBlock *blk, XiValue *base, XiValue *idx) {
    struct XrType *elem_type = base ? base->type : NULL;
    XiValue *load = xi_value_new(f, blk, XI_INDEX_GET, elem_type, 2);
    if (!load)
        return NULL;
    load->args[0] = base;
    load->args[1] = idx;
    load->flags |= xi_op_default_effects(XI_INDEX_GET);
    return load;
}

static XiValue *make_index_set(XiFunc *f, XiBlock *blk, XiValue *base, XiValue *idx, XiValue *val) {
    struct XrType *elem_type = val ? val->type : NULL;
    XiValue *store = xi_value_new(f, blk, XI_INDEX_SET, elem_type, 3);
    if (!store)
        return NULL;
    store->args[0] = base;
    store->args[1] = idx;
    store->args[2] = val;
    store->flags |= xi_op_default_effects(XI_INDEX_SET);
    return store;
}

static XiValue *make_binop(XiFunc *f, XiBlock *blk, XiOp op, XiValue *a, XiValue *b) {
    struct XrType *ty = a ? a->type : NULL;
    XiValue *v = xi_value_new(f, blk, op, ty, 2);
    if (!v)
        return NULL;
    v->args[0] = a;
    v->args[1] = b;
    v->flags |= xi_op_default_effects(op);
    return v;
}

static bool expand_vec_to_scalars(XiFunc *f, XiBlock *blk, XiValue *vec, XiValue **out,
                                  uint32_t vf) {
    if (!vec || vf == 0 || !out)
        return false;

    switch ((XiOp) vec->op) {
        case XI_VEC_LOAD: {
            if (vec->nargs < 1 || !vec->args[0])
                return false;
            XiValue *base = vec->args[0];
            XiValue *start = (vec->nargs >= 2) ? vec->args[1] : NULL;
            for (uint32_t i = 0; i < vf; i++) {
                XiValue *idx = make_const_index(f, blk, start, (int64_t) i);
                XiValue *load = idx ? make_index_get(f, blk, base, idx) : NULL;
                if (!load)
                    return false;
                out[i] = load;
            }
            return true;
        }
        case XI_VEC_ADD:
        case XI_VEC_SUB:
        case XI_VEC_MUL: {
            if (vec->nargs < 2)
                return false;
            XiValue *lhs[16];
            XiValue *rhs[16];
            if (vf > 16)
                return false;
            if (!expand_vec_to_scalars(f, blk, vec->args[0], lhs, vf))
                return false;
            if (!expand_vec_to_scalars(f, blk, vec->args[1], rhs, vf))
                return false;
            XiOp scalar_op = XI_ADD;
            if (vec->op == XI_VEC_SUB)
                scalar_op = XI_SUB;
            if (vec->op == XI_VEC_MUL)
                scalar_op = XI_MUL;
            for (uint32_t i = 0; i < vf; i++) {
                XiValue *v = make_binop(f, blk, scalar_op, lhs[i], rhs[i]);
                if (!v)
                    return false;
                out[i] = v;
            }
            return true;
        }
        default:
            return false;
    }
}

static bool lower_vec_store(XiFunc *f, XiBlock *blk, XiValue *store) {
    if (!store || store->op != XI_VEC_STORE)
        return false;
    uint32_t vf = (uint32_t) store->aux_int;
    if (vf == 0 || vf > 16 || store->nargs < 2 || !store->args[0] || !store->args[1])
        return false;

    XiValue *base = store->args[0];
    XiValue *start = (store->nargs >= 3) ? store->args[2] : NULL;
    XiValue *scalars[16];
    if (!expand_vec_to_scalars(f, blk, store->args[1], scalars, vf))
        return false;

    XiValue *last = NULL;
    for (uint32_t i = 0; i < vf; i++) {
        XiValue *idx = make_const_index(f, blk, start, (int64_t) i);
        XiValue *set = idx ? make_index_set(f, blk, base, idx, scalars[i]) : NULL;
        if (!set)
            return false;
        last = set;
    }

    store->op = XI_COPY;
    store->nargs = 1;
    store->args[0] = last;
    store->aux_int = XI_COPY_KIND_IDENTITY;
    store->aux = NULL;
    store->aux_kind = XI_AUX_KIND_NONE;
    store->flags |= xi_op_default_effects(XI_COPY);
    return true;
}

static bool lower_vec_func(XiFunc *f) {
    bool changed = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v && v->op == XI_VEC_STORE && lower_vec_store(f, blk, v))
                changed = true;
        }
    }
    return changed;
}

static bool lower_vec_walk(XiFunc *f) {
    bool changed = lower_vec_func(f);
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i] && lower_vec_walk(f->children[i]))
            changed = true;
    }
    return changed;
}

XR_FUNC bool xi_vec_scalar_lower(XiFunc *f) {
    if (!f)
        return false;
    return lower_vec_walk(f);
}
