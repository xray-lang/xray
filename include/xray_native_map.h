/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_map.h - Fixed String/I64 Map storage with real key owners
 *
 * KEY CONCEPT:
 *   Each capacity row owns one key or NULL. Bucket, order and rank indexes
 *   encode lookup and insertion order without another ownership relation.
 */
#ifndef XRAY_NATIVE_MAP_H
#define XRAY_NATIVE_MAP_H
#include "xray_native_string_value.h"
typedef struct XrtMapBackingRow XrtMapBackingRow;
typedef struct XrtMapBacking XrtMapBacking;
#ifndef __cplusplus
struct XrtMapBackingRow {
    XrtStringBacking *key;
    int64_t value;
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
    sizeof(XrtMapBacking) == 40 && sizeof(XrtMapBackingRow) == 56 && offsetof(XrtMapBackingRow, key) == 0,
    "Map has a fixed prefix and one owner in each repeated row");
#endif
#endif // XRAY_NATIVE_MAP_H
