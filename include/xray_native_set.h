/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_set.h - Closed Set owners and synchronous binding references
 *
 * KEY CONCEPT:
 *   Local and module references resolve the actual Set binding at each operation.
 */
#ifndef XRAY_NATIVE_SET_H
#define XRAY_NATIVE_SET_H
#include "xray_native_runtime.h"
#include "xray_native_state.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrtSet XrtSet;
typedef struct XrtSetRef {
    XrtSet **cell;
    XrtModuleAccess state;
} XrtSetRef;
XRAY_API XrtSet *xrt_set_i64_new(const int64_t *elements, size_t count, uint32_t source);
XRAY_API XrtSet *xrt_set_retain(const XrtSet *set, uint32_t source);
XRAY_API void xrt_set_drop(XrtSet **set);
XRAY_API void xrt_set_owned_drop(XrtSet **set);
XRAY_API void xrt_set_assign(XrtSet **destination, const XrtSet *value, uint32_t source);
XRAY_API int64_t xrt_set_length(const XrtSet *set, uint32_t source);
XRAY_API bool xrt_set_i64_contains(const XrtSet *set, int64_t value, uint32_t source);
XRAY_API void xrt_set_i64_add(XrtSet **set, int64_t value, uint32_t source);
XRAY_API bool xrt_set_i64_remove(XrtSet **set, int64_t value, uint32_t source);
XRAY_API void xrt_set_clear(XrtSet **set, uint32_t source);
XRAY_API XrtSetRef xrt_set_ref_local(XrtSet **cell, uint32_t source);
XRAY_API XrtSetRef xrt_set_ref_state(XrtModuleRef *state, uint32_t source);
XRAY_API XrtSet *xrt_set_ref_read(XrtSetRef reference, uint32_t source);
XRAY_API void xrt_set_ref_write(XrtSetRef reference, const XrtSet *value, uint32_t source);
XRAY_API void xrt_set_ref_i64_add(XrtSetRef reference, int64_t value, uint32_t source);
XRAY_API bool xrt_set_ref_i64_remove(XrtSetRef reference, int64_t value, uint32_t source);
XRAY_API void xrt_set_ref_clear(XrtSetRef reference, uint32_t source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_SET_H
