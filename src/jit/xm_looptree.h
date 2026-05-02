/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_looptree.h - Cached natural-loop forest.
 *
 * KEY CONCEPT:
 *   A single shared analysis result describing every natural loop in a
 *   function, including nesting relationships.  Passes that used to
 *   scan back-edges and manage their own fixed-size LicmLoop[]
 *   arrays now ask XmLoopInfo for loops and iterate through the
 *   cached structure instead.
 *
 * GUARANTEES (built by xm_func_get_loops):
 *   1. Every loop has a non-null |header| and at least one |latch|.
 *   2. |all_loops| is sorted innermost-first (children before their
 *      parents) so loop-local transformations automatically process
 *      inner loops before outer ones.
 *   3. |block_to_loop[bid]| is the innermost enclosing loop for each
 *      reachable block (NULL for blocks that live outside any loop).
 *   4. |preheader| is populated when — and only when — the header has
 *      exactly one out-of-loop predecessor.  Callers that need a
 *      guaranteed preheader must invoke xm_ensure_preheader(), which
 *      may rewrite the CFG to create one.
 *
 * INVALIDATION:
 *   Any pass that adds / removes / retargets CFG edges must call
 *   xm_func_invalidate_loops() before returning control.  The
 *   dominator tree is rebuilt lazily whenever the loop info is
 *   rebuilt, so passes never need to invalidate them separately.
 */

#ifndef XM_LOOPTREE_H
#define XM_LOOPTREE_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XmFunc XmFunc;
typedef struct XmBlock XmBlock;

typedef struct XmLoop {
    struct XmLoop *parent;   // outer loop (NULL at the forest root)
    struct XmLoop *child;    // first nested loop
    struct XmLoop *sibling;  // next loop sharing the same parent

    XmBlock *header;     // loop header
    XmBlock *preheader;  // unique out-of-loop predecessor, or NULL
    XmBlock *latch;      // back-edge source (first latch if multiple)

    XmBlock **body;  // blocks belonging to this loop (heap-allocated)
    uint32_t nbody;

    uint32_t depth;  // 1-based nesting depth
    uint32_t id;     // position in XmLoopInfo::all_loops[]
} XmLoop;

typedef struct XmLoopInfo {
    XmLoop *root_list;       // top-level loops linked via sibling
    XmLoop **all_loops;      // heap array, innermost-first
    XmLoop **block_to_loop;  // [nblk] innermost loop per block (NULL = none)
    uint32_t nloop;
    uint32_t nblk;  // size of block_to_loop[]
} XmLoopInfo;

/*
 * Lazily compute and cache the loop info for |func|.  Returns NULL
 * only if the function has no blocks.  The returned pointer is owned
 * by the cache.
 */
XR_FUNC const XmLoopInfo *xm_func_get_loops(XmFunc *func);

/*
 * Drop the cached loop info (also invalidates the dom-tree cache, as
 * loops depend on dominance).  Safe to call when nothing is cached.
 */
XR_FUNC void xm_func_invalidate_loops(XmFunc *func);

/*
 * Innermost loop nesting depth for |blk_id| using the cached loop
 * info.  Returns 0 for blocks outside any loop.  Passes that used to
 * read XmBlock::loop_depth should call this instead.
 */
XR_FUNC uint32_t xm_block_loop_depth(XmFunc *func, uint32_t blk_id);

#endif  // XM_LOOPTREE_H
