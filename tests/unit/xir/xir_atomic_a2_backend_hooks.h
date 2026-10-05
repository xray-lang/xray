/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_a2_backend_hooks.h - Bounded backend retry and physical ownership tests
 *
 * KEY CONCEPT:
 *   Real production bodies retain captured operands across exactly one CAS per poll.
 */
#ifndef XIR_ATOMIC_A2_BACKEND_HOOKS_H
#define XIR_ATOMIC_A2_BACKEND_HOOKS_H
#include "base/xdefs.h"
#include "base/xmalloc.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
XR_FUNC void *atomic_backend_allocate(size_t bytes);
XR_FUNC void atomic_backend_free(void *pointer);
XR_FUNC void *atomic_backend_calloc(size_t count,size_t bytes);
XR_FUNC bool atomic_backend_cell_cas(_Atomic(uint64_t) *cell,uint64_t *expected,
    uint64_t desired,memory_order success,memory_order failure);
#endif
