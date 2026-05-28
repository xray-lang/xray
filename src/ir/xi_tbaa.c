/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_tbaa.c - Type-Based Alias Analysis implementation
 *
 * Classifies memory-accessing Xi ops into disjoint groups in a simple
 * lattice.  Two ops from different groups are guaranteed not to alias.
 * The lattice is intentionally conservative: XI_MEM_TOP aliases everything.
 *
 * The annotation pass sets XiValue.mem_group for every value in the
 * function.  Non-memory ops get XI_MEM_NONE (the default from arena zeroing).
 */

#include "xi_tbaa.h"
#include "xi_pass.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

/* ========== Op Classification Tables ========== */

/* Memory load ops: read memory but do not write. */
XR_FUNC bool xi_is_memory_load(uint16_t op) {
    switch (op) {
        case XI_LOAD_FIELD:
        case XI_INDEX_GET:
        case XI_STRUCT_GET:
        case XI_JSON_GET_F:
        case XI_TUPLE_GET:
        case XI_LOAD_UPVAL:
        case XI_GET_SHARED:
        case XI_GET_GLOBAL:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_RECV:
        case XI_GET_BUILTIN:
            return true;
        default:
            return false;
    }
}

/* Memory store ops: write to memory. */
XR_FUNC bool xi_is_memory_store(uint16_t op) {
    switch (op) {
        case XI_STORE_FIELD:
        case XI_INDEX_SET:
        case XI_STRUCT_SET:
        case XI_JSON_SET_F:
        case XI_JSON_INIT_F:
        case XI_STORE_UPVAL:
        case XI_SET_SHARED:
        case XI_SET_GLOBAL:
        case XI_CHAN_SEND:
        case XI_CHAN_TRY_SEND:
            return true;
        default:
            return false;
    }
}

/* Any memory-accessing op (load or store). */
XR_FUNC bool xi_is_memory_op(uint16_t op) {
    return xi_is_memory_load(op) || xi_is_memory_store(op);
}

/* ========== TBAA Group Assignment ==========
 *
 * Determines the mem_group for a given op.  This is the core classification
 * that drives the alias query.  Conservative: unknown ops get XI_MEM_TOP. */
static XiMemGroup classify_op(uint16_t op) {
    switch (op) {
        /* Object field access */
        case XI_LOAD_FIELD:
        case XI_STORE_FIELD:
            return XI_MEM_FIELD;

        /* Array / collection indexing */
        case XI_INDEX_GET:
        case XI_INDEX_SET:
            return XI_MEM_ARRAY;

        /* Struct typed field access */
        case XI_STRUCT_GET:
        case XI_STRUCT_SET:
            return XI_MEM_STRUCT;

        /* JSON object fields */
        case XI_JSON_GET_F:
        case XI_JSON_SET_F:
        case XI_JSON_INIT_F:
            return XI_MEM_JSON;

        /* Tuple element access */
        case XI_TUPLE_GET:
            return XI_MEM_TUPLE;

        /* Closure upvalue slots */
        case XI_LOAD_UPVAL:
        case XI_STORE_UPVAL:
            return XI_MEM_UPVAL;

        /* Module shared variable array */
        case XI_GET_SHARED:
        case XI_SET_SHARED:
            return XI_MEM_SHARED;

        /* REPL global dict */
        case XI_GET_GLOBAL:
        case XI_SET_GLOBAL:
            return XI_MEM_GLOBAL;

        /* Channel buffer */
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
            return XI_MEM_CHAN;

        /* Builtins may access any memory — conservative. */
        case XI_GET_BUILTIN:
            return XI_MEM_CONST;

        /* Calls clobber everything (conservative). */
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_CALL_BUILTIN:
            return XI_MEM_TOP;

        default:
            return XI_MEM_NONE;
    }
}

/* ========== Alias Query ========== */

/* TBAA lattice disjointness matrix.
 *
 * Two groups are disjoint (no alias) when they represent provably separate
 * memory regions.  XI_MEM_TOP is the conservative "any" — aliases everything.
 * XI_MEM_NONE means "not a memory op" — trivially no alias with anything. */
XR_FUNC bool xi_tbaa_may_alias(const XiValue *a, const XiValue *b) {
    XR_DCHECK(a != NULL, "xi_tbaa_may_alias: NULL a");
    XR_DCHECK(b != NULL, "xi_tbaa_may_alias: NULL b");

    XiMemGroup ga = (XiMemGroup) a->mem_group;
    XiMemGroup gb = (XiMemGroup) b->mem_group;

    /* Non-memory ops never alias. */
    if (ga == XI_MEM_NONE || gb == XI_MEM_NONE)
        return false;

    /* TOP aliases everything (conservative). */
    if (ga == XI_MEM_TOP || gb == XI_MEM_TOP)
        return true;

    /* CONST never aliases stores (immutable data). */
    if (ga == XI_MEM_CONST && xi_is_memory_store(b->op))
        return false;
    if (gb == XI_MEM_CONST && xi_is_memory_store(a->op))
        return false;

    /* Different groups in the lattice are provably disjoint. */
    if (ga != gb)
        return false;

    /* Same group — may alias.
     *
     * Refinement for XI_MEM_FIELD_ID: two field accesses with different
     * known field IDs are disjoint even within the same group. */
    if (ga == XI_MEM_FIELD_ID) {
        XR_DCHECK(gb == XI_MEM_FIELD_ID, "symmetry");
        if (a->aux_int != b->aux_int)
            return false;
    }

    /* Same group for SHARED / UPVAL: disjoint if different slot index. */
    if (ga == XI_MEM_SHARED || ga == XI_MEM_UPVAL || ga == XI_MEM_GLOBAL) {
        if (a->aux_int != b->aux_int)
            return false;
    }

    return true;
}

/* ========== TBAA Annotation Pass ========== */

/* Annotate a single value with its TBAA group. */
static void annotate_value(XiValue *v) {
    XR_DCHECK(v != NULL, "annotate_value: NULL value");

    XiMemGroup g = classify_op(v->op);
    if (g == XI_MEM_NONE) {
        v->mem_group = XI_MEM_NONE;
        return;
    }

    /* Refine field access: if the field index is known (aux_int >= 0)
     * and the op is a field load/store, promote to XI_MEM_FIELD_ID
     * for per-field disjointness. */
    if (g == XI_MEM_FIELD && v->aux_int >= 0) {
        g = XI_MEM_FIELD_ID;
    }

    v->mem_group = (uint8_t) g;
}

/* Annotate all values in a function.  Sets XI_INV_TBAA_ANNOTATED on success. */
XR_FUNC XiPassChange xi_tbaa_annotate(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_tbaa_annotate: NULL func");

    bool any_change = false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Annotate phi nodes. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            uint8_t old = phi->value.mem_group;
            phi->value.mem_group = XI_MEM_NONE;
            if (old != XI_MEM_NONE)
                any_change = true;
        }

        /* Annotate instruction values. */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            uint8_t old = v->mem_group;
            annotate_value(v);
            if (v->mem_group != old)
                any_change = true;
        }
    }

    f->invariant_mask |= XI_INV_TBAA_ANNOTATED;

    return (XiPassChange) {
        .cfg_changed = false,
        .values_changed = any_change,
        .types_changed = false,
        .n_removed = 0,
        .n_added = 0,
    };
}
