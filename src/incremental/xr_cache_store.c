/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_store.c - Verified immutable content-addressed artifact storage
 */

#include "xr_cache_store.h"

#include "../base/xfileio.h"
#include "../base/xio_policy.inc.h"
#include <stdatomic.h>
#include "../base/xsha256.h"
#include "../os/os_dir.h"
#include "../os/os_fs.h"
#include "../os/os_random.h"
#include "../os/os_thread.h"
#include "../os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_CACHE_OBJECT_VERSION 1u
#define XR_CACHE_OBJECT_HEADER_SIZE 96u
#define XR_CACHE_TEMP_ATTEMPTS 16u

static const uint8_t XR_CACHE_OBJECT_MAGIC[8] = {'X', 'R', 'C', 'A', 'S', '0', '0', '1'};

typedef struct XrCacheDiskEntry {
    char *path;
    uint64_t size;
    int64_t mtime_ns;
} XrCacheDiskEntry;

typedef struct XrCacheObjectSnapshot {
    uint64_t size;
    uint8_t digest[XR_CACHE_KEY_BYTES];
} XrCacheObjectSnapshot;

struct XrCacheStore {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    atomic_int resource_status;
    char *root;
    char *lock_path;
    char *artifact_dirs[3];
    uint64_t quota_bytes;
    size_t max_entry_bytes;
    uint64_t stale_temp_age_ns;
    xr_mutex_t lock;
};

static bool cache_ready(const XrCacheStore *store) {
    return atomic_load_explicit(&store->resource_status, memory_order_relaxed) == XR_COMPILE_RESOURCE_OK;
}
static XrCompileResourceStatus cache_resource(XrCacheStore *store, XrCompileResourceStatus status) {
    if (status != XR_COMPILE_RESOURCE_OK) {
        int expected = XR_COMPILE_RESOURCE_OK;
        atomic_compare_exchange_strong_explicit(&store->resource_status, &expected, (int)status,
            memory_order_relaxed, memory_order_relaxed);
    }
    return (XrCompileResourceStatus)atomic_load_explicit(&store->resource_status, memory_order_relaxed);
}
static XrOsIoStatus cache_io_status(const XrCacheStore *store) {
    XrCompileResourceStatus status = (XrCompileResourceStatus)atomic_load_explicit(&store->resource_status, memory_order_relaxed);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_OS_IO_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_OS_IO_OUT_OF_MEMORY :
        status == XR_COMPILE_RESOURCE_BAD_ARGUMENT ? XR_OS_IO_BAD_ARGUMENT : XR_OS_IO_OK;
}
static XrOsIoStatus cache_failure_io(const XrCacheStore *store) {
    XrOsIoStatus status = cache_io_status(store);
    if (status == XR_OS_IO_OUT_OF_MEMORY) return XR_OS_IO_OUT_OF_MEMORY;
    if (status == XR_OS_IO_BUDGET) return XR_OS_IO_BUDGET;
    return XR_OS_IO_BAD_ARGUMENT;
}
static XrOsIoStatus cache_io(XrCacheStore *store, XrOsIoStatus status) {
    if (status == XR_OS_IO_BUDGET) cache_resource(store, XR_COMPILE_RESOURCE_BUDGET);
    else if (status == XR_OS_IO_OUT_OF_MEMORY) cache_resource(store, XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    else if (status == XR_OS_IO_BAD_ARGUMENT) cache_resource(store, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    return cache_ready(store) ? status : cache_failure_io(store);
}
#define CACHE_IO(store, operation) (cache_ready(store) ? cache_io(store, (operation)) : cache_failure_io(store))
static XrOsIoStatus cache_stat(XrCacheStore *store, const char *path, XrFsStat *output) {
    if (!cache_ready(store)) return cache_failure_io(store);
    return cache_io(store, xr_os_io_stat(&store->policy, path, output));
}
static XrOsIoStatus cache_dir_next(XrCacheStore *store, XrDirIter *iterator, XrDirEntry *output) {
    if (!cache_ready(store)) return cache_failure_io(store);
    return cache_io(store, xr_os_io_dir_next(iterator, output));
}
static bool cache_work(XrCacheStore *store, uint64_t units) {
    return cache_ready(store) && cache_resource(store, xr_compile_resources_work(store->resources, units)) == XR_COMPILE_RESOURCE_OK;
}
static void *cache_allocate(XrCacheStore *store, size_t size, bool clear) {
    void *result = NULL;
    if (cache_ready(store)) cache_resource(store, clear ? xr_compile_resources_calloc(store->resources, 1, size, &result) :
        xr_compile_resources_alloc(store->resources, size, &result));
    return result;
}
static bool cache_copy(XrCacheStore *store, void *target, const void *source, size_t size) {
    if (!cache_work(store, size)) return false;
    if (size) memcpy(target, source, size);
    return true;
}
static bool cache_clear(XrCacheStore *store, void *target, size_t size) {
    if (!cache_work(store, size)) return false;
    if (size) memset(target, 0, size);
    return true;
}
static size_t cache_length(XrCacheStore *store, const char *text) {
    size_t length = 0;
    while (cache_work(store, 1)) {
        if (!text[length]) return length;
        if (length == SIZE_MAX - 1) { cache_resource(store, XR_COMPILE_RESOURCE_BUDGET); return 0; }
        ++length;
    }
    return 0;
}
static int cache_compare(XrCacheStore *store, const void *left, const void *right, size_t size) {
    const uint8_t *a = left, *b = right;
    for (size_t i = 0; i < size; ++i) {
        if (!cache_work(store, 1)) return 0;
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
static int cache_text_compare(XrCacheStore *store, const char *a, const char *b) {
    size_t i = 0;
    while (cache_work(store, 1)) {
        unsigned char av = (unsigned char)a[i], bv = (unsigned char)b[i];
        if (av != bv) return av < bv ? -1 : 1;
        if (!av) return 0;
        ++i;
    }
    return 0;
}
static bool cache_hash(XrCacheStore *store, const uint8_t *bytes, size_t size, uint8_t digest[32]) {
    if (size > UINT64_MAX - 32) { cache_resource(store, XR_COMPILE_RESOURCE_BUDGET); return false; }
    if (!cache_work(store, size + 32)) return false;
    xr_sha256(bytes, size, digest); return true;
}
static char *cache_join(XrCacheStore *store, const char *dir, const char *name) {
    char *result = NULL;
    (void)CACHE_IO(store, xr_path_join_owned(&store->policy, dir, name, &result));
    return result;
}
static XrCompileCacheStatus cache_status(const XrCacheStore *store, bool ok) {
    XrOsIoStatus status = cache_io_status(store);
    return status == XR_OS_IO_BUDGET ? XR_COMPILE_CACHE_BUDGET : status == XR_OS_IO_OUT_OF_MEMORY ? XR_COMPILE_CACHE_OUT_OF_MEMORY :
        status == XR_OS_IO_BAD_ARGUMENT ? XR_COMPILE_CACHE_BAD_ARGUMENT : ok ? XR_COMPILE_CACHE_OK : XR_COMPILE_CACHE_IO;
}
static XrCacheLoadStatus cache_load_status(const XrCacheStore *store, XrCacheLoadStatus status) {
    XrOsIoStatus resource = cache_io_status(store);
    return resource == XR_OS_IO_BUDGET ? XR_CACHE_LOAD_BUDGET : resource == XR_OS_IO_OUT_OF_MEMORY ? XR_CACHE_LOAD_OUT_OF_MEMORY :
        resource == XR_OS_IO_BAD_ARGUMENT ? XR_CACHE_LOAD_BAD_ARGUMENT : status;
}
static XrCachePublishStatus cache_publish_status(const XrCacheStore *store, XrCachePublishStatus status) {
    XrOsIoStatus resource = cache_io_status(store);
    return resource == XR_OS_IO_BUDGET ? XR_CACHE_PUBLISH_BUDGET : resource == XR_OS_IO_OUT_OF_MEMORY ? XR_CACHE_PUBLISH_OUT_OF_MEMORY :
        resource == XR_OS_IO_BAD_ARGUMENT ? XR_CACHE_PUBLISH_BAD_ARGUMENT : status;
}
XR_FUNC XrCompileResources *xr_compile_cache_store_resources(const XrCacheStore *store) {
    return store ? store->resources : NULL;
}
XR_FUNC XrCompileResourceStatus xr_compile_cache_store_resource_status(const XrCacheStore *store) {
    return store ? (XrCompileResourceStatus)atomic_load_explicit(&store->resource_status, memory_order_relaxed) : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

static bool collect_locked(XrCacheStore *store, XrCacheCollectStats *stats,
                           const char *protected_path, uint64_t byte_limit);

static bool artifact_kind_valid(XrCacheArtifactKind kind) {
    return kind == XR_CACHE_ARTIFACT_XSM || kind == XR_CACHE_ARTIFACT_XTP;
}

static const char *artifact_dir_name(XrCacheArtifactKind kind) {
    if (kind == XR_CACHE_ARTIFACT_XSM)
        return "xsm";
    if (kind == XR_CACHE_ARTIFACT_XTP)
        return "xtp";
    return NULL;
}

static char *copy_text(XrCacheStore *store, const char *text) {
    size_t size = cache_length(store, text) + 1u;
    char *copy = (char *) cache_allocate(store, size, false);
    if (copy)
        if (!cache_copy(store, copy, text, size)) { xr_compile_resources_free(copy); return NULL; }
    return copy;
}

static void put_u32(XrCacheStore *store, uint8_t *out, uint32_t value) {
    for (size_t i = 0; i < 4u && cache_work(store, 1); i++)
        out[i] = (uint8_t) (value >> (i * 8u));
}

static void put_u64(XrCacheStore *store, uint8_t *out, uint64_t value) {
    for (size_t i = 0; i < 8u && cache_work(store, 1); i++)
        out[i] = (uint8_t) (value >> (i * 8u));
}

static uint32_t take_u32(XrCacheStore *store, const uint8_t *input) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4u && cache_work(store, 1); i++)
        value |= (uint32_t) input[i] << (i * 8u);
    return value;
}

static uint64_t take_u64(XrCacheStore *store, const uint8_t *input) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8u && cache_work(store, 1); i++)
        value |= (uint64_t) input[i] << (i * 8u);
    return value;
}

static char *entry_path(XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key) {
    if (!store || !artifact_kind_valid(kind))
        return NULL;
    char hex[XR_CACHE_KEY_HEX_SIZE];
    if (!cache_work(store, XR_CACHE_KEY_BYTES + XR_CACHE_KEY_HEX_SIZE)) return NULL;
    xr_cache_key_hex(key, hex);
    return cache_join(store, store->artifact_dirs[kind], hex);
}

XR_FUNC XrCompileCacheStatus xr_compile_cache_store_entry_path(XrCacheStore *store,
    XrCacheArtifactKind kind, XrCacheKey key, char **output) {
    if (!store || !artifact_kind_valid(kind) || !output) return XR_COMPILE_CACHE_BAD_ARGUMENT;
    char *path = entry_path(store, kind, key);
    if (path && cache_ready(store)) { *output = path; return XR_COMPILE_CACHE_OK; }
    xr_compile_resources_free(path); return cache_status(store, false);
}

static bool ensure_directory(XrCacheStore *store, const char *path) {
    XrFsStat stat;
    XrOsIoStatus found = cache_stat(store, path, &stat);
    if (found == XR_OS_IO_OK) return stat.kind == XR_FS_DIR;
    if (found != XR_OS_IO_NOT_FOUND) return false;
    if (CACHE_IO(store, xr_os_io_mkdir(&store->policy, path, 0700u)) != 0)
        return false;
    return cache_stat(store, path, &stat) == 0 && stat.kind == XR_FS_DIR;
}

static bool cache_layout_is_regular(XrCacheStore *store) {
    XrFsStat stat;
    if (!store || cache_stat(store, store->root, &stat) != 0 || stat.kind != XR_FS_DIR)
        return false;
    for (XrCacheArtifactKind kind = XR_CACHE_ARTIFACT_XSM; kind <= XR_CACHE_ARTIFACT_XTP; kind++) {
        if (!cache_work(store, 1)) return false;
        if (!store->artifact_dirs[kind] || cache_stat(store, store->artifact_dirs[kind], &stat) != 0 ||
            stat.kind != XR_FS_DIR)
            return false;
    }
    return true;
}

static bool acquire_root_lock(XrCacheStore *store, XrFsExclusiveLock *root_lock) {
    if (CACHE_IO(store, xr_os_io_lock_exclusive(&store->policy, store->lock_path, root_lock)) != 0)
        return false;
    if (cache_layout_is_regular(store))
        return true;
    (void) xr_fs_unlock_exclusive(root_lock);
    return false;
}

static void cache_store_free(XrCacheStore *store, bool destroy_lock) {
    if (!store)
        return;
    if (destroy_lock)
        xr_mutex_destroy(&store->lock);
    for (size_t i = 0; i < 3u; i++)
        xr_compile_resources_free(store->artifact_dirs[i]);
    xr_compile_resources_free(store->root);
    xr_compile_resources_free(store->lock_path);
    xr_compile_resources_free(store);
}

XR_FUNC XrCompileCacheStatus xr_compile_cache_store_open(XrCompileResources *resources,
    const XrCacheStoreConfig *config, XrCacheStore **output) {
    if (!resources || !output || !config || !config->root || !*config->root || config->max_entry_bytes == 0 ||
        config->quota_bytes < XR_CACHE_OBJECT_HEADER_SIZE || config->max_entry_bytes > SIZE_MAX - XR_CACHE_OBJECT_HEADER_SIZE ||
        config->max_entry_bytes > config->quota_bytes - XR_CACHE_OBJECT_HEADER_SIZE || !config->stale_temp_age_ns)
        return XR_COMPILE_CACHE_BAD_ARGUMENT;
    void *memory = NULL;
    XrCompileResourceStatus admitted = xr_compile_resources_calloc(resources, 1, sizeof(XrCacheStore), &memory);
    if (admitted != XR_COMPILE_RESOURCE_OK) return admitted == XR_COMPILE_RESOURCE_BUDGET ? XR_COMPILE_CACHE_BUDGET :
        admitted == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_COMPILE_CACHE_OUT_OF_MEMORY : XR_COMPILE_CACHE_BAD_ARGUMENT;
    XrCacheStore *store = memory; store->resources = resources; store->policy = xr_compile_io_policy(resources);
    atomic_init(&store->resource_status, XR_COMPILE_RESOURCE_OK);
    store->root = copy_text(store, config->root);
    store->lock_path = cache_join(store, config->root, ".cache-root.lock");
    store->quota_bytes = config->quota_bytes; store->max_entry_bytes = config->max_entry_bytes;
    store->stale_temp_age_ns = config->stale_temp_age_ns;
    bool ok = store->root && store->lock_path && ensure_directory(store, store->root);
    for (XrCacheArtifactKind kind = XR_CACHE_ARTIFACT_XSM; ok && kind <= XR_CACHE_ARTIFACT_XTP; ++kind) {
        if (!cache_work(store, 1)) { ok = false; break; }
        store->artifact_dirs[kind] = cache_join(store, store->root, artifact_dir_name(kind));
        ok = store->artifact_dirs[kind] && ensure_directory(store, store->artifact_dirs[kind]);
    }
    bool lock_live = false;
    if (ok && cache_work(store, 1)) {
        xr_mutex_init(&store->lock); lock_live = true;
        ok = xr_compile_cache_store_collect(store, NULL) == XR_COMPILE_CACHE_OK;
    } else ok = false;
    XrCompileCacheStatus status = cache_status(store, ok);
    if (status == XR_COMPILE_CACHE_OK) *output = store;
    else cache_store_free(store, lock_live);
    return status;
}
XR_FUNC void xr_compile_cache_store_close(XrCacheStore *store) { cache_store_free(store, true); }

static uint8_t *encode_object(XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key, const uint8_t *payload,
                              size_t payload_size, size_t *out_size) {
    if (payload_size > SIZE_MAX - XR_CACHE_OBJECT_HEADER_SIZE)
        return NULL;
    size_t object_size = XR_CACHE_OBJECT_HEADER_SIZE + payload_size;
    uint8_t *object = (uint8_t *) cache_allocate(store, object_size ? object_size : 1u, false);
    if (!object)
        return NULL;
    cache_clear(store, object, XR_CACHE_OBJECT_HEADER_SIZE);
    cache_copy(store, object, XR_CACHE_OBJECT_MAGIC, sizeof(XR_CACHE_OBJECT_MAGIC));
    put_u32(store, object + 8u, XR_CACHE_OBJECT_VERSION);
    put_u32(store, object + 12u, XR_CACHE_OBJECT_HEADER_SIZE);
    put_u32(store, object + 16u, (uint32_t) kind);
    put_u64(store, object + 24u, (uint64_t) payload_size);
    cache_copy(store, object + 32u, key.bytes, XR_CACHE_KEY_BYTES);
    cache_hash(store, payload, payload_size, object + 64u);
    if (payload_size != 0)
        cache_copy(store, object + XR_CACHE_OBJECT_HEADER_SIZE, payload, payload_size);
    if (!cache_ready(store)) { xr_compile_resources_free(object); return NULL; }
    *out_size = object_size;
    return object;
}

static bool decode_object(XrCacheStore *store, const uint8_t *object, size_t object_size, XrCacheArtifactKind kind,
                          XrCacheKey key, const uint8_t **payload, size_t *payload_size) {
    if (!object || object_size < XR_CACHE_OBJECT_HEADER_SIZE ||
        cache_compare(store, object, XR_CACHE_OBJECT_MAGIC, sizeof(XR_CACHE_OBJECT_MAGIC)) != 0 ||
        take_u32(store, object + 8u) != XR_CACHE_OBJECT_VERSION ||
        take_u32(store, object + 12u) != XR_CACHE_OBJECT_HEADER_SIZE ||
        take_u32(store, object + 16u) != (uint32_t) kind || take_u32(store, object + 20u) != 0 ||
        cache_compare(store, object + 32u, key.bytes, XR_CACHE_KEY_BYTES) != 0)
        return false;
    uint64_t encoded_size = take_u64(store, object + 24u);
    if (encoded_size > SIZE_MAX ||
        (size_t) encoded_size != object_size - XR_CACHE_OBJECT_HEADER_SIZE)
        return false;
    uint8_t digest[XR_CACHE_KEY_BYTES];
    cache_hash(store, object + XR_CACHE_OBJECT_HEADER_SIZE, (size_t) encoded_size, digest);
    if (cache_compare(store, digest, object + 64u, sizeof(digest)) != 0)
        return false;
    if (!cache_ready(store)) return false;
    *payload = object + XR_CACHE_OBJECT_HEADER_SIZE;
    *payload_size = (size_t) encoded_size;
    return true;
}

static bool remove_cache_path(XrCacheStore *store, XrCacheArtifactKind kind, const char *path) {
    if (CACHE_IO(store, xr_os_io_remove(&store->policy, path)) != 0)
        return false;
    XrOsIoStatus synced = CACHE_IO(store, xr_os_io_sync_directory(&store->policy, store->artifact_dirs[kind]));
    return synced == XR_OS_IO_OK || synced == XR_OS_IO_UNSUPPORTED;
}

static XrCacheLoadStatus load_locked(XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key,
                                     XrCacheBlob *out, XrCacheObjectSnapshot *snapshot) {
    char *path = entry_path(store, kind, key);
    if (!path)
        return XR_CACHE_LOAD_IO_ERROR;
    if (snapshot)
        cache_clear(store, snapshot, sizeof(*snapshot));
    XrFsStat stat;
    XrOsIoStatus found = cache_stat(store, path, &stat);
    if (found != XR_OS_IO_OK) {
        xr_compile_resources_free(path);
        return found == XR_OS_IO_NOT_FOUND ? XR_CACHE_LOAD_MISS : XR_CACHE_LOAD_IO_ERROR;
    }
    if (stat.kind != XR_FS_FILE) {
        bool removed = remove_cache_path(store, kind, path);
        xr_compile_resources_free(path);
        return removed ? XR_CACHE_LOAD_CORRUPT : XR_CACHE_LOAD_IO_ERROR;
    }
    if (stat.size > (uint64_t) store->max_entry_bytes + XR_CACHE_OBJECT_HEADER_SIZE) {
        bool removed = remove_cache_path(store, kind, path);
        xr_compile_resources_free(path);
        return removed ? XR_CACHE_LOAD_TOO_LARGE : XR_CACHE_LOAD_IO_ERROR;
    }

    uint8_t *object = NULL;
    size_t object_size = 0;
    size_t read_limit = store->max_entry_bytes + XR_CACHE_OBJECT_HEADER_SIZE;
    if (CACHE_IO(store, xr_os_io_read_regular_file(&store->policy, path, read_limit, &object, &object_size)) != 0) {
        xr_compile_resources_free(path);
        return XR_CACHE_LOAD_IO_ERROR;
    }
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (!decode_object(store, object, object_size, kind, key, &payload, &payload_size)) {
        bool removed = remove_cache_path(store, kind, path);
        xr_compile_resources_free(path);
        xr_compile_resources_free(object);
        return removed ? XR_CACHE_LOAD_CORRUPT : XR_CACHE_LOAD_IO_ERROR;
    }
    uint8_t *owned = (uint8_t *) cache_allocate(store, payload_size ? payload_size : 1u, false);
    if (!owned) {
        xr_compile_resources_free(path);
        xr_compile_resources_free(object);
        return XR_CACHE_LOAD_IO_ERROR;
    }
    if (payload_size != 0)
        cache_copy(store, owned, payload, payload_size);
    if (snapshot) {
        snapshot->size = object_size;
        cache_hash(store, object, object_size, snapshot->digest);
    }
    (void) CACHE_IO(store, xr_os_io_touch(&store->policy, path));
    xr_compile_resources_free(path);
    xr_compile_resources_free(object);
    if (!cache_work(store, sizeof(*out))) { xr_compile_resources_free(owned); return XR_CACHE_LOAD_IO_ERROR; }
    *out = (XrCacheBlob){kind, key, owned, payload_size};
    return XR_CACHE_LOAD_HIT;
}

static bool cleanup_rejected_snapshot_locked(XrCacheStore *store, XrCacheArtifactKind kind,
                                             XrCacheKey key,
                                             const XrCacheObjectSnapshot *snapshot) {
    char *path = entry_path(store, kind, key);
    if (!path)
        return false;
    XrFsStat stat;
    if (cache_stat(store, path, &stat) != 0) {
        xr_compile_resources_free(path);
        return false;
    }
    if (stat.kind != XR_FS_FILE || stat.size != snapshot->size) {
        xr_compile_resources_free(path);
        return true;
    }
    uint8_t *object = NULL;
    size_t object_size = 0;
    bool ok = CACHE_IO(store, xr_os_io_read_regular_file(&store->policy, path, (size_t) snapshot->size, &object, &object_size)) == 0;
    uint8_t digest[XR_CACHE_KEY_BYTES];
    if (ok)
        cache_hash(store, object, object_size, digest);
    bool matches = ok && object_size == snapshot->size &&
                   cache_compare(store, digest, snapshot->digest, sizeof(digest)) == 0;
    xr_compile_resources_free(object);
    if (matches)
        ok = remove_cache_path(store, kind, path);
    xr_compile_resources_free(path);
    return ok;
}

static XrCacheVerifyStatus cache_verify_failure(const XrCacheStore *store) {
    XrOsIoStatus failure = cache_io_status(store);
    return failure == XR_OS_IO_OUT_OF_MEMORY ? XR_CACHE_VERIFY_OUT_OF_MEMORY :
        failure == XR_OS_IO_BAD_ARGUMENT ? XR_CACHE_VERIFY_BAD_ARGUMENT : XR_CACHE_VERIFY_BUDGET;
}
static XrCacheVerifyStatus verify_artifact(XrCacheStore *store, XrCompileCacheArtifactVerifier verifier,
    XrCacheArtifactKind kind, XrCacheKey key, const uint8_t *bytes, size_t size, void *context) {
    if (!cache_work(store, 1)) return cache_verify_failure(store);
    XrCacheVerifyStatus status = verifier(store->resources, kind, key, bytes, size, context);
    if (status == XR_CACHE_VERIFY_BUDGET) cache_resource(store, XR_COMPILE_RESOURCE_BUDGET);
    else if (status == XR_CACHE_VERIFY_OUT_OF_MEMORY) cache_resource(store, XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    if (status == XR_CACHE_VERIFY_OK || status == XR_CACHE_VERIFY_REJECTED) {
        if (!cache_work(store, 1)) return cache_verify_failure(store);
    }
    return status;
}
static XrCacheLoadStatus verify_load_status(XrCacheVerifyStatus status) {
    switch (status) {
    case XR_CACHE_VERIFY_OK: return XR_CACHE_LOAD_HIT;
    case XR_CACHE_VERIFY_REJECTED: return XR_CACHE_LOAD_REJECTED;
    case XR_CACHE_VERIFY_BAD_ARGUMENT: return XR_CACHE_LOAD_BAD_ARGUMENT;
    case XR_CACHE_VERIFY_BUDGET: return XR_CACHE_LOAD_BUDGET;
    case XR_CACHE_VERIFY_OUT_OF_MEMORY: return XR_CACHE_LOAD_OUT_OF_MEMORY;
    case XR_CACHE_VERIFY_IO: return XR_CACHE_LOAD_IO_ERROR;
    }
    return XR_CACHE_LOAD_BAD_ARGUMENT;
}
XR_FUNC XrCacheLoadStatus xr_compile_cache_store_load(XrCacheStore *store, XrCacheArtifactKind kind,
    XrCacheKey key, XrCompileCacheArtifactVerifier verifier, void *context, XrCacheBlob *output) {
    if (!store || !artifact_kind_valid(kind) || !verifier || !output) return XR_CACHE_LOAD_BAD_ARGUMENT;
    XrFsExclusiveLock root_lock = {0};
    if (!acquire_root_lock(store, &root_lock)) return cache_load_status(store, XR_CACHE_LOAD_IO_ERROR);
    XrCacheBlob result = {0}; XrCacheObjectSnapshot snapshot = {0};
    if (!cache_work(store, 1)) { (void)xr_fs_unlock_exclusive(&root_lock); return cache_load_status(store, XR_CACHE_LOAD_IO_ERROR); }
    xr_mutex_lock(&store->lock);
    XrCacheLoadStatus status = load_locked(store, kind, key, &result, &snapshot);
    xr_mutex_unlock(&store->lock);
    if (xr_fs_unlock_exclusive(&root_lock)) status = XR_CACHE_LOAD_IO_ERROR;
    status = cache_load_status(store, status);
    if (status == XR_CACHE_LOAD_HIT) status = verify_load_status(verify_artifact(store, verifier, kind, key, result.bytes, result.size, context));
    if (status == XR_CACHE_LOAD_REJECTED) {
        XrFsExclusiveLock cleanup_lock = {0};
        if (!acquire_root_lock(store, &cleanup_lock)) status = XR_CACHE_LOAD_IO_ERROR;
        else {
            bool cleaned = false;
            if (cache_work(store, 1)) {
                xr_mutex_lock(&store->lock);
                cleaned = cleanup_rejected_snapshot_locked(store, kind, key, &snapshot);
                xr_mutex_unlock(&store->lock);
            }
            bool unlocked = xr_fs_unlock_exclusive(&cleanup_lock) == 0;
            if (!cleaned || !unlocked) status = XR_CACHE_LOAD_IO_ERROR;
        }
    }
    if (status == XR_CACHE_LOAD_HIT && cache_copy(store, output, &result, sizeof(result))) result.bytes = NULL;
    xr_compile_cache_blob_release(&result);
    return cache_load_status(store, status);
}

XR_FUNC void xr_compile_cache_blob_release(XrCacheBlob *blob) {
    if (!blob)
        return;
    xr_compile_resources_free(blob->bytes);
    memset(blob, 0, sizeof(*blob));
}

static char *make_temp_path(XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key) {
    uint8_t random[8]; char name[96]; static const char digits[] = "0123456789abcdef";
    if (!cache_work(store, 1 + sizeof(random))) return NULL;
    xr_random_bytes(random, sizeof(random));
    if (!cache_copy(store, name, ".tmp-", 5) || !cache_work(store, XR_CACHE_KEY_BYTES + XR_CACHE_KEY_HEX_SIZE)) return NULL;
    xr_cache_key_hex(key, name + 5);
    size_t offset = 5 + XR_CACHE_KEY_HEX_SIZE - 1;
    if (!cache_work(store, 1)) return NULL;
    name[offset++] = '-';
    for (size_t i = 0; i < sizeof(random); ++i) {
        if (!cache_work(store, 3)) return NULL;
        uint8_t value = random[i]; name[offset++] = digits[value >> 4]; name[offset++] = digits[value & 15];
    }
    if (!cache_work(store, 1)) return NULL;
    name[offset] = 0;
    return cache_join(store, store->artifact_dirs[kind], name);
}

static XrCachePublishStatus compare_existing(XrCacheStore *store, XrCacheArtifactKind kind,
                                             XrCacheKey key, const uint8_t *bytes, size_t size) {
    XrCacheBlob existing = {0};
    XrCacheLoadStatus status = load_locked(store, kind, key, &existing, NULL);
    if (status == XR_CACHE_LOAD_HIT) {
        bool equal =
            existing.size == size && (size == 0 || cache_compare(store, existing.bytes, bytes, size) == 0);
        xr_compile_cache_blob_release(&existing);
        return equal ? XR_CACHE_PUBLISH_EXISTS : XR_CACHE_PUBLISH_CONFLICT;
    }
    if (status == XR_CACHE_LOAD_MISS || status == XR_CACHE_LOAD_CORRUPT ||
        status == XR_CACHE_LOAD_REJECTED || status == XR_CACHE_LOAD_TOO_LARGE)
        return XR_CACHE_PUBLISH_OK;
    return XR_CACHE_PUBLISH_IO_ERROR;
}

static XrCachePublishStatus publish_locked(XrCacheStore *store, XrCacheArtifactKind kind,
                                           XrCacheKey key, const uint8_t *bytes, size_t size) {
    size_t object_size = 0;
    uint8_t *object = encode_object(store, kind, key, bytes, size, &object_size);
    if (!object)
        return XR_CACHE_PUBLISH_IO_ERROR;
    char *final_path = entry_path(store, kind, key);
    if (!final_path) {
        xr_compile_resources_free(object);
        return XR_CACHE_PUBLISH_IO_ERROR;
    }

    XrCachePublishStatus result = XR_CACHE_PUBLISH_IO_ERROR;
    for (unsigned attempt = 0; attempt < 2u && cache_work(store, 1); attempt++) {
        char *temp_path = NULL;
        for (unsigned candidate = 0; candidate < XR_CACHE_TEMP_ATTEMPTS && cache_work(store, 1); candidate++) {
            temp_path = make_temp_path(store, kind, key);
            if (!temp_path)
                break;
            if (CACHE_IO(store, xr_os_io_write_new_file_sync(&store->policy, temp_path, object, object_size)) == 0)
                break;
            xr_compile_resources_free(temp_path);
            temp_path = NULL;
        }
        if (!temp_path)
            break;
        XrOsIoStatus published = CACHE_IO(store, xr_os_io_publish_noreplace(&store->policy, temp_path, final_path));
        if (published == XR_OS_IO_OK) {
            XrOsIoStatus sync = CACHE_IO(store, xr_os_io_sync_directory(&store->policy, store->artifact_dirs[kind]));
            /* MoveFileEx WRITE_THROUGH is the strongest portable Windows
             * publication contract; directory handles have no equivalent
             * flush operation. A later cache miss remains safe recomputation. */
            if ((sync == XR_OS_IO_OK || sync == XR_OS_IO_UNSUPPORTED))
                result = XR_CACHE_PUBLISH_OK;
            xr_compile_resources_free(temp_path);
            break;
        }
        (void) CACHE_IO(store, xr_os_io_remove(&store->policy, temp_path));
        xr_compile_resources_free(temp_path);
        if (published != XR_OS_IO_EXISTS)
            break;
        result = compare_existing(store, kind, key, bytes, size);
        if (result != XR_CACHE_PUBLISH_OK)
            break;
    }
    xr_compile_resources_free(final_path);
    xr_compile_resources_free(object);
    return result;
}

static XrCachePublishStatus verify_publish_status(XrCacheVerifyStatus status) {
    switch (status) {
    case XR_CACHE_VERIFY_OK: return XR_CACHE_PUBLISH_OK;
    case XR_CACHE_VERIFY_REJECTED: return XR_CACHE_PUBLISH_REJECTED;
    case XR_CACHE_VERIFY_BAD_ARGUMENT: return XR_CACHE_PUBLISH_BAD_ARGUMENT;
    case XR_CACHE_VERIFY_BUDGET: return XR_CACHE_PUBLISH_BUDGET;
    case XR_CACHE_VERIFY_OUT_OF_MEMORY: return XR_CACHE_PUBLISH_OUT_OF_MEMORY;
    case XR_CACHE_VERIFY_IO: return XR_CACHE_PUBLISH_IO_ERROR;
    }
    return XR_CACHE_PUBLISH_BAD_ARGUMENT;
}
XR_FUNC XrCachePublishStatus xr_compile_cache_store_publish(XrCacheStore *store, XrCacheArtifactKind kind,
    XrCacheKey key, const uint8_t *bytes, size_t size, XrCompileCacheArtifactVerifier verifier, void *context) {
    if (!store || !artifact_kind_valid(kind) || !verifier || (!bytes && size)) return XR_CACHE_PUBLISH_BAD_ARGUMENT;
    if (!cache_ready(store)) return cache_publish_status(store, XR_CACHE_PUBLISH_IO_ERROR);
    if (size > store->max_entry_bytes) return XR_CACHE_PUBLISH_TOO_LARGE;
    XrCachePublishStatus status = verify_publish_status(verify_artifact(store, verifier, kind, key, bytes, size, context));
    if (status != XR_CACHE_PUBLISH_OK) return status;
    XrFsExclusiveLock root_lock = {0};
    if (!acquire_root_lock(store, &root_lock)) return cache_publish_status(store, XR_CACHE_PUBLISH_IO_ERROR);
    if (!cache_work(store, 1)) { (void)xr_fs_unlock_exclusive(&root_lock); return cache_publish_status(store, XR_CACHE_PUBLISH_IO_ERROR); }
    xr_mutex_lock(&store->lock);
    status = compare_existing(store, kind, key, bytes, size);
    if (status == XR_CACHE_PUBLISH_OK && cache_ready(store)) {
        uint64_t reservation = (uint64_t)size + XR_CACHE_OBJECT_HEADER_SIZE;
        if (size > SIZE_MAX - XR_CACHE_OBJECT_HEADER_SIZE || reservation > store->quota_bytes) status = XR_CACHE_PUBLISH_TOO_LARGE;
        else {
            XrCacheCollectStats stats = {0};
            if (!collect_locked(store, &stats, NULL, store->quota_bytes - reservation)) status = XR_CACHE_PUBLISH_IO_ERROR;
            else status = publish_locked(store, kind, key, bytes, size);
        }
    }
    xr_mutex_unlock(&store->lock);
    if (xr_fs_unlock_exclusive(&root_lock)) status = XR_CACHE_PUBLISH_IO_ERROR;
    return cache_publish_status(store, status);
}

static bool append_disk_entry(XrCacheStore *store, XrCacheDiskEntry **entries,
    size_t *count, size_t *capacity, const char *path, const XrFsStat *stat) {
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 16;
        if (next < *capacity || next > SIZE_MAX / sizeof(**entries)) {
            cache_resource(store, XR_COMPILE_RESOURCE_BUDGET); return false;
        }
        void *memory = *entries;
        if (!cache_ready(store) || cache_resource(store, xr_compile_resources_resize(store->resources,
            &memory, next * sizeof(**entries))) != XR_COMPILE_RESOURCE_OK) return false;
        *entries = memory; *capacity = next;
    }
    char *owned = copy_text(store, path);
    if (!owned) return false;
    if (!cache_work(store, sizeof(**entries))) { xr_compile_resources_free(owned); return false; }
    (*entries)[(*count)++] = (XrCacheDiskEntry){owned, stat->size, stat->mtime_ns};
    return true;
}
static bool is_temp_name(XrCacheStore *store, const char *name) {
    const char prefix[] = ".tmp-";
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i) {
        if (!cache_work(store, 1) || name[i] != prefix[i]) return false;
    }
    return true;
}
static bool is_entry_name(XrCacheStore *store, const char *name) {
    if (cache_length(store, name) != XR_CACHE_KEY_HEX_SIZE - 1) return false;
    for (size_t i = 0; i < XR_CACHE_KEY_HEX_SIZE - 1; ++i) {
        if (!cache_work(store, 1)) return false;
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
static bool should_remove_stale_temp(const XrCacheStore *store, const XrFsStat *stat, uint64_t now) {
    return stat->mtime_ns > 0 && now >= (uint64_t)stat->mtime_ns &&
        now - (uint64_t)stat->mtime_ns >= store->stale_temp_age_ns;
}
static bool scan_directory(XrCacheStore *store, XrCacheArtifactKind kind,
    XrCacheDiskEntry **entries, size_t *count, size_t *capacity, XrCacheCollectStats *stats, uint64_t now) {
    XrDirIter *iterator = NULL;
    if (CACHE_IO(store, xr_os_io_dir_open(&store->policy, store->artifact_dirs[kind], &iterator)) != XR_OS_IO_OK) return false;
    XrOsIoStatus next = XR_OS_IO_OK; bool ok = true; XrDirEntry entry;
    while (ok && cache_work(store, 1) &&
        (next = cache_dir_next(store, iterator, &entry)) == XR_OS_IO_OK) {
        if (entry.is_dir) continue;
        char *path = cache_join(store, store->artifact_dirs[kind], entry.name);
        if (!path) { ok = false; break; }
        XrFsStat stat;
        XrOsIoStatus found = cache_stat(store, path, &stat);
        if (found == XR_OS_IO_NOT_FOUND) { xr_compile_resources_free(path); continue; }
        if (found != XR_OS_IO_OK) ok = false;
        else if (stat.kind != XR_FS_FILE) {
            if (CACHE_IO(store, xr_os_io_remove(&store->policy, path)) == XR_OS_IO_OK) ++stats->corrupt_entries_removed;
            else ok = false;
        } else if (is_temp_name(store, entry.name)) {
            if (should_remove_stale_temp(store, &stat, now)) {
                if (CACHE_IO(store, xr_os_io_remove(&store->policy, path)) == XR_OS_IO_OK) ++stats->stale_temps_removed;
                else ok = false;
            } else ok = append_disk_entry(store, entries, count, capacity, path, &stat);
        } else if (!is_entry_name(store, entry.name)) {
            if (CACHE_IO(store, xr_os_io_remove(&store->policy, path)) == XR_OS_IO_OK) ++stats->corrupt_entries_removed;
            else ok = false;
        } else ok = append_disk_entry(store, entries, count, capacity, path, &stat);
        xr_compile_resources_free(path);
    }
    xr_os_io_dir_close(iterator);
    return ok && next == XR_OS_IO_END && cache_ready(store);
}
static int compare_disk_entry(XrCacheStore *store, const XrCacheDiskEntry *a, const XrCacheDiskEntry *b) {
    if (!cache_work(store, 1)) return 0;
    if (a->mtime_ns != b->mtime_ns) return a->mtime_ns < b->mtime_ns ? -1 : 1;
    return cache_text_compare(store, a->path, b->path);
}
static bool swap_disk_entry(XrCacheStore *store, XrCacheDiskEntry *a, XrCacheDiskEntry *b) {
    if (!cache_work(store, 3 * sizeof(*a))) return false;
    XrCacheDiskEntry temporary = *a; *a = *b; *b = temporary; return true;
}
static void sift_entries(XrCacheStore *store, XrCacheDiskEntry *entries, size_t root, size_t count) {
    while (root < count / 2 && cache_work(store, 1)) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare_disk_entry(store, &entries[child], &entries[child + 1]) < 0) ++child;
        if (compare_disk_entry(store, &entries[root], &entries[child]) >= 0) break;
        if (!swap_disk_entry(store, &entries[root], &entries[child])) break;
        root = child;
    }
}
static bool sort_entries(XrCacheStore *store, XrCacheDiskEntry *entries, size_t count) {
    for (size_t i = count / 2; i && cache_work(store, 1); --i) sift_entries(store, entries, i - 1, count);
    for (size_t i = count; i > 1 && cache_work(store, 1); --i) {
        if (!swap_disk_entry(store, &entries[0], &entries[i - 1])) break;
        sift_entries(store, entries, 0, i - 1);
    }
    return cache_ready(store);
}
static bool collect_locked(XrCacheStore *store, XrCacheCollectStats *stats,
    const char *protected_path, uint64_t byte_limit) {
    XrCacheDiskEntry *entries = NULL; size_t count = 0, capacity = 0; uint64_t total = 0;
    if (!cache_work(store, 1)) return false;
    uint64_t now = xr_time_realtime_ns();
    bool ok = scan_directory(store, XR_CACHE_ARTIFACT_XSM, &entries, &count, &capacity, stats, now) &&
        scan_directory(store, XR_CACHE_ARTIFACT_XTP, &entries, &count, &capacity, stats, now);
    for (size_t i = 0; ok && i < count; ++i) {
        if (!cache_work(store, 1)) { ok = false; break; }
        if (entries[i].size > UINT64_MAX - total) {
            cache_resource(store, XR_COMPILE_RESOURCE_BUDGET); ok = false; break;
        }
        total += entries[i].size;
    }
    if (ok) ok = sort_entries(store, entries, count);
    for (size_t i = 0; ok && total > byte_limit && i < count; ++i) {
        if (!cache_work(store, 1)) { ok = false; break; }
        if (protected_path && cache_text_compare(store, entries[i].path, protected_path) == 0) continue;
        if (CACHE_IO(store, xr_os_io_remove(&store->policy, entries[i].path)) != XR_OS_IO_OK) { ok = false; break; }
        total -= entries[i].size; stats->removed_bytes += entries[i].size;
        ++stats->removed_entries; entries[i].size = 0;
    }
    if (total > byte_limit) ok = false;
    for (size_t i = 0; i < count; ++i) {
        if (ok && !cache_work(store, 1)) ok = false;
        if (ok && entries[i].size) { stats->live_bytes += entries[i].size; ++stats->live_entries; }
        xr_compile_resources_free(entries[i].path);
    }
    xr_compile_resources_free(entries);
    if (ok && (stats->removed_entries || stats->stale_temps_removed || stats->corrupt_entries_removed)) {
        for (XrCacheArtifactKind kind = XR_CACHE_ARTIFACT_XSM; kind <= XR_CACHE_ARTIFACT_XTP; ++kind) {
            if (!cache_work(store, 1)) { ok = false; break; }
            XrOsIoStatus sync = CACHE_IO(store, xr_os_io_sync_directory(&store->policy, store->artifact_dirs[kind]));
            if (sync != XR_OS_IO_OK && sync != XR_OS_IO_UNSUPPORTED) ok = false;
        }
    }
    return ok && cache_ready(store);
}
XR_FUNC XrCompileCacheStatus xr_compile_cache_store_collect(XrCacheStore *store, XrCacheCollectStats *output) {
    if (!store) return XR_COMPILE_CACHE_BAD_ARGUMENT;
    XrFsExclusiveLock root_lock = {0};
    if (!acquire_root_lock(store, &root_lock)) return cache_status(store, false);
    XrCacheCollectStats stats = {0}; bool ok = false;
    if (cache_work(store, 1)) {
        xr_mutex_lock(&store->lock);
        ok = collect_locked(store, &stats, NULL, store->quota_bytes);
        xr_mutex_unlock(&store->lock);
    }
    if (xr_fs_unlock_exclusive(&root_lock)) ok = false;
    if (ok && output) ok = cache_copy(store, output, &stats, sizeof(stats));
    return cache_status(store, ok);
}
