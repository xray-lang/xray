/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_split.h - Loop splitting for Xi IR
 */

#ifndef XI_OPT_LOOP_SPLIT_H
#define XI_OPT_LOOP_SPLIT_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_loop_split(XiFunc *f);

#endif  // XI_OPT_LOOP_SPLIT_H
