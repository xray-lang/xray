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
 *   xi_ensure_rpo(f);          // recomputes only if CFG changed since last call
 *   xi_ensure_dominators(f);   // requires RPO; same lazy behaviour
 *   xi_compute_liveness(f);    // requires RPO, returns heap-allocated result
 *
 * The xi_compute_* entry points are unconditional and remain available
 * for tests and tools that need to force a fresh computation.
 */

#ifndef XI_ANALYSIS_H
#define XI_ANALYSIS_H

#include "xi.h"

/* ========== CFG-derived Analysis Cache ========== */

/* Bump cfg_version so the next xi_ensure_*() call recomputes.
 * Pipeline driver calls this automatically when a pass reports
 * XiPassChange.cfg_changed; passes that mutate the CFG outside the
 * driver must call it manually. */
XR_FUNC void xi_cfg_invalidate(XiFunc *f);

/* ========== RPO (Reverse Post-Order) ========== */

/* Compute reverse post-order numbering for all reachable blocks.
 * Sets XiBlock.rpo for each block (1-based; 0 = unreachable).
 * Returns the number of reachable blocks.
 * This is the unconditional form — prefer xi_ensure_rpo() inside passes. */
XR_FUNC uint32_t xi_compute_rpo(XiFunc *f);

/* Cached form: recompute only if cfg_version has advanced since the
 * last call.  Returns true if a recomputation occurred (cache miss). */
XR_FUNC bool xi_ensure_rpo(XiFunc *f);

/* ========== Dominator Tree ========== */

/* Compute immediate dominators using the Cooper-Harvey-Kennedy algorithm.
 * Requires RPO to be computed first (xi_compute_rpo).
 * Sets XiBlock.idom and XiBlock.dom_depth for each reachable block.
 * Entry block: idom = NULL, dom_depth = 0.
 * Unconditional form — prefer xi_ensure_dominators() inside passes. */
XR_FUNC void xi_compute_dominators(XiFunc *f);

/* Cached form: ensures both RPO and dominators are up to date.
 * Recomputes only what cfg_version requires.  Returns true if any
 * recomputation occurred. */
XR_FUNC bool xi_ensure_dominators(XiFunc *f);

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
