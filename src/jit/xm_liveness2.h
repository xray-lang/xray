/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_liveness2.h - Dataflow-based liveness analysis for Xm
 *
 * KEY CONCEPT:
 *   Computes per-block live-in and live-out sets using standard
 *   backward dataflow equations with BSet (dynamic bitsets).
 *   Handles SSA phi nodes correctly: phi arguments are live-out
 *   of the corresponding predecessor block.
 *
 * WHY THIS DESIGN:
 *   - Replaces the simple last_use[] array with proper set-based
 *     liveness, enabling interference graph construction and
 *     more precise dead code elimination.
 *   - Separated from regalloc so any pass can query liveness.
 *   - Uses XmBSet for dynamic sizing (no fixed 512-vreg limit).
 */

#ifndef XM_LIVENESS2_H
#define XM_LIVENESS2_H

#include "xm.h"
#include "xm_bset.h"
#include "../base/xdefs.h"

// Per-block liveness sets
typedef struct {
    XmBSet def;       // vregs defined in this block
    XmBSet use;       // vregs used before being defined in this block
    XmBSet live_in;   // vregs live at block entry
    XmBSet live_out;  // vregs live at block exit
} XmBlockLive;

// Per-function liveness result
typedef struct {
    XmBlockLive *blocks;  // array indexed by block layout order [0..nblk)
    uint32_t nblk;
    uint32_t nvreg;  // bitset width
} XmLive;

/*
 * Compute live-in and live-out sets for all blocks.
 *
 * Algorithm:
 *   1. For each block, compute local def/use sets.
 *   2. Iterate backward dataflow equations until fixed point:
 *        live_out[B] = union of live_in[S] for each successor S
 *                    + phi arguments targeting B from successor phis
 *        live_in[B]  = use[B] | (live_out[B] - def[B])
 *
 * The caller must free the result with xm_live_free().
 */
XR_FUNC void xm_live_compute(XmLive *live, XmFunc *func);

// Release all memory held by a XmLive result
XR_FUNC void xm_live_free(XmLive *live);

// Query: is vreg v live at the entry of block bi?
static inline bool xm_live_at_entry(const XmLive *live, uint32_t bi, uint32_t v) {
    if (bi >= live->nblk || v >= live->nvreg)
        return false;
    return xm_bset_has(&live->blocks[bi].live_in, v);
}

// Query: is vreg v live at the exit of block bi?
static inline bool xm_live_at_exit(const XmLive *live, uint32_t bi, uint32_t v) {
    if (bi >= live->nblk || v >= live->nvreg)
        return false;
    return xm_bset_has(&live->blocks[bi].live_out, v);
}

#endif  // XM_LIVENESS2_H
