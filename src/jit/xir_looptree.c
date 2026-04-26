/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_looptree.c - Natural-loop forest derived from the dominator tree.
 *
 * ALGORITHM:
 *   1. Build the cached dominator tree (xir_func_get_domtree).
 *   2. Scan every block's successors; a CFG edge (src -> hdr) is a
 *      back-edge iff hdr dominates src.  Each distinct back-edge
 *      target becomes a loop header.
 *   3. For each header collect the natural-loop body by a reverse BFS
 *      from the back-edge sources; the body is the union of hdr plus
 *      every block that can reach a latch without leaving through hdr.
 *   4. Wire up parent/child/sibling by comparing header dominance:
 *      loop L is nested in M iff M's header dominates L's header and
 *      L's header lies in M's body.
 *   5. Sort by descending depth so innermost loops come first.
 */

#include "xir_looptree.h"

#include <string.h>

#include "xir.h"
#include "xir_domtree.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

static uint32_t block_index(XirFunc *func, const XirBlock *blk) {
    if (!blk)
        return UINT32_MAX;
    for (uint32_t i = 0; i < func->nblk; i++)
        if (func->blocks[i] == blk)
            return i;
    return UINT32_MAX;
}

/*
 * Collect all blocks that belong to the natural loop rooted at
 * |hdr_bi|, given one of its latches |latch_bi|.  Accumulates into
 * the caller-supplied bitmap |in_loop| (1 bit per block).  Callers
 * can re-use the bitmap to merge the bodies of multiple latches that
 * share a header.
 */
static void collect_loop_body(XirFunc *func, const XirDomTree *dt, uint32_t hdr_bi,
                              uint32_t latch_bi, uint8_t *in_loop) {
    (void) dt;
    in_loop[hdr_bi] = 1;

    uint32_t *stack = (uint32_t *) xr_malloc(func->nblk * sizeof(uint32_t));
    uint32_t sp = 0;
    if (!in_loop[latch_bi]) {
        in_loop[latch_bi] = 1;
        stack[sp++] = latch_bi;
    }
    while (sp > 0) {
        uint32_t b = stack[--sp];
        XirBlock *blk = func->blocks[b];
        for (uint32_t p = 0; p < blk->npred; p++) {
            uint32_t pi = block_index(func, blk->preds[p]);
            if (pi == UINT32_MAX)
                continue;
            if (in_loop[pi])
                continue;
            in_loop[pi] = 1;
            stack[sp++] = pi;
        }
    }
    xr_free(stack);
}

/*
 * Identify the preheader of |hdr|, if one exists.
 *
 * A preheader is the unique out-of-loop predecessor of the loop
 * header.  When the header has zero or more than one such
 * predecessors we leave |preheader| NULL so passes that strictly
 * require a single entry can decide whether to call
 * xir_ensure_preheader().
 */
static XirBlock *find_unique_preheader(XirBlock *hdr, const uint8_t *in_loop, XirFunc *func) {
    XirBlock *found = NULL;
    for (uint32_t p = 0; p < hdr->npred; p++) {
        XirBlock *cand = hdr->preds[p];
        uint32_t ci = block_index(func, cand);
        if (ci == UINT32_MAX)
            continue;
        if (in_loop[ci])
            continue;  // skip back-edge predecessors
        if (found)
            return NULL;  // more than one → ambiguous
        found = cand;
    }
    return found;
}

/*
 * Allocate a XirLoop populated from the given header + body bitmap.
 * The body array is heap-allocated and owned by the returned XirLoop
 * (freed by xir_loopinfo_free).
 */
static XirLoop *alloc_loop(XirFunc *func, uint32_t hdr_bi, uint32_t latch_bi,
                           const uint8_t *in_loop) {
    XirBlock *hdr = func->blocks[hdr_bi];

    uint32_t body_count = 0;
    for (uint32_t b = 0; b < func->nblk; b++)
        if (in_loop[b])
            body_count++;

    XirLoop *loop = (XirLoop *) xr_calloc(1, sizeof(XirLoop));
    if (!loop)
        return NULL;
    loop->header = hdr;
    loop->latch = func->blocks[latch_bi];
    loop->preheader = find_unique_preheader(hdr, in_loop, func);

    if (body_count > 0) {
        loop->body = (XirBlock **) xr_malloc(body_count * sizeof(XirBlock *));
        if (!loop->body) {
            xr_free(loop);
            return NULL;
        }
        uint32_t w = 0;
        for (uint32_t b = 0; b < func->nblk; b++)
            if (in_loop[b])
                loop->body[w++] = func->blocks[b];
        loop->nbody = body_count;
    }
    return loop;
}

static void free_loop(XirLoop *loop) {
    if (!loop)
        return;
    xr_free(loop->body);
    xr_free(loop);
}

static void xir_loopinfo_free(XirLoopInfo *info) {
    if (!info)
        return;
    if (info->all_loops) {
        for (uint32_t i = 0; i < info->nloop; i++)
            free_loop(info->all_loops[i]);
        xr_free(info->all_loops);
    }
    xr_free(info->block_to_loop);
    xr_free(info);
}

/*
 * Compute a XirLoopInfo for |func|.
 *
 * Allocates a temporary bitmap matrix [nloop * nblk] during
 * construction so that body membership checks are constant time; the
 * matrix is freed before returning.
 */
static XirLoopInfo *build_loop_info(XirFunc *func) {
    const XirDomTree *dt = xir_func_get_domtree(func);
    if (!dt)
        return NULL;
    uint32_t n = func->nblk;

    /* First pass: find every back-edge (src -> hdr where hdr dominates
     * src) and group them by header.  A header may have more than one
     * latch; we merge their loop bodies. */
    uint8_t *headers = (uint8_t *) xr_calloc(n, sizeof(uint8_t));
    uint8_t *first_latch_seen = (uint8_t *) xr_calloc(n, sizeof(uint8_t));
    uint32_t *first_latch = (uint32_t *) xr_malloc(n * sizeof(uint32_t));
    if (!headers || !first_latch_seen || !first_latch) {
        xr_free(headers);
        xr_free(first_latch_seen);
        xr_free(first_latch);
        return NULL;
    }

    for (uint32_t s = 0; s < n; s++) {
        XirBlock *src = func->blocks[s];
        XirBlock *succ[2] = {src->s1, src->s2};
        for (int k = 0; k < 2; k++) {
            if (!succ[k])
                continue;
            uint32_t t = block_index(func, succ[k]);
            if (t == UINT32_MAX)
                continue;
            if (!xir_dom_covers(dt, t, s))
                continue;
            headers[t] = 1;
            if (!first_latch_seen[t]) {
                first_latch_seen[t] = 1;
                first_latch[t] = s;
            }
        }
    }

    /* Count headers and build a matrix of per-loop body bitmaps. */
    uint32_t nloop = 0;
    for (uint32_t b = 0; b < n; b++)
        if (headers[b])
            nloop++;

    XirLoopInfo *info = (XirLoopInfo *) xr_calloc(1, sizeof(XirLoopInfo));
    if (!info) {
        xr_free(headers);
        xr_free(first_latch_seen);
        xr_free(first_latch);
        return NULL;
    }
    info->nblk = n;
    info->block_to_loop = (XirLoop **) xr_calloc(n, sizeof(XirLoop *));
    info->all_loops = nloop ? (XirLoop **) xr_calloc(nloop, sizeof(XirLoop *)) : NULL;
    if ((!info->block_to_loop && n) || (nloop && !info->all_loops)) {
        xir_loopinfo_free(info);
        xr_free(headers);
        xr_free(first_latch_seen);
        xr_free(first_latch);
        return NULL;
    }

    uint8_t *body_bitmap = (uint8_t *) xr_calloc((size_t) nloop * n, sizeof(uint8_t));
    if (nloop > 0 && !body_bitmap) {
        xir_loopinfo_free(info);
        xr_free(headers);
        xr_free(first_latch_seen);
        xr_free(first_latch);
        return NULL;
    }

    uint32_t idx = 0;
    for (uint32_t hbi = 0; hbi < n; hbi++) {
        if (!headers[hbi])
            continue;
        uint8_t *bitmap = body_bitmap + (size_t) idx * n;

        /* Every back-edge contributes its own reverse-BFS body; we
         * union them here so the final body covers all latches. */
        for (uint32_t s = 0; s < n; s++) {
            XirBlock *src = func->blocks[s];
            XirBlock *succ[2] = {src->s1, src->s2};
            for (int k = 0; k < 2; k++) {
                if (!succ[k])
                    continue;
                uint32_t t = block_index(func, succ[k]);
                if (t != hbi)
                    continue;
                if (!xir_dom_covers(dt, hbi, s))
                    continue;
                collect_loop_body(func, dt, hbi, s, bitmap);
            }
        }

        XirLoop *L = alloc_loop(func, hbi, first_latch[hbi], bitmap);
        if (!L) {
            xr_free(body_bitmap);
            xr_free(headers);
            xr_free(first_latch_seen);
            xr_free(first_latch);
            xir_loopinfo_free(info);
            return NULL;
        }
        L->id = idx;
        info->all_loops[idx++] = L;
    }
    info->nloop = idx;

    /* Parent/child wiring: for each loop L, find the smallest
     * containing loop M (body ⊇ L->body ∧ M->header dominates L->header
     * ∧ M ≠ L).  We pick the one with the smallest body cardinality to
     * get the immediate parent. */
    for (uint32_t li = 0; li < info->nloop; li++) {
        XirLoop *L = info->all_loops[li];
        uint32_t hL = block_index(func, L->header);
        XirLoop *best_parent = NULL;
        uint32_t best_size = UINT32_MAX;
        for (uint32_t mi = 0; mi < info->nloop; mi++) {
            if (mi == li)
                continue;
            XirLoop *M = info->all_loops[mi];
            uint32_t hM = block_index(func, M->header);
            if (hM == UINT32_MAX)
                continue;
            if (!xir_dom_covers(dt, hM, hL))
                continue;
            /* Confirm M actually contains L's header. */
            uint8_t *mbm = body_bitmap + (size_t) mi * n;
            if (!mbm[hL])
                continue;
            if (M->nbody < best_size) {
                best_size = M->nbody;
                best_parent = M;
            }
        }
        L->parent = best_parent;
    }

    /* Hook children into parents' child lists, then compute depth. */
    for (uint32_t li = 0; li < info->nloop; li++) {
        XirLoop *L = info->all_loops[li];
        if (!L->parent) {
            L->sibling = info->root_list;
            info->root_list = L;
        } else {
            L->sibling = L->parent->child;
            L->parent->child = L;
        }
    }
    for (uint32_t li = 0; li < info->nloop; li++) {
        XirLoop *L = info->all_loops[li];
        uint32_t d = 1;
        for (XirLoop *p = L->parent; p; p = p->parent)
            d++;
        L->depth = d;
    }

    /* Populate block_to_loop with the innermost enclosing loop for
     * every block.  A block may be in several loop bodies; we pick
     * the one with the greatest depth. */
    for (uint32_t b = 0; b < n; b++) {
        XirLoop *best = NULL;
        uint32_t best_depth = 0;
        for (uint32_t li = 0; li < info->nloop; li++) {
            if (body_bitmap[(size_t) li * n + b]) {
                XirLoop *L = info->all_loops[li];
                if (L->depth > best_depth) {
                    best_depth = L->depth;
                    best = L;
                }
            }
        }
        info->block_to_loop[b] = best;
    }

    /* Final: sort all_loops by descending depth so callers naturally
     * process innermost loops first. */
    for (uint32_t i = 1; i < info->nloop; i++) {
        XirLoop *tmp = info->all_loops[i];
        uint32_t j = i;
        while (j > 0 && info->all_loops[j - 1]->depth < tmp->depth) {
            info->all_loops[j] = info->all_loops[j - 1];
            j--;
        }
        info->all_loops[j] = tmp;
    }
    /* Re-stamp ids after sort so they match the sorted order. */
    for (uint32_t i = 0; i < info->nloop; i++)
        info->all_loops[i]->id = i;

    xr_free(body_bitmap);
    xr_free(headers);
    xr_free(first_latch_seen);
    xr_free(first_latch);
    return info;
}

const XirLoopInfo *xir_func_get_loops(XirFunc *func) {
    if (!func || func->nblk == 0)
        return NULL;
    if (func->loopinfo)
        return func->loopinfo;
    func->loopinfo = build_loop_info(func);
    return func->loopinfo;
}

void xir_func_invalidate_loops(XirFunc *func) {
    if (!func)
        return;
    if (func->loopinfo) {
        xir_loopinfo_free(func->loopinfo);
        func->loopinfo = NULL;
    }
    /* Loops depend on the dominator tree, so keep invalidation
     * transitive. */
    xir_func_invalidate_domtree(func);
}

uint32_t xir_block_loop_depth(XirFunc *func, uint32_t blk_id) {
    const XirLoopInfo *info = xir_func_get_loops(func);
    if (!info || blk_id >= info->nblk)
        return 0;
    XirLoop *L = info->block_to_loop[blk_id];
    return L ? L->depth : 0;
}
