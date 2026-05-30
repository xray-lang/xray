/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_rotate.h - Loop rotation for Xi IR
 */

#ifndef XI_OPT_LOOP_ROTATE_H
#define XI_OPT_LOOP_ROTATE_H

#include "xi.h"
#include "xi_pass.h"

/* Rotate a canonical while-style natural loop into a guarded loop body.
 * The implementation is deliberately conservative: it only duplicates
 * clone-safe header computations and preserves SSA with edge phis. */
XR_FUNC XiPassChange xi_opt_loop_rotate(XiFunc *f);

#endif  // XI_OPT_LOOP_ROTATE_H
