/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_ifconv.h - If-Conversion for Xi IR
 *
 * KEY CONCEPT:
 *   Convert simple diamond CFG patterns into XI_SELECT values,
 *   eliminating branches and phi nodes.  Reduces branch misprediction
 *   cost for small conditional expressions (ternary, min/max, clamp).
 *
 *   Pattern: ifblk(cond → then, else) → joinblk(phi) becomes
 *            ifblk: [then_ins] + [else_ins] + SELECT(cond, t, f)
 */

#ifndef XI_OPT_IFCONV_H
#define XI_OPT_IFCONV_H

#include "xi.h"
#include "xi_pass.h"

/* Run if-conversion on the function.  Requires dominators computed. */
XR_FUNC XiPassChange xi_opt_ifconv(XiFunc *f);

#endif  // XI_OPT_IFCONV_H
