/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_dynamic_entry_runtime.h - Generation-owned dynamic entry registry/cache
 */

#ifndef XR_DYNAMIC_ENTRY_RUNTIME_H
#define XR_DYNAMIC_ENTRY_RUNTIME_H

#include "xr_entry_cell.h"
#include "../vm/xr_vm_dynamic_entry.h"

#define XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES UINT32_C(65536)
#define XR_RUNTIME_DYNAMIC_ENTRY_MAX_CACHE_BYTES ((size_t) 8u * 1024u * 1024u)

typedef struct XrRuntimeEntryHandle XrRuntimeEntryHandle;
typedef struct XrRuntimeEntryRegistry XrRuntimeEntryRegistry;
typedef struct XrRuntimeDynamicEntryCache XrRuntimeDynamicEntryCache;

typedef struct XrRuntimeDynamicEntryCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t registry_scans;
    uint64_t replacements;
} XrRuntimeDynamicEntryCacheStats;

typedef struct XrRuntimeEntryRegistryStats {
    uint32_t allocated_rows;
    uint32_t active_rows;
    uint64_t mutations;
} XrRuntimeEntryRegistryStats;

XR_FUNC bool xr_runtime_entry_registry_create(
    XrRuntimeEntryRegistry **registry, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC bool xr_runtime_entry_registry_destroy(
    XrRuntimeEntryRegistry **registry, char *diagnostic,
    size_t diagnostic_size);

XR_FUNC bool xr_runtime_entry_handle_create(
    XrRuntimeEntryHandle **handle, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC bool xr_runtime_entry_handle_retain(
    XrRuntimeEntryHandle *handle, XrRuntimeEntryHandle **retained,
    char *diagnostic, size_t diagnostic_size);
XR_FUNC bool xr_runtime_entry_handle_release(
    XrRuntimeEntryHandle **handle, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC XrEntryCell *xr_runtime_entry_handle_cell(
    XrRuntimeEntryHandle *handle);
XR_FUNC const XrEntryCellExpectation *xr_runtime_entry_handle_expectation(
    const XrRuntimeEntryHandle *handle);
XR_FUNC bool xr_runtime_entry_handle_bind(
    XrRuntimeEntryHandle *handle,
    const XrEntryCellRegistration *registration, bool *already_bound,
    char *diagnostic, size_t diagnostic_size);

XR_FUNC bool xr_runtime_entry_registry_publish(
    XrRuntimeGenerationAuthority *authority, const XrTargetPlan *plan,
    uint32_t source_export, XrRuntimeEntryHandle *handle, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC bool xr_runtime_entry_registry_unpublish(
    XrRuntimeGenerationAuthority *authority, XrRuntimeEntryHandle *handle,
    char *diagnostic, size_t diagnostic_size);
XR_FUNC bool xr_runtime_entry_registry_stats(
    XrRuntimeGenerationAuthority *authority,
    XrRuntimeEntryRegistryStats *stats);

XR_FUNC bool xr_runtime_dynamic_entry_cache_create(
    XrLoadedModuleGeneration *generation,
    XrRuntimeDynamicEntryCache **cache, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC bool xr_runtime_dynamic_entry_cache_free(
    XrRuntimeDynamicEntryCache **cache, char *diagnostic,
    size_t diagnostic_size);
XR_FUNC bool xr_runtime_dynamic_entry_cache_stats(
    const XrRuntimeDynamicEntryCache *cache,
    XrRuntimeDynamicEntryCacheStats *stats);
XR_FUNC void xr_runtime_dynamic_entry_context_init(
    XrLoadedModuleGeneration *generation,
    XrVmDynamicEntryContext *context);

#endif  // XR_DYNAMIC_ENTRY_RUNTIME_H
