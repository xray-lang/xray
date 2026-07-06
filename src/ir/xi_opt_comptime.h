/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_comptime.h - Fixpoint driver for IR constant simplification
 */

#ifndef XI_OPT_COMPTIME_H
#define XI_OPT_COMPTIME_H

#include "xi_pass.h"

#define XI_CONST_FIXPOINT_MAX_ROUNDS 8

/* Run const-folding/copy-prop/SCCP/DCE pipeline to a fixed point.
 * This is an IR optimization pass, not the frontend `comptime` evaluator. */
XR_FUNC XiPassChange xi_opt_const_fixpoint(XiFunc *f);

#endif /* XI_OPT_COMPTIME_H */
