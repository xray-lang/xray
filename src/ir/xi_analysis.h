/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_analysis.h - Static analysis passes for Xi IR
 *
 * KEY CONCEPT:
 *   Analysis passes compute derived information from the IR without
 *   modifying it. Results are stored on the IR structures themselves
 *   (XiBlock.rpo, XiBlock.idom, XiBlock.dom_depth) to avoid extra
 *   allocations and simplify downstream access.
 *
 *   RPO is the foundation for all other analyses.
 *   Dominator tree enables loop detection and SSA validation.
 *   Liveness enables register allocation and spill decisions.
 *
 * USAGE:
 *   xi_compute_rpo(f);        // must be called first
 *   xi_compute_dominators(f); // requires RPO
 *   xi_compute_liveness(f);   // requires RPO, returns heap-allocated result
 */

#ifndef XI_ANALYSIS_H
#define XI_ANALYSIS_H

#include "xi.h"

/* ========== RPO (Reverse Post-Order) ========== */

/* Compute reverse post-order numbering for all reachable blocks.
 * Sets XiBlock.rpo for each block (1-based; 0 = unreachable).
 * Returns the number of reachable blocks. */
XR_FUNC uint32_t xi_compute_rpo(XiFunc *f);

/* ========== Dominator Tree ========== */

/* Compute immediate dominators using the Cooper-Harvey-Kennedy algorithm.
 * Requires RPO to be computed first (xi_compute_rpo).
 * Sets XiBlock.idom and XiBlock.dom_depth for each reachable block.
 * Entry block: idom = NULL, dom_depth = 0. */
XR_FUNC void xi_compute_dominators(XiFunc *f);

/* Query: does block 'a' dominate block 'b'?
 * Requires dominators to be computed. O(depth) walk. */
XR_FUNC bool xi_dominates(const XiBlock *a, const XiBlock *b);

/* ========== Liveness Analysis ========== */

/* Per-block liveness sets.
 * Bitsets indexed by value ID. Allocated as a single contiguous block. */
typedef struct XiLiveness {
    uint32_t nblocks;   /* number of blocks */
    uint32_t set_words; /* uint64_t words per bitset */
    uint64_t *live_in;  /* [nblocks * set_words] — values live at block entry */
    uint64_t *live_out; /* [nblocks * set_words] — values live at block exit */
} XiLiveness;

/* Compute liveness for all values in the function.
 * Requires RPO. Returns heap-allocated XiLiveness (caller must free).
 * Uses standard backward dataflow: live_out = union of successor live_in;
 * live_in = (live_out - defs) | uses. */
XR_FUNC XiLiveness *xi_compute_liveness(XiFunc *f);

/* Query: is value 'v' live at the entry of block 'blk'? */
XR_FUNC bool xi_is_live_in(const XiLiveness *l, const XiBlock *blk, const XiValue *v);

/* Query: is value 'v' live at the exit of block 'blk'? */
XR_FUNC bool xi_is_live_out(const XiLiveness *l, const XiBlock *blk, const XiValue *v);

/* Free liveness data. */
XR_FUNC void xi_liveness_free(XiLiveness *l);

#endif  // XI_ANALYSIS_H
