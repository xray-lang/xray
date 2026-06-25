/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_hash_core.h - Runtime-neutral content hash primitives
 *
 * These hashes are used by AOT runtime containers and by C generation when
 * precomputing emitted string-literal hashes. Keep the constants stable unless
 * the generated C/literal-hash ABI is intentionally versioned.
 */

#ifndef XR_HASH_CORE_H
#define XR_HASH_CORE_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t xr_hash_core_mix_u64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static inline uint64_t xr_hash_core_bytes(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ull; /* AOT literal-hash ABI seed. */
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t) p[i];
        h *= 1099511628211ull;
    }
    return xr_hash_core_mix_u64(h);
}

static inline uint32_t xr_hash_core_str_hash_bytes(const char *p, size_t n) {
    uint32_t h = (uint32_t) xr_hash_core_bytes(p, n);
    return h ? h : 1u;
}

#endif /* XR_HASH_CORE_H */
