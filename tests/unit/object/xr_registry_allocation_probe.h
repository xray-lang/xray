/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_registry_allocation_probe.h - Runtime metadata allocation qualification
 *
 * KEY CONCEPT:
 *   Real registry and map allocation sites retain fixed failure and ownership expectations.
 */
#ifndef XR_REGISTRY_ALLOCATION_PROBE_H
#define XR_REGISTRY_ALLOCATION_PROBE_H
#include "base/xmalloc.h"
XR_FUNC void *xr_registry_test_malloc(size_t bytes, const char *source);
XR_FUNC void *xr_registry_test_realloc(void *memory, size_t bytes, const char *source);
XR_FUNC void xr_registry_test_free(void *memory);
#ifdef XR_REGISTRY_ALLOCATION_PROBE_OBJECT
#undef xr_malloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) xr_registry_test_malloc((bytes), __FILE__)
#define xr_realloc(memory, bytes) xr_registry_test_realloc((memory), (bytes), __FILE__)
#define xr_free(memory) xr_registry_test_free(memory)
#endif
#endif // XR_REGISTRY_ALLOCATION_PROBE_H
