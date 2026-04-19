/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_domtree.h - Cached dominator-tree with O(1) dominance queries.
 *
 * KEY CONCEPT:
 *   Every JIT optimisation pass that reasons about dominance goes
 *   through this cache, so the dominator analysis runs *once* per
 *   function and is invalidated only when a pass rewrites the CFG.
 *
 * WHY THIS DESIGN:
 *   - The tree is a proper data structure (idom + flattened children
 *     list + DFS in/out numbering), not just the raw idom[] array,
 *     so queries like "a dominates b" become O(1) interval tests and
 *     "children of a" iteration avoids scanning every block.
 *   - Clients get a const pointer and the cache is owned by XirFunc;
 *     ownership / lifetime rules stay simple.
 *
 * ALGORITHM:
 *   1. Compute idom[] with Cooper's iterative intersection (same as
 *      the legacy implementation; runs in O(n^2 / loops) on the small
 *      JIT CFGs we see in practice).
 *   2. Build the child list and a DFS over it to assign dfs_in /
 *      dfs_out timestamps: a dominates b iff
 *        dfs_in[a] <= dfs_in[b] < dfs_out[a]
 *
 * FUTURE:
 *   Swap step 1 for Semi-NCA when large functions make the O(n^2)
 *   bound bite; the DFS in step 2 stays.
 */

#ifndef XIR_DOMTREE_H
#define XIR_DOMTREE_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XirFunc XirFunc;

typedef struct XirDomTree {
    uint32_t *idom;         // [nblk] immediate dominator block id
    uint32_t *dfs_in;       // [nblk] DFS entry timestamp
    uint32_t *dfs_out;      // [nblk] DFS exit timestamp (exclusive)
    uint32_t *children;     // flattened child index list
    uint32_t *child_start;  // [nblk+1] offsets into children[]
    uint32_t  nblk;
} XirDomTree;

/* O(1) dominance test: does |a| dominate |b|?
 *
 * Encoded as a DFS-interval containment check: a dominates b iff b's
 * DFS timestamp falls inside a's DFS subtree interval. */
static inline bool xir_dom_covers(const XirDomTree *dt, uint32_t a, uint32_t b) {
    if (!dt || a >= dt->nblk || b >= dt->nblk) return false;
    return dt->dfs_in[a] <= dt->dfs_in[b] && dt->dfs_in[b] < dt->dfs_out[a];
}

/*
 * Access the immediate dominator of |blk_id|; UINT32_MAX if unreachable.
 *
 * Equivalent to dt->idom[blk_id] with bounds checking, provided for
 * callers that do not want to reach into the struct directly.
 */
static inline uint32_t xir_dom_idom(const XirDomTree *dt, uint32_t blk_id) {
    if (!dt || blk_id >= dt->nblk) return UINT32_MAX;
    return dt->idom[blk_id];
}

/*
 * Iterate dom-tree children of |blk_id|.
 *
 * The children are stored in a single flat array; |*out_n| receives
 * the count and the return value points at the first child id.  Do
 * not free the returned pointer — it is owned by the tree.
 */
static inline const uint32_t *xir_dom_children(const XirDomTree *dt,
                                                uint32_t blk_id,
                                                uint32_t *out_n) {
    if (!dt || blk_id >= dt->nblk) { if (out_n) *out_n = 0; return NULL; }
    uint32_t s = dt->child_start[blk_id];
    uint32_t e = dt->child_start[blk_id + 1];
    if (out_n) *out_n = e - s;
    return dt->children + s;
}

/*
 * Get or lazily compute the dominator tree cached on |func|.
 * Returns NULL if the function has no blocks.
 */
XR_FUNC const XirDomTree *xir_func_get_domtree(XirFunc *func);

/*
 * Invalidate the cached dominator tree.  Must be called by any pass
 * that modifies the CFG (adds/removes blocks, retargets terminators,
 * rewires predecessors).  Safe to call when no tree is cached.
 */
XR_FUNC void xir_func_invalidate_domtree(XirFunc *func);

#endif // XIR_DOMTREE_H
