/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_coalesce.c - Aggressive register coalescing for XIR
 *
 * KEY CONCEPT:
 *   Merges MOV operands that don't interfere, reducing vreg count
 *   and eliminating redundant moves before register allocation.
 *
 * WHY THIS DESIGN:
 *   - Conflict matrix gives deterministic coalescing decisions
 *   - Frequency-ordered processing maximizes hot-path benefit
 *   - Union-Find for O(α(n)) merge/query operations
 */

#include "xir_coalesce.h"
#include "xir_bset.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>

/* ========== Union-Find ========== */

typedef struct {
    uint32_t *parent;
    uint32_t *rank;
    uint32_t  n;
} UnionFind;

static void uf_init(UnionFind *uf, uint32_t n) {
    XR_DCHECK(uf != NULL, "uf_init: NULL uf");
    uf->n = n;
    uf->parent = xr_malloc(n * sizeof(uint32_t));
    uf->rank   = xr_calloc(n, sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) uf->parent[i] = i;
}

static void uf_free(UnionFind *uf) {
    XR_DCHECK(uf != NULL, "uf_free: NULL uf");
    xr_free(uf->parent);
    xr_free(uf->rank);
}

static uint32_t uf_find(UnionFind *uf, uint32_t x) {
    while (uf->parent[x] != x) {
        uf->parent[x] = uf->parent[uf->parent[x]]; // path halving
        x = uf->parent[x];
    }
    return x;
}

static void uf_union(UnionFind *uf, uint32_t a, uint32_t b) {
    XR_DCHECK(uf != NULL, "uf_union: NULL uf");
    a = uf_find(uf, a);
    b = uf_find(uf, b);
    if (a == b) return;
    // Union by rank, prefer lower vreg index as root (stable naming)
    if (uf->rank[a] < uf->rank[b]) {
        uint32_t t = a; a = b; b = t;
    }
    uf->parent[b] = a;
    if (uf->rank[a] == uf->rank[b]) uf->rank[a]++;
}

/* ========== Move Collection ========== */

typedef struct {
    uint32_t dst_vreg;
    uint32_t src_vreg;
    uint32_t blk_idx; // block index for loop depth
    uint32_t ins_idx; // instruction index within block
    uint32_t freq; // loop-depth weighted frequency
} CoalesceMove;

static int move_freq_cmp(const void *a, const void *b) {
    const CoalesceMove *ma = (const CoalesceMove *)a;
    const CoalesceMove *mb = (const CoalesceMove *)b;
    if (ma->freq > mb->freq) return -1;
    if (ma->freq < mb->freq) return 1;
    // Tie-break by position for determinism
    if (ma->blk_idx != mb->blk_idx) return (int)ma->blk_idx - (int)mb->blk_idx;
    return (int)ma->ins_idx - (int)mb->ins_idx;
}

/* ========== Conflict Matrix ========== */

/*
 * Triangular bit matrix: conflict(a, b) stored at bit index
 * min(a,b) * nvreg + max(a,b). Only tracks vreg pairs involved
 * in MOV instructions to keep the matrix small.
 */
typedef struct {
    uint64_t *bits;
    uint32_t  nvreg;
    uint32_t  nwords;
} ConflictMatrix;

static void cm_init(ConflictMatrix *cm, uint32_t nvreg) {
    XR_DCHECK(cm != NULL, "cm_init: NULL cm");
    cm->nvreg = nvreg;
    // Upper triangle: nvreg * (nvreg-1) / 2 bits, rounded up
    uint64_t nbits = (uint64_t)nvreg * nvreg;
    cm->nwords = (uint32_t)((nbits + 63) / 64);
    cm->bits = xr_calloc(cm->nwords, sizeof(uint64_t));
}

static void cm_free(ConflictMatrix *cm) {
    xr_free(cm->bits);
}

static inline void cm_set(ConflictMatrix *cm, uint32_t a, uint32_t b) {
    if (a == b) return;
    if (a > b) { uint32_t t = a; a = b; b = t; }
    uint64_t idx = (uint64_t)a * cm->nvreg + b;
    cm->bits[idx / 64] |= (uint64_t)1 << (idx % 64);
}

static inline bool cm_test(const ConflictMatrix *cm, uint32_t a, uint32_t b) {
    if (a == b) return false;
    if (a > b) { uint32_t t = a; a = b; b = t; }
    uint64_t idx = (uint64_t)a * cm->nvreg + b;
    return (cm->bits[idx / 64] & ((uint64_t)1 << (idx % 64))) != 0;
}

/*
 * Check if two coalesced groups conflict.
 * Walk all members of group_a and group_b via union-find,
 * checking pairwise conflicts in the original matrix.
 */
static bool groups_conflict(const ConflictMatrix *cm, UnionFind *uf,
                            uint32_t root_a, uint32_t root_b) {
    /* For each member of group_a, check against all members of group_b.
     * Since union-find doesn't provide member enumeration, we scan all
     * vregs and check if they belong to either group. This is O(n) per
     * call, but total work is bounded by the number of MOVs. */
    for (uint32_t i = 0; i < uf->n; i++) {
        if (uf_find(uf, i) != root_a) continue;
        for (uint32_t j = 0; j < uf->n; j++) {
            if (uf_find(uf, j) != root_b) continue;
            if (cm_test(cm, i, j)) return true;
        }
    }
    return false;
}

/* ========== Build Conflict Matrix from Liveness ========== */

static void build_conflicts(ConflictMatrix *cm, XirFunc *func, XirLive *live) {
    XR_DCHECK(cm != NULL, "build_conflicts: NULL cm");
    XR_DCHECK(func != NULL, "build_conflicts: NULL func");
    XR_DCHECK(live != NULL, "build_conflicts: NULL live");
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        XirBSet live_set;
        xir_bset_init(&live_set, func->nvreg);

        // Start with live-out
        if (bi < live->nblk)
            xir_bset_copy(&live_set, &live->blocks[bi].live_out);

        /* Backward scan: at each def point, the defined vreg conflicts
         * with all currently live vregs (except the MOV source) */
        for (int32_t ii = (int32_t)blk->nins - 1; ii >= 0; ii--) {
            XirIns *ins = &blk->ins[ii];

            // For MOV, the src doesn't conflict with dst at the def point
            uint32_t ignore_vreg = UINT32_MAX;
            if (xir_op_is_copy(ins->op) && xir_ref_is_vreg(ins->args[0]))
                ignore_vreg = XIR_REF_INDEX(ins->args[0]);

            // Process def: dst conflicts with all live vregs
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t dv = XIR_REF_INDEX(ins->dst);
                if (dv < func->nvreg) {
                    int iter = 0, bit;
                    while ((bit = xir_bset_iter(&live_set, &iter)) >= 0) {
                        uint32_t lv = (uint32_t)bit;
                        if (lv != dv && lv != ignore_vreg)
                            cm_set(cm, dv, lv);
                    }
                    xir_bset_clr(&live_set, dv);
                }
            }

            // Process uses: mark as live
            for (int a = 0; a < 2; a++) {
                if (xir_ref_is_vreg(ins->args[a])) {
                    uint32_t av = XIR_REF_INDEX(ins->args[a]);
                    if (av < func->nvreg)
                        xir_bset_set(&live_set, av);
                }
            }
        }

        // Phi defs also create conflicts
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            if (xir_ref_is_vreg(phi->dst)) {
                uint32_t dv = XIR_REF_INDEX(phi->dst);
                if (dv < func->nvreg) {
                    int iter = 0, bit;
                    while ((bit = xir_bset_iter(&live_set, &iter)) >= 0) {
                        uint32_t lv = (uint32_t)bit;
                        if (lv != dv) cm_set(cm, dv, lv);
                    }
                }
            }
        }

        xir_bset_free(&live_set);
    }
}

/* ========== Rewrite Instructions ========== */

static inline void rewrite_ref(UnionFind *uf, XirRef *ref) {
    if (xir_ref_is_vreg(*ref)) {
        uint32_t v = XIR_REF_INDEX(*ref);
        uint32_t root = uf_find(uf, v);
        if (root != v)
            *ref = XIR_REF(XIR_REF_VREG, root);
    }
}

static void rewrite_instructions(XirFunc *func, UnionFind *uf) {
    XR_DCHECK(func != NULL, "rewrite_instructions: NULL func");
    XR_DCHECK(uf != NULL, "rewrite_instructions: NULL uf");
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        // Rewrite Phi nodes
        for (XirPhi *phi = blk->phis; phi; phi = phi->next) {
            rewrite_ref(uf, &phi->dst);
            for (uint32_t a = 0; a < phi->narg; a++)
                rewrite_ref(uf, &phi->args[a]);
        }

        // Rewrite instructions
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            rewrite_ref(uf, &ins->dst);
            rewrite_ref(uf, &ins->args[0]);
            rewrite_ref(uf, &ins->args[1]);
        }

        // Rewrite terminator
        rewrite_ref(uf, &blk->jmp.arg);
    }
}

/* ========== Delete Coalesced MOVs ========== */

static uint32_t delete_coalesced_movs(XirFunc *func) {
    uint32_t deleted = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (!xir_op_is_copy(ins->op)) continue;
            if (!xir_ref_is_vreg(ins->dst) || !xir_ref_is_vreg(ins->args[0]))
                continue;

            uint32_t dv = XIR_REF_INDEX(ins->dst);
            uint32_t sv = XIR_REF_INDEX(ins->args[0]);
            // After rewrite, if dst == src, the MOV is a no-op
            if (dv == sv) {
                ins->op = XIR_NOP;
                ins->dst = XIR_NONE;
                ins->args[0] = XIR_NONE;
                ins->args[1] = XIR_NONE;
                deleted++;
            }
        }
    }
    return deleted;
}

/* ========== Main Entry ========== */

uint32_t xir_coalesce(XirFunc *func) {
    if (!func || func->nblk == 0 || func->nvreg == 0) return 0;

    // Bail out if too many vregs for conflict matrix
    if (func->nvreg > XIR_MAX_COALESCE_VREGS) return 0;

    // Step 1: Compute liveness
    XirLive live;
    xir_live_compute(&live, func);

    // Step 2: Collect MOV instructions
    uint32_t nmov = 0, mov_cap = 64;
    CoalesceMove *movs = xr_malloc(mov_cap * sizeof(CoalesceMove));

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk) continue;

        int depth = blk->loop_depth;
        if (depth < 0) depth = 0;
        if (depth > 10) depth = 10;
        uint32_t w = 1u << depth;
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (!xir_op_is_copy(ins->op)) continue;
            if (!xir_ref_is_vreg(ins->dst) || !xir_ref_is_vreg(ins->args[0]))
                continue;

            uint32_t dv = XIR_REF_INDEX(ins->dst);
            uint32_t sv = XIR_REF_INDEX(ins->args[0]);
            if (dv >= func->nvreg || sv >= func->nvreg) continue;
            if (dv == sv) continue; // already trivial

            if (nmov >= mov_cap) {
                mov_cap *= 2;
                XR_REALLOC_OR_ABORT(movs,
                                    mov_cap * sizeof(CoalesceMove),
                                    "coalesce movs grow");
            }
            movs[nmov++] = (CoalesceMove){
                .dst_vreg = dv, .src_vreg = sv,
                .blk_idx = bi, .ins_idx = ii, .freq = w
            };
        }
    }

    if (nmov == 0) {
        xr_free(movs);
        xir_live_free(&live);
        return 0;
    }

    // Step 3: Build conflict matrix
    ConflictMatrix cm;
    cm_init(&cm, func->nvreg);
    build_conflicts(&cm, func, &live);

    // Step 4: Sort MOVs by frequency descending
    qsort(movs, nmov, sizeof(CoalesceMove), move_freq_cmp);

    // Step 5: Coalesce — process in frequency order
    UnionFind uf;
    uf_init(&uf, func->nvreg);
    uint32_t coalesced = 0;

    for (uint32_t i = 0; i < nmov; i++) {
        uint32_t dv = movs[i].dst_vreg;
        uint32_t sv = movs[i].src_vreg;

        uint32_t root_d = uf_find(&uf, dv);
        uint32_t root_s = uf_find(&uf, sv);

        if (root_d == root_s) {
            // Already in same group
            coalesced++;
            continue;
        }

        // Check if merging would create a conflict
        if (!groups_conflict(&cm, &uf, root_d, root_s)) {
            uf_union(&uf, root_d, root_s);
            coalesced++;
        }
    }

    // Step 6: Rewrite all instructions
    if (coalesced > 0) {
        rewrite_instructions(func, &uf);
    }

    // Step 7: Delete coalesced MOVs (now trivial dst=dst)
    uint32_t deleted = 0;
    if (coalesced > 0) {
        deleted = delete_coalesced_movs(func);
    }

    // Cleanup
    uf_free(&uf);
    cm_free(&cm);
    xr_free(movs);
    xir_live_free(&live);

    return deleted;
}
