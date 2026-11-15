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
#endif // XRAY_NATIVE_ARRAY_H
