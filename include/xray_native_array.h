/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_array.h - Typed logical Array values on the canonical runtime
 */
#ifndef XRAY_NATIVE_ARRAY_H
#define XRAY_NATIVE_ARRAY_H
#include "xray_native_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrtArray XrtArray;

XRAY_API XrtArray *xrt_array_i64_new(const int64_t *values, size_t count, uint32_t source_id);
XRAY_API XrtArray *xrt_array_string_new(XrtString *const *values, size_t count, uint32_t source_id);
/* A retain creates one independent logical owner and one real canonical ref.
 * Selected SSA edges and returns transfer their existing owner unchanged. */
XRAY_API XrtArray *xrt_array_retain(const XrtArray *, uint32_t source_id);
XRAY_API void xrt_array_drop(XrtArray **);
/* An independently held result can retire after its execution root ends. */
XRAY_API void xrt_array_owned_drop(XrtArray **);
XRAY_API void xrt_array_assign(XrtArray **destination, const XrtArray *, uint32_t source_id);
XRAY_API int64_t xrt_array_length(const XrtArray *, uint32_t source_id);
XRAY_API int64_t xrt_array_i64_get(const XrtArray *, int64_t index, uint32_t source_id);
XRAY_API XrtString *xrt_array_string_get(const XrtArray *, int64_t index, uint32_t source_id);
/* The binding is loaded after all arguments have been evaluated. A failure
 * preserves that current binding; RHS effects already performed remain. */
XRAY_API void xrt_array_i64_set(XrtArray **, int64_t index, int64_t value, uint32_t source_id);
XRAY_API void xrt_array_string_set(XrtArray **, int64_t index, XrtString *, uint32_t source_id);
XRAY_API void xrt_array_i64_push(XrtArray **, int64_t value, uint32_t source_id);
XRAY_API void xrt_array_string_push(XrtArray **, XrtString *, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_ARRAY_H
