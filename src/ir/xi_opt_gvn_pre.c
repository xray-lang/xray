/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_gvn_pre.c - Global Value Numbering with Partial Redundancy
 *                    Elimination for Xi IR
 *
 * ALGORITHM (dominator-based GVN-PRE):
 *
 *   1. VALUE NUMBERING:
 *      Hash-consing table keyed by the complete semantic identity of an
 *      instruction: op, result type/rep, every operand VN, semantic aux
 *      data, lowering contract, and proof identities.  Binary commutative
 *      ops are normalized. Each unique expression receives a dense value
 *      number (VN); two values with the same VN are semantically equivalent.
 *
 *   2. FULL REDUNDANCY ELIMINATION:
 *      Walk blocks in dominator-tree RPO. If a value's VN already has
 *      a dominating definition, replace the use with XI_COPY.
 *
 *   3. TBAA-AWARE LOAD ELIMINATION:
 *      Memory loads participate when current alias evidence exists. A
 *      load is redundant if an earlier load with the same VN exists and
 *      no intervening store may alias it (checked via mem_group).
 *
 *   4. PARTIAL REDUNDANCY ELIMINATION (pure expressions only):
 *      For each multi-predecessor block, if a pure expression's VN is
 *      already available on at least one incoming edge, materialize a
 *      phi merging per-edge leaders and replace the join expression
 *      with XI_COPY(phi). Missing edges receive a clone, but only when
 *      the predecessor flows uniquely into the join (no critical edge
 *      splitting) and all operands already dominate it. Memory loads
 *      stay on path 3 — speculating them past potential clobbers
 *      requires Memory SSA version comparison, not just TBAA.
 */

#include "xi_opt_gvn_pre.h"
#include "xi_evidence.h"
#include "xi_analysis.h"
#include "xi_effect.h"
#include "xi_own.h"
#include "xi_tbaa.h"
#include "xi_memssa.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Pure / Commutative Classification ========== */

static bool gvn_is_eligible(const XiValue *v) {
    if (!v)
        return false;
    /* Owned RC results carry a distinct lifetime edge even when their value
     * semantics compare equal.  Replacing one with a dominating leader after
     * the Owned stage would also have to rebuild the leader's retain/release
     * frontier.  GVN has no current ownership/lifetime proof for that rewrite,
     * so fail closed instead of manufacturing a use-after-release. */
    bool explicit_vector_value = v->op >= XI_VEC_LOAD && v->op <= XI_VEC_REDUCE_ADD;
    if (!explicit_vector_value && xi_own_type_is_rc(v->type))
        return false;
    uint8_t vn_kind = xi_op_value_numbering_kind(v->op);
    if (vn_kind == XI_GEN_VN_NONE)
        return false;
    /* 0-arg memory loads (shared/global/upval) use aux_int, not args. */
    if (v->nargs == 0 && vn_kind != XI_GEN_VN_MEMORY_READ)
        return false;
    if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW))
        return false;
    return true;
}

static bool gvn_is_commutative(uint16_t op) {
    return xi_op_is_commutative(op);
}

static bool gvn_is_load(uint16_t op) {
    return xi_op_value_numbering_reads_mem(op);
}

static void gvn_rewrite_to_copy(XiValue *value, XiValue *source, int64_t copy_kind) {
    XR_DCHECK(value != NULL && source != NULL, "gvn_rewrite_to_copy: NULL value");
    value->op = XI_COPY;
    value->args[0] = source;
    value->nargs = 1;
    value->flags = xi_op_default_effects(XI_COPY);
    value->transfer_mode = XR_TRANSFER_SHARE;
    value->aux_kind = XI_AUX_KIND_NONE;
    value->mem_group = XI_MEM_NONE;
    value->lowering_flags = 0;
    value->param_mode = XR_PARAM_READ;
    value->aux_int = copy_kind;
    value->aux = NULL;
    memset(&value->conversion, 0, sizeof(value->conversion));
    value->call_plan = NULL;
    memset(&value->view_evidence, 0, sizeof(value->view_evidence));
    memset(&value->call_return_ownership, 0, sizeof(value->call_return_ownership));
    value->call_return_ownership.param_index = -1;
    value->result_alias_operand = -1;
    value->xg_callsite_id = 0;
    value->xa_intrinsic_id = 0;
    value->xg_method_id = 0;
    value->xg_interface_dispatch_slot = UINT32_MAX;
    value->xg_json_codec_id = 0;
    value->xg_object_access_id = 0;
    value->xg_object_merge_id = 0;
    value->xg_key_access_id = 0;
    value->xg_map_shape_id = 0;
    value->xg_class_field_id = 0;
    value->xg_sequence_access_id = 0;
    value->xg_capacity_op_id = 0;
    value->xg_bulk_op_id = 0;
    value->xg_encoding_op_id = 0;
    value->move_evidence_id = 0;
    value->move_source_root_id = 0;
    value->move_source_symbol_id = 0;
    value->move_storage_plan_id = 0;
    value->move_transfer_plan_id = 0;
    value->move_evidence_bits = 0;
    value->move_source_capability = 0;
    value->move_target_capability = 0;
    value->move_source_domain = 0;
    value->move_target_domain = 0;
    value->enum_metadata_owner = NULL;
    value->json_decode_target_type = NULL;
    value->enum_metadata_field = 0;
    value->enum_metadata_kind = 0;
}

/* ========== Value Number Table ========== */

#define GVN_MIN_TABLE 64
#define GVN_NO_VN UINT32_MAX

typedef struct {
    uint32_t hash;       /* 0 = empty slot */
    uint32_t vn;         /* the assigned value number */
    XiValue *leader;     /* canonical leader value */
    uint32_t leader_blk; /* block index of leader (for dominance check) */
} VnEntry;

typedef struct {
    VnEntry *table;
    uint32_t size;
    uint32_t mask;
    uint32_t next_vn;

    /* Per-value VN assignment (indexed by value id). */
    uint32_t *val_vn;
    uint32_t val_vn_cap;
} VnTable;

static void vn_init(VnTable *vn, uint32_t est_values) {
    vn->size = GVN_MIN_TABLE;
    while (vn->size < est_values * 2)
        vn->size <<= 1;
    vn->mask = vn->size - 1;
    vn->table = (VnEntry *) xr_calloc(vn->size, sizeof(VnEntry));
    vn->next_vn = 1; /* VN 0 reserved for "unique" (no match) */
    vn->val_vn_cap = est_values + 16;
    vn->val_vn = (uint32_t *) xr_malloc(vn->val_vn_cap * sizeof(uint32_t));
    if (vn->val_vn) {
        for (uint32_t i = 0; i < vn->val_vn_cap; i++)
            vn->val_vn[i] = GVN_NO_VN;
    }
}

static void vn_destroy(VnTable *vn) {
    xr_free(vn->table);
    xr_free(vn->val_vn);
    memset(vn, 0, sizeof(*vn));
}

static uint32_t vn_get(const VnTable *vn, const XiValue *v) {
    if (!v || v->id >= vn->val_vn_cap)
        return GVN_NO_VN;
    return vn->val_vn[v->id];
}

static void vn_set(VnTable *vn, const XiValue *v, uint32_t num) {
    XR_DCHECK(v != NULL, "vn_set: NULL value");
    if (v->id >= vn->val_vn_cap)
        return;
    vn->val_vn[v->id] = num;
}

static uint32_t vn_hash_mix(uint32_t h, uint64_t word) {
    h ^= (uint32_t) word;
    h *= 16777619u;
    h ^= (uint32_t) (word >> 32);
    h *= 16777619u;
    return h;
}

static void vn_normalized_binary_vns(const VnTable *vn, const XiValue *v, uint32_t *out0,
                                     uint32_t *out1) {
    uint32_t vn0 = vn_get(vn, v->args[0]);
    uint32_t vn1 = vn_get(vn, v->args[1]);
    if (gvn_is_commutative(v->op) && vn0 > vn1) {
        uint32_t tmp = vn0;
        vn0 = vn1;
        vn1 = tmp;
    }
    *out0 = vn0;
    *out1 = vn1;
}

static uint32_t vn_value_hash(const VnTable *vn, const XiValue *v) {
    uint32_t h = 2166136261u;
    h = vn_hash_mix(h, v->op);
    h = vn_hash_mix(h, v->nargs);
    h = vn_hash_mix(h, (uintptr_t) v->type);
    h = vn_hash_mix(h, v->flags);
    h = vn_hash_mix(h, v->rep);
    h = vn_hash_mix(h, v->transfer_mode);
    h = vn_hash_mix(h, v->aux_kind);
    h = vn_hash_mix(h, v->mem_group);
    h = vn_hash_mix(h, v->lowering_flags);
    h = vn_hash_mix(h, (uint64_t) v->conversion.kind);
    h = vn_hash_mix(h, v->conversion.source_scalar_rep);
    h = vn_hash_mix(h, v->conversion.target_scalar_rep);
    h = vn_hash_mix(h, v->conversion.is_implicit ? 1u : 0u);
    h = vn_hash_mix(h, v->conversion.is_compile_time ? 1u : 0u);
    h = vn_hash_mix(h, (uint64_t) v->aux_int);
    h = vn_hash_mix(h, (uintptr_t) v->aux);
    h = vn_hash_mix(h, (uintptr_t) v->call_plan);
    h = vn_hash_mix(h, (uint16_t) v->result_alias_operand);
    h = vn_hash_mix(h, v->xa_intrinsic_id);
    h = vn_hash_mix(h, v->xg_callsite_id);
    h = vn_hash_mix(h, v->xg_method_id);
    h = vn_hash_mix(h, v->xg_interface_dispatch_slot);
    h = vn_hash_mix(h, v->xg_json_codec_id);
    h = vn_hash_mix(h, v->xg_object_access_id);
    h = vn_hash_mix(h, v->xg_object_merge_id);
    h = vn_hash_mix(h, v->xg_key_access_id);
    h = vn_hash_mix(h, v->xg_map_shape_id);
    h = vn_hash_mix(h, v->xg_class_field_id);
    h = vn_hash_mix(h, v->xg_sequence_access_id);
    h = vn_hash_mix(h, v->xg_capacity_op_id);
    h = vn_hash_mix(h, v->xg_bulk_op_id);
    h = vn_hash_mix(h, v->xg_encoding_op_id);

    if (v->nargs == 2) {
        uint32_t vn0 = GVN_NO_VN, vn1 = GVN_NO_VN;
        vn_normalized_binary_vns(vn, v, &vn0, &vn1);
        h = vn_hash_mix(h, vn0);
        h = vn_hash_mix(h, vn1);
    } else {
        for (uint16_t i = 0; i < v->nargs; i++)
            h = vn_hash_mix(h, vn_get(vn, v->args[i]));
    }
    return h ? h : 1;
}

static bool vn_same_semantic_metadata(const XiValue *a, const XiValue *b) {
    return a->op == b->op && a->nargs == b->nargs && a->type == b->type && a->flags == b->flags &&
           a->rep == b->rep && a->transfer_mode == b->transfer_mode && a->aux_kind == b->aux_kind &&
           a->mem_group == b->mem_group && a->lowering_flags == b->lowering_flags &&
           a->conversion.kind == b->conversion.kind &&
           a->conversion.source_scalar_rep == b->conversion.source_scalar_rep &&
           a->conversion.target_scalar_rep == b->conversion.target_scalar_rep &&
           a->conversion.is_implicit == b->conversion.is_implicit &&
           a->conversion.is_compile_time == b->conversion.is_compile_time &&
           a->aux_int == b->aux_int && a->aux == b->aux && a->call_plan == b->call_plan &&
           a->result_alias_operand == b->result_alias_operand &&
           a->xa_intrinsic_id == b->xa_intrinsic_id && a->xg_callsite_id == b->xg_callsite_id &&
           a->xg_method_id == b->xg_method_id &&
           a->xg_interface_dispatch_slot == b->xg_interface_dispatch_slot &&
           a->xg_json_codec_id == b->xg_json_codec_id &&
           a->xg_object_access_id == b->xg_object_access_id &&
           a->xg_object_merge_id == b->xg_object_merge_id &&
           a->xg_key_access_id == b->xg_key_access_id && a->xg_map_shape_id == b->xg_map_shape_id &&
           a->xg_class_field_id == b->xg_class_field_id &&
           a->xg_sequence_access_id == b->xg_sequence_access_id &&
           a->xg_capacity_op_id == b->xg_capacity_op_id && a->xg_bulk_op_id == b->xg_bulk_op_id &&
           a->xg_encoding_op_id == b->xg_encoding_op_id;
}

static bool vn_values_equal(const VnTable *vn, const XiValue *a, const XiValue *b) {
    if (!vn_same_semantic_metadata(a, b))
        return false;
    if (a->nargs == 2) {
        uint32_t a0 = GVN_NO_VN, a1 = GVN_NO_VN, b0 = GVN_NO_VN, b1 = GVN_NO_VN;
        vn_normalized_binary_vns(vn, a, &a0, &a1);
        vn_normalized_binary_vns(vn, b, &b0, &b1);
        return a0 == b0 && a1 == b1;
    }
    for (uint16_t i = 0; i < a->nargs; i++) {
        if (vn_get(vn, a->args[i]) != vn_get(vn, b->args[i]))
            return false;
    }
    return true;
}

/* Look up or insert an expression.  Returns the VN and sets *leader
 * to the canonical value (NULL if this is the first occurrence). */
static uint32_t vn_lookup(VnTable *vn, XiValue *val, uint32_t blk_idx, XiValue **leader) {
    XR_DCHECK(vn->table != NULL, "vn_lookup: NULL table");
    uint32_t h = vn_value_hash(vn, val);
    uint32_t slot = h & vn->mask;

    for (uint32_t probe = 0; probe < vn->size; probe++) {
        uint32_t idx = (slot + probe) & vn->mask;
        VnEntry *e = &vn->table[idx];

        if (e->hash == 0) {
            /* Empty: this is a new expression. */
            uint32_t new_vn = vn->next_vn++;
            e->hash = h;
            e->vn = new_vn;
            e->leader = val;
            e->leader_blk = blk_idx;
            *leader = NULL;
            return new_vn;
        }

        if (e->hash == h && vn_values_equal(vn, e->leader, val)) {
            /* Match: return existing VN + leader. */
            *leader = e->leader;
            return e->vn;
        }
    }

    /* Table full (should not happen at 50% load factor). */
    *leader = NULL;
    return vn->next_vn++;
}

/* ========== Store Tracking for Load Elimination ========== */

/* Check if any store between a leader load and the current load may
 * alias.  When Memory SSA is available, cross-block loads are safe if
 * they consume the same memory version (no intervening clobber).
 * Without MemSSA, falls back to same-block-only scanning. */
static bool has_aliasing_store_between(const XiFunc *f, const XiValue *leader,
                                       const XiValue *current, const XiBlock *cur_blk,
                                       uint32_t cur_vi, const XiMemSSA *mssa) {
    (void) f;

    /* If leader is not in the same block, try Memory SSA. */
    if (!leader->block || leader->block != cur_blk) {
        if (mssa) {
            XiMemAccess *la = xi_memssa_access(mssa, leader);
            XiMemAccess *ca = xi_memssa_access(mssa, current);
            if (la && ca && la->use_ver == ca->use_ver)
                return false;
        }
        return true;
    }

    /* Scan from leader's position to current position in the block. */
    bool past_leader = false;
    for (uint32_t vi = 0; vi < cur_vi; vi++) {
        XiValue *v = cur_blk->values[vi];
        if (!v)
            continue;
        if (v == leader) {
            past_leader = true;
            continue;
        }
        if (!past_leader)
            continue;

        /* An ordering barrier invalidates every load regardless of its TBAA
         * group: a suspension lets another task mutate arbitrary reachable
         * state, and a :sync edge means the memory model forbids reusing a
         * value loaded on the other side of it (spec §16.9). */
        if (xi_op_is_ordering_barrier(v->op))
            return true;

        /* Check if this instruction is a store that may alias current. */
        if (xi_is_memory_store(v->op) || xi_is_memory_clobber(v->op)) {
            if (xi_tbaa_may_alias(v, current))
                return true;
        }
    }

    return false;
}

/* ========== Partial Redundancy Elimination ========== */

typedef struct {
    uint32_t n_inserted;
    uint32_t n_phis;
    uint32_t n_replaced;
} GvnPreStats;

/* Pure-expression PRE candidate: eligible op without memory effects.
 * Memory loads still rely on TBAA-based liveness reasoning, so they
 * stay on the full-redundancy path. */
static bool pre_is_candidate(const XiValue *v) {
    if (!gvn_is_eligible(v))
        return false;
    /* PRE materializes new definitions and PHIs after ARC insertion.  An
     * ownership-root result would therefore require a freshly proven lifetime
     * frontier, which this pass does not construct.  Full redundancy remains
     * available for explicit vector values below, where the replacement is an
     * explicit value clone rather than an ownership-identity copy. */
    if (xi_own_type_is_rc(v->type))
        return false;
    if (gvn_is_load(v->op))
        return false;
    if (v->flags & XI_FLAG_MEM_ANY)
        return false;
    return true;
}

/* True if 'pred' flows only into 'join' (no critical edge to split). */
static bool pre_pred_clean(const XiBlock *pred, const XiBlock *join) {
    return pred && pred->kind == XI_BLOCK_PLAIN && pred->succs[0] == join && pred->succs[1] == NULL;
}

/* All operands of 'v' must be defined in blocks that dominate 'pred',
 * so that a clone of 'v' can be safely inserted at the end of 'pred'. */
static bool pre_operands_dominate_pred(const XiValue *v, const XiBlock *pred) {
    for (uint16_t a = 0; a < v->nargs; a++) {
        XiValue *arg = v->args[a];
        if (!arg || !arg->block)
            return false;
        if (!xi_dominates(arg->block, pred))
            return false;
    }
    return true;
}

/* Look up a value with VN == target_vn that is available along the edge
 * entering 'pred' — defined in 'pred' itself or in any block dominating
 * 'pred'. Returns NULL if no such leader exists. */
static XiValue *pre_find_available(const XiFunc *f, const VnTable *vn, uint32_t target_vn,
                                   const XiBlock *pred, const XiValue *skip) {
    XR_DCHECK(f != NULL, "pre_find_available: NULL func");
    if (!pred)
        return NULL;
    for (uint32_t vi = 0; vi < pred->nvalues; vi++) {
        XiValue *v = pred->values[vi];
        if (v && v != skip && vn_get(vn, v) == target_vn)
            return v;
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *b = f->blocks[bi];
        if (!b || b == pred)
            continue;
        if (!xi_dominates(b, pred))
            continue;
        for (uint32_t vi = 0; vi < b->nvalues; vi++) {
            XiValue *v = b->values[vi];
            if (v && v != skip && vn_get(vn, v) == target_vn)
                return v;
        }
    }
    return NULL;
}

/* Clone the pure expression 'tmpl' at the end of 'pred', reusing the
 * same operand pointers (callers must have verified that they dominate
 * 'pred'). Returns NULL on allocation failure. */
static XiValue *pre_clone_value(XiFunc *f, XiBlock *pred, const XiValue *tmpl) {
    XR_DCHECK(f != NULL && pred != NULL && tmpl != NULL, "pre_clone_value: NULL arg");
    XiValue *ins = xi_value_new(f, pred, tmpl->op, tmpl->type, tmpl->nargs);
    if (!ins)
        return NULL;
    for (uint16_t a = 0; a < tmpl->nargs; a++)
        ins->args[a] = tmpl->args[a];
    if (!xi_value_clone_metadata(f, ins, tmpl))
        return NULL;
    return ins;
}

/* Try to materialize a partially-available pure expression at 'blk' by
 * inserting clones along edges where it is missing and replacing the
 * join definition with a copy of a freshly inserted phi. Bails out (no
 * mutation) if any predecessor is unsuitable. */
static bool pre_try_value(XiFunc *f, VnTable *vn, XiBlock *blk, XiValue *v, GvnPreStats *stats) {
    if (!v || blk->npreds < 2 || !pre_is_candidate(v))
        return false;
    uint32_t target_vn = vn_get(vn, v);
    if (target_vn == GVN_NO_VN)
        return false;

    XiValue **edge_vals = (XiValue **) xr_calloc(blk->npreds, sizeof(XiValue *));
    if (!edge_vals)
        return false;

    /* Each predecessor must either already provide a leader for this VN
     * or accept a clone (PLAIN single-successor block whose operands
     * already dominate it). At least one edge must already be available
     * to keep the transform anticipability-bounded — pure speculation
     * (no edge available) would risk inserting work onto cold paths. */
    bool any_avail = false;
    bool ok = true;
    for (uint16_t p = 0; p < blk->npreds; p++) {
        XiBlock *pred = blk->preds[p];
        if (!pred) {
            ok = false;
            break;
        }
        XiValue *avail = pre_find_available(f, vn, target_vn, pred, v);
        if (avail) {
            edge_vals[p] = avail;
            any_avail = true;
            continue;
        }
        if (!pre_pred_clean(pred, blk) || !pre_operands_dominate_pred(v, pred)) {
            ok = false;
            break;
        }
    }
    if (!ok || !any_avail) {
        xr_free(edge_vals);
        return false;
    }

    uint32_t inserted = 0;
    for (uint16_t p = 0; p < blk->npreds; p++) {
        if (edge_vals[p])
            continue;
        XiValue *clone = pre_clone_value(f, blk->preds[p], v);
        if (!clone) {
            xr_free(edge_vals);
            return false;
        }
        vn_set(vn, clone, target_vn);
        edge_vals[p] = clone;
        inserted++;
    }

    XiPhi *phi = xi_phi_new(f, blk, v->type, blk->npreds);
    if (!phi) {
        xr_free(edge_vals);
        return false;
    }
    phi->value.rep = v->rep;
    phi->value.line = v->line;
    for (uint16_t p = 0; p < blk->npreds; p++)
        phi->value.args[p] = edge_vals[p];

    /* Replace the join expression with a copy of the phi. */
    gvn_rewrite_to_copy(v, &phi->value, XI_COPY_KIND_IDENTITY);
    vn_set(vn, v, target_vn);

    stats->n_inserted += inserted;
    stats->n_phis++;
    stats->n_replaced++;
    xr_free(edge_vals);
    return true;
}

/* Walk every multi-predecessor block and try PRE on each value. Each
 * attempt either commits or leaves the IR untouched. */
static GvnPreStats gvn_eliminate_partial(XiFunc *f, VnTable *vn) {
    GvnPreStats stats = {0, 0, 0};
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk || blk->npreds < 2)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++)
            pre_try_value(f, vn, blk, blk->values[vi], &stats);
    }
    return stats;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_gvn_pre(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_gvn_pre: NULL func");
    if (f->nblocks < 1)
        return xi_pass_no_change();

    /* Ensure dominator tree is available (cached across passes). */
    xi_ensure_dominators(f);

    bool tbaa_active = xi_evidence_domain_is_proven_current(f, XI_EVD_ALIAS);

    /* Build Memory SSA for cross-block load elimination. */
    XiMemSSA *mssa = NULL;
    if (tbaa_active)
        mssa = xi_memssa_build(f);

    /* Count total values for table sizing. */
    uint32_t total_values = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi])
            total_values += f->blocks[bi]->nvalues;
    }

    VnTable vn;
    vn_init(&vn, total_values);
    if (!vn.table || !vn.val_vn) {
        vn_destroy(&vn);
        return xi_pass_no_change();
    }

    /* Assign unique VNs to params and constants (they are their own leaders). */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_PARAM || v->op == XI_CONST) {
                vn_set(&vn, v, vn.next_vn++);
            }
        }
    }

    uint32_t n_replaced = 0;

    /* Walk blocks in RPO (dominator-tree pre-order).
     * f->blocks is already in approximate RPO after lowering. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            /* Skip already-processed or ineligible values. */
            if (!gvn_is_eligible(v)) {
                /* Assign unique VN so dependents can still hash. */
                if (vn_get(&vn, v) == GVN_NO_VN)
                    vn_set(&vn, v, vn.next_vn++);
                continue;
            }

            /* A semantic key is valid only when every operand already has a VN. */
            bool missing_operand_vn = false;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                if (!v->args[ai] || vn_get(&vn, v->args[ai]) == GVN_NO_VN) {
                    missing_operand_vn = true;
                    break;
                }
            }
            if (missing_operand_vn) {
                vn_set(&vn, v, vn.next_vn++);
                continue;
            }

            XiValue *leader = NULL;
            uint32_t this_vn = vn_lookup(&vn, v, bi, &leader);

            if (!leader) {
                vn_set(&vn, v, this_vn);
                continue; /* First occurrence — nothing to eliminate. */
            }

            /* Check dominance. */
            if (!leader->block || !xi_dominates(leader->block, blk)) {
                if (gvn_is_load(v->op))
                    vn_set(&vn, v, vn.next_vn++);
                else
                    vn_set(&vn, v, this_vn);
                continue;
            }

            /* For memory loads: check no aliasing store intervenes. */
            if (gvn_is_load(v->op)) {
                if (!tbaa_active || has_aliasing_store_between(f, leader, v, blk, vi, mssa)) {
                    vn_set(&vn, v, vn.next_vn++);
                    continue;
                }
            }

            vn_set(&vn, v, this_vn);

            /* Replace with copy of leader. */
            if (v->nargs == 0 || !v->args) {
                v->args = (XiValue **) xi_func_arena_alloc(f, sizeof(XiValue *));
                if (!v->args)
                    continue;
            }
            gvn_rewrite_to_copy(v, leader,
                                xi_own_type_is_rc(v->type) ? XI_COPY_KIND_VALUE_CLONE
                                                           : XI_COPY_KIND_IDENTITY);
            n_replaced++;
        }
    }

    GvnPreStats pre = gvn_eliminate_partial(f, &vn);
    vn_destroy(&vn);
    xi_memssa_destroy(mssa);

    if (n_replaced == 0 && pre.n_replaced == 0 && pre.n_inserted == 0 && pre.n_phis == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.values_changed = true;
    chg.n_removed = n_replaced + pre.n_replaced;
    chg.n_added = pre.n_inserted + pre.n_phis;
    return chg;
}
