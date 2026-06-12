/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_hash.h - Content hash primitives shared by the AOT runtime and the
 * C backend.
 *
 * Single source of truth for string/byte hashing: xrt_value.h uses these
 * for runtime hashing, and xi_cgen.c uses them to precompute literal
 * hashes embedded in generated C. This header is intentionally free of
 * XrValue so the compiler (which carries the VM XrValue) can include it.
 * Changing these functions invalidates every emitted literal hash.
 */

#ifndef XRT_HASH_H
#define XRT_HASH_H

#include <stdint.h>
#include <stddef.h>

static inline uint64_t xrt_hash_mix_u64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static inline uint64_t xrt_hash_bytes(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a 64 */
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t) p[i];
        h *= 1099511628211ull;
    }
    return xrt_hash_mix_u64(h);
}

/* 32-bit content hash stored in xrt_str_t.hash; never returns 0. */
static inline uint32_t xrt_str_hash_bytes(const char *p, size_t n) {
    uint32_t h = (uint32_t) xrt_hash_bytes(p, n);
    return h ? h : 1u;
}

#endif /* XRT_HASH_H */
