/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_math_runtime.h - Active execution entropy with per-draw accounting
 */
#ifndef XXIR_MATH_RUNTIME_H
#define XXIR_MATH_RUNTIME_H
#include "xxir_math.h"
#include "xxir_call.h"

XR_FUNC XrXirRunStatus xr_xir_call_math_random(XrXirCallView *view,uint32_t operation,
    int64_t minimum,int64_t maximum,int64_t *output);
#endif // XXIR_MATH_RUNTIME_H
