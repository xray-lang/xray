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
#include "../base/xmalloc.h"
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
    char *root;
    char *lock_path;
    char *artifact_dirs[3];
    uint64_t quota_bytes;
    size_t max_entry_bytes;
    uint64_t stale_temp_age_ns;
    XrCacheArtifactVerifier verifier;
    void *verifier_context;
    xr_mutex_t lock;
};

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

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1u;
    char *copy = (char *) xr_malloc(size);
    if (copy)
        memcpy(copy, text, size);
    return copy;
}

static void put_u32(uint8_t *out, uint32_t value) {
    for (size_t i = 0; i < 4u; i++)
        out[i] = (uint8_t) (value >> (i * 8u));
}

static void put_u64(uint8_t *out, uint64_t value) {
    for (size_t i = 0; i < 8u; i++)
        out[i] = (uint8_t) (value >> (i * 8u));
}

static uint32_t take_u32(const uint8_t *input) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4u; i++)
        value |= (uint32_t) input[i] << (i * 8u);
    return value;
}

static uint64_t take_u64(const uint8_t *input) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8u; i++)
        value |= (uint64_t) input[i] << (i * 8u);
    return value;
}

static char *entry_path(const XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key) {
    if (!store || !artifact_kind_valid(kind))
        return NULL;
    char hex[XR_CACHE_KEY_HEX_SIZE];
    xr_cache_key_hex(key, hex);
    return xr_path_join(store->artifact_dirs[kind], hex);
}

char *xr_cache_store_entry_path(const XrCacheStore *store, XrCacheArtifactKind kind,
                                XrCacheKey key) {
    return entry_path(store, kind, key);
}

static bool ensure_directory(const char *path) {
    XrFsStat stat;
    if (xr_fs_stat(path, &stat) == 0)
        return stat.kind == XR_FS_DIR;
    if (xr_fs_mkdir(path, 0700u) != 0)
        return false;
    return xr_fs_stat(path, &stat) == 0 && stat.kind == XR_FS_DIR;
}

static bool cache_layout_is_regular(const XrCacheStore *store) {
    XrFsStat stat;
    if (!store || xr_fs_stat(store->root, &stat) != 0 ||
        stat.kind != XR_FS_DIR)
        return false;
    for (XrCacheArtifactKind kind = XR_CACHE_ARTIFACT_XSM;
         kind <= XR_CACHE_ARTIFACT_XTP; kind++) {
        if (!store->artifact_dirs[kind] ||
            xr_fs_stat(store->artifact_dirs[kind], &stat) != 0 ||
            stat.kind != XR_FS_DIR)
            return false;
    }
    return true;
}

static bool acquire_root_lock(const XrCacheStore *store,
                              XrFsExclusiveLock *root_lock) {
    if (xr_fs_lock_exclusive(store->lock_path, root_lock) != 0)
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
        xr_free(store->artifact_dirs[i]);
    xr_free(store->root);
    xr_free(store->lock_path);
    xr_free(store);
}

XrCacheStore *xr_cache_store_open(const XrCacheStoreConfig *config) {
    if (!config || !config->root || !*config->root || !config->verifier ||
        config->max_entry_bytes == 0 || config->quota_bytes < XR_CACHE_OBJECT_HEADER_SIZE ||
        config->max_entry_bytes > SIZE_MAX - XR_CACHE_OBJECT_HEADER_SIZE ||
        config->max_entry_bytes > config->quota_bytes - XR_CACHE_OBJECT_HEADER_SIZE ||
        config->stale_temp_age_ns == 0)
        return NULL;

    XrCacheStore *store = (XrCacheStore *) xr_calloc(1, sizeof(*store));
    if (!store)
        return NULL;
    store->root = copy_text(config->root);
    store->lock_path = xr_path_join(config->root, ".cache-root.lock");
    store->quota_bytes = config->quota_bytes;
    store->max_entry_bytes = config->max_entry_bytes;
    store->stale_temp_age_ns = config->stale_temp_age_ns;
    store->verifier = config->verifier;
    store->verifier_context = config->verifier_context;
    if (!store->root || !store->lock_path || !ensure_directory(store->root)) {
        cache_store_free(store, false);
        return NULL;
    }

    for (XrCacheArtifactKind kind = XR_CACHE_ARTIFACT_XSM; kind <= XR_CACHE_ARTIFACT_XTP;
         kind++) {
        store->artifact_dirs[kind] = xr_path_join(store->root, artifact_dir_name(kind));
        if (!store->artifact_dirs[kind] || !ensure_directory(store->artifact_dirs[kind])) {
            cache_store_free(store, false);
            return NULL;
        }
    }
    xr_mutex_init(&store->lock);
    if (!xr_cache_store_collect(store, NULL)) {
        cache_store_free(store, true);
        return NULL;
    }
    return store;
}

void xr_cache_store_close(XrCacheStore *store) {
    cache_store_free(store, true);
}

static uint8_t *encode_object(XrCacheArtifactKind kind, XrCacheKey key, const uint8_t *payload,
                              size_t payload_size, size_t *out_size) {
    if (payload_size > SIZE_MAX - XR_CACHE_OBJECT_HEADER_SIZE)
        return NULL;
    size_t object_size = XR_CACHE_OBJECT_HEADER_SIZE + payload_size;
    uint8_t *object = (uint8_t *) xr_malloc(object_size ? object_size : 1u);
    if (!object)
        return NULL;
    memset(object, 0, XR_CACHE_OBJECT_HEADER_SIZE);
    memcpy(object, XR_CACHE_OBJECT_MAGIC, sizeof(XR_CACHE_OBJECT_MAGIC));
    put_u32(object + 8u, XR_CACHE_OBJECT_VERSION);
    put_u32(object + 12u, XR_CACHE_OBJECT_HEADER_SIZE);
    put_u32(object + 16u, (uint32_t) kind);
    put_u64(object + 24u, (uint64_t) payload_size);
    memcpy(object + 32u, key.bytes, XR_CACHE_KEY_BYTES);
    xr_sha256(payload, payload_size, object + 64u);
    if (payload_size != 0)
        memcpy(object + XR_CACHE_OBJECT_HEADER_SIZE, payload, payload_size);
    *out_size = object_size;
    return object;
}

static bool decode_object(const uint8_t *object, size_t object_size, XrCacheArtifactKind kind,
                          XrCacheKey key, const uint8_t **payload, size_t *payload_size) {
    if (!object || object_size < XR_CACHE_OBJECT_HEADER_SIZE ||
        memcmp(object, XR_CACHE_OBJECT_MAGIC, sizeof(XR_CACHE_OBJECT_MAGIC)) != 0 ||
        take_u32(object + 8u) != XR_CACHE_OBJECT_VERSION ||
        take_u32(object + 12u) != XR_CACHE_OBJECT_HEADER_SIZE ||
        take_u32(object + 16u) != (uint32_t) kind || take_u32(object + 20u) != 0 ||
        memcmp(object + 32u, key.bytes, XR_CACHE_KEY_BYTES) != 0)
        return false;
    uint64_t encoded_size = take_u64(object + 24u);
    if (encoded_size > SIZE_MAX || (size_t) encoded_size != object_size - XR_CACHE_OBJECT_HEADER_SIZE)
        return false;
    uint8_t digest[XR_CACHE_KEY_BYTES];
    xr_sha256(object + XR_CACHE_OBJECT_HEADER_SIZE, (size_t) encoded_size, digest);
    if (memcmp(digest, object + 64u, sizeof(digest)) != 0)
        return false;
    *payload = object + XR_CACHE_OBJECT_HEADER_SIZE;
    *payload_size = (size_t) encoded_size;
    return true;
}

static bool remove_cache_path(XrCacheStore *store, XrCacheArtifactKind kind,
                              const char *path) {
    if (xr_fs_remove(path) != 0)
        return false;
    return xr_fs_sync_directory(store->artifact_dirs[kind]) !=
           XR_FS_SYNC_ERROR;
}

static XrCacheLoadStatus load_locked(XrCacheStore *store,
                                     XrCacheArtifactKind kind, XrCacheKey key,
                                     XrCacheBlob *out,
                                     XrCacheObjectSnapshot *snapshot) {
    char *path = entry_path(store, kind, key);
    if (!path)
        return XR_CACHE_LOAD_IO_ERROR;
    if (snapshot)
        memset(snapshot, 0, sizeof(*snapshot));
    XrFsStat stat;
    if (xr_fs_stat(path, &stat) != 0) {
        xr_free(path);
        return XR_CACHE_LOAD_MISS;
    }
    if (stat.kind != XR_FS_FILE) {
        bool removed = remove_cache_path(store, kind, path);
        xr_free(path);
        return removed ? XR_CACHE_LOAD_CORRUPT : XR_CACHE_LOAD_IO_ERROR;
    }
    if (stat.size > (uint64_t) store->max_entry_bytes + XR_CACHE_OBJECT_HEADER_SIZE) {
        bool removed = remove_cache_path(store, kind, path);
        xr_free(path);
        return removed ? XR_CACHE_LOAD_TOO_LARGE : XR_CACHE_LOAD_IO_ERROR;
    }

    uint8_t *object = NULL;
    size_t object_size = 0;
    size_t read_limit = store->max_entry_bytes + XR_CACHE_OBJECT_HEADER_SIZE;
    if (xr_fs_read_regular_file(path, read_limit, &object, &object_size) != 0) {
        xr_free(path);
        return XR_CACHE_LOAD_IO_ERROR;
    }
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    if (!decode_object(object, object_size, kind, key, &payload, &payload_size)) {
        bool removed = remove_cache_path(store, kind, path);
        xr_free(path);
        xr_free(object);
        return removed ? XR_CACHE_LOAD_CORRUPT : XR_CACHE_LOAD_IO_ERROR;
    }
    uint8_t *owned = (uint8_t *) xr_malloc(payload_size ? payload_size : 1u);
    if (!owned) {
        xr_free(path);
        xr_free(object);
        return XR_CACHE_LOAD_IO_ERROR;
    }
    if (payload_size != 0)
        memcpy(owned, payload, payload_size);
    if (snapshot) {
        snapshot->size = object_size;
        xr_sha256(object, object_size, snapshot->digest);
    }
    (void) xr_fs_touch(path);
    xr_free(path);
    xr_free(object);
    out->kind = kind;
    out->key = key;
    out->bytes = owned;
    out->size = payload_size;
    return XR_CACHE_LOAD_HIT;
}

static bool cleanup_rejected_snapshot_locked(
    XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key,
    const XrCacheObjectSnapshot *snapshot) {
    char *path = entry_path(store, kind, key);
    if (!path)
        return false;
    XrFsStat stat;
    if (xr_fs_stat(path, &stat) != 0) {
        xr_free(path);
        return false;
    }
    if (stat.kind != XR_FS_FILE || stat.size != snapshot->size) {
        xr_free(path);
        return true;
    }
    uint8_t *object = NULL;
    size_t object_size = 0;
    bool ok = xr_fs_read_regular_file(path, (size_t) snapshot->size, &object,
                                      &object_size) == 0;
    uint8_t digest[XR_CACHE_KEY_BYTES];
    if (ok)
        xr_sha256(object, object_size, digest);
    bool matches = ok && object_size == snapshot->size &&
                   memcmp(digest, snapshot->digest, sizeof(digest)) == 0;
    xr_free(object);
    if (matches)
        ok = remove_cache_path(store, kind, path);
    xr_free(path);
    return ok;
}

XrCacheLoadStatus xr_cache_store_load(XrCacheStore *store, XrCacheArtifactKind kind,
                                      XrCacheKey key, XrCacheBlob *out) {
    if (!store || !artifact_kind_valid(kind) || !out)
        return XR_CACHE_LOAD_IO_ERROR;
    memset(out, 0, sizeof(*out));
    XrFsExclusiveLock root_lock = {0};
    if (!acquire_root_lock(store, &root_lock))
        return XR_CACHE_LOAD_IO_ERROR;
    XrCacheObjectSnapshot snapshot = {0};
    xr_mutex_lock(&store->lock);
    XrCacheLoadStatus status = load_locked(store, kind, key, out, &snapshot);
    xr_mutex_unlock(&store->lock);
    if (xr_fs_unlock_exclusive(&root_lock) != 0) {
        xr_cache_blob_release(out);
        return XR_CACHE_LOAD_IO_ERROR;
    }
    if (status == XR_CACHE_LOAD_HIT &&
        !store->verifier(kind, key, out->bytes, out->size, store->verifier_context)) {
        xr_cache_blob_release(out);
        XrFsExclusiveLock cleanup_lock = {0};
        if (!acquire_root_lock(store, &cleanup_lock))
            return XR_CACHE_LOAD_IO_ERROR;
        xr_mutex_lock(&store->lock);
        bool cleaned = cleanup_rejected_snapshot_locked(store, kind, key,
                                                        &snapshot);
        xr_mutex_unlock(&store->lock);
        bool unlocked = xr_fs_unlock_exclusive(&cleanup_lock) == 0;
        return cleaned && unlocked ? XR_CACHE_LOAD_REJECTED
                                   : XR_CACHE_LOAD_IO_ERROR;
    }
    return status;
}

void xr_cache_blob_release(XrCacheBlob *blob) {
    if (!blob)
        return;
    xr_free(blob->bytes);
    memset(blob, 0, sizeof(*blob));
}

static char *make_temp_path(const XrCacheStore *store, XrCacheArtifactKind kind, XrCacheKey key) {
    uint8_t random[8];
    char key_hex[XR_CACHE_KEY_HEX_SIZE];
    char name[96];
    static const char digits[] = "0123456789abcdef";
    xr_random_bytes(random, sizeof(random));
    xr_cache_key_hex(key, key_hex);
    int written = snprintf(name, sizeof(name), ".tmp-%s-", key_hex);
    if (written < 0 || (size_t) written + sizeof(random) * 2u + 1u > sizeof(name))
        return NULL;
    size_t offset = (size_t) written;
    for (size_t i = 0; i < sizeof(random); i++) {
        name[offset++] = digits[random[i] >> 4u];
        name[offset++] = digits[random[i] & 0x0fu];
    }
    name[offset] = '\0';
    return xr_path_join(store->artifact_dirs[kind], name);
}

static XrCachePublishStatus compare_existing(XrCacheStore *store, XrCacheArtifactKind kind,
                                             XrCacheKey key, const uint8_t *bytes, size_t size) {
    XrCacheBlob existing = {0};
    XrCacheLoadStatus status = load_locked(store, kind, key, &existing, NULL);
    if (status == XR_CACHE_LOAD_HIT) {
        bool equal = existing.size == size &&
                     (size == 0 || memcmp(existing.bytes, bytes, size) == 0);
        xr_cache_blob_release(&existing);
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
    uint8_t *object = encode_object(kind, key, bytes, size, &object_size);
    if (!object)
        return XR_CACHE_PUBLISH_IO_ERROR;
    char *final_path = entry_path(store, kind, key);
    if (!final_path) {
        xr_free(object);
        return XR_CACHE_PUBLISH_IO_ERROR;
    }

    XrCachePublishStatus result = XR_CACHE_PUBLISH_IO_ERROR;
    for (unsigned attempt = 0; attempt < 2u; attempt++) {
        char *temp_path = NULL;
        for (unsigned candidate = 0; candidate < XR_CACHE_TEMP_ATTEMPTS; candidate++) {
            temp_path = make_temp_path(store, kind, key);
            if (!temp_path)
                break;
            if (xr_fs_write_new_file_sync(temp_path, object, object_size) == 0)
                break;
            xr_free(temp_path);
            temp_path = NULL;
        }
        if (!temp_path)
            break;
        XrFsPublishResult published = xr_fs_publish_noreplace(temp_path, final_path);
        if (published == XR_FS_PUBLISH_OK) {
            XrFsSyncResult sync = xr_fs_sync_directory(store->artifact_dirs[kind]);
            /* MoveFileEx WRITE_THROUGH is the strongest portable Windows
             * publication contract; directory handles have no equivalent
             * flush operation. A later cache miss remains safe recomputation. */
            if (sync != XR_FS_SYNC_ERROR)
                result = XR_CACHE_PUBLISH_OK;
            xr_free(temp_path);
            break;
        }
        (void) xr_fs_remove(temp_path);
        xr_free(temp_path);
        if (published != XR_FS_PUBLISH_EXISTS)
            break;
        result = compare_existing(store, kind, key, bytes, size);
        if (result != XR_CACHE_PUBLISH_OK)
            break;
    }
    xr_free(final_path);
    xr_free(object);
    return result;
}

XrCachePublishStatus xr_cache_store_publish(XrCacheStore *store, XrCacheArtifactKind kind,
                                            XrCacheKey key, const uint8_t *bytes, size_t size) {
    if (!store || !artifact_kind_valid(kind) || (!bytes && size != 0))
        return XR_CACHE_PUBLISH_IO_ERROR;
    if (size > store->max_entry_bytes)
        return XR_CACHE_PUBLISH_TOO_LARGE;
    if (!store->verifier(kind, key, bytes, size, store->verifier_context))
        return XR_CACHE_PUBLISH_REJECTED;

    XrFsExclusiveLock root_lock = {0};
    if (!acquire_root_lock(store, &root_lock))
        return XR_CACHE_PUBLISH_IO_ERROR;
    xr_mutex_lock(&store->lock);
    XrCachePublishStatus status = compare_existing(store, kind, key, bytes, size);
    if (status != XR_CACHE_PUBLISH_OK) {
        xr_mutex_unlock(&store->lock);
        bool unlocked = xr_fs_unlock_exclusive(&root_lock) == 0;
        return unlocked ? status : XR_CACHE_PUBLISH_IO_ERROR;
    }
    uint64_t reservation = (uint64_t) size + XR_CACHE_OBJECT_HEADER_SIZE;
    if (size > SIZE_MAX - XR_CACHE_OBJECT_HEADER_SIZE || reservation > store->quota_bytes) {
        xr_mutex_unlock(&store->lock);
        (void) xr_fs_unlock_exclusive(&root_lock);
        return XR_CACHE_PUBLISH_IO_ERROR;
    }
    XrCacheCollectStats stats = {0};
    uint64_t committed_limit = store->quota_bytes - reservation;
    if (!collect_locked(store, &stats, NULL, committed_limit)) {
        xr_mutex_unlock(&store->lock);
        (void) xr_fs_unlock_exclusive(&root_lock);
        return XR_CACHE_PUBLISH_IO_ERROR;
    }
    status = publish_locked(store, kind, key, bytes, size);
    xr_mutex_unlock(&store->lock);
    bool unlocked = xr_fs_unlock_exclusive(&root_lock) == 0;
    return unlocked ? status : XR_CACHE_PUBLISH_IO_ERROR;
}

static bool append_disk_entry(XrCacheDiskEntry **entries, size_t *count, size_t *capacity,
                              const char *path, const XrFsStat *stat) {
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2u : 16u;
        if (next_capacity < *capacity || next_capacity > SIZE_MAX / sizeof(**entries))
            return false;
        XrCacheDiskEntry *grown =
            (XrCacheDiskEntry *) xr_realloc(*entries, next_capacity * sizeof(**entries));
        if (!grown)
            return false;
        *entries = grown;
        *capacity = next_capacity;
    }
    char *owned_path = copy_text(path);
    if (!owned_path)
        return false;
    (*entries)[*count] = (XrCacheDiskEntry) {
        .path = owned_path,
        .size = stat->size,
        .mtime_ns = stat->mtime_ns,
    };
    (*count)++;
    return true;
}

static bool is_temp_name(const char *name) {
    return strncmp(name, ".tmp-", 5u) == 0;
}

static bool is_entry_name(const char *name) {
    XrCacheKey ignored;
    return xr_cache_key_from_hex(name, &ignored);
}

static bool should_remove_stale_temp(const XrCacheStore *store, const XrFsStat *stat,
                                     uint64_t now_ns) {
    if (stat->mtime_ns <= 0 || now_ns < (uint64_t) stat->mtime_ns)
        return false;
    return now_ns - (uint64_t) stat->mtime_ns >= store->stale_temp_age_ns;
}

static bool scan_directory(XrCacheStore *store, XrCacheArtifactKind kind,
                           XrCacheDiskEntry **entries, size_t *count, size_t *capacity,
                           XrCacheCollectStats *stats, uint64_t now_ns) {
    XrDirIter *iterator = xr_dir_open(store->artifact_dirs[kind]);
    if (!iterator)
        return false;
    bool ok = true;
    XrDirEntry entry;
    while (ok && xr_dir_next(iterator, &entry)) {
        if (entry.is_dir)
            continue;
        char *path = xr_path_join(store->artifact_dirs[kind], entry.name);
        if (!path) {
            ok = false;
            break;
        }
        XrFsStat stat;
        if (xr_fs_stat(path, &stat) != 0 || stat.kind != XR_FS_FILE) {
            if (xr_fs_remove(path) == 0)
                stats->corrupt_entries_removed++;
        } else if (is_temp_name(entry.name)) {
            if (should_remove_stale_temp(store, &stat, now_ns) && xr_fs_remove(path) == 0)
                stats->stale_temps_removed++;
            else if (!append_disk_entry(entries, count, capacity, path, &stat))
                ok = false;
        } else if (!is_entry_name(entry.name)) {
            if (xr_fs_remove(path) == 0)
                stats->corrupt_entries_removed++;
        } else if (!append_disk_entry(entries, count, capacity, path, &stat)) {
            ok = false;
        }
        xr_free(path);
    }
    xr_dir_close(iterator);
    return ok;
}

static int compare_disk_entry(const void *left, const void *right) {
    const XrCacheDiskEntry *a = (const XrCacheDiskEntry *) left;
    const XrCacheDiskEntry *b = (const XrCacheDiskEntry *) right;
    if (a->mtime_ns < b->mtime_ns)
        return -1;
    if (a->mtime_ns > b->mtime_ns)
        return 1;
    return strcmp(a->path, b->path);
}

static bool collect_locked(XrCacheStore *store, XrCacheCollectStats *stats,
                           const char *protected_path, uint64_t byte_limit) {
    XrCacheDiskEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    uint64_t now_ns = xr_time_realtime_ns();
    bool ok = scan_directory(store, XR_CACHE_ARTIFACT_XSM, &entries, &count, &capacity, stats,
                             now_ns) &&
              scan_directory(store, XR_CACHE_ARTIFACT_XTP, &entries, &count, &capacity, stats,
                             now_ns);
    uint64_t total = 0;
    for (size_t i = 0; ok && i < count; i++) {
        if (entries[i].size > UINT64_MAX - total)
            ok = false;
        else
            total += entries[i].size;
    }
    qsort(entries, count, sizeof(*entries), compare_disk_entry);
    for (size_t i = 0; ok && total > byte_limit && i < count; i++) {
        if (protected_path && strcmp(entries[i].path, protected_path) == 0)
            continue;
        if (xr_fs_remove(entries[i].path) != 0) {
            ok = false;
            break;
        }
        total -= entries[i].size;
        stats->removed_bytes += entries[i].size;
        stats->removed_entries++;
        entries[i].size = 0;
    }
    if (total > byte_limit)
        ok = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].size != 0) {
            stats->live_bytes += entries[i].size;
            stats->live_entries++;
        }
        xr_free(entries[i].path);
    }
    xr_free(entries);
    if (ok && (stats->removed_entries != 0 || stats->stale_temps_removed != 0 ||
               stats->corrupt_entries_removed != 0)) {
        for (XrCacheArtifactKind kind = XR_CACHE_ARTIFACT_XSM;
             kind <= XR_CACHE_ARTIFACT_XTP; kind++) {
            if (xr_fs_sync_directory(store->artifact_dirs[kind]) == XR_FS_SYNC_ERROR)
                ok = false;
        }
    }
    return ok;
}

bool xr_cache_store_collect(XrCacheStore *store, XrCacheCollectStats *out) {
    if (!store)
        return false;
    XrFsExclusiveLock root_lock = {0};
    if (!acquire_root_lock(store, &root_lock))
        return false;
    XrCacheCollectStats stats = {0};
    xr_mutex_lock(&store->lock);
    bool ok = collect_locked(store, &stats, NULL, store->quota_bytes);
    xr_mutex_unlock(&store->lock);
    if (xr_fs_unlock_exclusive(&root_lock) != 0)
        ok = false;
    if (out)
        *out = stats;
    return ok;
}
