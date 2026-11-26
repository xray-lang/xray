/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmax_align.h - One fundamental allocator alignment on every C provider
 *
 * KEY CONCEPT:
 *   Compiler-specific C library spelling does not change allocation alignment.
 */
#ifndef XMAX_ALIGN_H
#define XMAX_ALIGN_H
#include "xplatform.h"
#include <stddef.h>
#include <stdint.h>

/* Microsoft's C headers omit max_align_t; these scalar types cover its
 * fundamental alignments without depending on an OS allocation API. */
#if defined(XR_COMPILER_MSVC)
typedef union XrMaxAlignment {
    long double floating;
    uint64_t integer;
    void *pointer;
} XrMaxAlignment;
#else
typedef max_align_t XrMaxAlignment;
#endif

/* An explicit alignment promise can exceed what one plain allocation returns.
 * The managed allocator honours requests up to this limit by over-allocating a
 * block and remembering the provider pointer in front of the returned address;
 * everything above the fundamental alignment therefore needs no new provider
 * entry point. The language-side admission limit and this runtime limit are the
 * same number, so a declaration the compiler admits is one the runtime can
 * physically place. */
#define XR_ALIGNMENT_LIMIT 128u
/* Zero is an absent promise. Exponents that cannot be represented must
 * exceed the admission limit, including those from untrusted artifacts. */
static inline uint32_t xr_decode_alignment(uint32_t encoded) {
    if (!encoded) return 0;
    return encoded <= 32 ? UINT32_C(1) << (encoded - 1) : UINT32_MAX;
}
#endif // XMAX_ALIGN_H
