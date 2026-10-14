/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mcp_runtime_allocations.h - Observe the canonical runtime allocation boundary
 */
#ifndef MCP_RUNTIME_ALLOCATIONS_H
#define MCP_RUNTIME_ALLOCATIONS_H
#include "base/xmalloc.h"
XR_FUNC void *mcp_runtime_malloc(size_t bytes);
XR_FUNC void *mcp_runtime_calloc(size_t count, size_t bytes);
XR_FUNC void *mcp_runtime_realloc(void *pointer, size_t bytes);
XR_FUNC void mcp_runtime_free(void *pointer);
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc mcp_runtime_malloc
#define xr_calloc mcp_runtime_calloc
#define xr_realloc mcp_runtime_realloc
#define xr_free mcp_runtime_free
#endif // MCP_RUNTIME_ALLOCATIONS_H
