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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrCacheStore XrCacheStore;

typedef bool (*XrCacheArtifactVerifier)(XrCacheArtifactKind kind, XrCacheKey key,
                                        const uint8_t *bytes, size_t size, void *context);

typedef struct XrCacheStoreConfig {
    const char *root;
    uint64_t quota_bytes;
    size_t max_entry_bytes;
    uint64_t stale_temp_age_ns;
    XrCacheArtifactVerifier verifier;
    void *verifier_context;
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
} XrCachePublishStatus;

typedef enum XrCacheLoadStatus {
    XR_CACHE_LOAD_HIT = 0,
    XR_CACHE_LOAD_MISS,
    XR_CACHE_LOAD_CORRUPT,
    XR_CACHE_LOAD_REJECTED,
    XR_CACHE_LOAD_TOO_LARGE,
    XR_CACHE_LOAD_IO_ERROR,
} XrCacheLoadStatus;

typedef struct XrCacheCollectStats {
    uint64_t live_bytes;
    size_t live_entries;
    uint64_t removed_bytes;
    size_t removed_entries;
    size_t stale_temps_removed;
    size_t corrupt_entries_removed;
} XrCacheCollectStats;

XR_FUNC XrCacheStore *xr_cache_store_open(const XrCacheStoreConfig *config);
XR_FUNC void xr_cache_store_close(XrCacheStore *store);
XR_FUNC XrCachePublishStatus xr_cache_store_publish(XrCacheStore *store,
                                                    XrCacheArtifactKind kind, XrCacheKey key,
                                                    const uint8_t *bytes, size_t size);
XR_FUNC XrCacheLoadStatus xr_cache_store_load(XrCacheStore *store, XrCacheArtifactKind kind,
                                              XrCacheKey key, XrCacheBlob *out);
XR_FUNC void xr_cache_blob_release(XrCacheBlob *blob);
XR_FUNC bool xr_cache_store_collect(XrCacheStore *store, XrCacheCollectStats *out);
XR_FUNC char *xr_cache_store_entry_path(const XrCacheStore *store, XrCacheArtifactKind kind,
                                        XrCacheKey key);

#endif  // XR_CACHE_STORE_H
