/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_string_value.h - Fixed UTF-8 storage for admitted typed C reads
 *
 * KEY CONCEPT:
 *   A live String borrow exposes bytes and cached lengths directly. The fixed
 *   physical layout grants neither ownership nor permission to mutate storage.
 */
#ifndef XRAY_NATIVE_STRING_VALUE_H
#define XRAY_NATIVE_STRING_VALUE_H
#include "xray_native_managed.h"

typedef struct XrtStringBacking XrtStringBacking;
#ifndef __cplusplus
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
struct XrtStringBacking {
    XrtManagedHeader managed;
    size_t length, runes, capacity;
    char bytes[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(sizeof(size_t) == 8 && offsetof(XrtStringBacking, length) == 16 &&
    offsetof(XrtStringBacking, runes) == 24 && offsetof(XrtStringBacking, capacity) == 32 &&
    offsetof(XrtStringBacking, bytes) == 40 && sizeof(XrtStringBacking) == 40,
    "String backing has a fixed forty-byte prefix");
#endif
#endif // XRAY_NATIVE_STRING_VALUE_H
