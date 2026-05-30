/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_vec_cost.h - Vectorization profitability heuristics
 */

#ifndef XI_VEC_COST_H
#define XI_VEC_COST_H

#include "xi_loop.h"
#include <stdint.h>
#include <stdbool.h>

#define XI_VEC_DEFAULT_VF 4
#define XI_VEC_MIN_TRIP 8

/* Return true when vectorizing loop with factor vf is likely profitable. */
XR_FUNC bool xi_vec_loop_profitable(const XiLoop *loop, uint32_t vf);

#endif /* XI_VEC_COST_H */
