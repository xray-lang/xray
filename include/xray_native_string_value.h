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
/* All three slots belong to one stable Frame and are distinct. The source is
 * an owned String consumed on success, the result can hold an old owner, and
 * the temporary is truly empty. Suffix is borrowed throughout the call and may
 * alias either String. These coordinates carry no mutation permission. */
typedef struct XrtStringAppendOwned {
    size_t source_slot, result_slot, temporary_slot;
    const XrtStringBacking *suffix;
} XrtStringAppendOwned;
#ifndef __cplusplus
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
struct XrtStringBacking {
    XrtManagedHeader managed;
    size_t length, runes, capacity;
    _Atomic(uint64_t) cached_hash;
    char bytes[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(sizeof(size_t) == 8 && offsetof(XrtStringBacking, length) == 16 &&
    offsetof(XrtStringBacking, runes) == 24 && offsetof(XrtStringBacking, capacity) == 32 &&
    offsetof(XrtStringBacking, cached_hash) == 40 && offsetof(XrtStringBacking, bytes) == 48 &&
    sizeof(XrtStringBacking) == 48, "String backing has a fixed forty-eight-byte prefix");
#endif
#endif // XRAY_NATIVE_STRING_VALUE_H
