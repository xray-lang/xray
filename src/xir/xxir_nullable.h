/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nullable.h - Exact typed sums with independently owned metadata
 */
#ifndef XXIR_NULLABLE_H
#define XXIR_NULLABLE_H
#include "xxir_value.h"

/* A NULL payload constructs None; a live exact element constructs Some.
 * Only successful construction publishes output. Both variants pin the arena. */
XR_FUNC XrXirValueStatus xr_xir_nullable_new(XrXirType type,
    const XrXirValue *payload, XrXirValueAdmission *admission, XrXirValue *output);
/* The returned payload is borrowed from value and exists only for Some. */
XR_FUNC bool xr_xir_nullable_view(const XrXirValue *value, bool *some,
    const XrXirValue **payload);
/* Invalid decimal syntax constructs None. Work or allocation failures publish
 * nothing; the exact decimal kernel bills this admission's existing Domain. */
XR_FUNC XrXirValueStatus xr_xir_f64_try_parse(const XrXirValue *text, XrXirType type,
    XrXirValueAdmission *admission, XrXirValue *output);
#endif // XXIR_NULLABLE_H
