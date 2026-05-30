/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_inv_branch.h - Loop-invariant branch hoisting for Xi IR
 */

#ifndef XI_OPT_LOOP_INV_BRANCH_H
#define XI_OPT_LOOP_INV_BRANCH_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_loop_inv_branch(XiFunc *f);

#endif  // XI_OPT_LOOP_INV_BRANCH_H
