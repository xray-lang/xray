/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_hash.h - XrValue-aware hash functions
 *
 * KEY CONCEPT:
 *   Hash functions for XrValue types, used by Map/Set.
 *   Built on top of xhash.h primitive hash functions.
 *
 * RELATED MODULES:
 *   - xhash.h: Pure hash functions (no XrValue dependency)
 *   - xmap.h: Runtime Map object (uses this)
 *   - xset.h: Runtime Set object (uses this)
 */

#ifndef XVALUE_HASH_H
#define XVALUE_HASH_H

#include "xvalue.h"
#include "../../base/xhash.h"

// Forward declaration
typedef struct XrString XrString;

// Hash never returns 0 (0 is used for tombstone)
XR_FUNC uint32_t xr_hash_value(XrValue val);
XR_FUNC uint32_t xr_hash_string(XrString *str);

// Shallow value equality: primitives by value, strings by content, objects by pointer
XR_FUNC bool xr_value_eq(XrValue a, XrValue b);

#endif // XVALUE_HASH_H
