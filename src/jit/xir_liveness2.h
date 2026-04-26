/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_liveness2.h - Dataflow-based liveness analysis for XIR
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
 *   - Uses XirBSet for dynamic sizing (no fixed 512-vreg limit).
 */

#ifndef XIR_LIVENESS2_H
#define XIR_LIVENESS2_H

#include "xir.h"
#include "xir_bset.h"
#include "../base/xdefs.h"

// Per-block liveness sets
typedef struct {
    XirBSet def;       // vregs defined in this block
    XirBSet use;       // vregs used before being defined in this block
    XirBSet live_in;   // vregs live at block entry
    XirBSet live_out;  // vregs live at block exit
} XirBlockLive;

// Per-function liveness result
typedef struct {
    XirBlockLive *blocks;  // array indexed by block layout order [0..nblk)
    uint32_t nblk;
    uint32_t nvreg;  // bitset width
} XirLive;

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
 * The caller must free the result with xir_live_free().
 */
XR_FUNC void xir_live_compute(XirLive *live, XirFunc *func);

// Release all memory held by a XirLive result
XR_FUNC void xir_live_free(XirLive *live);

// Query: is vreg v live at the entry of block bi?
static inline bool xir_live_at_entry(const XirLive *live, uint32_t bi, uint32_t v) {
    if (bi >= live->nblk || v >= live->nvreg)
        return false;
    return xir_bset_has(&live->blocks[bi].live_in, v);
}

// Query: is vreg v live at the exit of block bi?
static inline bool xir_live_at_exit(const XirLive *live, uint32_t bi, uint32_t v) {
    if (bi >= live->nblk || v >= live->nvreg)
        return false;
    return xir_bset_has(&live->blocks[bi].live_out, v);
}

#endif  // XIR_LIVENESS2_H
