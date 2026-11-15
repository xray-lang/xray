/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_array.h - One fixed physical prefix for four admitted Array tails
 *
 * KEY CONCEPT:
 *   The admitted type supplies the element kind. Every String capacity slot is
 *   a real Header owner or NULL, independently of the logical length.
 */
#ifndef XRAY_NATIVE_ARRAY_H
#define XRAY_NATIVE_ARRAY_H
#include "xray_native_string_value.h"
#include "xray_native_place.h"

typedef enum XrtBackingArrayKind {
    XRT_BACKING_ARRAY_I64, XRT_BACKING_ARRAY_STRING,
    XRT_BACKING_ARRAY_U8, XRT_BACKING_ARRAY_U64, XRT_BACKING_ARRAY_COUNT
} XrtBackingArrayKind;
typedef struct XrtArrayItem {
    XrtBackingArrayKind kind;
    union { int64_t i64; XrtStringBacking *string; uint8_t u8; uint64_t u64; } value;
} XrtArrayItem;
typedef enum XrtArrayUpdateKind { XRT_ARRAY_UPDATE_SET, XRT_ARRAY_UPDATE_PUSH } XrtArrayUpdateKind;
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
    32 % _Alignof(uint64_t) == 0 && 32 % _Alignof(XrtManagedHeader *) == 0,
    "all four Array tails share one aligned physical prefix");
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
XRAY_API void xrt_native_array_repeat(XrtManagedFrame *, size_t slot,
    const XrtArrayItem *, int64_t length, uint32_t source_id);
/* String GET returns a synchronous borrow, not a new owner. */
XRAY_API void xrt_native_array_get(const XrtManagedHeader *, XrtBackingArrayKind,
    int64_t index, XrtArrayItem *output, uint32_t source_id);
XRAY_API void xrt_native_array_display(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *, XrtBackingArrayKind, uint32_t source_id);
/* binding_kind is supplied independently by the binding's exact descriptor.
 * Its REPLACE guard proves readiness and the caller's existing authority.
 * The workspace is distinct and true-empty; no source retain forces COW. */
XRAY_API void xrt_native_array_update_place(XrtPlace, XrtBackingArrayKind binding_kind,
    XrtManagedFrame *, size_t workspace, const XrtArrayUpdate *, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_ARRAY_H
