/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_unroll.h - Loop unrolling for Xi IR
 */

#ifndef XI_OPT_LOOP_UNROLL_H
#define XI_OPT_LOOP_UNROLL_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_loop_unroll(XiFunc *f);

#endif  // XI_OPT_LOOP_UNROLL_H
