/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_a2_backend_call.c - Bounded backend retry and physical ownership tests
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
#include "xir/xxir_call.c"
