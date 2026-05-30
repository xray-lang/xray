/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_strength.h - Strength reduction for Xi IR
 *
 * KEY CONCEPT:
 *   Algebraic identity rewrites for integer binary operations.
 *   Each match collapses the value to either a COPY (operand passes
 *   through unchanged) or a CONST (zero / absorbing element).
 *
 *   Patterns recognized:
 *     x + 0  /  0 + x        -> x
 *     x - 0                  -> x
 *     x - x                  -> 0
 *     x * 1  /  1 * x        -> x
 *     x * 0  /  0 * x        -> 0   (numeric only; string * 0 stays)
 *     x / 1                  -> x
 *     x & 0  /  0 & x        -> 0
 *     x & x                  -> x
 *     x | 0  /  0 | x        -> x
 *     x | x                  -> x
 *     x ^ 0  /  0 ^ x        -> x
 *     x ^ x                  -> 0
 *     x << 0  /  x >> 0      -> x
 */

#ifndef XI_OPT_STRENGTH_H
#define XI_OPT_STRENGTH_H

#include "xi.h"
#include "xi_pass.h"

/* Run strength reduction on every block of the function.
 * Subsequent copy_prop / dce passes clean up the inserted COPY values. */
XR_FUNC XiPassChange xi_opt_strength_reduce(XiFunc *f);

#endif  // XI_OPT_STRENGTH_H
