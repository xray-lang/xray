/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_array.h - Shared Array prefix and scalar-family operations
 *
 * KEY CONCEPT:
 *   The admitted type supplies the tail layout. Every owned field of every
 *   capacity row holds a real Header owner or NULL, independent of length.
 */
#ifndef XRAY_NATIVE_ARRAY_H
#define XRAY_NATIVE_ARRAY_H
#include "xray_native_string_value.h"
#include "xray_native_place.h"
#include "xray_native_value_image.h"

typedef enum XrtBackingArrayKind {
    XRT_BACKING_ARRAY_I64, XRT_BACKING_ARRAY_STRING,
    XRT_BACKING_ARRAY_U8, XRT_BACKING_ARRAY_U64, XRT_BACKING_ARRAY_F64, XRT_BACKING_ARRAY_COUNT
} XrtBackingArrayKind;

static inline XrtBackingArrayKind xrt_backing_array_kind_from_value(XrtValueKind kind) {
    if (kind == XRT_VALUE_ARRAY_F64) return XRT_BACKING_ARRAY_F64;
    return xrt_value_kind_is_scalar_array(kind) ? (XrtBackingArrayKind)(kind - XRT_VALUE_ARRAY_I64) :
        XRT_BACKING_ARRAY_COUNT;
}
static inline XrtValueKind xrt_backing_array_value_kind(XrtBackingArrayKind kind) {
    if (kind == XRT_BACKING_ARRAY_F64) return XRT_VALUE_ARRAY_F64;
    return (unsigned)kind < XRT_BACKING_ARRAY_COUNT ? (XrtValueKind)(XRT_VALUE_ARRAY_I64 + kind) : XRT_VALUE_UNIT;
}
typedef struct XrtArrayItem {
    XrtBackingArrayKind kind;
    union { int64_t i64; XrtStringBacking *string; uint8_t u8; uint64_t u64; double f64; } value;
} XrtArrayItem;
typedef enum XrtArrayUpdateKind {
    XRT_ARRAY_UPDATE_SET, XRT_ARRAY_UPDATE_PUSH,
    /* Fill the logical elements of Array<u8> from the system CSPRNG.
     * Use index zero and item.kind U8. The item value is unused. */
    XRT_ARRAY_UPDATE_RANDOM_BYTES
} XrtArrayUpdateKind;
typedef struct XrtArrayUpdate {
    XrtArrayUpdateKind operation;
    int64_t index;
    XrtArrayItem item;
} XrtArrayUpdate;
typedef struct XrtArrayInput {
    XrtBackingArrayKind kind;
    const void *values;
    size_t count;
} XrtArrayInput;
typedef struct XrtArrayBacking XrtArrayBacking;
#ifndef __cplusplus
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
struct XrtArrayBacking {
    XrtManagedHeader managed;
    size_t length, capacity;
    unsigned char elements[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(offsetof(XrtArrayBacking, length) == 16 && offsetof(XrtArrayBacking, capacity) == 24 &&
    offsetof(XrtArrayBacking, elements) == 32 && sizeof(XrtArrayBacking) == 32 &&
    32 % _Alignof(uint64_t) == 0 && 32 % _Alignof(double) == 0 && 32 % _Alignof(XrtManagedHeader *) == 0,
    "every Array tail begins at the same aligned physical prefix");
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* These receivers consume compiler-admitted live borrows of the exact kind.
 * They neither validate arbitrary C pointers nor grant mutation authority.
 * Input values have their exact scalar array type, or XrtStringBacking *const[].
 * Sources and RHS borrows remain live until return. Construction destinations
 * are true-empty stable frame slots owned by the active execution. Failures
 * leave the kernel guard before entering the execution's nonlocal recovery. */
XRAY_API void xrt_native_array_create(XrtManagedFrame *, size_t slot,
    const XrtArrayInput *, uint32_t source_id);
/* Generated callers provide the same one-based Array type id used by their
 * receiving homes. Row bytes and owners stay live until construction returns.
 * A successful read borrows the row while the caller keeps the Array live. */
XRAY_API void xrt_native_array_inline_create(XrtManagedFrame *, size_t slot,
    const XrtValueImage *, uint32_t type, const void *rows, size_t count, uint32_t source_id);
/* Allocate the actual zero-capacity backing in a private empty slot of the
 * active execution. An error leaves the destination unchanged and does not
 * transfer control. Inline arrays use the receiver's exact image binding. */
XRAY_API XrtManagedStatus xrt_native_array_empty_checked(XrtManagedFrame *, size_t slot,
    XrtBackingArrayKind kind);
XRAY_API XrtManagedStatus xrt_native_array_inline_empty_checked(XrtManagedFrame *, size_t slot,
    const XrtValueImage *, uint32_t type);
/* Append through a verified local binding in the active execution frame. */
XRAY_API void xrt_native_array_inline_class_append(XrtPlace, size_t temporary,
    const XrtValueImage *, uint32_t type, XrtManagedHeader *item, uint32_t source_id);
XRAY_API const void *xrt_native_array_inline_at(const XrtManagedHeader *, const XrtValueImage *,
    uint32_t type, int64_t index, uint32_t source_id);
XRAY_API void xrt_native_array_repeat(XrtManagedFrame *, size_t slot,
    const XrtArrayItem *, int64_t length, uint32_t source_id);
/* Construct an empty Array whose allocated tail holds `capacity` rows. The
 * receiver keeps every capacity row initialized, so no element owner exists
 * before the first append. A negative or oversized capacity is a failure, not
 * an empty Array. */
XRAY_API void xrt_native_array_with_capacity(XrtManagedFrame *, size_t slot,
    XrtBackingArrayKind, int64_t capacity, uint32_t source_id);
XRAY_API void xrt_native_string_copy_bytes(XrtManagedFrame *, size_t slot, const XrtStringBacking *, uint32_t source_id);
XRAY_API int64_t xrt_native_array_capacity(const XrtManagedHeader *, XrtBackingArrayKind, uint32_t source_id);
/* Fill an empty temporary owner. The caller publishes to the checked place
 * only after receiving the complete candidate. */
XRAY_API void xrt_native_array_reserve(XrtManagedFrame *, size_t slot, const XrtManagedHeader *,
    XrtBackingArrayKind, int64_t capacity, uint32_t source_id);
/* Borrow two admitted Array<u8> values and compare their logical byte spans.
 * Failure leaves the output unchanged. No allocation or binding change. */
XRAY_API XrtManagedStatus xrt_native_array_timing_equal(const XrtManagedHeader *,
    const XrtManagedHeader *, bool *);
/* String GET returns a synchronous borrow, not a new owner. */
XRAY_API void xrt_native_array_get(const XrtManagedHeader *, XrtBackingArrayKind,
    int64_t index, XrtArrayItem *output, uint32_t source_id);
/* Keep the Array live while its String is borrowed and retained. Publish the
 * new owner in the caller's temporary slot, preserving the slot on failure. */
XRAY_API void xrt_native_array_string_read_copy(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *, int64_t index, uint32_t source_id);
XRAY_API void xrt_native_array_display(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *, XrtBackingArrayKind, uint32_t source_id);
/* binding_kind is supplied independently by the binding's exact descriptor.
 * Its REPLACE guard proves readiness and the caller's existing authority.
 * The workspace is distinct and true-empty; no source retain forces COW. */
XRAY_API void xrt_native_array_update_place(XrtPlace, XrtBackingArrayKind binding_kind,
    XrtManagedFrame *, size_t workspace, const XrtArrayUpdate *, uint32_t source_id);
/* Only generated code with an admitted local capacity window can call this
 * receiver. It may move the backing when growing. All old backing borrows have
 * ended, and the binding and empty workspace belong to the same live Frame. */
XRAY_API void xrt_native_array_push_local_i64(XrtPlace, size_t workspace, int64_t item, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_ARRAY_H
