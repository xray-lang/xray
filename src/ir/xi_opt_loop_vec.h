/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_vec.h - Simple loop vectorization
 */

#ifndef XI_OPT_LOOP_VEC_H
#define XI_OPT_LOOP_VEC_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_loop_vec(XiFunc *f);

#endif /* XI_OPT_LOOP_VEC_H */
