/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_looptree.h - Cached natural-loop forest.
 *
 * KEY CONCEPT:
 *   A single shared analysis result describing every natural loop in a
 *   function, including nesting relationships.  Passes that used to
 *   scan back-edges and manage their own fixed-size LicmLoop[]
 *   arrays now ask XirLoopInfo for loops and iterate through the
 *   cached structure instead.
 *
 * GUARANTEES (built by xir_func_get_loops):
 *   1. Every loop has a non-null |header| and at least one |latch|.
 *   2. |all_loops| is sorted innermost-first (children before their
 *      parents) so loop-local transformations automatically process
 *      inner loops before outer ones.
 *   3. |block_to_loop[bid]| is the innermost enclosing loop for each
 *      reachable block (NULL for blocks that live outside any loop).
 *   4. |preheader| is populated when — and only when — the header has
 *      exactly one out-of-loop predecessor.  Callers that need a
 *      guaranteed preheader must invoke xir_ensure_preheader(), which
 *      may rewrite the CFG to create one.
 *
 * INVALIDATION:
 *   Any pass that adds / removes / retargets CFG edges must call
 *   xir_func_invalidate_loops() before returning control.  The
 *   dominator tree is rebuilt lazily whenever the loop info is
 *   rebuilt, so passes never need to invalidate them separately.
 */

#ifndef XIR_LOOPTREE_H
#define XIR_LOOPTREE_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XirFunc XirFunc;
typedef struct XirBlock XirBlock;

typedef struct XirLoop {
    struct XirLoop *parent;   // outer loop (NULL at the forest root)
    struct XirLoop *child;    // first nested loop
    struct XirLoop *sibling;  // next loop sharing the same parent

    XirBlock *header;     // loop header
    XirBlock *preheader;  // unique out-of-loop predecessor, or NULL
    XirBlock *latch;      // back-edge source (first latch if multiple)

    XirBlock **body;  // blocks belonging to this loop (heap-allocated)
    uint32_t nbody;

    uint32_t depth;  // 1-based nesting depth
    uint32_t id;     // position in XirLoopInfo::all_loops[]
} XirLoop;

typedef struct XirLoopInfo {
    XirLoop *root_list;       // top-level loops linked via sibling
    XirLoop **all_loops;      // heap array, innermost-first
    XirLoop **block_to_loop;  // [nblk] innermost loop per block (NULL = none)
    uint32_t nloop;
    uint32_t nblk;  // size of block_to_loop[]
} XirLoopInfo;

/*
 * Lazily compute and cache the loop info for |func|.  Returns NULL
 * only if the function has no blocks.  The returned pointer is owned
 * by the cache.
 */
XR_FUNC const XirLoopInfo *xir_func_get_loops(XirFunc *func);

/*
 * Drop the cached loop info (also invalidates the dom-tree cache, as
 * loops depend on dominance).  Safe to call when nothing is cached.
 */
XR_FUNC void xir_func_invalidate_loops(XirFunc *func);

/*
 * Innermost loop nesting depth for |blk_id| using the cached loop
 * info.  Returns 0 for blocks outside any loop.  Passes that used to
 * read XirBlock::loop_depth should call this instead.
 */
XR_FUNC uint32_t xir_block_loop_depth(XirFunc *func, uint32_t blk_id);

#endif  // XIR_LOOPTREE_H
