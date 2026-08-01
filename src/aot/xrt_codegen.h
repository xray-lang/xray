/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_codegen.h - Provider adapters for semantic-neutral code-shape controls.
 */

#ifndef XRT_CODEGEN_H
#define XRT_CODEGEN_H

#include <stdint.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#endif

static inline int64_t xrt_codegen_opaque_i64(int64_t value) {
#if defined(_MSC_VER)
    volatile int64_t opaque = value;
    _ReadWriteBarrier();
    return opaque;
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : "+r"(value));
#else
#error "selected provider cannot realize codegen.opaque(int)"
#endif
    return value;
}

static inline uint64_t xrt_codegen_opaque_u64(uint64_t value) {
#if defined(_MSC_VER)
    volatile uint64_t opaque = value;
    _ReadWriteBarrier();
    return opaque;
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : "+r"(value));
#else
#error "selected provider cannot realize codegen.opaque(uint)"
#endif
    return value;
}

static inline void *xrt_codegen_opaque_ptr(void *value) {
#if defined(_MSC_VER)
    void *volatile opaque = value;
    _ReadWriteBarrier();
    return opaque;
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : "+r"(value));
#else
#error "selected provider cannot realize codegen.opaque(Ptr)"
#endif
    return value;
}

static inline const void *xrt_codegen_opaque_const_ptr(const void *value) {
#if defined(_MSC_VER)
    const void *volatile opaque = value;
    _ReadWriteBarrier();
    return opaque;
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : "+r"(value));
#else
#error "selected provider cannot realize codegen.opaque(const Ptr)"
#endif
    return value;
}

static inline void xrt_codegen_compiler_fence(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#else
#error "selected provider cannot realize codegen.compilerFence()"
#endif
}

#endif /* XRT_CODEGEN_H */
