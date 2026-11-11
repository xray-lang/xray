/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string.h - Physical owners of canonical String references
 *
 * KEY CONCEPT:
 *   A root holds one real reference per String its guest owns and counts the
 *   guest references itself, so ending the root releases exactly those.
 */
#ifndef XNATIVE_STRING_H
#define XNATIVE_STRING_H
#include "xnative_value.h"

/* The authenticated producer transfers one current root owner before close.
 * No retained handle into the root bookkeeping is exposed to its receiver. */
XR_FUNC XrtString *xrt_string_root_take(XrtString *string);
/* Produce a real independently lived reference without changing guest counts.
 * A root-private String is escaped through the shared value copy operation. */
XR_FUNC XrXirValueStatus xrt_string_owned_copy(const XrtString *, XrtString **);
#endif // XNATIVE_STRING_H
