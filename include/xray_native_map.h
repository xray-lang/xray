/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_map.h - Closed String/I64 Map values and synchronous binding references
 *
 * KEY CONCEPT:
 *   Each value owns one canonical Map reference. Borrowed references name the
 *   actual local, module or captured Cell binding inside its synchronous call.
 */
#ifndef XRAY_NATIVE_MAP_H
#define XRAY_NATIVE_MAP_H
#include "xray_native_runtime.h"
#include "xray_native_state.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrtMap XrtMap;
typedef struct XrtMapRef {
    XrtMap **cell;
    XrtModuleAccess state;
    struct XirCell *captured;
} XrtMapRef;

/* Each result owns one real canonical reference. Parameters borrow their
 * exact String/I64 shape through the synchronous call. */
XRAY_API XrtMap *xrt_map_string_i64_new(XrtString *const *keys, const int64_t *values,
    size_t count, uint32_t source);
XRAY_API XrtMap *xrt_map_retain(const XrtMap *, uint32_t source);
XRAY_API void xrt_map_drop(XrtMap **);
XRAY_API void xrt_map_owned_drop(XrtMap **);
XRAY_API void xrt_map_assign(XrtMap **, const XrtMap *, uint32_t source);
XRAY_API int64_t xrt_map_length(const XrtMap *, uint32_t source);
/* A miss writes zero and returns false. Failure publishes no result. */
XRAY_API bool xrt_map_string_i64_get(const XrtMap *, const XrtString *, int64_t *value, uint32_t source);
XRAY_API int64_t xrt_map_string_i64_index(const XrtMap *, const XrtString *, uint32_t source);
XRAY_API bool xrt_map_string_i64_contains(const XrtMap *, const XrtString *, uint32_t source);
XRAY_API void xrt_map_string_i64_set(XrtMap **, const XrtString *, int64_t value, uint32_t source);
XRAY_API bool xrt_map_string_i64_remove(XrtMap **, const XrtString *, uint32_t source);
XRAY_API void xrt_map_clear(XrtMap **, uint32_t source);

/* References borrow the actual local, module or captured Cell binding, with no retained
 * payload snapshot. They cannot be stored, returned or used after that call. */
XRAY_API XrtMapRef xrt_map_ref_local(XrtMap **, uint32_t source);
XRAY_API XrtMapRef xrt_map_ref_state(XrtModuleRef *, uint32_t source);
XRAY_API XrtMapRef xrt_map_ref_captured(struct XirCell *, uint32_t source);
XRAY_API XrtMap *xrt_map_ref_read(XrtMapRef, uint32_t source);
XRAY_API void xrt_map_ref_write(XrtMapRef, const XrtMap *, uint32_t source);
XRAY_API void xrt_map_ref_string_i64_set(XrtMapRef, const XrtString *, int64_t value, uint32_t source);
XRAY_API bool xrt_map_ref_string_i64_remove(XrtMapRef, const XrtString *, uint32_t source);
XRAY_API void xrt_map_ref_clear(XrtMapRef, uint32_t source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_MAP_H
