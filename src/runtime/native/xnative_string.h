/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string.h - Physical owners of canonical String references
 */
#ifndef XNATIVE_STRING_H
#define XNATIVE_STRING_H
#include "xray_native_runtime.h"
#include "xir/xxir_value.h"

typedef struct XrtStringOwner XrtStringOwner;
typedef struct XrtStringRoot {
    XrXirDomain *domain;
    XrtStringOwner *owners;
    uint64_t byte_limit; // Zero selects the ordinary unbounded runtime policy.
} XrtStringRoot;

XR_FUNC XrtStringRoot *xrt_string_root_current(uint32_t source_id);
XR_FUNC XR_NORET void xrt_value_failure(XrXirValueStatus status, uint32_t source_id);
XR_FUNC void xrt_string_root_drain(XrtStringRoot *root);
/* The authenticated producer transfers one current root owner before close.
 * No retained handle into the root bookkeeping is exposed to its receiver. */
XR_FUNC XrtString *xrt_string_root_take(XrtString *string);
#endif // XNATIVE_STRING_H
