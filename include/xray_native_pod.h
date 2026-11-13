/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_pod.h - Exact inline data layouts and synchronous display
 */
#ifndef XRAY_NATIVE_POD_H
#define XRAY_NATIVE_POD_H
#include "xray_native_runtime.h"
#include <stdbool.h>

typedef enum XrPodScalar {
    XR_POD_UNIT, XR_POD_BOOL,
    XR_POD_I8, XR_POD_I16, XR_POD_I32, XR_POD_I64,
    XR_POD_U8, XR_POD_U16, XR_POD_U32, XR_POD_U64,
    XR_POD_F32, XR_POD_F64, XR_POD_SCALAR_COUNT
} XrPodScalar;
typedef struct XrPodLayout { uint32_t size, alignment, scalar_count; } XrPodLayout;
typedef enum XrPodTypeKind { XR_POD_FIXED_U8 = 1, XR_POD_NULLABLE, XR_POD_TUPLE_I64 } XrPodTypeKind;
typedef struct XrPodType {
    XrPodTypeKind kind;
    uint32_t count;
    /* Scalar IDs precede constructed ordinals at XR_POD_SCALAR_COUNT. */
    uint32_t element;
    XrPodLayout physical;
} XrPodType;

static inline bool xr_pod_scalar_layout(XrPodScalar scalar, XrPodLayout *output) {
    static const uint8_t sizes[] = {0, 1, 1, 2, 4, 8, 1, 2, 4, 8, 4, 8};
    if (!output || (uint32_t)scalar >= XR_POD_SCALAR_COUNT) return false;
    uint32_t size = sizes[scalar];
    XrPodLayout layout = {size, size ? size : 1, size != 0};
    *output = layout;
    return true;
}
static inline bool xr_pod_nullable_layout(XrPodLayout payload, XrPodLayout *output) {
    if (!output || !payload.alignment || payload.alignment > 8 ||
        (payload.alignment & (payload.alignment - 1)) || payload.scalar_count == UINT32_MAX) return false;
    uint64_t size = (uint64_t)payload.alignment + payload.size;
    size = (size + payload.alignment - 1) & ~(uint64_t)(payload.alignment - 1);
    if (size > UINT32_MAX) return false;
    XrPodLayout layout = {(uint32_t)size, payload.alignment, payload.scalar_count + 1};
    *output = layout;
    return true;
}
static inline bool xr_pod_tuple_i64_layout(uint32_t count, XrPodLayout *output) {
    if (!output || !count || count > UINT32_MAX / 8) return false;
    *output = (XrPodLayout){count * 8, 8, count};
    return true;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Synchronously borrows an admitted receiver-owned catalog or compiler-emitted
 * immutable catalog and its exact value span. Construction/admission establishes
 * complete shape and display qualification. This call validates the span and
 * dynamic data, then publishes one independently owned String. */
XRAY_API XrtString *xrt_pod_display(const XrPodType *types, uint32_t count,
    uint32_t type, const void *bytes, size_t size, uint32_t source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_POD_H
