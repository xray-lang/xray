/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_loop.c - Natural-loop forest for Xi IR
 *
 * ALGORITHM:
 *   1. Use the dominator tree (xi_compute_dominators).
 *   2. Scan CFG edges; (src -> hdr) is a back-edge iff hdr dominates src.
 *   3. For each header, collect the natural-loop body via reverse BFS
 *      from latch(es) to header.
 *   4. Wire parent/child/sibling by header dominance + body containment.
 *   5. Sort innermost-first by descending depth.
 */

#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Helpers ========== */

/* Collect all blocks in the natural loop body rooted at hdr_bi,
 * given latch latch_bi.  Merges into caller-supplied bitmap. */
static void collect_loop_body(XiFunc *f, uint32_t hdr_bi,
                               uint32_t latch_bi, uint8_t *in_loop) {
    XR_DCHECK(f != NULL, "collect_loop_body: NULL func");
    XR_DCHECK(hdr_bi < f->nblocks, "collect_loop_body: header OOB");
    XR_DCHECK(latch_bi < f->nblocks, "collect_loop_body: latch OOB");

    in_loop[hdr_bi] = 1;

    uint32_t *stack = (uint32_t *)xr_malloc(f->nblocks * sizeof(uint32_t));
    if (!stack) return;
    uint32_t sp = 0;

    if (!in_loop[latch_bi]) {
        in_loop[latch_bi] = 1;
        stack[sp++] = latch_bi;
    }

    while (sp > 0) {
        uint32_t b = stack[--sp];
        XiBlock *blk = f->blocks[b];
        for (uint16_t p = 0; p < blk->npreds; p++) {
            XiBlock *pred = blk->preds[p];
            if (!pred || pred->id >= f->nblocks) continue;
            if (in_loop[pred->id]) continue;
            in_loop[pred->id] = 1;
            stack[sp++] = pred->id;
        }
    }
    xr_free(stack);
}

/* Find the unique out-of-loop predecessor of the header, if any. */
static XiBlock *find_preheader(XiBlock *hdr, const uint8_t *in_loop) {
    XR_DCHECK(hdr != NULL, "find_preheader: NULL header");
    XiBlock *found = NULL;
    for (uint16_t p = 0; p < hdr->npreds; p++) {
        XiBlock *cand = hdr->preds[p];
        if (!cand) continue;
        if (in_loop[cand->id]) continue;
        if (found) return NULL;  /* more than one out-of-loop pred */
        found = cand;
    }
    return found;
}

/* Allocate and populate an XiLoop from header + body bitmap. */
static XiLoop *alloc_loop(XiFunc *f, uint32_t hdr_bi,
                            uint32_t latch_bi, const uint8_t *in_loop) {
    XR_DCHECK(f != NULL, "alloc_loop: NULL func");

    uint32_t body_count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++)
        if (in_loop[b]) body_count++;

    XiLoop *loop = (XiLoop *)xr_calloc(1, sizeof(XiLoop));
    if (!loop) return NULL;

    loop->header = f->blocks[hdr_bi];
    loop->latch = f->blocks[latch_bi];
    loop->preheader = find_preheader(loop->header, in_loop);

    if (body_count > 0) {
        loop->body = (XiBlock **)xr_malloc(body_count * sizeof(XiBlock *));
        if (!loop->body) { xr_free(loop); return NULL; }
        uint32_t w = 0;
        for (uint32_t b = 0; b < f->nblocks; b++)
            if (in_loop[b])
                loop->body[w++] = f->blocks[b];
        loop->nbody = body_count;
    }
    return loop;
}

static void free_loop(XiLoop *loop) {
    if (!loop) return;
    xr_free(loop->body);
    xr_free(loop);
}

/* ========== Public API ========== */

XR_FUNC XiLoopInfo *xi_compute_loops(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_compute_loops: NULL func");
    if (f->nblocks == 0) return NULL;

    uint32_t n = f->nblocks;

    /* Back-edge detection: (src -> hdr) where hdr dominates src */
    uint8_t *headers = (uint8_t *)xr_calloc(n, sizeof(uint8_t));
    uint32_t *first_latch = (uint32_t *)xr_malloc(n * sizeof(uint32_t));
    uint8_t *latch_seen = (uint8_t *)xr_calloc(n, sizeof(uint8_t));
    if (!headers || !first_latch || !latch_seen) {
        xr_free(headers); xr_free(first_latch); xr_free(latch_seen);
        return NULL;
    }

    for (uint32_t s = 0; s < n; s++) {
        XiBlock *src = f->blocks[s];
        if (!src) continue;
        for (int k = 0; k < 2; k++) {
            XiBlock *tgt = src->succs[k];
            if (!tgt) continue;
            XR_DCHECK(tgt->id < n, "successor block ID out of range");
            if (!xi_dominates(tgt, src)) continue;
            /* tgt dominates src: back-edge found */
            headers[tgt->id] = 1;
            if (!latch_seen[tgt->id]) {
                latch_seen[tgt->id] = 1;
                first_latch[tgt->id] = s;
            }
        }
    }

    uint32_t nloop = 0;
    for (uint32_t b = 0; b < n; b++)
        if (headers[b]) nloop++;

    if (nloop == 0) {
        xr_free(headers); xr_free(first_latch); xr_free(latch_seen);
        return NULL;
    }

    /* Allocate XiLoopInfo */
    XiLoopInfo *info = (XiLoopInfo *)xr_calloc(1, sizeof(XiLoopInfo));
    if (!info) {
        xr_free(headers); xr_free(first_latch); xr_free(latch_seen);
        return NULL;
    }
    info->nblocks = n;
    info->block_to_loop = (XiLoop **)xr_calloc(n, sizeof(XiLoop *));
    info->all_loops = (XiLoop **)xr_calloc(nloop, sizeof(XiLoop *));
    if (!info->block_to_loop || !info->all_loops) {
        xi_loopinfo_free(info);
        xr_free(headers); xr_free(first_latch); xr_free(latch_seen);
        return NULL;
    }

    /* Per-loop body bitmaps for parent/child wiring */
    uint8_t *body_bitmaps = (uint8_t *)xr_calloc((size_t)nloop * n, sizeof(uint8_t));
    if (!body_bitmaps) {
        xi_loopinfo_free(info);
        xr_free(headers); xr_free(first_latch); xr_free(latch_seen);
        return NULL;
    }

    /* Build each loop's body by merging all back-edges to the same header */
    uint32_t idx = 0;
    for (uint32_t hbi = 0; hbi < n; hbi++) {
        if (!headers[hbi]) continue;
        uint8_t *bitmap = body_bitmaps + (size_t)idx * n;

        /* Collect body from all back-edges to this header */
        for (uint32_t s = 0; s < n; s++) {
            XiBlock *src = f->blocks[s];
            if (!src) continue;
            for (int k = 0; k < 2; k++) {
                XiBlock *tgt = src->succs[k];
                if (!tgt || tgt->id != hbi) continue;
                if (!xi_dominates(tgt, src)) continue;
                collect_loop_body(f, hbi, s, bitmap);
            }
        }

        XiLoop *L = alloc_loop(f, hbi, first_latch[hbi], bitmap);
        if (!L) {
            xr_free(body_bitmaps);
            xr_free(headers); xr_free(first_latch); xr_free(latch_seen);
            xi_loopinfo_free(info);
            return NULL;
        }
        L->id = idx;
        info->all_loops[idx++] = L;
    }
    info->nloop = idx;
    XR_DCHECK(idx == nloop, "loop count mismatch");

    /* Parent/child wiring: for each loop L, find the smallest
     * containing loop M (M.header dominates L.header AND
     * M.body contains L.header AND M != L). */
    for (uint32_t li = 0; li < nloop; li++) {
        XiLoop *L = info->all_loops[li];
        uint32_t hL = L->header->id;
        XiLoop *best = NULL;
        uint32_t best_size = UINT32_MAX;

        for (uint32_t mi = 0; mi < nloop; mi++) {
            if (mi == li) continue;
            XiLoop *M = info->all_loops[mi];
            if (!xi_dominates(M->header, L->header)) continue;
            uint8_t *mbm = body_bitmaps + (size_t)mi * n;
            if (!mbm[hL]) continue;
            if (M->nbody < best_size) {
                best_size = M->nbody;
                best = M;
            }
        }
        L->parent = best;
    }

    /* Link children into parent lists and compute depth */
    for (uint32_t li = 0; li < nloop; li++) {
        XiLoop *L = info->all_loops[li];
        if (!L->parent) {
            L->sibling = info->root_list;
            info->root_list = L;
        } else {
            L->sibling = L->parent->child;
            L->parent->child = L;
        }
    }
    for (uint32_t li = 0; li < nloop; li++) {
        XiLoop *L = info->all_loops[li];
        uint32_t d = 1;
        for (XiLoop *p = L->parent; p; p = p->parent) d++;
        L->depth = d;
    }

    /* Populate block_to_loop: innermost (deepest) enclosing loop */
    for (uint32_t b = 0; b < n; b++) {
        XiLoop *best = NULL;
        uint32_t best_depth = 0;
        for (uint32_t li = 0; li < nloop; li++) {
            if (body_bitmaps[(size_t)li * n + b]) {
                XiLoop *L = info->all_loops[li];
                if (L->depth > best_depth) {
                    best_depth = L->depth;
                    best = L;
                }
            }
        }
        info->block_to_loop[b] = best;
    }

    /* Sort innermost-first by descending depth (insertion sort) */
    for (uint32_t i = 1; i < nloop; i++) {
        XiLoop *tmp = info->all_loops[i];
        uint32_t j = i;
        while (j > 0 && info->all_loops[j - 1]->depth < tmp->depth) {
            info->all_loops[j] = info->all_loops[j - 1];
            j--;
        }
        info->all_loops[j] = tmp;
    }
    /* Re-stamp IDs to match sorted order */
    for (uint32_t i = 0; i < nloop; i++)
        info->all_loops[i]->id = i;

    xr_free(body_bitmaps);
    xr_free(headers);
    xr_free(first_latch);
    xr_free(latch_seen);
    return info;
}

XR_FUNC void xi_loopinfo_free(XiLoopInfo *info) {
    if (!info) return;
    if (info->all_loops) {
        for (uint32_t i = 0; i < info->nloop; i++)
            free_loop(info->all_loops[i]);
        xr_free(info->all_loops);
    }
    xr_free(info->block_to_loop);
    xr_free(info);
}

XR_FUNC uint32_t xi_block_loop_depth(const XiLoopInfo *info, uint32_t blk_id) {
    if (!info || blk_id >= info->nblocks) return 0;
    XiLoop *L = info->block_to_loop[blk_id];
    return L ? L->depth : 0;
}

XR_FUNC bool xi_loop_contains_block(const XiLoop *loop, const XiBlock *blk) {
    if (!loop || !blk) return false;
    for (uint32_t i = 0; i < loop->nbody; i++)
        if (loop->body[i] == blk)
            return true;
    return false;
}
