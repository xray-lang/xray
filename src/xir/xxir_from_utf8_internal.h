/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_from_utf8_internal.h - Strict conversion inside an authenticated live view
 */
#ifndef XXIR_FROM_UTF8_INTERNAL_H
#define XXIR_FROM_UTF8_INTERNAL_H
#include "xxir_view_value_internal.h"
/* The view and its exact origin loan remain live throughout the synchronous
 * copy. BAD_UTF8 alone denotes malformed input; failure preserves Unit output. */
XR_FUNC XrXirValueStatus xir_from_utf8(const XrXirValue *input,
    const XrXirViewAccess *access,XrXirValue *output);
#endif
