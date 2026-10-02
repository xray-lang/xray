/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_store.h - Verified immutable content-addressed artifact storage
 *
 * KEY CONCEPT:
 *   The store owns bytes, integrity metadata, atomic publication, and quota.
 *   Artifact meaning remains behind a mandatory independent verifier callback.
 */

#ifndef XR_CACHE_STORE_H
#define XR_CACHE_STORE_H

#include "xr_cache_key.h"
#include "../base/xcompile_resources.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrCacheStore XrCacheStore;

typedef enum XrCompileCacheStatus {
    XR_COMPILE_CACHE_OK,
    XR_COMPILE_CACHE_BAD_ARGUMENT,
    XR_COMPILE_CACHE_BUDGET,
    XR_COMPILE_CACHE_OUT_OF_MEMORY,
    XR_COMPILE_CACHE_IO
} XrCompileCacheStatus;

typedef enum XrCacheVerifyStatus {
    XR_CACHE_VERIFY_OK, XR_CACHE_VERIFY_REJECTED, XR_CACHE_VERIFY_BAD_ARGUMENT,
    XR_CACHE_VERIFY_BUDGET, XR_CACHE_VERIFY_OUT_OF_MEMORY, XR_CACHE_VERIFY_IO
} XrCacheVerifyStatus;
/* The verifier receives the store's ledger. Only REJECTED permits the
 * corruption-removal policy; resource/argument/IO failures preserve entries. */
typedef XrCacheVerifyStatus (*XrCompileCacheArtifactVerifier)(XrCompileResources *resources,
    XrCacheArtifactKind kind, XrCacheKey key, const uint8_t *bytes, size_t size, void *context);

typedef struct XrCacheStoreConfig {
    const char *root;
    uint64_t quota_bytes;
    size_t max_entry_bytes;
    uint64_t stale_temp_age_ns;
} XrCacheStoreConfig;

typedef struct XrCacheBlob {
    XrCacheArtifactKind kind;
    XrCacheKey key;
    uint8_t *bytes;
    size_t size;
} XrCacheBlob;

typedef enum XrCachePublishStatus {
    XR_CACHE_PUBLISH_OK = 0,
    XR_CACHE_PUBLISH_EXISTS,
    XR_CACHE_PUBLISH_REJECTED,
    XR_CACHE_PUBLISH_CONFLICT,
    XR_CACHE_PUBLISH_TOO_LARGE,
    XR_CACHE_PUBLISH_IO_ERROR,
    XR_CACHE_PUBLISH_BAD_ARGUMENT,
    XR_CACHE_PUBLISH_BUDGET,
    XR_CACHE_PUBLISH_OUT_OF_MEMORY,
} XrCachePublishStatus;

typedef enum XrCacheLoadStatus {
    XR_CACHE_LOAD_HIT = 0,
    XR_CACHE_LOAD_MISS,
    XR_CACHE_LOAD_CORRUPT,
    XR_CACHE_LOAD_REJECTED,
    XR_CACHE_LOAD_TOO_LARGE,
    XR_CACHE_LOAD_IO_ERROR,
    XR_CACHE_LOAD_BAD_ARGUMENT,
    XR_CACHE_LOAD_BUDGET,
    XR_CACHE_LOAD_OUT_OF_MEMORY,
} XrCacheLoadStatus;

typedef struct XrCacheCollectStats {
    uint64_t live_bytes;
    size_t live_entries;
    uint64_t removed_bytes;
    size_t removed_entries;
    size_t stale_temps_removed;
    size_t corrupt_entries_removed;
} XrCacheCollectStats;

/* The required ledger owns all store and blob memory. Failure preserves output. */
XR_FUNC XrCompileCacheStatus xr_compile_cache_store_open(XrCompileResources *resources,
    const XrCacheStoreConfig *config, XrCacheStore **output);
XR_FUNC void xr_compile_cache_store_close(XrCacheStore *store);
XR_FUNC XrCompileResources *xr_compile_cache_store_resources(const XrCacheStore *store);
XR_FUNC XrCompileResourceStatus xr_compile_cache_store_resource_status(const XrCacheStore *store);
XR_FUNC XrCachePublishStatus xr_compile_cache_store_publish(XrCacheStore *store,
                                                    XrCacheArtifactKind kind, XrCacheKey key,
                                                    const uint8_t *bytes, size_t size,
                                                    XrCompileCacheArtifactVerifier verifier,
                                                    void *verifier_context);
XR_FUNC XrCacheLoadStatus xr_compile_cache_store_load(XrCacheStore *store, XrCacheArtifactKind kind,
                                              XrCacheKey key,
                                              XrCompileCacheArtifactVerifier verifier,
                                              void *verifier_context,
                                              XrCacheBlob *out);
XR_FUNC void xr_compile_cache_blob_release(XrCacheBlob *blob);
XR_FUNC XrCompileCacheStatus xr_compile_cache_store_collect(XrCacheStore *store,
    XrCacheCollectStats *out);
/* The caller releases the path with xr_compile_resources_free. */
XR_FUNC XrCompileCacheStatus xr_compile_cache_store_entry_path(XrCacheStore *store,
    XrCacheArtifactKind kind, XrCacheKey key, char **output);

#endif  // XR_CACHE_STORE_H
