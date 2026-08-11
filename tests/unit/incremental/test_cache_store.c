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
#include "os/os_proc.h"
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

static XrCacheStore *open_store_with_verifier(const char *root, uint64_t quota,
                                              size_t max_entry, uint64_t stale_age_ns,
                                              XrCacheArtifactVerifier verifier, void *context) {
    XrCacheStoreConfig config = {
        .root = root,
        .quota_bytes = quota,
        .max_entry_bytes = max_entry,
        .stale_temp_age_ns = stale_age_ns,
        .verifier = verifier,
        .verifier_context = context,
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
    clear_directory(root);
}

static bool create_root(char root[XR_PATH_MAX]) {
    return xr_temp_dir_create("xray-cache-test", root, XR_PATH_MAX) == 0;
}

enum {
    CHILD_PUBLISH_EXISTS = 10,
    CHILD_PUBLISH_CONFLICT = 11,
    CHILD_OPERATION_FAILED = 20,
};

typedef struct BlockingVerifierContext {
    const char *ready_path;
    const char *release_path;
} BlockingVerifierContext;

static bool write_signal(const char *path) {
    static const uint8_t marker = 1u;
    return xr_fs_write_new_file_sync(path, &marker, sizeof(marker)) == 0;
}

static bool wait_for_path(const char *path, uint64_t timeout_ms) {
    uint64_t deadline = xr_time_monotonic_ms() + timeout_ms;
    while (xr_time_monotonic_ms() < deadline) {
        if (xr_fs_exists(path))
            return true;
        xr_time_sleep_ms(5u);
    }
    return xr_fs_exists(path);
}

static bool blocking_load_verifier(XrCacheArtifactKind kind, XrCacheKey key,
                                   const uint8_t *bytes, size_t size, void *context) {
    BlockingVerifierContext *blocking = (BlockingVerifierContext *) context;
    if (!blocking || !write_signal(blocking->ready_path) ||
        !wait_for_path(blocking->release_path, 5000u)) {
        return false;
    }
    return verify_artifact(kind, key, bytes, size, NULL);
}

static int child_publish(const char *root, const char *key_text, const char *payload) {
    XrCacheStore *store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    if (!store)
        return CHILD_OPERATION_FAILED;
    XrCachePublishStatus status =
        xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, cache_key(key_text),
                               (const uint8_t *) payload, strlen(payload) + 1u);
    xr_cache_store_close(store);
    if (status == XR_CACHE_PUBLISH_OK)
        return 0;
    if (status == XR_CACHE_PUBLISH_EXISTS)
        return CHILD_PUBLISH_EXISTS;
    if (status == XR_CACHE_PUBLISH_CONFLICT)
        return CHILD_PUBLISH_CONFLICT;
    return CHILD_OPERATION_FAILED;
}

static int child_hold_lock(const char *root, const char *ready_path, const char *release_path) {
    char *lock_path = xr_path_join(root, ".cache-root.lock");
    if (!lock_path)
        return CHILD_OPERATION_FAILED;
    XrFsExclusiveLock lock = {0};
    bool acquired = xr_fs_lock_exclusive(lock_path, &lock) == 0;
    bool ready = acquired && write_signal(ready_path);
    bool released = ready && wait_for_path(release_path, 5000u);
    bool unlocked = acquired && xr_fs_unlock_exclusive(&lock) == 0;
    xr_free(lock_path);
    return ready && released && unlocked ? 0 : CHILD_OPERATION_FAILED;
}

static int child_load_blocking(const char *root, const char *key_text, const char *payload,
                               const char *ready_path, const char *release_path) {
    BlockingVerifierContext verifier_context = {
        .ready_path = ready_path,
        .release_path = release_path,
    };
    XrCacheStore *store = open_store_with_verifier(
        root, 4096u, 512u, UINT64_C(1000000000), blocking_load_verifier, &verifier_context);
    if (!store)
        return CHILD_OPERATION_FAILED;
    XrCacheBlob blob = {0};
    XrCacheLoadStatus status =
        xr_cache_store_load(store, XR_CACHE_ARTIFACT_XSM, cache_key(key_text), &blob);
    size_t expected_size = strlen(payload) + 1u;
    bool complete = status == XR_CACHE_LOAD_HIT && blob.size == expected_size &&
                    memcmp(blob.bytes, payload, expected_size) == 0;
    xr_cache_blob_release(&blob);
    xr_cache_store_close(store);
    return complete ? 0 : CHILD_OPERATION_FAILED;
}

static int child_cleanup(const char *root, const char *key_text) {
    XrCacheStore *store = open_store(root, 112u, 16u, UINT64_C(1000000000));
    if (!store)
        return CHILD_OPERATION_FAILED;
    XrCacheCollectStats stats = {0};
    char *path = xr_cache_store_entry_path(store, XR_CACHE_ARTIFACT_XSM, cache_key(key_text));
    bool cleaned = path && xr_cache_store_collect(store, &stats) && !xr_fs_exists(path);
    xr_free(path);
    xr_cache_store_close(store);
    return cleaned ? 0 : CHILD_OPERATION_FAILED;
}

static int run_child_mode(int argc, char **argv) {
    if (argc <= 1)
        return -1;
    if (strcmp(argv[1], "--cache-child-publish") == 0 && argc == 5)
        return child_publish(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "--cache-child-publish-signaled") == 0 && argc == 6)
        return write_signal(argv[5]) ? child_publish(argv[2], argv[3], argv[4])
                                     : CHILD_OPERATION_FAILED;
    if (strcmp(argv[1], "--cache-child-hold-lock") == 0 && argc == 5)
        return child_hold_lock(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "--cache-child-load-blocking") == 0 && argc == 7)
        return child_load_blocking(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (strcmp(argv[1], "--cache-child-cleanup") == 0 && argc == 4)
        return child_cleanup(argv[2], argv[3]);
    return CHILD_OPERATION_FAILED;
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

TEST(cross_process_conflicting_writers_publish_one_complete_object) {
    char root[XR_PATH_MAX];
    char executable[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    ASSERT_EQ_INT(xr_proc_self_exe_path(executable, sizeof(executable)), 0);
    const char *first_argv[] = {executable, "--cache-child-publish", root, "process-key",
                                "first-process-payload", NULL};
    const char *second_argv[] = {executable, "--cache-child-publish", root, "process-key",
                                 "second-process-payload", NULL};
    XrProcId first = xr_proc_spawn(executable, first_argv);
    XrProcId second = xr_proc_spawn(executable, second_argv);
    int first_exit = -1;
    int second_exit = -1;
    int first_wait = xr_proc_wait(first, &first_exit);
    int second_wait = xr_proc_wait(second, &second_exit);

    ASSERT_EQ_INT(first_wait, 0);
    ASSERT_EQ_INT(second_wait, 0);
    ASSERT_TRUE((first_exit == 0 && second_exit == CHILD_PUBLISH_CONFLICT) ||
                (second_exit == 0 && first_exit == CHILD_PUBLISH_CONFLICT));
    XrCacheStore *store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    ASSERT_NOT_NULL(store);
    XrCacheBlob blob = {0};
    ASSERT_EQ_INT(xr_cache_store_load(store, XR_CACHE_ARTIFACT_XSM,
                                      cache_key("process-key"), &blob),
                  XR_CACHE_LOAD_HIT);
    static const char first_payload[] = "first-process-payload";
    static const char second_payload[] = "second-process-payload";
    ASSERT_TRUE((blob.size == sizeof(first_payload) &&
                 memcmp(blob.bytes, first_payload, sizeof(first_payload)) == 0) ||
                (blob.size == sizeof(second_payload) &&
                 memcmp(blob.bytes, second_payload, sizeof(second_payload)) == 0));
    xr_cache_blob_release(&blob);
    xr_cache_store_close(store);
    remove_store_root(root);
}

TEST(cross_process_root_lock_blocks_publish_until_release) {
    char root[XR_PATH_MAX];
    char executable[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    ASSERT_EQ_INT(xr_proc_self_exe_path(executable, sizeof(executable)), 0);
    char *holder_ready = xr_path_join(root, "holder-ready");
    char *publisher_started = xr_path_join(root, "publisher-started");
    char *release = xr_path_join(root, "holder-release");
    ASSERT_NOT_NULL(holder_ready);
    ASSERT_NOT_NULL(publisher_started);
    ASSERT_NOT_NULL(release);

    const char *holder_argv[] = {executable, "--cache-child-hold-lock", root, holder_ready,
                                 release, NULL};
    XrProcId holder = xr_proc_spawn(executable, holder_argv);
    bool holder_acquired = wait_for_path(holder_ready, 5000u);
    const char *publisher_argv[] = {executable, "--cache-child-publish-signaled", root,
                                    "locked-key", "locked-payload", publisher_started, NULL};
    XrProcId publisher = holder_acquired ? xr_proc_spawn(executable, publisher_argv)
                                         : XR_PROC_INVALID;
    bool publisher_entered = wait_for_path(publisher_started, 5000u);
    xr_time_sleep_ms(50u);
    int publisher_exit = -1;
    XrProcWaitResult before_release = xr_proc_try_wait(publisher, &publisher_exit);
    bool release_written = write_signal(release);
    int holder_exit = -1;
    int holder_wait = xr_proc_wait(holder, &holder_exit);
    int publisher_wait = before_release == XR_PROC_WAIT_RUNNING
                             ? xr_proc_wait(publisher, &publisher_exit)
                             : (before_release == XR_PROC_WAIT_EXITED ? 0 : -1);

    xr_free(release);
    xr_free(publisher_started);
    xr_free(holder_ready);
    remove_store_root(root);
    ASSERT_TRUE(holder_acquired);
    ASSERT_TRUE(publisher_entered);
    ASSERT_EQ_INT(before_release, XR_PROC_WAIT_RUNNING);
    ASSERT_TRUE(release_written);
    ASSERT_EQ_INT(holder_wait, 0);
    ASSERT_EQ_INT(holder_exit, 0);
    ASSERT_EQ_INT(publisher_wait, 0);
    ASSERT_EQ_INT(publisher_exit, 0);
}

TEST(cross_process_loaded_blob_survives_cleanup_after_locked_read) {
    char root[XR_PATH_MAX];
    char executable[XR_PATH_MAX];
    ASSERT_TRUE(create_root(root));
    ASSERT_EQ_INT(xr_proc_self_exe_path(executable, sizeof(executable)), 0);
    static const char payload[] = "reader-owned-payload-that-exceeds-cleaner-entry-limit";
    XrCacheKey key = cache_key("reader-cleanup-key");
    XrCacheStore *store = open_store(root, 4096u, 512u, UINT64_C(1000000000));
    ASSERT_NOT_NULL(store);
    ASSERT_EQ_INT(xr_cache_store_publish(store, XR_CACHE_ARTIFACT_XSM, key,
                                         (const uint8_t *) payload, sizeof(payload)),
                  XR_CACHE_PUBLISH_OK);
    char *entry = xr_cache_store_entry_path(store, XR_CACHE_ARTIFACT_XSM, key);
    ASSERT_NOT_NULL(entry);
    xr_cache_store_close(store);

    char *reader_ready = xr_path_join(root, "reader-ready");
    char *reader_release = xr_path_join(root, "reader-release");
    ASSERT_NOT_NULL(reader_ready);
    ASSERT_NOT_NULL(reader_release);
    const char *reader_argv[] = {executable,
                                 "--cache-child-load-blocking",
                                 root,
                                 "reader-cleanup-key",
                                 payload,
                                 reader_ready,
                                 reader_release,
                                 NULL};
    XrProcId reader = xr_proc_spawn(executable, reader_argv);
    bool reader_has_owned_bytes = wait_for_path(reader_ready, 5000u);
    const char *cleaner_argv[] = {executable, "--cache-child-cleanup", root,
                                  "reader-cleanup-key", NULL};
    XrProcId cleaner = reader_has_owned_bytes ? xr_proc_spawn(executable, cleaner_argv)
                                              : XR_PROC_INVALID;
    int cleaner_exit = -1;
    int cleaner_wait = xr_proc_wait(cleaner, &cleaner_exit);
    bool removed = !xr_fs_exists(entry);
    bool release_written = write_signal(reader_release);
    int reader_exit = -1;
    int reader_wait = xr_proc_wait(reader, &reader_exit);

    xr_free(reader_release);
    xr_free(reader_ready);
    xr_free(entry);
    remove_store_root(root);
    ASSERT_TRUE(reader_has_owned_bytes);
    ASSERT_EQ_INT(cleaner_wait, 0);
    ASSERT_EQ_INT(cleaner_exit, 0);
    ASSERT_TRUE(removed);
    ASSERT_TRUE(release_written);
    ASSERT_EQ_INT(reader_wait, 0);
    ASSERT_EQ_INT(reader_exit, 0);
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
    int child_status = run_child_mode(argc, argv);
    if (child_status >= 0) {
        XR_TEST_PROCESS_SHUTDOWN();
        return child_status;
    }
    RUN_TEST(publish_load_and_existing_object_are_immutable);
    RUN_TEST(corruption_and_unverified_payload_fail_closed);
    RUN_TEST(concurrent_same_key_writers_publish_one_complete_object);
    RUN_TEST(cross_process_conflicting_writers_publish_one_complete_object);
    RUN_TEST(cross_process_root_lock_blocks_publish_until_release);
    RUN_TEST(cross_process_loaded_blob_survives_cleanup_after_locked_read);
    RUN_TEST(quota_evicts_oldest_entry_and_stale_temps_recover);
TEST_MAIN_END()
