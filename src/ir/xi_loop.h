/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_loop.h - Natural-loop forest for Xi IR
 *
 * KEY CONCEPT:
 *   Identifies all natural loops in a function's CFG using the dominator
 *   tree.  A back-edge (src -> hdr) exists when hdr dominates src.
 *   The natural-loop body is all blocks reachable backward from the latch
 *   without crossing the header.
 *
 * GUARANTEES:
 *   1. Every XiLoop has a non-null header and at least one latch.
 *   2. all_loops[] is sorted innermost-first (children before parents)
 *      so loop-local transforms process inner loops before outer ones.
 *   3. block_to_loop[bid] is the innermost enclosing loop for block bid.
 *   4. preheader is set only when the header has exactly one out-of-loop
 *      predecessor; NULL otherwise.
 *
 * INVARIANTS:
 *   Requires RPO + dominator tree computed via xi_analysis.h.
 *   Any pass that modifies the CFG must call xi_loopinfo_free()
 *   and recompute if loop info is still needed.
 */

#ifndef XI_LOOP_H
#define XI_LOOP_H

#include "xi.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Loop Structures ========== */

typedef struct XiLoop {
    struct XiLoop *parent;    /* outer loop (NULL at forest root) */
    struct XiLoop *child;     /* first nested loop */
    struct XiLoop *sibling;   /* next loop sharing the same parent */

    XiBlock *header;          /* loop header block */
    XiBlock *preheader;       /* unique out-of-loop predecessor, or NULL */
    XiBlock *latch;           /* back-edge source (first if multiple) */

    XiBlock **body;           /* heap-allocated body block array */
    uint32_t nbody;           /* number of blocks in body */

    uint32_t depth;           /* 1-based nesting depth */
    uint32_t id;              /* index in XiLoopInfo::all_loops[] */
} XiLoop;

typedef struct XiLoopInfo {
    XiLoop  *root_list;       /* top-level loops linked via sibling */
    XiLoop **all_loops;       /* heap array, innermost-first */
    XiLoop **block_to_loop;   /* [nblocks] innermost loop per block */
    uint32_t nloop;           /* total number of loops */
    uint32_t nblocks;         /* size of block_to_loop[] */
} XiLoopInfo;

/* ========== API ========== */

/* Compute the natural-loop forest for the function.
 * Requires RPO + dominators already computed (xi_compute_rpo,
 * xi_compute_dominators).  Returns heap-allocated result.
 * Returns NULL if no loops found or on allocation failure. */
XR_FUNC XiLoopInfo *xi_compute_loops(XiFunc *f);

/* Free loop info and all associated memory. */
XR_FUNC void xi_loopinfo_free(XiLoopInfo *info);

/* Query: innermost loop nesting depth for block.
 * Returns 0 for blocks outside any loop. */
XR_FUNC uint32_t xi_block_loop_depth(const XiLoopInfo *info, uint32_t blk_id);

/* Query: does the given block belong to the given loop? */
XR_FUNC bool xi_loop_contains_block(const XiLoop *loop, const XiBlock *blk);

#endif // XI_LOOP_H
