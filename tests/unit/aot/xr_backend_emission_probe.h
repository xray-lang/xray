/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 * xr_backend_emission_probe.h - Real emission allocator interception
 */
#ifndef XR_BACKEND_EMISSION_PROBE_H
#define XR_BACKEND_EMISSION_PROBE_H
#include "base/xmalloc.h"
XR_FUNC void backend_probe_site(const char *file, unsigned line);
XR_FUNC void *backend_probe_malloc(size_t size);
XR_FUNC void *backend_probe_calloc(size_t count, size_t size);
XR_FUNC void *backend_probe_realloc(void *memory, size_t size);
XR_FUNC void backend_probe_free(void *memory);
#undef xr_calloc
#undef xr_malloc
#undef xr_realloc
#undef xr_free
#define xr_calloc(count, size)                                                                     \
    (backend_probe_site(__FILE__, __LINE__), backend_probe_calloc(count, size))
#define xr_malloc(size) (backend_probe_site(__FILE__, __LINE__), backend_probe_malloc(size))
#define xr_realloc(memory, size)                                                                   \
    (backend_probe_site(__FILE__, __LINE__), backend_probe_realloc(memory, size))
#define xr_free backend_probe_free
#endif
