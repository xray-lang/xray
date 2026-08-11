/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_fs_atomic.c - No-follow and immutable filesystem primitive tests
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "os/os_fs.h"
#include "os/os_temp.h"
#include "os/os_time.h"

#include <stdio.h>
#include <string.h>

#if defined(XR_OS_WINDOWS)
#include <windows.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#else
#include <unistd.h>
#endif

static char test_root[XR_PATH_MAX];

static bool test_path(char *out, size_t out_size, const char *name) {
    int written = snprintf(out, out_size, "%s/%s", test_root, name);
    return written >= 0 && (size_t) written < out_size;
}

static bool create_file_symlink(const char *link_path, const char *target_path) {
#if defined(XR_OS_WINDOWS)
    return CreateSymbolicLinkA(link_path, target_path,
                               SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
#else
    return symlink(target_path, link_path) == 0;
#endif
}

static void remove_test_root(void) {
#if defined(XR_OS_WINDOWS)
    (void) RemoveDirectoryA(test_root);
#else
    (void) rmdir(test_root);
#endif
}

TEST(fs_private_root) {
    ASSERT_EQ_INT(xr_temp_dir_create("xray-fs-atomic", test_root, sizeof(test_root)), 0);
    ASSERT_TRUE(xr_fs_is_dir(test_root));
}

TEST(fs_complete_publish) {
    static const uint8_t payload[] = {'c', 'a', 'c', 'h', 'e'};
    char temp[XR_PATH_MAX];
    char final[XR_PATH_MAX];
    ASSERT_TRUE(test_path(temp, sizeof(temp), "complete.tmp"));
    ASSERT_TRUE(test_path(final, sizeof(final), "complete.obj"));
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(temp, payload, sizeof(payload)), 0);
    ASSERT_EQ_INT(xr_fs_publish_noreplace(temp, final), XR_FS_PUBLISH_OK);
    ASSERT_FALSE(xr_fs_exists(temp));
    ASSERT_TRUE(xr_fs_is_file(final));

    XrFsSyncResult sync = xr_fs_sync_directory(test_root);
#if defined(XR_OS_WINDOWS)
    ASSERT_EQ_INT(sync, XR_FS_SYNC_UNSUPPORTED);
#else
    ASSERT_EQ_INT(sync, XR_FS_SYNC_OK);
#endif

    uint8_t *bytes = NULL;
    size_t size = 0;
    ASSERT_EQ_INT(xr_fs_read_regular_file(final, sizeof(payload), &bytes, &size), 0);
    ASSERT_EQ_UINT(size, sizeof(payload));
    ASSERT_MEM_EQ(bytes, payload, sizeof(payload));
    xr_free(bytes);
    ASSERT_EQ_INT(xr_fs_remove(final), 0);
}

TEST(fs_existing_publish_does_not_replace) {
    static const uint8_t original[] = {'o', 'l', 'd'};
    static const uint8_t replacement[] = {'n', 'e', 'w'};
    char first_temp[XR_PATH_MAX];
    char second_temp[XR_PATH_MAX];
    char final[XR_PATH_MAX];
    ASSERT_TRUE(test_path(first_temp, sizeof(first_temp), "existing-first.tmp"));
    ASSERT_TRUE(test_path(second_temp, sizeof(second_temp), "existing-second.tmp"));
    ASSERT_TRUE(test_path(final, sizeof(final), "existing.obj"));
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(first_temp, original, sizeof(original)), 0);
    ASSERT_EQ_INT(xr_fs_publish_noreplace(first_temp, final), XR_FS_PUBLISH_OK);
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(second_temp, replacement, sizeof(replacement)), 0);
    ASSERT_EQ_INT(xr_fs_publish_noreplace(second_temp, final), XR_FS_PUBLISH_EXISTS);
    ASSERT_TRUE(xr_fs_is_file(second_temp));

    uint8_t *bytes = NULL;
    size_t size = 0;
    ASSERT_EQ_INT(xr_fs_read_regular_file(final, sizeof(original), &bytes, &size), 0);
    ASSERT_EQ_UINT(size, sizeof(original));
    ASSERT_MEM_EQ(bytes, original, sizeof(original));
    xr_free(bytes);
    ASSERT_EQ_INT(xr_fs_remove(second_temp), 0);
    ASSERT_EQ_INT(xr_fs_remove(final), 0);
}

TEST(fs_oversize_read_is_rejected) {
    static const uint8_t payload[] = {'t', 'o', 'o', '-', 'l', 'a', 'r', 'g', 'e'};
    char path[XR_PATH_MAX];
    ASSERT_TRUE(test_path(path, sizeof(path), "oversize.obj"));
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(path, payload, sizeof(payload)), 0);
    uint8_t *bytes = (uint8_t *) (uintptr_t) 1;
    size_t size = 99;
    ASSERT_EQ_INT(xr_fs_read_regular_file(path, sizeof(payload) - 1, &bytes, &size), -1);
    ASSERT_NULL(bytes);
    ASSERT_EQ_UINT(size, 0);
    ASSERT_EQ_INT(xr_fs_remove(path), 0);
}

TEST(fs_touch_updates_regular_file) {
    static const uint8_t payload[] = {'x'};
    char path[XR_PATH_MAX];
    ASSERT_TRUE(test_path(path, sizeof(path), "touch.obj"));
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(path, payload, sizeof(payload)), 0);
    XrFsStat before;
    XrFsStat after;
    ASSERT_EQ_INT(xr_fs_stat(path, &before), 0);
    xr_time_sleep_ms(20);
    ASSERT_EQ_INT(xr_fs_touch(path), 0);
    ASSERT_EQ_INT(xr_fs_stat(path, &after), 0);
    ASSERT_EQ_INT(after.kind, XR_FS_FILE);
    ASSERT_GE(after.mtime_ns, before.mtime_ns);
    ASSERT_EQ_UINT(after.size, before.size);
    ASSERT_EQ_INT(xr_fs_remove(path), 0);
}

TEST(fs_symlink_is_not_followed) {
    static const uint8_t payload[] = {'t', 'a', 'r', 'g', 'e', 't'};
    char target[XR_PATH_MAX];
    char link_path[XR_PATH_MAX];
    ASSERT_TRUE(test_path(target, sizeof(target), "symlink-target.obj"));
    ASSERT_TRUE(test_path(link_path, sizeof(link_path), "symlink.obj"));
    ASSERT_EQ_INT(xr_fs_write_new_file_sync(target, payload, sizeof(payload)), 0);
    if (create_file_symlink(link_path, target)) {
        XrFsStat stat;
        ASSERT_EQ_INT(xr_fs_stat(link_path, &stat), 0);
        ASSERT_EQ_INT(stat.kind, XR_FS_OTHER);
        uint8_t *bytes = NULL;
        size_t size = 0;
        ASSERT_EQ_INT(xr_fs_read_regular_file(link_path, sizeof(payload), &bytes, &size), -1);
        ASSERT_NULL(bytes);
        ASSERT_EQ_UINT(size, 0);
        ASSERT_EQ_INT(xr_fs_touch(link_path), -1);
        ASSERT_EQ_INT(xr_fs_remove(link_path), 0);
    }
    ASSERT_EQ_INT(xr_fs_remove(target), 0);
}

TEST(fs_remove_private_root) {
    remove_test_root();
    ASSERT_FALSE(xr_fs_exists(test_root));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Atomic filesystem primitives");
RUN_TEST(fs_private_root);
RUN_TEST(fs_complete_publish);
RUN_TEST(fs_existing_publish_does_not_replace);
RUN_TEST(fs_oversize_read_is_rejected);
RUN_TEST(fs_touch_updates_regular_file);
RUN_TEST(fs_symlink_is_not_followed);
RUN_TEST(fs_remove_private_root);
TEST_MAIN_END()
