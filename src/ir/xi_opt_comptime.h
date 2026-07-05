/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_comptime.h - Fixpoint driver for compile-time constant simplification
 */

#ifndef XI_OPT_COMPTIME_H
#define XI_OPT_COMPTIME_H

#include "xi_pass.h"

#define XI_COMPTIME_MAX_ROUNDS 8

/* Run const-folding/copy-prop/SCCP/DCE pipeline to a fixed point.
 * This is an IR optimization pass, not the frontend `comptime` evaluator. */
XR_FUNC XiPassChange xi_opt_comptime_eval(XiFunc *f);

#endif /* XI_OPT_COMPTIME_H */
