/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cache_store.c - Immutable cache storage integrity tests
 */

#include "incremental/xr_cache_store.h"
#include "os/os_dir.h"
#include "os/os_fs.h"
#include "os/os_temp.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "test_framework.h"

static bool verify_artifact(XrCacheArtifactKind kind, XrCacheKey key, const uint8_t *bytes,
                            size_t size, void *context) {
    (void) kind;
    (void) key;
    (void) context;
    return size == 0 || (bytes && bytes[0] != 0xffu);
}

static XrCacheKey cache_key(const char *text) {
    XrCacheFingerprint fingerprint;
    XrCacheKey key;
    xr_cache_fingerprint_bytes((const uint8_t *) text, strlen(text), &fingerprint);
    memcpy(key.bytes, fingerprint.bytes, sizeof(key.bytes));
    return key;
}

static XrCacheStore *open_store(const char *root, uint64_t quota, size_t max_entry,
                                uint64_t stale_age_ns) {
    XrCacheStoreConfig config = {
        .root = root,
        .quota_bytes = quota,
        .max_entry_bytes = max_entry,
        .stale_temp_age_ns = stale_age_ns,
        .verifier = verify_artifact,
        .verifier_context = NULL,
    };
    return xr_cache_store_open(&config);
}

static void clear_directory(const char *path) {
    XrDirIter *iterator = xr_dir_open(path);
    if (iterator) {
        XrDirEntry entry;
        while (xr_dir_next(iterator, &entry)) {
            if (entry.is_dir)
                continue;
            char *child = xr_path_join(path, entry.name);
            if (child) {
                (void) xr_fs_remove(child);
                xr_free(child);
            }
        }
        xr_dir_close(iterator);
    }
    (void) xr_test_rmdir(path);
}

static void remove_store_root(const char *root) {
    char *xsm = xr_path_join(root, "xsm");
    char *xtp = xr_path_join(root, "xtp");
    if (xsm)
        clear_directory(xsm);
    if (xtp)
        clear_directory(xtp);
    xr_free(xsm);
    xr_free(xtp);
    (void) xr_test_rmdir(root);
}

static bool create_root(char root[XR_PATH_MAX]) {
    return xr_temp_dir_create("xray-cache-test", root, XR_PATH_MAX) == 0;
}

TEST(publish_load_and_existing_object_are_immutable) {
    char root[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    XrCacheStore *store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    ASSERT_NOT_NULL(store);
    XrCacheKey key = cache_key("one");
    const uint8_t payload[] = "verified-plan";

    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key, payload,
                                         sizeof(payload)),
                  XR_CACHE_PUBLISH_OK);
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key, payload,
                                         sizeof(payload)),
                  XR_CACHE_PUBLISH_EXISTS);
    const uint8_t conflicting[] = "different-plan";
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key, conflicting,
                                         sizeof(conflicting)),
                  XR_CACHE_PUBLISH_CONFLICT);

    XrCacheBlob blob;
    ASSERT_EQ_INT(xr_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, key, &blob),
                  XR_CACHE_LOAD_HIT);
    ASSERT_EQ_UINT(blob.size, sizeof(payload));
    ASSERT_MEM_EQ(blob.bytes, payload, sizeof(payload));
    xr_cache_blob_release(&blob);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST(corruption_and_unverified_payload_fail_closed) {
    char root[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    XrCacheStore *store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    ASSERT_NOT_NULL(store);
    XrCacheKey key = cache_key("corrupt");
    const uint8_t payload[] = "valid";
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key, payload,
                                         sizeof(payload)),
                  XR_CACHE_PUBLISH_OK);

    char *path = xr_cache_store_entry_path(store, XR_CACHE_ARTIFACT_XSM, key);
    ASSERT_NOT_NULL(path);
    uint8_t *disk = NULL;
    size_t disk_size = 0;
    ASSERT_EQ_INT(xr_fs_read_regular_file(path, 4096u, &disk, &disk_size), 0);
    ASSERT_GT(disk_size, sizeof(payload));
    disk[disk_size - 1u] ^= 0x80u;
    ASSERT_EQ_INT(xr_fs_remove(path), 0);
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(path, disk, disk_size), 0);
    xr_free(disk);

    XrCacheBlob blob;
    ASSERT_EQ_INT(xr_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, key, &blob),
                  XR_CACHE_LOAD_CORRUPT);
    ASSERT_FALSE(xr_fs_exists(path));
    const uint8_t rejected[] = {0xffu, 0u};
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key, rejected,
                                         sizeof(rejected)),
                  XR_CACHE_PUBLISH_REJECTED);
    xr_free(path);
    xr_cache_store_close(store);
    remove_store_root(root);
}

typedef struct PublishThreadContext {
    XrCacheStore *store;
    XrCacheKey key;
    const uint8_t *payload;
    size_t size;
    XrCachePublishStatus status;
} PublishThreadContext;

static void *publish_thread(void *argument) {
    PublishThreadContext *context = (PublishThreadContext *) argument;
    context->status = xr_cache_store_publish(context->store, XR_CACHE_ARTIFACT_XTP, context->key,
                                             context->payload, context->size);
    return NULL;
}

TEST(concurrent_same_key_writers_publish_one_complete_object) {
    char root[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    XrCacheStore *first_store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    XrCacheStore *second_store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    ASSERT_NOT_NULL(first_store);
    ASSERT_NOT_NULL(second_store);
    const uint8_t payload[] = "same-target-plan";
    PublishThreadContext contexts[2] = {
        {.store = first_store,
         .key = cache_key("parallel"),
         .payload = payload,
         .size = sizeof(payload)},
        {.store = second_store,
         .key = cache_key("parallel"),
         .payload = payload,
         .size = sizeof(payload)},
    };
    xr_thread_t threads[2];
    ASSERT_TRUE(xr_thread_create(&threads[0], publish_thread, &contexts[0]));
    ASSERT_TRUE(xr_thread_create(&threads[1], publish_thread, &contexts[1]));
    ASSERT_EQ_INT(xr_thread_join(threads[0], NULL), 0);
    ASSERT_EQ_INT(xr_thread_join(threads[1], NULL), 0);
    ASSERT_TRUE((contexts[0].status == XR_CACHE_PUBLISH_OK &&
                 contexts[1].status == XR_CACHE_PUBLISH_EXISTS) ||
                (contexts[1].status == XR_CACHE_PUBLISH_OK &&
                 contexts[0].status == XR_CACHE_PUBLISH_EXISTS));

    XrCacheBlob blob;
    ASSERT_EQ_INT(xr_cache_store_load(first_store, XR_CACHE_ARTIFACT_XTP, contexts[0].key, &blob),
                  XR_CACHE_LOAD_HIT);
    ASSERT_MEM_EQ(blob.bytes, payload, sizeof(payload));
    xr_cache_blob_release(&blob);
    xr_cache_store_close(second_store);
    xr_cache_store_close(first_store);
    remove_store_root(root);
}

TEST(quota_evicts_oldest_entry_and_stale_temps_recover) {
    char root[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    XrCacheStore *store = open_store(root, 250u, 128u, 1u);
    ASSERT_NOT_NULL(store);
    uint8_t payload[24];
    memset(payload, 7, sizeof(payload));
    XrCacheKey first = cache_key("first");
    XrCacheKey second = cache_key("second");
    XrCacheKey third = cache_key("third");
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, first, payload,
                                         sizeof(payload)),
                  XR_CACHE_PUBLISH_OK);
    xr_time_sleep_ms(2u);
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, second, payload,
                                         sizeof(payload)),
                  XR_CACHE_PUBLISH_OK);
    xr_time_sleep_ms(2u);
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, third, payload,
                                         sizeof(payload)),
                  XR_CACHE_PUBLISH_OK);

    XrCacheBlob blob;
    ASSERT_EQ_INT(xr_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, first, &blob),
                  XR_CACHE_LOAD_MISS);
    ASSERT_EQ_INT(xr_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, third, &blob),
                  XR_CACHE_LOAD_HIT);
    xr_cache_blob_release(&blob);

    char *xsm_dir = xr_path_join(root, "xsm");
    char *temp = xr_path_join(xsm_dir, ".tmp-stale");
    ASSERT_NOT_NULL(temp);
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(temp, payload, sizeof(payload)), 0);
    xr_time_sleep_ms(1u);
    XrCacheCollectStats stats;
    ASSERT_TRUE(xr_cache_store_collect(store, &stats));
    ASSERT_FALSE(xr_fs_exists(temp));
    ASSERT_GE(stats.stale_temps_removed, 1u);
    ASSERT_LE(stats.live_bytes, 250u);
    xr_free(temp);
    xr_free(xsm_dir);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST_MAIN_BEGIN()
    RUN_TEST(publish_load_and_existing_object_are_immutable);
    RUN_TEST(corruption_and_unverified_payload_fail_closed);
    RUN_TEST(concurrent_same_key_writers_publish_one_complete_object);
    RUN_TEST(quota_evicts_oldest_entry_and_stale_temps_recover);
TEST_MAIN_END()
