/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_key.h - Finite built-in key equivalence and hashing
 *
 * KEY CONCEPT:
 *   Key equivalence is reflexive even where ordinary floating equality is not.
 *   Kernel support grants no declaration-site Hashable or operation authority.
 */
#ifndef XXIR_KEY_H
#define XXIR_KEY_H
#include "xxir_value.h"
/* Borrowed, same-typed built-in keys retain no owner and allocate no storage.
 * All NaNs share one key; both signed zeros share one key. Strings use all bytes.
 * Hash work is one plus eight scalar bytes or the String byte length. Equality
 * work is one scalar/length decision plus all equal-length String bytes.
 * A current receiving admission Domain is required. Failures preserve output;
 * consumed work is not refunded. Unsupported families confer no false proof. */
XR_FUNC XrXirValueStatus xr_xir_key_hash(const XrXirValue *value,
    XrXirValueAdmission *admission, uint64_t *output);
XR_FUNC XrXirValueStatus xr_xir_key_equal(const XrXirValue *left,
    const XrXirValue *right, XrXirValueAdmission *admission, bool *output);
#endif // XXIR_KEY_H
