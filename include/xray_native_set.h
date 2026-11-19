/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_set.h - Fixed I64 Set values over actual managed bindings
 */
#ifndef XRAY_NATIVE_SET_H
#define XRAY_NATIVE_SET_H
#include "xray_native_place.h"
typedef enum XrtSetUpdateKind {
    XRT_SET_UPDATE_ADD, XRT_SET_UPDATE_REMOVE, XRT_SET_UPDATE_CLEAR
} XrtSetUpdateKind;
typedef struct XrtSetUpdate { XrtSetUpdateKind operation; int64_t key; } XrtSetUpdate;
typedef struct XrtSetInput { const int64_t *values; size_t count; } XrtSetInput;
typedef struct XrtSetBackingRow XrtSetBackingRow;
typedef struct XrtSetBacking XrtSetBacking;
#ifndef __cplusplus
struct XrtSetBackingRow {
    int64_t key;
    uint64_t hash;
    size_t entry_order, bucket, order, rank;
};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
struct XrtSetBacking {
    XrtManagedHeader managed;
    size_t length, capacity, order_count;
    XrtSetBackingRow rows[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(sizeof(XrtSetBacking) == 40 && offsetof(XrtSetBacking,rows) == 40 &&
    offsetof(XrtSetBacking,capacity) == 24 && sizeof(XrtSetBackingRow) == 48,
    "I64 Set has a fixed prefix and scalar insertion-order rows");
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Header and Place arguments must have the exact admitted I64 Set plan.
 * Arbitrary managed Headers and foreign storage are not inputs to this ABI.
 * The receiving boundary proves the value family and access capability. */
/* Inputs are admitted scalar borrows, live and unchanged for this synchronous
 * call. No input descriptor is stored. A constructor receives a true-empty
 * stable slot; the resulting pointer is owned by the ordinary Root protocol. */
XRAY_API void xrt_native_set_create(XrtManagedFrame *, size_t slot,
    const XrtSetInput *, uint32_t source_id);
XRAY_API bool xrt_native_set_contains(const XrtManagedHeader *, int64_t key, uint32_t source_id);
XRAY_API void xrt_native_set_display(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *, uint32_t source_id);
/* The real binding and independent empty workspace enter one replacement
 * transaction. A failure preserves the binding and removed. References decide
 * only whether COW is necessary; they never grant mutation authority. */
XRAY_API void xrt_native_set_update_place(XrtPlace, XrtManagedFrame *, size_t workspace,
    const XrtSetUpdate *, bool *removed, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_SET_H
