/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_regalloc.h - Linear scan register allocation with per-segment mapping
 *
 * KEY CONCEPT:
 *   Per-segment register mapping: each vreg's lifetime is
 *   split into segments, each with its own register or spill assignment.
 *   Queries use RA position to find the covering segment — no per-block
 *   tables, no global fallback, just precise segment lookup.
 *
 * WHY THIS DESIGN:
 *   - Eliminates redundant per-block tables and error-prone global fallback
 *   - Segment lookup is O(segments) per vreg, typically 1-3 segments
 *   - blk_start[] exported to XraResult lets codegen compute RA positions
 *   - Gap moves handle mid-block register transitions
 *   - Edge copies handle block-boundary transitions
 */

#ifndef XIR_REGALLOC_H
#define XIR_REGALLOC_H

#include "xir.h"
#include "xir_liveness2.h"
#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

#define XRA_MAX_GP_REGS 22
#define XRA_MAX_FP_REGS 16

/*
 * Maximum number of spill slots a single JIT function may use.
 *
 * This is the single source of truth shared by LSRA, codegen, and the
 * ARM64 target descriptor.  It is upper-bounded by the width of the GC
 * stack-map spill bitmap (see XrStackMapEntry::spill_bitmap, 32 bits):
 * slots beyond bit 31 could not be reported to GC, so a PTR vreg spilled
 * there would be missed during stack scanning and cause use-after-free.
 *
 * If regalloc needs more than this many slots for a function, LSRA sets
 * XraResult::had_error = true and codegen refuses compilation, letting
 * the interpreter continue executing that function.
 */
#define XIR_MAX_SPILL_SLOTS 32

// Spill slot special values
#define XRA_SPILL_NONE (-1)
#define XRA_SPILL_REMAT (-2)

/*
 * Gap move: intra-block register transfer at mid-block split points.
 * Emitted as MOV before instruction gap_ins_idx in block gap_blk.
 * After emission, vreg is in dst_reg (codegen tracks via override).
 */
typedef struct {
    uint32_t gap_blk;      // block ID
    uint16_t gap_ins_idx;  // emit before this instruction index
    uint16_t vreg;         // vreg being transferred
    int8_t src_reg;        // source alloc index (-1 = reload from spill)
    int8_t dst_reg;        // dest alloc index (-1 = store to spill)
    int16_t spill_slot;    // spill slot when src/dst is -1
    bool is_fp;
} XraGapMove;

/*
 * Per-segment allocation: one segment per sibling in the split chain.
 * Segments are sorted by start position and non-overlapping.
 */
typedef struct {
    int32_t start;    // RA position start (inclusive)
    int32_t end;      // RA position end (exclusive)
    int8_t assigned;  // alloc index (-1 = spilled)
    bool is_fp;
} XraSegment;

/*
 * Per-vreg allocation info: segment chain + spill.
 */
typedef struct {
    XraSegment *segs;  // heap-allocated, sorted by start
    uint16_t nseg;
    int16_t spill;  // spill slot (XRA_SPILL_NONE / XRA_SPILL_REMAT / >=0)
} XraVRegAlloc;

/*
 * Register allocation result (per-segment architecture).
 *
 * Each vreg has a segment chain describing its register assignment across
 * its lifetime. Codegen uses blk_start[] to compute RA positions and queries
 * segments directly — no per-block tables or global fallback needed.
 */
typedef struct {
    XraVRegAlloc *valloc;  // [nvreg] per-vreg segment info

    // RA position mapping: indexed by block ID
    int32_t *blk_start;  // [nblk] RA position of block start
    int32_t *blk_end;    // [nblk] RA position of block end

    // Per-block live register bitmasks
    uint32_t *blk_gp_live;   // [nblk] bitmask: active GP alloc indices
    uint32_t *blk_fp_live;   // [nblk] bitmask: active FP alloc indices
    uint32_t *blk_ptr_live;  // [nblk] bitmask: GP alloc indices holding PTR

    // Gap moves for mid-block split transitions
    XraGapMove *gap_moves;  // heap-allocated, sorted by (gap_blk, gap_ins_idx)
    uint32_t ngap_move;

    uint32_t nvreg;
    uint32_t nblk;  // max_blk_id + 1
    uint32_t nspill;
    uint32_t callee_saved;  // bitmask of callee-saved regs used

    // Graceful compilation refusal: set by LSRA when register allocation
    // cannot produce valid code for this function (e.g. spill slot count
    // exceeds XIR_MAX_SPILL_SLOTS).  Codegen must check this before
    // emitting any code and abort compilation with a diagnostic error.
    bool had_error;
} XraResult;

/*
 * Run linear scan register allocation on a XIR function.
 *
 * Algorithm:
 *   1. Compute liveness (live-in / live-out per block)
 *   2. Build live ranges with UseInterval chains (backward scan)
 *   3. Build fixed intervals for CALL clobbers (caller-saved regs)
 *   4. Compute bundles (Phi/MOV-connected non-overlapping ranges)
 *   5. Compute hints (bundles, params, MOV/Phi coalescing)
 *   6. Sort by start position ascending
 *   7. Linear scan with Active/Inactive state machine,
 *      spill-then-reload, fixed-range protection
 *   8. Build per-segment result + gap moves + edge copies
 *
 * Returns heap-allocated XraResult (caller calls xra_result_free).
 * Returns NULL on failure.
 */
XR_FUNC XraResult *xra_run(XirFunc *func);

// Free register allocation result
XR_FUNC void xra_result_free(XraResult *r);

// Core query: find register at exact RA position.
// All xra_* query helpers tolerate r == NULL to simplify callers that may
// operate on functions for which register allocation was skipped or failed.
static inline int8_t xra_reg_at_pos(const XraResult *r, uint32_t vreg, int32_t pos) {
    if (!r || vreg >= r->nvreg)
        return -1;
    const XraVRegAlloc *va = &r->valloc[vreg];
    for (uint16_t i = 0; i < va->nseg; i++) {
        if (va->segs[i].start <= pos && pos < va->segs[i].end)
            return va->segs[i].assigned;
    }
    return -1;
}

// Convenience: register at block start (most common codegen query)
static inline int8_t xra_vreg_reg_at(const XraResult *r, uint32_t blk_id, uint32_t vreg) {
    if (!r || blk_id >= r->nblk || !r->blk_start)
        return -1;
    return xra_reg_at_pos(r, vreg, r->blk_start[blk_id]);
}

/* Register at block end — needed for edge copies where the source vreg
 * may be defined mid-block (e.g. loop increment before back-edge). */
static inline int8_t xra_vreg_reg_at_end(const XraResult *r, uint32_t blk_id, uint32_t vreg) {
    if (!r || blk_id >= r->nblk || !r->blk_end)
        return -1;
    int32_t end = r->blk_end[blk_id];
    return xra_reg_at_pos(r, vreg, end > 0 ? end - 1 : 0);
}

// Look up spill slot for vreg. Returns XRA_SPILL_NONE/-2 if not spilled.
static inline int16_t xra_vreg_spill(const XraResult *r, uint32_t vreg) {
    if (!r || vreg >= r->nvreg)
        return XRA_SPILL_NONE;
    return r->valloc[vreg].spill;
}

/* Returns true if vreg has any live-interval segment covering pos.
 * Unlike xra_reg_at_pos, this returns true even for spilled segments
 * (assigned == -1), allowing callers to distinguish "live but spilled"
 * from "dead (no segment covers pos)". */
static inline bool xra_vreg_live_at(const XraResult *r, uint32_t vreg, int32_t pos) {
    if (!r || vreg >= r->nvreg)
        return false;
    const XraVRegAlloc *va = &r->valloc[vreg];
    for (uint16_t i = 0; i < va->nseg; i++) {
        if (va->segs[i].start <= pos && pos < va->segs[i].end)
            return true;
    }
    return false;
}

// First assigned register for vreg (any segment). Used for prologue params.
static inline int8_t xra_vreg_first_reg(const XraResult *r, uint32_t vreg) {
    if (!r || vreg >= r->nvreg)
        return -1;
    const XraVRegAlloc *va = &r->valloc[vreg];
    for (uint16_t i = 0; i < va->nseg; i++)
        if (va->segs[i].assigned >= 0)
            return va->segs[i].assigned;
    return -1;
}

/*
 * Edge copies for CFG edge from→target.
 * Includes both Phi resolution and split-transition moves.
 */
typedef struct {
    uint8_t dst_idx;  // alloc_regs/alloc_fp_regs index
    uint8_t src_idx;  // alloc_regs/alloc_fp_regs index
    bool is_fp;
    bool is_reload;      // true = src is spill slot, not register
    int16_t spill_slot;  // valid when is_reload = true
} XraEdgeCopy;

XR_FUNC uint32_t xra_edge_copies(const XraResult *r, XirFunc *func, XirBlock *target,
                                 XirBlock *from, XraEdgeCopy *out, uint32_t max_copies);

#endif  // XIR_REGALLOC_H
