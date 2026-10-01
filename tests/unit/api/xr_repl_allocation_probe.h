/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_repl_allocation_probe.h - Actual REPL coordinator allocator substitution
 */
#ifndef XR_REPL_ALLOCATION_PROBE_H
#define XR_REPL_ALLOCATION_PROBE_H
#include "base/xmalloc.h"
XR_FUNC void *xr_repl_probe_malloc(size_t size);
XR_FUNC void *xr_repl_probe_calloc(size_t count, size_t size);
XR_FUNC void *xr_repl_probe_realloc(void *pointer, size_t size);
XR_FUNC void xr_repl_probe_free(void *pointer);
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) xr_repl_probe_malloc(size)
#define xr_calloc(count, size) xr_repl_probe_calloc(count, size)
#define xr_realloc(pointer, size) xr_repl_probe_realloc(pointer, size)
#define xr_free(pointer) xr_repl_probe_free(pointer)
#endif
