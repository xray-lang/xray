/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_view.h - Typed synchronous owners for canonical integer views
 *
 * KEY CONCEPT:
 *   Numeric identities resolve through the actual active execution owner.
 *   They never contain an address of a caller's movable register storage.
 */
#ifndef XRAY_NATIVE_VIEW_H
#define XRAY_NATIVE_VIEW_H
#include "xray_native_array.h"

typedef struct XrtViewFrame { uint64_t identity; } XrtViewFrame;
typedef struct XrtViewCell { uint64_t identity; } XrtViewCell;
typedef struct XrtSliceI64 { uint64_t identity; } XrtSliceI64;
typedef struct XrtViewRange { int64_t start, end; bool open_end; } XrtViewRange;

/* Counts describe owned activation slots, including conditionally initialized
 * values. Empty slot cleanup is valid; it never reads a future SSA word. */
XRAY_API XrtViewFrame xrt_view_frame_enter(uint32_t cells, uint32_t views, uint32_t source);
XRAY_API void xrt_view_frame_leave(XrtViewFrame, uint32_t source);
/* Construction copies its actual Array argument. The unique temporary used
 * inside the runtime is transferred only after all fallible preparation. */
XRAY_API XrtViewCell xrt_view_cell_new(XrtViewFrame, uint32_t slot, const XrtArray *, uint32_t source);
XRAY_API XrtArray *xrt_view_cell_read(XrtViewFrame, XrtViewCell, uint32_t source);
XRAY_API void xrt_view_cell_write(XrtViewFrame, XrtViewCell, const XrtArray *, uint32_t source);
XRAY_API void xrt_view_cell_drop(XrtViewFrame, XrtViewCell, uint32_t source);
XRAY_API int64_t xrt_view_cell_i64_get(XrtViewFrame, XrtViewCell, int64_t index, uint32_t source);
XRAY_API void xrt_view_cell_i64_set(XrtViewFrame, XrtViewCell, int64_t index, int64_t value, uint32_t source);

XRAY_API XrtSliceI64 xrt_slice_i64_new(XrtViewFrame, uint32_t slot,
    XrtViewCell, XrtViewRange, uint32_t source);
XRAY_API XrtSliceI64 xrt_slice_i64_reborrow(XrtViewFrame, uint32_t slot,
    XrtSliceI64, XrtViewRange, uint32_t source);
/* A binding owns its own logical reference to the same actual scope. */
XRAY_API XrtSliceI64 xrt_slice_i64_bind(XrtViewFrame, uint32_t slot, XrtSliceI64, uint32_t source);
XRAY_API int64_t xrt_slice_i64_length(XrtViewFrame, XrtSliceI64, uint32_t source);
XRAY_API int64_t xrt_slice_i64_get(XrtViewFrame, XrtSliceI64, int64_t index, uint32_t source);
XRAY_API XrtArray *xrt_slice_i64_copy(XrtViewFrame, XrtSliceI64, uint32_t source);
XRAY_API void xrt_slice_i64_end(XrtViewFrame, XrtSliceI64, uint32_t source);
XRAY_API void xrt_slice_i64_cleanup(XrtViewFrame, uint32_t slot, uint32_t source);
/* Return transfers one real logical owner to the enclosing admitted call.
 * The receiving binding consumes that pending owner before another result. */
XRAY_API XrtSliceI64 xrt_slice_i64_return(XrtViewFrame, XrtSliceI64, uint32_t source);
#endif // XRAY_NATIVE_VIEW_H
