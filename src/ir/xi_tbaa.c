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
#include "xi_ops_gen.h"
#include "xi_pass.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

/* ========== Op Classification Metadata ========== */

static bool op_has_direct_tbaa_access(uint16_t op) {
    uint8_t group = xi_generated_op_tbaa_group(op);
    return group != XI_GEN_TBAA_NONE && group != XI_GEN_TBAA_TOP;
}

/* Memory load ops: read a concrete TBAA group but do not write. */
XR_FUNC bool xi_is_memory_load(uint16_t op) {
    uint32_t effects = xi_generated_op_effects(op);
    return op_has_direct_tbaa_access(op) && (effects & XI_EFFECT_MEMORY_READ) != 0 &&
           (effects & XI_EFFECT_MEMORY_WRITE) == 0;
}

/* Memory store ops: write to a concrete TBAA group. */
XR_FUNC bool xi_is_memory_store(uint16_t op) {
    uint32_t effects = xi_generated_op_effects(op);
    return op_has_direct_tbaa_access(op) && (effects & XI_EFFECT_MEMORY_WRITE) != 0;
}

/* Any memory-accessing op (load or store). */
XR_FUNC bool xi_is_memory_op(uint16_t op) {
    return xi_is_memory_load(op) || xi_is_memory_store(op);
}

/* ========== TBAA Group Assignment ========== */

static XiMemGroup generated_group_to_mem_group(uint8_t group) {
    switch (group) {
        case XI_GEN_TBAA_TOP:
            return XI_MEM_TOP;
        case XI_GEN_TBAA_CONST:
            return XI_MEM_CONST;
        case XI_GEN_TBAA_FIELD:
            return XI_MEM_FIELD;
        case XI_GEN_TBAA_ARRAY:
            return XI_MEM_ARRAY;
        case XI_GEN_TBAA_STRUCT:
            return XI_MEM_STRUCT;
        case XI_GEN_TBAA_SHARED:
            return XI_MEM_SHARED;
        case XI_GEN_TBAA_GLOBAL:
            return XI_MEM_GLOBAL;
        case XI_GEN_TBAA_UPVAL:
            return XI_MEM_UPVAL;
        case XI_GEN_TBAA_TLS:
            return XI_MEM_TLS;
        case XI_GEN_TBAA_JSON:
            return XI_MEM_JSON;
        case XI_GEN_TBAA_TUPLE:
            return XI_MEM_TUPLE;
        case XI_GEN_TBAA_CHAN:
            return XI_MEM_CHAN;
        case XI_GEN_TBAA_NONE:
        case XI_GEN_TBAA__COUNT:
            return XI_MEM_NONE;
    }
    return XI_MEM_NONE;
}

static XiMemGroup classify_op(uint16_t op) {
    return generated_group_to_mem_group(xi_generated_op_tbaa_group(op));
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

XR_FUNC void xi_tbaa_annotate_value(XiValue *v) {
    XR_DCHECK(v != NULL, "xi_tbaa_annotate_value: NULL value");

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
            xi_tbaa_annotate_value(v);
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
