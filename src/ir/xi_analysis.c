/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_analysis.c - Static analysis passes for Xi IR
 *
 * Implements RPO, dominator tree, and liveness analysis.
 * All analyses operate on the IR without modifying instructions.
 */

#include "xi_analysis.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== RPO (Reverse Post-Order) ========== */

/* DFS post-order traversal using the XiBlock.visited scratch flag. */
static void rpo_dfs(XiBlock *blk, XiBlock **post_order, uint32_t *post_idx) {
    XR_DCHECK(blk != NULL, "rpo_dfs: NULL block");
    if (blk->visited)
        return;
    blk->visited = true;

    for (int s = 0; s < 2; s++) {
        if (blk->succs[s])
            rpo_dfs(blk->succs[s], post_order, post_idx);
    }
    post_order[(*post_idx)++] = blk;
}

XR_FUNC uint32_t xi_compute_rpo(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_compute_rpo: NULL func");
    XR_DCHECK(f->nblocks > 0, "xi_compute_rpo: no blocks");

    /* Clear visited flags and rpo indices */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        f->blocks[b]->visited = false;
        f->blocks[b]->rpo = 0;
    }

    XiBlock **post_order = (XiBlock **) xr_calloc(f->nblocks, sizeof(XiBlock *));
    if (!post_order)
        return 0;

    uint32_t post_idx = 0;
    rpo_dfs(f->entry, post_order, &post_idx);

    /* Reverse the post-order to get RPO. Assign 1-based indices. */
    uint32_t rpo_num = 1;
    for (int i = (int) post_idx - 1; i >= 0; i--) {
        post_order[i]->rpo = rpo_num++;
    }

    uint32_t reachable = post_idx;
    xr_free(post_order);

    /* Exception handler blocks (catch / finally targets from XI_TRY)
     * are unreachable via normal successor edges.  BFS-assign RPO
     * numbers AFTER normal blocks so they appear later in RPO order
     * than their containing try blocks.  This ensures the dominator
     * algorithm processes predecessors before exception handlers. */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_TRY)
                continue;

            /* v->aux = catch block, v->aux_int = finally block id */
            XiBlock *seed = (XiBlock *) v->aux;
            if (!seed && v->aux_int >= 0 && (uint32_t) v->aux_int < f->nblocks)
                seed = f->blocks[(uint32_t) v->aux_int];
            if (!seed)
                continue;

            /* BFS from seed, assigning RPO numbers to unvisited blocks */
            XiBlock *queue[64];
            int qhead = 0, qtail = 0;
            if (seed->rpo == 0) {
                seed->rpo = ++reachable;
                queue[qtail++] = seed;
            }
            while (qhead < qtail && qtail < 64) {
                XiBlock *cur = queue[qhead++];
                for (int s = 0; s < 2; s++) {
                    XiBlock *succ = cur->succs[s];
                    if (succ && succ->rpo == 0) {
                        succ->rpo = ++reachable;
                        queue[qtail++] = succ;
                    }
                }
            }
        }
    }

    /* Clear visited flags */
    for (uint32_t b = 0; b < f->nblocks; b++)
        f->blocks[b]->visited = false;

    return reachable;
}

/* ========== Dominator Tree ========== */

/*
 * Cooper-Harvey-Kennedy (CHK) algorithm for immediate dominators.
 *
 * Iterative algorithm that uses RPO numbering. Simple, fast for
 * typical function sizes. Converges in 2-3 iterations for reducible
 * CFGs (which xray always produces).
 *
 * Reference: "A Simple, Fast Dominance Algorithm" (2001)
 */

/* Helper: find an RPO-ordered block array. */
static XiBlock **build_rpo_order(XiFunc *f, uint32_t reachable) {
    XiBlock **rpo_order = (XiBlock **) xr_calloc(reachable + 1, sizeof(XiBlock *));
    if (!rpo_order)
        return NULL;

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (blk->rpo > 0 && blk->rpo <= reachable)
            rpo_order[blk->rpo] = blk;
    }
    return rpo_order;
}

/* Intersect two idom paths to find common dominator. */
static XiBlock *intersect(XiBlock *a, XiBlock *b) {
    XR_DCHECK(a != NULL && b != NULL, "intersect: NULL block");
    while (a != b) {
        while (a->rpo > b->rpo) {
            XR_DCHECK(a->idom != NULL, "intersect: NULL idom");
            a = a->idom;
        }
        while (b->rpo > a->rpo) {
            XR_DCHECK(b->idom != NULL, "intersect: NULL idom");
            b = b->idom;
        }
    }
    return a;
}

XR_FUNC void xi_compute_dominators(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_compute_dominators: NULL func");
    XR_DCHECK(f->entry != NULL, "xi_compute_dominators: NULL entry");
    XR_DCHECK(f->entry->rpo > 0, "xi_compute_dominators: RPO not computed");

    /* Count reachable blocks */
    uint32_t reachable = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        f->blocks[b]->idom = NULL;
        f->blocks[b]->dom_depth = 0;
        if (f->blocks[b]->rpo > 0)
            reachable++;
    }

    if (reachable == 0)
        return;

    /* Build RPO-indexed array */
    XiBlock **rpo_order = build_rpo_order(f, reachable);
    if (!rpo_order)
        return;

    /* Entry dominates itself */
    f->entry->idom = f->entry;

    /* Iterate until convergence */
    bool changed = true;
    while (changed) {
        changed = false;

        /* Traverse in RPO order, skip entry (rpo=1) */
        for (uint32_t r = 2; r <= reachable; r++) {
            XiBlock *blk = rpo_order[r];
            if (!blk)
                continue;

            /* Find first processed predecessor */
            XiBlock *new_idom = NULL;
            for (uint16_t p = 0; p < blk->npreds; p++) {
                XiBlock *pred = blk->preds[p];
                if (pred->idom != NULL) {
                    if (new_idom == NULL) {
                        new_idom = pred;
                    } else {
                        new_idom = intersect(new_idom, pred);
                    }
                }
            }

            XR_DCHECK(new_idom != NULL, "xi_compute_dominators: no processed pred");

            if (blk->idom != new_idom) {
                blk->idom = new_idom;
                changed = true;
            }
        }
    }

    /* Fix entry: idom = NULL (not self) */
    f->entry->idom = NULL;

    /* Compute dom_depth in RPO order */
    f->entry->dom_depth = 0;
    for (uint32_t r = 2; r <= reachable; r++) {
        XiBlock *blk = rpo_order[r];
        if (!blk || !blk->idom)
            continue;
        blk->dom_depth = blk->idom->dom_depth + 1;
    }

    xr_free(rpo_order);
}

XR_FUNC bool xi_dominates(const XiBlock *a, const XiBlock *b) {
    if (!a || !b)
        return false;
    if (a == b)
        return true;

    /* Walk up the dominator tree from b */
    const XiBlock *cur = b;
    while (cur && cur->dom_depth > a->dom_depth) {
        cur = cur->idom;
    }
    return cur == a;
}

/* ========== Liveness Analysis ========== */

/* Bitset helpers — use uint64_t words for efficiency. */
#define BIT_WORD(id) ((id) / 64)
#define BIT_MASK(id) (1ULL << ((id) & 63))

static void bitset_set(uint64_t *set, uint32_t id) {
    set[BIT_WORD(id)] |= BIT_MASK(id);
}

static void bitset_clear(uint64_t *set, uint32_t id) {
    set[BIT_WORD(id)] &= ~BIT_MASK(id);
}

static bool bitset_test(const uint64_t *set, uint32_t id) {
    return (set[BIT_WORD(id)] & BIT_MASK(id)) != 0;
}

/* Union: dst |= src. Returns true if dst changed. */
static bool bitset_union(uint64_t *dst, const uint64_t *src, uint32_t words) {
    bool changed = false;
    for (uint32_t w = 0; w < words; w++) {
        uint64_t old = dst[w];
        dst[w] |= src[w];
        if (dst[w] != old)
            changed = true;
    }
    return changed;
}

/* Get pointer to live_in set for block with given rpo index. */
static uint64_t *live_in_set(XiLiveness *l, const XiBlock *blk) {
    XR_DCHECK(blk->rpo > 0, "live_in_set: block not in RPO");
    return l->live_in + (uint64_t) (blk->rpo - 1) * l->set_words;
}

/* Get pointer to live_out set for block with given rpo index. */
static uint64_t *live_out_set(XiLiveness *l, const XiBlock *blk) {
    XR_DCHECK(blk->rpo > 0, "live_out_set: block not in RPO");
    return l->live_out + (uint64_t) (blk->rpo - 1) * l->set_words;
}

XR_FUNC XiLiveness *xi_compute_liveness(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_compute_liveness: NULL func");
    XR_DCHECK(f->entry->rpo > 0, "xi_compute_liveness: RPO not computed");

    /* Count reachable blocks */
    uint32_t reachable = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (f->blocks[b]->rpo > 0)
            reachable++;
    }

    /* Determine bitset size */
    uint32_t max_id = f->next_value_id;
    uint32_t set_words = (max_id + 63) / 64;
    if (set_words == 0)
        set_words = 1;

    /* Allocate liveness structure */
    XiLiveness *l = (XiLiveness *) xr_calloc(1, sizeof(XiLiveness));
    if (!l)
        return NULL;

    l->nblocks = reachable;
    l->set_words = set_words;

    size_t total_bytes = (size_t) reachable * set_words * sizeof(uint64_t);
    l->live_in = (uint64_t *) xr_calloc(1, total_bytes);
    l->live_out = (uint64_t *) xr_calloc(1, total_bytes);
    if (!l->live_in || !l->live_out) {
        xr_free(l->live_in);
        xr_free(l->live_out);
        xr_free(l);
        return NULL;
    }

    /* Build reverse-RPO order array for backward iteration */
    XiBlock **rpo_order = build_rpo_order(f, reachable);
    if (!rpo_order) {
        xi_liveness_free(l);
        return NULL;
    }

    /*
     * Backward dataflow iteration:
     *   live_out[B] = union { live_in[S] : S in succs(B) }
     *   live_in[B]  = uses[B] | (live_out[B] - defs[B])
     *
     * Iterate in reverse RPO (= approx post-order) until convergence.
     */
    bool changed = true;
    while (changed) {
        changed = false;

        /* Iterate blocks in reverse RPO order */
        for (int r = (int) reachable; r >= 1; r--) {
            XiBlock *blk = rpo_order[r];
            if (!blk)
                continue;

            uint64_t *out = live_out_set(l, blk);
            uint64_t *in = live_in_set(l, blk);

            /* live_out = union of successor live_in */
            for (int s = 0; s < 2; s++) {
                XiBlock *succ = blk->succs[s];
                if (succ && succ->rpo > 0) {
                    bitset_union(out, live_in_set(l, succ), set_words);
                }
            }

            /* Compute new live_in: start from live_out */
            uint64_t new_in[set_words];
            memcpy(new_in, out, set_words * sizeof(uint64_t));

            /* Walk instructions backward: remove defs, add uses */
            for (int i = (int) blk->nvalues - 1; i >= 0; i--) {
                XiValue *v = blk->values[i];

                /* Remove def */
                bitset_clear(new_in, v->id);

                /* Add uses */
                for (uint16_t a = 0; a < v->nargs; a++) {
                    if (v->args[a])
                        bitset_set(new_in, v->args[a]->id);
                }
            }

            /* Add uses from block control */
            if (blk->control)
                bitset_set(new_in, blk->control->id);

            /* Add uses from phi nodes of successors (this block's contribution) */
            for (int s = 0; s < 2; s++) {
                XiBlock *succ = blk->succs[s];
                if (!succ)
                    continue;
                for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
                    /* Find which pred index this block is */
                    for (uint16_t p = 0; p < succ->npreds; p++) {
                        if (succ->preds[p] == blk && p < phi->value.nargs) {
                            XiValue *arg = phi->value.args[p];
                            if (arg)
                                bitset_set(new_in, arg->id);
                            break;
                        }
                    }
                }
            }

            /* Phi defs are live-in to the block but not defined by instructions.
             * Remove phi defs from new_in (they are defined at block entry). */
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                bitset_clear(new_in, phi->value.id);
            }

            /* Check if live_in changed */
            if (memcmp(in, new_in, set_words * sizeof(uint64_t)) != 0) {
                memcpy(in, new_in, set_words * sizeof(uint64_t));
                changed = true;
            }
        }
    }

    xr_free(rpo_order);
    return l;
}

XR_FUNC bool xi_is_live_in(const XiLiveness *l, const XiBlock *blk, const XiValue *v) {
    XR_DCHECK(l != NULL, "xi_is_live_in: NULL liveness");
    XR_DCHECK(blk != NULL, "xi_is_live_in: NULL block");
    XR_DCHECK(v != NULL, "xi_is_live_in: NULL value");

    if (blk->rpo == 0 || blk->rpo > l->nblocks)
        return false;
    const uint64_t *in = l->live_in + (uint64_t) (blk->rpo - 1) * l->set_words;
    return bitset_test(in, v->id);
}

XR_FUNC bool xi_is_live_out(const XiLiveness *l, const XiBlock *blk, const XiValue *v) {
    XR_DCHECK(l != NULL, "xi_is_live_out: NULL liveness");
    XR_DCHECK(blk != NULL, "xi_is_live_out: NULL block");
    XR_DCHECK(v != NULL, "xi_is_live_out: NULL value");

    if (blk->rpo == 0 || blk->rpo > l->nblocks)
        return false;
    const uint64_t *out = l->live_out + (uint64_t) (blk->rpo - 1) * l->set_words;
    return bitset_test(out, v->id);
}

XR_FUNC void xi_liveness_free(XiLiveness *l) {
    if (!l)
        return;
    xr_free(l->live_in);
    xr_free(l->live_out);
    xr_free(l);
}
