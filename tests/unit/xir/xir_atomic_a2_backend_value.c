/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_a2_backend_value.c - Bounded backend retry and physical ownership tests
 *
 * KEY CONCEPT:
 *   Real production bodies retain captured operands across exactly one CAS per poll.
 */
#include "xir_atomic_a2_backend_hooks.h"
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) atomic_backend_allocate(bytes)
#define xr_free(pointer) atomic_backend_free(pointer)
#undef xr_calloc
#define xr_calloc(count,bytes) atomic_backend_calloc(count,bytes)
static bool atomic_backend_bool_cas(_Atomic(bool) *cell, bool *expected, bool desired,
    memory_order success, memory_order failure) {
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
static bool atomic_backend_u32_cas(_Atomic(uint32_t) *cell, uint32_t *expected, uint32_t desired,
    memory_order success, memory_order failure) {
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
static bool atomic_backend_i64_cas(_Atomic(int64_t) *cell, int64_t *expected, int64_t desired,
    memory_order success, memory_order failure) {
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure) \
    _Generic((cell), _Atomic(bool) *: atomic_backend_bool_cas, _Atomic(uint32_t) *: atomic_backend_u32_cas, \
        _Atomic(int64_t) *: atomic_backend_i64_cas, _Atomic(uint64_t) *: atomic_backend_cell_cas)(cell,expected,desired,success,failure)
#include "xir/xxir_value.c"
