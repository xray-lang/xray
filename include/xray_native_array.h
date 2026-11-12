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
#include "xray_native_state.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrtArray XrtArray;

/* A synchronous borrow names the live binding. It owns neither its frame nor
 * its module state and cannot be retained, stored or returned by guest code. */
typedef struct XrtArrayRef {
    XrtArray **cell;
    XrtModuleAccess state;
} XrtArrayRef;
XRAY_API XrtArrayRef xrt_array_ref_local(XrtArray **, XrtModuleTypeKind, uint32_t source_id);
XRAY_API XrtArrayRef xrt_array_ref_state(XrtModuleRef *, XrtModuleTypeKind, uint32_t source_id);
XRAY_API XrtArray *xrt_array_ref_read(XrtArrayRef, uint32_t source_id);
XRAY_API void xrt_array_ref_write(XrtArrayRef, const XrtArray *, uint32_t source_id);
XRAY_API void xrt_array_ref_i64_set(XrtArrayRef, int64_t index, int64_t value, uint32_t source_id);
XRAY_API void xrt_array_ref_string_set(XrtArrayRef, int64_t index, XrtString *, uint32_t source_id);
XRAY_API void xrt_array_ref_u8_set(XrtArrayRef, int64_t index, uint8_t value, uint32_t source_id);
XRAY_API void xrt_array_ref_u64_set(XrtArrayRef, int64_t index, uint64_t value, uint32_t source_id);
XRAY_API void xrt_array_ref_i64_push(XrtArrayRef, int64_t value, uint32_t source_id);
XRAY_API void xrt_array_ref_string_push(XrtArrayRef, XrtString *, uint32_t source_id);
XRAY_API void xrt_array_ref_u8_push(XrtArrayRef, uint8_t value, uint32_t source_id);
XRAY_API void xrt_array_ref_u64_push(XrtArrayRef, uint64_t value, uint32_t source_id);

XRAY_API XrtArray *xrt_array_i64_new(const int64_t *values, size_t count, uint32_t source_id);
XRAY_API XrtArray *xrt_array_string_new(XrtString *const *values, size_t count, uint32_t source_id);
XRAY_API XrtArray *xrt_array_i64_repeat(int64_t length, int64_t fill, uint32_t source_id);
XRAY_API XrtArray *xrt_array_string_repeat(int64_t length, XrtString *fill, uint32_t source_id);
XRAY_API XrtArray *xrt_array_u8_new(const uint8_t *values, size_t count, uint32_t source_id);
XRAY_API XrtArray *xrt_array_u64_new(const uint64_t *values, size_t count, uint32_t source_id);
XRAY_API XrtArray *xrt_array_u8_repeat(int64_t length, uint8_t fill, uint32_t source_id);
XRAY_API XrtArray *xrt_array_u64_repeat(int64_t length, uint64_t fill, uint32_t source_id);
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
XRAY_API uint8_t xrt_array_u8_get(const XrtArray *, int64_t index, uint32_t source_id);
XRAY_API uint64_t xrt_array_u64_get(const XrtArray *, int64_t index, uint32_t source_id);
/* Display borrows the exact Array family and returns one current String owner. */
XRAY_API XrtString *xrt_array_i64_display(const XrtArray *, uint32_t source_id);
XRAY_API XrtString *xrt_array_string_display(const XrtArray *, uint32_t source_id);
XRAY_API XrtString *xrt_array_u8_display(const XrtArray *, uint32_t source_id);
XRAY_API XrtString *xrt_array_u64_display(const XrtArray *, uint32_t source_id);
/* The binding is loaded after all arguments have been evaluated. A failure
 * preserves that current binding; RHS effects already performed remain. */
XRAY_API void xrt_array_i64_set(XrtArray **, int64_t index, int64_t value, uint32_t source_id);
XRAY_API void xrt_array_string_set(XrtArray **, int64_t index, XrtString *, uint32_t source_id);
XRAY_API void xrt_array_i64_push(XrtArray **, int64_t value, uint32_t source_id);
XRAY_API void xrt_array_string_push(XrtArray **, XrtString *, uint32_t source_id);
XRAY_API void xrt_array_u8_set(XrtArray **, int64_t index, uint8_t value, uint32_t source_id);
XRAY_API void xrt_array_u64_set(XrtArray **, int64_t index, uint64_t value, uint32_t source_id);
XRAY_API void xrt_array_u8_push(XrtArray **, uint8_t value, uint32_t source_id);
XRAY_API void xrt_array_u64_push(XrtArray **, uint64_t value, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_ARRAY_H
