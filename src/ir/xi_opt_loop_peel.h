/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_peel.h - Loop peeling for Xi IR
 */

#ifndef XI_OPT_LOOP_PEEL_H
#define XI_OPT_LOOP_PEEL_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_loop_peel(XiFunc *f);

#endif  // XI_OPT_LOOP_PEEL_H
