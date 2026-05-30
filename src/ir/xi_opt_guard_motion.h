/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_guard_motion.h - Guard hoisting / sinking
 *
 * KEY CONCEPT:
 *   Hoists loop-invariant XI_GUARD_TYPE instructions to the loop
 *   preheader so the type check runs once rather than every iteration.
 *   A guard is loop-invariant when its receiver operand is defined
 *   outside the loop (i.e., in a block not belonging to the loop body).
 */

#ifndef XI_OPT_GUARD_MOTION_H
#define XI_OPT_GUARD_MOTION_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_guard_motion(XiFunc *f);

#endif /* XI_OPT_GUARD_MOTION_H */
