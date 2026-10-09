/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_runtime_map_probe.h - Real runtime map allocation observation
 *
 * KEY CONCEPT:
 *   Production calls retain their actual outcomes while selected owner
 *   allocations pass through a fixed failure and physical lifetime observer.
 */
#ifndef XR_RUNTIME_MAP_PROBE_H
#define XR_RUNTIME_MAP_PROBE_H
#include "base/xmalloc.h"
#include "base/xhashmap.h"
#include "api/xglobal_object.h"
#include "runtime/xisolate_internal.h"
XR_FUNC void *xr_runtime_map_malloc(size_t bytes, const char *source);
XR_FUNC void *xr_runtime_map_calloc(size_t count, size_t bytes);
XR_FUNC void *xr_runtime_map_realloc(void *memory, size_t bytes);
XR_FUNC void xr_runtime_map_free(void *memory);
XR_FUNC XrOsIoStatus xr_runtime_map_strings_new(const XrOsIoPolicy *policy, XrHashMap **output);
XR_FUNC XrOsIoStatus xr_runtime_map_registered_set(XrHashMap *map, const char *key, void *value);
XR_FUNC int xr_runtime_map_engine_init(XrVMRuntime *runtime);
XR_FUNC XrGlobalObject *xr_runtime_map_global_create(XrVMRuntime *runtime);
#ifdef XR_RUNTIME_MAP_ALLOCATION_PROBE
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) xr_runtime_map_malloc((bytes), __FILE__)
#define xr_calloc(count, bytes) xr_runtime_map_calloc((count), (bytes))
#define xr_realloc(memory, bytes) xr_runtime_map_realloc((memory), (bytes))
#define xr_free(memory) xr_runtime_map_free(memory)
#endif
#ifdef XR_RUNTIME_MAP_ENGINE_PROBE
#define xr_hashmap_owned_new(policy, output) xr_runtime_map_strings_new((policy), (output))
#endif
#ifdef XR_RUNTIME_MAP_GLOBAL_PROBE
#define xr_hashmap_owned_set(map, key, value) xr_runtime_map_registered_set((map), (key), (value))
#endif
#ifdef XR_RUNTIME_MAP_CALL_PROBE
#define xr_execution_engine_init(runtime) xr_runtime_map_engine_init(runtime)
#define xr_global_object_create(runtime) xr_runtime_map_global_create(runtime)
#endif
#endif // XR_RUNTIME_MAP_PROBE_H
