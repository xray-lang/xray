/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhash.h - Pure hash functions (no XrValue dependency)
 *
 * KEY CONCEPT:
 *   Low-level hash utilities for byte arrays and primitive types.
 *   Algorithm: FNV-1a (fast, good distribution, simple).
 *
 * IMPORTANT INVARIANTS:
 *   - Hash never returns 0 (0 reserved for empty/tombstone slots)
 *   - Same value always produces same hash (deterministic)
 *
 * SHORT HASH:
 *   7-bit prefix stored in bucket metadata for fast mismatch filtering.
 *   Avoids full key comparison in ~99% of negative lookups.
 *
 * RELATED MODULES:
 *   - xvalue_hash.h: XrValue-aware hash functions (depends on this)
 */

#ifndef XHASH_H
#define XHASH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "xdefs.h"

// FNV-1a constants (32-bit)
#define XR_FNV_OFFSET_BASIS 2166136261u
#define XR_FNV_PRIME 16777619u
#define XR_SHORT_HASH_VALID 0x80

// FNV-1a constants (64-bit)
#define XR_FNV64_OFFSET_BASIS 14695981039346656037ULL
#define XR_FNV64_PRIME 1099511628211ULL

// ============================================================================
// Generic byte array hash (foundation for all other hash functions)
// Inline for performance - these are hot path functions
// ============================================================================

// Disable unsigned integer overflow sanitizer for hash functions.
// FNV-1a intentionally uses unsigned overflow (well-defined in C as modular arithmetic).
#if defined(__clang__) || defined(__GNUC__)
#define XR_NO_SANITIZE_UNSIGNED __attribute__((no_sanitize("unsigned-integer-overflow")))
#else
#define XR_NO_SANITIZE_UNSIGNED
#endif

// 32-bit FNV-1a hash for byte arrays
XR_NO_SANITIZE_UNSIGNED
static inline uint32_t xr_hash_bytes(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *) data;
    uint32_t hash = XR_FNV_OFFSET_BASIS;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= XR_FNV_PRIME;
    }
    return hash;
}

// 64-bit FNV-1a hash for byte arrays (use for content hashing)
XR_NO_SANITIZE_UNSIGNED
static inline uint64_t xr_hash_bytes64(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *) data;
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= XR_FNV64_PRIME;
    }
    return hash;
}

// ============================================================================
// Primitive type hash functions (no XrValue dependency)
// Inline for performance — these are hot path functions.
// Hash never returns 0 (0 is used for tombstone).
// ============================================================================

// Splitmix64 finalizer: 3 multiply-xorshift ops, much faster than byte-by-byte FNV
XR_NO_SANITIZE_UNSIGNED
static inline uint32_t xr_hash_int(int64_t val) {
    uint64_t x = (uint64_t) val;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    uint32_t h = (uint32_t) (x ^ (x >> 32));
    return h == 0 ? 1 : h;
}

static inline uint32_t xr_hash_float(double val) {
    // Normalize: +0.0 == -0.0, all NaN equal
    if (val == 0.0)
        val = 0.0;
    if (val != val)
        return 0x7FC00001;  // NaN: non-zero constant
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    uint32_t hash = xr_hash_bytes(&bits, sizeof(bits));
    return hash == 0 ? 1 : hash;
}

static inline uint32_t xr_hash_bool(int val) {
    return val ? 5 : 4;
}

// Extract 7-bit prefix for fast key mismatch filtering
static inline uint8_t xr_short_hash(uint32_t hash) {
    return (uint8_t) ((hash >> 25) | XR_SHORT_HASH_VALID);
}

#endif  // XHASH_H
