/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_source_allocation_probe.h - Source coordinator allocation boundary
 *
 * Only the test executable substitutes the two coordinator allocators.
 */

#ifndef XR_MODULE_SOURCE_ALLOCATION_PROBE_H
#define XR_MODULE_SOURCE_ALLOCATION_PROBE_H
#include "base/xmalloc.h"
XR_FUNC void *xr_module_test_malloc(size_t size);
XR_FUNC void *xr_module_test_calloc(size_t count, size_t size);
XR_FUNC void *xr_module_test_realloc(void *pointer, size_t size);
XR_FUNC void xr_module_test_free(void *pointer);
XR_FUNC char *xr_module_test_strdup(const char *source);
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) xr_module_test_malloc(size)
#define xr_calloc(count,size) xr_module_test_calloc(count,size)
#define xr_realloc(pointer,size) xr_module_test_realloc(pointer,size)
#define xr_free(pointer) xr_module_test_free(pointer)
#define xr_strdup(source) xr_module_test_strdup(source)
#endif
