/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array.h - Canonical Array ownership shared by roots and module slots
 */
#ifndef XNATIVE_ARRAY_H
#define XNATIVE_ARRAY_H
#include "xray_native_array.h"
#include "xnative_value.h"
#include "xir/xxir_type_arena.h"
typedef enum XrtArrayElement {
    XRT_ARRAY_I64, XRT_ARRAY_STRING, XRT_ARRAY_U8, XRT_ARRAY_U64, XRT_ARRAY_ELEMENT_COUNT
} XrtArrayElement;

/* The same independently owned closed context serves a layout and its root.
 * Its two structural descriptors never borrow Source or generated-C memory. */
XR_FUNC XrXirValueStatus xrt_array_types_new(uint64_t limit, XrXirTypeArena **, uint64_t *bytes);
XR_FUNC XrXirType xrt_array_type(XrtArrayElement element);
XR_FUNC XrXirValueStatus xrt_array_owned_copy(const XrtArray *, const XrXirTypeArena *,
    XrtArrayElement, XrtArray **empty);
XR_FUNC XrXirValueStatus xrt_view_types_new(uint64_t limit, XrXirTypeArena **, uint64_t *bytes);
XR_FUNC XrtArray *xrt_array_publish_pending(uint32_t source);
XR_FUNC XrtArray *xrt_array_root_take(XrtArray *);
XR_FUNC void xrt_array_root_drain(XrtValueRoot *);
XR_FUNC XrXirValueStatus xrt_value_root_share_types(const XrXirTypeArena *);
#endif // XNATIVE_ARRAY_H
