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
 * The annotation pass sets XiValue.mem_group for every value in the function
 * from the group ops.def declares.  XI_MEM_NONE means "touches no memory" and
 * answers no-alias to every query, so it is never a fallback for memory the
 * lattice cannot name — that is XI_MEM_TOP.  See xi_tbaa.h.
 *
 * Alias disjointness is not a licence to reorder: that is the memory model's
 * question, answered by xi_op_is_ordering_barrier().
 */

#include "xi_tbaa.h"
#include "xi_evidence.h"
#include "xi_ops_gen.h"
#include "xi_pass.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

/* ========== Op Classification Metadata ========== */

/* A group is tracked when more than one op can name the same region through
 * it, which is what makes an access worth versioning and killing.  The three
 * groups outside the lattice proper are not:
 *
 *   NONE  - the op touches no memory
 *   TOP   - unknown memory; handled as a clobber, never as a tracked access
 *   FRESH - storage this op just allocated; nothing else can name it yet, so
 *           it neither kills nor is killed
 */
static bool op_has_tracked_tbaa_group(uint16_t op) {
    uint8_t group = xi_generated_op_tbaa_group(op);
    return group != XI_GEN_TBAA_NONE && group != XI_GEN_TBAA_TOP && group != XI_GEN_TBAA_FRESH;
}

/* Memory load ops: read a concrete TBAA group but do not write. */
XR_FUNC bool xi_is_memory_load(uint16_t op) {
    uint32_t effects = xi_generated_op_effects(op);
    return op_has_tracked_tbaa_group(op) && (effects & XI_EFFECT_MEMORY_READ) != 0 &&
           (effects & XI_EFFECT_MEMORY_WRITE) == 0;
}

/* Memory store ops: write to a concrete TBAA group. */
XR_FUNC bool xi_is_memory_store(uint16_t op) {
    uint32_t effects = xi_generated_op_effects(op);
    return op_has_tracked_tbaa_group(op) && (effects & XI_EFFECT_MEMORY_WRITE) != 0;
}

/* Memory clobber ops write through an unknown top-level boundary. */
XR_FUNC bool xi_is_memory_clobber(uint16_t op) {
    uint32_t effects = xi_generated_op_effects(op);
    return xi_generated_op_tbaa_group(op) == XI_GEN_TBAA_TOP &&
           (effects & XI_EFFECT_MEMORY_WRITE) != 0;
}

/* Any memory-accessing op (load or store). */
XR_FUNC bool xi_is_memory_op(uint16_t op) {
    return xi_is_memory_load(op) || xi_is_memory_store(op);
}

/* ========== Ordering Barriers (spec §16.9) ========== */

XR_FUNC bool xi_op_is_ordering_barrier(uint16_t op) {
    if (xi_generated_op_sync_order(op) != XI_GEN_SYNC_NONE)
        return true;
    return (xi_generated_op_effects(op) & XI_EFFECT_MAY_SUSPEND) != 0;
}

XR_FUNC bool xi_op_ends_memory_version(uint16_t op) {
    return xi_is_memory_store(op) || xi_is_memory_clobber(op) || xi_op_is_ordering_barrier(op);
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
        case XI_GEN_TBAA_FRESH:
            return XI_MEM_FRESH;
        case XI_GEN_TBAA_NONE:
        case XI_GEN_TBAA__COUNT:
            return XI_MEM_NONE;
    }
    return XI_MEM_NONE;
}

XR_FUNC XiMemGroup xi_tbaa_group_for_op(uint16_t op) {
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

    /* Non-memory ops never alias.  Reaching here with NONE on a value that
     * does touch memory would silently answer "no alias" for unclassified
     * memory, so ops.def forbids the combination and this asserts it. */
    if (ga == XI_MEM_NONE || gb == XI_MEM_NONE) {
        XR_DCHECK(ga != XI_MEM_NONE || !xi_is_memory_op(a->op),
                  "xi_tbaa_may_alias: memory op with XI_MEM_NONE group");
        XR_DCHECK(gb != XI_MEM_NONE || !xi_is_memory_op(b->op),
                  "xi_tbaa_may_alias: memory op with XI_MEM_NONE group");
        return false;
    }

    /* FRESH storage is reachable only through the allocating op's result, so
     * nothing else can name it yet. */
    if (ga == XI_MEM_FRESH || gb == XI_MEM_FRESH)
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

    XiMemGroup g = xi_tbaa_group_for_op(v->op);
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

XR_FUNC bool xi_tbaa_group_matches_op(const XiValue *v) {
    XR_DCHECK(v != NULL, "xi_tbaa_group_matches_op: NULL value");

    XiMemGroup declared = xi_tbaa_group_for_op(v->op);
    if ((XiMemGroup) v->mem_group == declared)
        return true;
    /* The only permitted divergence is the FIELD -> FIELD_ID refinement. */
    return declared == XI_MEM_FIELD && (XiMemGroup) v->mem_group == XI_MEM_FIELD_ID;
}

/* Annotate all values in a function and publish revision-bound alias evidence. */
XR_FUNC XiPassChange xi_tbaa_annotate(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_tbaa_annotate: NULL func");

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Annotate phi nodes. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            phi->value.mem_group = XI_MEM_NONE;
        }

        /* Annotate instruction values. */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            xi_tbaa_annotate_value(v);
            if (xi_is_memory_op(v->op)) {
                XiEvidencePayload payload = {
                    .kind = XI_EVIDENCE_PAYLOAD_U64_PAIR,
                    .as.u64_pair = {.first = v->mem_group, .second = v->aux_int},
                };
                xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_value(v), XI_PROOF_PROVEN,
                                    XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TBAA, v->line,
                                    &payload);
            }
        }
    }

    xi_evidence_publish(f, XI_EVD_ALIAS, xi_evidence_subject_function(), XI_PROOF_PROVEN,
                        XI_EVIDENCE_REASON_NONE, XI_EVIDENCE_PRODUCER_TBAA, 0, NULL);

    return (XiPassChange) {
        .cfg_changed = false,
        .values_changed = false,
        .types_changed = false,
        .n_removed = 0,
        .n_added = 0,
    };
}
