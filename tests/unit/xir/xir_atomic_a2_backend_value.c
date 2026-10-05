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
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure) \
    atomic_backend_cell_cas(cell,expected,desired,success,failure)
#include "xir/xxir_value.c"
