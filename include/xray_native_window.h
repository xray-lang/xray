/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_window.h - Compiler-matched readonly Slice transport
 */
#ifndef XRAY_NATIVE_WINDOW_H
#define XRAY_NATIVE_WINDOW_H
#include "xray_native_array_slice.h"

/* The compiler and runtime agree on one natural window for both producers.
 * It never grants loan permission. Only the separate Frame hold owns a ref.
 * The installed Array API retains its actual Array pointer and its own type. */
typedef struct XrtReadonlyWindow {
    const XrtManagedHeader *owner;
    size_t start, count;
} XrtReadonlyWindow;
typedef struct XrtReadonlyWindowBundle {
    XrtReadonlyWindow value;
    XrtManagedHeader *hold;
} XrtReadonlyWindowBundle;
#ifndef __cplusplus
_Static_assert(sizeof(XrtReadonlyWindow) == sizeof(XrtReadonlyArraySlice) &&
    _Alignof(XrtReadonlyWindow) == _Alignof(XrtReadonlyArraySlice) &&
    offsetof(XrtReadonlyWindow, owner) == offsetof(XrtReadonlyArraySlice, backing) &&
    offsetof(XrtReadonlyWindow, start) == offsetof(XrtReadonlyArraySlice, start) &&
    offsetof(XrtReadonlyWindow, count) == offsetof(XrtReadonlyArraySlice, count),
    "both natural windows agree on the Frame layout");
_Static_assert(sizeof(XrtReadonlyWindowBundle) == sizeof(XrtArraySliceBundle) &&
    _Alignof(XrtReadonlyWindowBundle) == _Alignof(XrtArraySliceBundle) &&
    offsetof(XrtReadonlyWindowBundle, value) == offsetof(XrtArraySliceBundle, value) &&
    offsetof(XrtReadonlyWindowBundle, hold) == offsetof(XrtArraySliceBundle, hold),
    "both receivers use the same real keeper coordinates");
#endif

#ifdef __cplusplus
extern "C" {
#endif
XRAY_API XrtManagedStatus xrt_window_range_bind(const XrtInlineValueHome *,
    XrtBackingArrayKind, const XrtManagedHeader *, int64_t start, int64_t limit);
XRAY_API XrtManagedStatus xrt_string_bytes_bind(const XrtInlineValueHome *,
    const XrtManagedHeader *);
XRAY_API XrtManagedStatus xrt_window_rebind(const XrtInlineValueHome *,
    XrtBackingArrayKind, const XrtReadonlyWindow *);
XRAY_API XrtManagedStatus xrt_window_range_reborrow(const XrtInlineValueHome *,
    XrtBackingArrayKind, const XrtReadonlyWindow *, int64_t start, int64_t limit, bool open);
XRAY_API XrtManagedStatus xrt_window_length(const XrtReadonlyWindow *, int64_t *);
XRAY_API XrtManagedStatus xrt_window_read_bits(const XrtReadonlyWindow *,
    XrtBackingArrayKind, int64_t index, uint64_t *);
XRAY_API void xrt_native_window_copy(XrtManagedFrame *, size_t,
    XrtBackingArrayKind, const XrtReadonlyWindow *, uint32_t source_id);
XRAY_API XrtManagedStatus xrt_window_strict_utf8(XrtManagedFrame *, size_t destination,
    size_t temporary, size_t offset, size_t temporary_owner_offset,
    const XrtReadonlyWindow *, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_WINDOW_H
