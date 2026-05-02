/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_domtree.c - Cached dominator tree with DFS numbering.
 *
 * See xm_domtree.h for the public contract.  This file also owns
 * the XmFunc::domtree lifetime (alloc / free via xr_malloc).
 */

#include "xm_domtree.h"

#include <string.h>

#include "xm.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

/*
 * Build a XmDomTree from an already-computed idom[] array.
 *
 * This separates the "who-dominates-whom" computation (Cooper, lives
 * in xm.c) from the tree-materialisation step (DFS numbering + child
 * flattening), so Phase 2 can later swap in Semi-NCA for the first
 * step without touching the downstream tree layout.
 *
 * Out-of-CFG blocks (idom == UINT32_MAX) are intentionally excluded
 * from the child index.  Queries on them return false, matching the
 * "unreachable cannot dominate anything" semantics.
 */
static XmDomTree *build_tree_from_idom(const uint32_t *idom, uint32_t nblk) {
    XmDomTree *dt = (XmDomTree *) xr_calloc(1, sizeof(XmDomTree));
    if (!dt)
        return NULL;
    dt->nblk = nblk;
    dt->idom = (uint32_t *) xr_malloc(nblk * sizeof(uint32_t));
    dt->dfs_in = (uint32_t *) xr_calloc(nblk, sizeof(uint32_t));
    dt->dfs_out = (uint32_t *) xr_calloc(nblk, sizeof(uint32_t));
    dt->child_start = (uint32_t *) xr_calloc(nblk + 1, sizeof(uint32_t));
    if (!dt->idom || !dt->dfs_in || !dt->dfs_out || !dt->child_start) {
        xr_free(dt->idom);
        xr_free(dt->dfs_in);
        xr_free(dt->dfs_out);
        xr_free(dt->child_start);
        xr_free(dt);
        return NULL;
    }
    memcpy(dt->idom, idom, nblk * sizeof(uint32_t));

    /* Count children per parent.  The entry node (self-dominated) is
     * skipped so it is not wrongly listed as its own child. */
    for (uint32_t b = 1; b < nblk; b++) {
        uint32_t p = idom[b];
        if (p == UINT32_MAX || p >= nblk)
            continue;
        dt->child_start[p + 1]++;
    }
    for (uint32_t i = 1; i <= nblk; i++)
        dt->child_start[i] += dt->child_start[i - 1];

    uint32_t total = dt->child_start[nblk];
    dt->children = total ? (uint32_t *) xr_malloc(total * sizeof(uint32_t)) : NULL;
    if (total && !dt->children) {
        xr_free(dt->idom);
        xr_free(dt->dfs_in);
        xr_free(dt->dfs_out);
        xr_free(dt->child_start);
        xr_free(dt);
        return NULL;
    }

    uint32_t *cursor = (uint32_t *) xr_calloc(nblk, sizeof(uint32_t));
    if (!cursor) {
        xr_free(dt->children);
        xr_free(dt->idom);
        xr_free(dt->dfs_in);
        xr_free(dt->dfs_out);
        xr_free(dt->child_start);
        xr_free(dt);
        return NULL;
    }
    for (uint32_t b = 1; b < nblk; b++) {
        uint32_t p = idom[b];
        if (p == UINT32_MAX || p >= nblk)
            continue;
        uint32_t slot = dt->child_start[p] + cursor[p]++;
        dt->children[slot] = b;
    }
    xr_free(cursor);

    /* Iterative DFS stamping (we avoid recursion so very deep chains
     * cannot blow the C stack).  Each block is visited exactly twice:
     * entry stamp on push, exit stamp on pop. */
    uint32_t tick = 0;
    uint32_t *stack = (uint32_t *) xr_malloc(2 * nblk * sizeof(uint32_t));
    uint32_t *child_iter = (uint32_t *) xr_calloc(nblk, sizeof(uint32_t));
    if (!stack || !child_iter) {
        xr_free(stack);
        xr_free(child_iter);
        xr_free(dt->children);
        xr_free(dt->idom);
        xr_free(dt->dfs_in);
        xr_free(dt->dfs_out);
        xr_free(dt->child_start);
        xr_free(dt);
        return NULL;
    }

    uint32_t sp = 0;
    stack[sp++] = 0;  // start at the entry block
    dt->dfs_in[0] = tick++;
    while (sp > 0) {
        uint32_t top = stack[sp - 1];
        uint32_t cs = dt->child_start[top];
        uint32_t ce = dt->child_start[top + 1];
        if (child_iter[top] < ce - cs) {
            uint32_t c = dt->children[cs + child_iter[top]++];
            dt->dfs_in[c] = tick++;
            stack[sp++] = c;
        } else {
            dt->dfs_out[top] = tick++;
            sp--;
        }
    }

    xr_free(stack);
    xr_free(child_iter);
    return dt;
}

static void xm_domtree_free(XmDomTree *dt) {
    if (!dt)
        return;
    xr_free(dt->idom);
    xr_free(dt->dfs_in);
    xr_free(dt->dfs_out);
    xr_free(dt->children);
    xr_free(dt->child_start);
    xr_free(dt);
}

const XmDomTree *xm_func_get_domtree(XmFunc *func) {
    if (!func || func->nblk == 0)
        return NULL;
    if (func->domtree)
        return func->domtree;

    uint32_t *idom = (uint32_t *) xr_malloc(func->nblk * sizeof(uint32_t));
    if (!idom)
        return NULL;
    xm_compute_idom(func, idom, func->nblk);

    XmDomTree *dt = build_tree_from_idom(idom, func->nblk);
    xr_free(idom);
    if (!dt)
        return NULL;

    func->domtree = dt;
    return dt;
}

void xm_func_invalidate_domtree(XmFunc *func) {
    if (!func)
        return;
    if (func->domtree) {
        xm_domtree_free(func->domtree);
        func->domtree = NULL;
    }
}
