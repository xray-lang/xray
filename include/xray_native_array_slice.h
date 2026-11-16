/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_array_slice.h - Borrowed Array windows and separate real keepers
 */
#ifndef XRAY_NATIVE_ARRAY_SLICE_H
#define XRAY_NATIVE_ARRAY_SLICE_H
#include "xray_native_array.h"
#include "xray_native_value_image.h"

/* All three fields are borrowed bytes. The backing has its actual fixed Array
 * prefix and the caller-admitted I64 or U8 kind. Neither this address nor the
 * physical keeper below grants a live loan or permission to change an owner. */
typedef struct XrtReadonlyArraySlice {
    const XrtArrayBacking *backing;
    size_t start, count;
} XrtReadonlyArraySlice;
/* Private Frame storage: only hold is a real owner. The natural Slice plan has
 * no owned leaf. A distinct same-shaped temporary belongs to the same Frame;
 * the common producer supplies both offsets and both actual slot ordinals. */
typedef struct XrtArraySliceBundle {
    XrtReadonlyArraySlice value;
    XrtManagedHeader *hold;
} XrtArraySliceBundle;
#ifndef __cplusplus
_Static_assert(sizeof(XrtReadonlyArraySlice) == 24 && _Alignof(XrtReadonlyArraySlice) == 8 &&
    offsetof(XrtReadonlyArraySlice, backing) == 0 && offsetof(XrtReadonlyArraySlice, start) == 8 &&
    offsetof(XrtReadonlyArraySlice, count) == 16, "readonly Slice uses one natural borrowed layout");
_Static_assert(sizeof(XrtArraySliceBundle) == 32 && _Alignof(XrtArraySliceBundle) == 8 &&
    offsetof(XrtArraySliceBundle, value) == 0 && offsetof(XrtArraySliceBundle, hold) == 24,
    "a private keeper is outside the natural Slice bytes");
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* Cold shared geometry, not a type directory or an owning Slice plan. */
XRAY_API XrtInlineValueSpec xrt_array_slice_bundle_spec(void);
/* The caller has already checked the real source binding and loan permission.
 * It keeps that borrow live without an allocating callback until this call
 * retains into the preallocated true temporary. Full publication precedes old
 * keeper retirement. Rejections preserve the old bundle and empty staging. */
XRAY_API XrtManagedStatus xrt_array_slice_bind(const XrtInlineValueHome *,
    XrtBackingArrayKind, const XrtArrayBacking *, size_t start, size_t count);
XRAY_API XrtManagedStatus xrt_array_slice_reborrow(const XrtInlineValueHome *,
    XrtBackingArrayKind, const XrtReadonlyArraySlice *, size_t start, size_t count);
/* Only a confirmed END/cleanup may clear a bundle. Its zero bytes do not form
 * another readable Slice. Each reader consumes an already admitted live loan;
 * binding/reborrow has checked the backing extent and element geometry. Normal
 * readers check only the dynamic index, never revalidate that cold geometry.
 * They do not query a compiler pool or acquire RC. */
XRAY_API XrtManagedStatus xrt_array_slice_clear(const XrtInlineValueHome *);
XRAY_API XrtManagedStatus xrt_array_slice_length(const XrtReadonlyArraySlice *, int64_t *);
XRAY_API XrtManagedStatus xrt_array_slice_read_bits(const XrtReadonlyArraySlice *,
    XrtBackingArrayKind, int64_t index, uint64_t *);
/* The live Slice's real keeper protects its source through allocating setup
 * and copy callbacks. Destination is an ordinary true-empty Array slot. */
XRAY_API void xrt_native_array_slice_copy(XrtManagedFrame *, size_t,
    XrtBackingArrayKind, const XrtReadonlyArraySlice *, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_ARRAY_SLICE_H
