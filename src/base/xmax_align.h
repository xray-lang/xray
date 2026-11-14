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
#endif // XMAX_ALIGN_H
