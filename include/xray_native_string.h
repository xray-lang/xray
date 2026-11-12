/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_string.h - Synchronous mutation of an actual String binding
 */
#ifndef XRAY_NATIVE_STRING_H
#define XRAY_NATIVE_STRING_H
#include "xray_native_state.h"

/* A borrow keeps the caller's local cell or actual module slot. Its lifetime
 * ends with the synchronous call and it never owns the referenced String. */
typedef struct XrtStringRef {
    XrtString **cell;
    XrtModuleAccess state;
} XrtStringRef;
#ifdef __cplusplus
extern "C" {
#endif
XRAY_API XrtStringRef xrt_string_ref_local(XrtString **, uint32_t source);
XRAY_API XrtStringRef xrt_string_ref_state(XrtModuleAccess, uint32_t source);
XRAY_API XrtString *xrt_string_ref_read(XrtStringRef, uint32_t source);
XRAY_API void xrt_string_ref_write(XrtStringRef, const XrtString *, uint32_t source);
XRAY_API void xrt_string_ref_append(XrtStringRef, const XrtString *, uint32_t source);
/* The existing owner is consumed only after successful preparation. Shared
 * guest aliases retain their old value; failed preparation keeps the binding. */
XRAY_API void xrt_string_cell_append(XrtString **, const XrtString *, uint32_t source);
XRAY_API XrtString *xrt_string_append_owned(XrtString *, const XrtString *, uint32_t source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_STRING_H
