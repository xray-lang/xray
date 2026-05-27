/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_ivsr.h - Induction Variable Strength Reduction
 *
 * KEY CONCEPT:
 *   Replace `j = i * c + k` inside a loop, where i is a basic IV with
 *   constant step s, with an additive recurrence:
 *     preheader:  j_start = i_start * c + k
 *     header:     j_phi   = phi(j_start, j_next)
 *     latch:      j_next  = j_phi + (s * c)
 *   The original derived expression is rewritten to COPY(j_phi); the
 *   stale multiply is reaped by the subsequent copy-prop / dce passes.
 *
 *   Multiplications inside loops are turned into a single per-iteration
 *   add — typical 4-10× speedup for tight array-stride loops.
 *
 * GUARANTEES:
 *   - Only the value list is mutated; the CFG is untouched, so the
 *     dom / loop caches stay valid.
 *   - Strict prerequisites: the basic IV must have a constant step,
 *     the derived IV must have a constant scale, and the loop must
 *     have both a preheader and a latch.  Anything else is left alone.
 */

#ifndef XI_OPT_IVSR_H
#define XI_OPT_IVSR_H

#include "xi.h"
#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_ivsr(XiFunc *f);

#endif  // XI_OPT_IVSR_H
