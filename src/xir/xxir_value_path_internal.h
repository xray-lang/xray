/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_path_internal.h - Owned replacements without writable root access
 */
#ifndef XXIR_VALUE_PATH_INTERNAL_H
#define XXIR_VALUE_PATH_INTERNAL_H
#include "xxir_value_place.h"
/* Build through the ordinary path kernel on an owned private value. Traversal
 * through reference identities is rejected before mutation; replacing such a
 * complete leaf is allowed. Declaration access remains the caller's duty.
 * Failure leaves the source value and empty output unchanged. */
XR_FUNC XrXirValueStatus xir_value_path_replacement(const XrXirValue *,const XrXirValuePath *,const XrXirValue *,
    XrXirValueAdmission *,XrXirValue *,XrXirFaultDetail *);
#endif // XXIR_VALUE_PATH_INTERNAL_H
