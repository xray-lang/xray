/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_coalesce.h - Aggressive register coalescing for Xm
 *
 * KEY CONCEPT:
 *   Pre-RA pass that merges MOV operands when they don't interfere.
 *   Uses a conflict matrix built from liveness to determine safety.
 *   Processes MOVs in frequency-descending order (loop-depth weighted)
 *   to maximize benefit for hot code.
 *
 * WHY THIS DESIGN:
 *   - Deterministic coalescing (vs V8's hint-based "hope for the best")
 *   - Reduces vreg count before RA, making allocation faster
 *   - Eliminates 30-60% of MOV instructions (measured in MIR)
 *   - Runs before RA so coalesced ranges get better allocation
 */

#ifndef XM_COALESCE_H
#define XM_COALESCE_H

#include "xm.h"
#include "xm_liveness2.h"
#include "../base/xdefs.h"

// Max vregs for conflict matrix (n^2 bits / 8 bytes)
#define XM_MAX_COALESCE_VREGS 4096

/*
 * Run aggressive MOV coalescing on a Xm function.
 *
 * Algorithm:
 *   1. Collect all XM_MOV instructions, weighted by loop depth
 *   2. Build interference (conflict) matrix for MOV-related vregs
 *   3. Sort MOVs by frequency descending
 *   4. For each MOV(dst, src): if no conflict, merge src into dst
 *   5. Rewrite all instructions to use merged vreg names
 *   6. Delete coalesced MOVs (replace with NOP)
 *
 * Modifies func in-place. Liveness must be recomputed after this pass.
 * Returns the number of MOVs eliminated.
 */
XR_FUNC uint32_t xm_coalesce(XmFunc *func);

#endif  // XM_COALESCE_H
