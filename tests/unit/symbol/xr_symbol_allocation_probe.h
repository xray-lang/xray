/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_symbol_allocation_probe.h - Physical symbol allocation observation
 *
 * KEY CONCEPT:
 *   The observer delegates to the real allocator and records each owned block.
 */
#ifndef XR_SYMBOL_ALLOCATION_PROBE_H
#define XR_SYMBOL_ALLOCATION_PROBE_H
#include "base/xmalloc.h"
#include "os/os_thread.h"
XR_FUNC void *xr_symbol_test_malloc(size_t bytes, const char *source);
XR_FUNC void *xr_symbol_test_realloc(void *memory, size_t bytes, const char *source);
XR_FUNC void xr_symbol_test_free(void *memory);
XR_FUNC void xr_symbol_test_rwlock_init(xr_rwlock_t *lock);
XR_FUNC void xr_symbol_test_rwlock_destroy(xr_rwlock_t *lock);
#ifdef XR_SYMBOL_ALLOCATION_PROBE_OBJECT
#undef xr_malloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) xr_symbol_test_malloc((bytes), __FILE__)
#define xr_realloc(memory, bytes) xr_symbol_test_realloc((memory), (bytes), __FILE__)
#define xr_free(memory) xr_symbol_test_free(memory)
#define xr_rwlock_init(lock) xr_symbol_test_rwlock_init(lock)
#define xr_rwlock_destroy(lock) xr_symbol_test_rwlock_destroy(lock)
#endif
#endif // XR_SYMBOL_ALLOCATION_PROBE_H
