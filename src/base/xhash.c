/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhash.c - Pure hash functions implementation (no XrValue dependency)
 */

#include "xhash.h"
#include "xchecks.h"
#include <string.h>
#include <math.h>

// xr_hash_bytes() and xr_hash_bytes64() are now inline in xhash.h

// ============================================================================
// Primitive type hash functions
// ============================================================================

uint32_t xr_hash_int(int64_t val) {
    return xr_hash_bytes((const uint8_t *)&val, sizeof(val));
}

uint32_t xr_hash_float(double val) {
    // Normalize special cases to canonical bit patterns
    if (val == 0.0) val = 0.0;   // 0.0 == -0.0
    if (isnan(val)) return 0x7FC00001;  // All NaN equal, non-zero constant
    
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    uint32_t hash = xr_hash_bytes((const uint8_t *)&bits, sizeof(bits));
    return hash == 0 ? 1 : hash;
}

uint32_t xr_hash_bool(int val) {
    return val ? 5 : 4;
}

uint8_t xr_short_hash(uint32_t hash) {
    return (uint8_t)((hash >> 25) | XR_SHORT_HASH_VALID);
}

