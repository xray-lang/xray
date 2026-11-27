/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_map.h - Fixed String-key Map storage with typed row owners
 *
 * KEY CONCEPT:
 *   Each row owns its key. String-valued maps also own the value. Empty rows
 *   have null owners. Bucket, order and rank indexes are scalars.
 */
#ifndef XRAY_NATIVE_MAP_H
#define XRAY_NATIVE_MAP_H
#include "xray_native_string_value.h"
#include "xray_native_place.h"

typedef enum XrtMapUpdateKind {
    XRT_MAP_UPDATE_SET, XRT_MAP_UPDATE_REMOVE, XRT_MAP_UPDATE_CLEAR
} XrtMapUpdateKind;

typedef struct XrtMapUpdate {
    XrtMapUpdateKind operation;
    const XrtStringBacking *key;
    int64_t value;
} XrtMapUpdate;

typedef struct XrtMapInput {
    XrtStringBacking *const *keys;
    const int64_t *values;
    size_t count;
} XrtMapInput;
typedef struct XrtMapBackingRow XrtMapBackingRow;
typedef struct XrtMapBacking XrtMapBacking;
typedef struct XrtMapLiteralOrigin { uint32_t kind, line, column; } XrtMapLiteralOrigin;
typedef struct XrtMapLiteral {
    const char *path;
    size_t path_length;
    const XrtMapLiteralOrigin *items;
    size_t count;
} XrtMapLiteral;
/* Kind zero consumes an explicit key/value; kind one consumes a spread Map.
 * Inputs and provenance remain live and unchanged through the synchronous call. */
typedef union XrtMapLiteralSource {
    const XrtStringBacking *key;
    const XrtMapBacking *spread;
} XrtMapLiteralSource;
typedef struct XrtMapLiteralInput {
    XrtMapLiteralSource source;
    int64_t value;
} XrtMapLiteralInput;
typedef struct XrtMapStringLiteralInput {
    XrtMapLiteralSource source;
    XrtStringBacking *value;
} XrtMapStringLiteralInput;
typedef struct XrtMapLiteralDuplicate { size_t first, second; } XrtMapLiteralDuplicate;
#ifndef __cplusplus
struct XrtMapBackingRow {
    XrtStringBacking *key;
    union { int64_t value; XrtStringBacking *string_value; };
    uint64_t hash;
    size_t entry_order, bucket, order, rank;
};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
struct XrtMapBacking {
    XrtManagedHeader managed;
    size_t length, capacity, order_count;
    XrtMapBackingRow rows[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(offsetof(XrtMapBacking, length) == 16 && offsetof(XrtMapBacking, capacity) == 24 &&
    offsetof(XrtMapBacking, order_count) == 32 && offsetof(XrtMapBacking, rows) == 40 &&
    sizeof(XrtMapBacking) == 40 && sizeof(XrtMapBackingRow) == 56 && offsetof(XrtMapBackingRow, key) == 0 &&
    offsetof(XrtMapBackingRow, string_value) == 8,
    "Map has a fixed prefix and fixed offsets for each potential row owner");
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Receivers consume compiler-admitted live String/Map borrows from the active
 * execution. They do not validate arbitrary headers or grant mutation rights.
 * Constructors receive a true-empty stable slot. GET copies a scalar result;
 * INDEX preserves the missing-key typed panic. All borrows survive the call. */
XRAY_API void xrt_native_map_create(XrtManagedFrame *, size_t slot,
    const XrtMapInput *, uint32_t source_id);
/* Literal inputs keep explicit provenance through spreads. A duplicate leaves
 * the constructor transaction before creating the full owned E0434 message. */
XRAY_API void xrt_native_map_literal_create(XrtManagedFrame *, size_t slot,
    const XrtMapLiteral *, const XrtMapLiteralInput *, uint32_t source_id);
XRAY_API void xrt_native_map_string_literal_create(XrtManagedFrame *, size_t slot,
    const XrtMapLiteral *, const XrtMapStringLiteralInput *, uint32_t source_id);
typedef struct XrtNativeMapStringGet {
    XrtManagedFrame *frame;
    size_t destination, temporary, offset, temporary_owner_offset;
    const XrtManagedHeader *map;
    const XrtStringBacking *key;
} XrtNativeMapStringGet;
XRAY_API void xrt_native_map_string_get(const XrtNativeMapStringGet *, uint32_t source_id);
XRAY_API bool xrt_native_map_get(const XrtManagedHeader *, const XrtStringBacking *,
    int64_t *value, uint32_t source_id);
typedef enum XrtMapProjection {
    XRT_MAP_KEYS, XRT_MAP_VALUES, XRT_MAP_DISPLAY,
} XrtMapProjection;
XRAY_API void xrt_native_map_project(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *, XrtMapProjection, uint32_t source_id);
XRAY_API int64_t xrt_native_map_index(const XrtManagedHeader *, const XrtStringBacking *, uint32_t source_id);
/* REPLACE checks the actual binding's readiness and existing authority. The
 * distinct workspace is true-empty; failures leave the transaction before
 * entering nonlocal recovery, and removed changes only after success. */
XRAY_API void xrt_native_map_update_place(XrtPlace, XrtManagedFrame *, size_t workspace,
    const XrtMapUpdate *, bool *removed, uint32_t source_id);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_MAP_H
