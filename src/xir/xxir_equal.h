/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_equal.h - Bounded typed value comparison without ownership transfer
 *
 * KEY CONCEPT:
 *   A boolean result is published only after a successful typed comparison.
 */
#ifndef XXIR_EQUAL_H
#define XXIR_EQUAL_H
#include "xxir_value.h"

/* Inputs remain borrowed. Failure preserves output and refunds owned scratch. */
XR_FUNC XrXirValueStatus xr_xir_value_equal(const XrXirValue *left,
    const XrXirValue *right, XrXirType type,
    XrXirValueAdmission *admission, bool *output);
#endif // XXIR_EQUAL_H
