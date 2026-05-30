/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_vec_scalar_lower.h - Expand XI_VEC_* ops to scalar Xi IR before codegen
 */

#ifndef XI_VEC_SCALAR_LOWER_H
#define XI_VEC_SCALAR_LOWER_H

#include "xi.h"
#include <stdbool.h>

/* Replace vector ops with scalar INDEX_GET/SET and arithmetic. */
XR_FUNC bool xi_vec_scalar_lower(XiFunc *f);

#endif /* XI_VEC_SCALAR_LOWER_H */
