/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_array_push_status.h - Runtime-neutral Array.push status contract
 *
 * KEY CONCEPT:
 *   The typed executor and hosted runtime exchange one fail-closed status
 *   vocabulary without making the runtime-only VM archive link object code.
 */

#ifndef XR_ARRAY_PUSH_STATUS_H
#define XR_ARRAY_PUSH_STATUS_H

typedef enum XrArrayPushStatus {
    XR_ARRAY_PUSH_OK = 0,
    XR_ARRAY_PUSH_INVALID_ARRAY,
    XR_ARRAY_PUSH_SLICE,
    XR_ARRAY_PUSH_TYPE_MISMATCH,
    XR_ARRAY_PUSH_ALLOCATION_FAILED,
} XrArrayPushStatus;

#endif  // XR_ARRAY_PUSH_STATUS_H
