/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_bce.h - Bounds Check Elimination for Xi IR
 *
 * Eliminates redundant XI_BOUNDS_CHECK instructions when the index
 * range can be statically proven to be within [0, len).  Uses range
 * analysis results (XI_INV_RANGE_ANNOTATED).
 *
 * Two elimination strategies:
 *   1. Range proof: range(idx) ⊆ [0, range(len).lo - 1]
 *   2. Dominator dedup: a dominating check with same (idx, len)
 *      already guards this access
 */

#ifndef XI_OPT_BCE_H
#define XI_OPT_BCE_H

#include "xi.h"
#include "xi_pass.h"

/* Run bounds check elimination.
 * Requires: XI_INV_RANGE_ANNOTATED (run xi_range_analyze first).
 * Eliminates provably-safe bounds checks by replacing them with XI_COPY
 * of the index value (preserving SSA def). */
XR_FUNC XiPassChange xi_opt_bce(XiFunc *f);

#endif  // XI_OPT_BCE_H
