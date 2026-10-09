/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_sysheap_allocation_probe.h - Observe real heap initialization owners
 */
#ifndef XR_SYSHEAP_ALLOCATION_PROBE_H
#define XR_SYSHEAP_ALLOCATION_PROBE_H

#include "base/xmalloc.h"
#include "os/os_thread.h"

XR_FUNC void *xr_sysheap_test_malloc(size_t bytes, const char *source);
XR_FUNC void *xr_sysheap_test_calloc(size_t count, size_t bytes, const char *source);
XR_FUNC void xr_sysheap_test_free(void *memory);
XR_FUNC void xr_sysheap_test_mutex_init(xr_mutex_t *mutex);
XR_FUNC void xr_sysheap_test_mutex_destroy(xr_mutex_t *mutex);

#ifdef XR_SYSHEAP_ALLOCATION_PROBE_OBJECT
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(bytes) xr_sysheap_test_malloc((bytes), __FILE__)
#define xr_calloc(count, bytes) xr_sysheap_test_calloc((count), (bytes), __FILE__)
#define xr_free(memory) xr_sysheap_test_free(memory)
#define xr_mutex_init(mutex) xr_sysheap_test_mutex_init(mutex)
#define xr_mutex_destroy(mutex) xr_sysheap_test_mutex_destroy(mutex)
#endif

#endif // XR_SYSHEAP_ALLOCATION_PROBE_H
