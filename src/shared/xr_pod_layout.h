/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_pod_layout.h - Checked physical layouts for inline data values
 */
#ifndef XR_POD_LAYOUT_H
#define XR_POD_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>

typedef enum XrPodScalar {
    XR_POD_UNIT, XR_POD_BOOL,
    XR_POD_I8, XR_POD_I16, XR_POD_I32, XR_POD_I64,
    XR_POD_U8, XR_POD_U16, XR_POD_U32, XR_POD_U64,
    XR_POD_F32, XR_POD_F64, XR_POD_SCALAR_COUNT
} XrPodScalar;

typedef struct XrPodLayout { uint32_t size, alignment, scalar_count; } XrPodLayout;

static inline bool xr_pod_scalar_layout(XrPodScalar scalar, XrPodLayout *output) {
    static const uint8_t sizes[] = {0, 1, 1, 2, 4, 8, 1, 2, 4, 8, 4, 8};
    if (!output || (uint32_t)scalar >= XR_POD_SCALAR_COUNT) return false;
    uint32_t size = sizes[scalar];
    *output = (XrPodLayout){size, size ? size : 1, size != 0};
    return true;
}

static inline bool xr_pod_nullable_layout(XrPodLayout payload, XrPodLayout *output) {
    if (!output || !payload.alignment || payload.alignment > 8 ||
        (payload.alignment & (payload.alignment - 1)) || payload.scalar_count == UINT32_MAX) return false;
    uint64_t size = (uint64_t)payload.alignment + payload.size;
    size = (size + payload.alignment - 1) & ~(uint64_t)(payload.alignment - 1);
    if (size > UINT32_MAX) return false;
    *output = (XrPodLayout){(uint32_t)size, payload.alignment, payload.scalar_count + 1};
    return true;
}
#endif // XR_POD_LAYOUT_H
