/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_reduction.h - Classic reduction pattern recognition
 */

#ifndef XI_OPT_REDUCTION_H
#define XI_OPT_REDUCTION_H

#include "xi_pass.h"

typedef enum XiReductionKind {
    XI_REDUCE_NONE = 0,
    XI_REDUCE_SUM,
    XI_REDUCE_PRODUCT,
    XI_REDUCE_MIN,
    XI_REDUCE_MAX,
} XiReductionKind;

XR_FUNC XiPassChange xi_opt_reduction(XiFunc *f);

#endif /* XI_OPT_REDUCTION_H */
