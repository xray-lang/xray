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
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
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
    bool created = CreateSymbolicLinkA(link_path, target_path,
                               SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    printf("file symlink created=%u\n", (unsigned)created);
    return created;
#else
    return symlink(target_path, link_path) == 0;
#endif
}

static bool create_dir_symlink(const char *link_path, const char *target_path) {
#if defined(XR_OS_WINDOWS)
    bool created = CreateSymbolicLinkA(link_path, target_path,
                               SYMBOLIC_LINK_FLAG_DIRECTORY |
                                   SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    printf("directory symlink created=%u\n", (unsigned)created);
    return created;
#else
    return symlink(target_path, link_path) == 0;
#endif
}

static void remove_dir(const char *path) {
#if defined(XR_OS_WINDOWS)
    (void) RemoveDirectoryA(path);
#else
    (void) rmdir(path);
#endif
}

// A directory symlink is removed with RemoveDirectoryA on Windows, not
// DeleteFileA, so the remove primitive (which maps to the latter) cannot retire one.
static void remove_dir_symlink(const char *path) {
#if defined(XR_OS_WINDOWS)
    (void) RemoveDirectoryA(path);
#else
    (void) unlink(path);
#endif
}

static void remove_test_root(void) {
#if defined(XR_OS_WINDOWS)
    (void) RemoveDirectoryA(test_root);
#else
    (void) rmdir(test_root);
#endif
}

static XrOsIoPolicy fs_policy;
static bool fs_query(XrOsIoStatus (*query)(const XrOsIoPolicy *, const char *, bool *), const char *path) {
    bool found = false;
    XrOsIoStatus status = query(&fs_policy, path, &found);
    if (status == XR_OS_IO_NOT_FOUND) return false;
    if (status != XR_OS_IO_OK) {
        fprintf(stderr, "filesystem query failed: %u\n", (unsigned)status);
        exit(1);
    }
    return found;
}

TEST(fs_private_root) {
    fs_policy = xr_os_io_system_policy();
    ASSERT_EQ_INT(xr_temp_dir_create("xray-fs-atomic", test_root, sizeof(test_root)), 0);
    ASSERT_TRUE(fs_query(xr_os_io_is_dir, test_root));
}

TEST(fs_complete_publish) {
    static const uint8_t payload[] = {'c', 'a', 'c', 'h', 'e'};
    char temp[XR_PATH_MAX];
    char final[XR_PATH_MAX];
    ASSERT_TRUE(test_path(temp, sizeof(temp), "complete.tmp"));
    ASSERT_TRUE(test_path(final, sizeof(final), "complete.obj"));
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, temp, payload, sizeof(payload)), 0);
    ASSERT_EQ_INT(xr_os_io_publish_noreplace(&fs_policy, temp, final), XR_OS_IO_OK);
    ASSERT_FALSE(fs_query(xr_os_io_exists, temp));
    ASSERT_TRUE(fs_query(xr_os_io_is_file, final));

    XrOsIoStatus sync = xr_os_io_sync_directory(&fs_policy, test_root);
#if defined(XR_OS_WINDOWS)
    ASSERT_EQ_INT(sync, XR_OS_IO_UNSUPPORTED);
#else
    ASSERT_EQ_INT(sync, XR_OS_IO_OK);
#endif

    uint8_t *bytes = NULL;
    size_t size = 0;
    ASSERT_EQ_INT(xr_os_io_read_regular_file(&fs_policy, final, sizeof(payload), &bytes, &size), 0);
    ASSERT_EQ_UINT(size, sizeof(payload));
    ASSERT_MEM_EQ(bytes, payload, sizeof(payload));
    xr_free(bytes);
    ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, final), 0);
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
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, first_temp, original, sizeof(original)), 0);
    ASSERT_EQ_INT(xr_os_io_publish_noreplace(&fs_policy, first_temp, final), XR_OS_IO_OK);
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, second_temp, replacement, sizeof(replacement)), 0);
    ASSERT_EQ_INT(xr_os_io_publish_noreplace(&fs_policy, second_temp, final), XR_OS_IO_EXISTS);
    ASSERT_TRUE(fs_query(xr_os_io_is_file, second_temp));

    uint8_t *bytes = NULL;
    size_t size = 0;
    ASSERT_EQ_INT(xr_os_io_read_regular_file(&fs_policy, final, sizeof(original), &bytes, &size), 0);
    ASSERT_EQ_UINT(size, sizeof(original));
    ASSERT_MEM_EQ(bytes, original, sizeof(original));
    xr_free(bytes);
    ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, second_temp), 0);
    ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, final), 0);
}

TEST(fs_oversize_read_is_rejected) {
    static const uint8_t payload[] = {'t', 'o', 'o', '-', 'l', 'a', 'r', 'g', 'e'};
    char path[XR_PATH_MAX];
    ASSERT_TRUE(test_path(path, sizeof(path), "oversize.obj"));
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, path, payload, sizeof(payload)), 0);
    uint8_t *bytes = (uint8_t *) (uintptr_t) 1;
    size_t size = 99;
    ASSERT_EQ_INT(xr_os_io_read_regular_file(&fs_policy, path, sizeof(payload) - 1, &bytes, &size), XR_OS_IO_OVERFLOW);
    ASSERT_EQ_PTR(bytes, (uint8_t *)(uintptr_t)1);
    ASSERT_EQ_UINT(size, 99);
    ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, path), 0);
}

TEST(fs_touch_updates_regular_file) {
    static const uint8_t payload[] = {'x'};
    char path[XR_PATH_MAX];
    ASSERT_TRUE(test_path(path, sizeof(path), "touch.obj"));
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, path, payload, sizeof(payload)), 0);
    XrFsStat before;
    XrFsStat after;
    ASSERT_EQ_INT(xr_os_io_stat(&fs_policy, path, &before), 0);
    xr_time_sleep_ms(20);
    ASSERT_EQ_INT(xr_os_io_touch(&fs_policy, path), 0);
    ASSERT_EQ_INT(xr_os_io_stat(&fs_policy, path, &after), 0);
    ASSERT_EQ_INT(after.kind, XR_FS_FILE);
    ASSERT_GE(after.mtime_ns, before.mtime_ns);
    ASSERT_EQ_UINT(after.size, before.size);
    ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, path), 0);
}

TEST(fs_symlink_is_not_followed) {
    static const uint8_t payload[] = {'t', 'a', 'r', 'g', 'e', 't'};
    char target[XR_PATH_MAX];
    char link_path[XR_PATH_MAX];
    ASSERT_TRUE(test_path(target, sizeof(target), "symlink-target.obj"));
    ASSERT_TRUE(test_path(link_path, sizeof(link_path), "symlink.obj"));
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, target, payload, sizeof(payload)), 0);
    if (create_file_symlink(link_path, target)) {
        XrFsStat stat;
        ASSERT_EQ_INT(xr_os_io_stat(&fs_policy, link_path, &stat), 0);
        ASSERT_EQ_INT(stat.kind, XR_FS_OTHER);
        uint8_t *bytes = NULL;
        size_t size = 0;
        ASSERT_NE(xr_os_io_read_regular_file(&fs_policy, link_path, sizeof(payload), &bytes, &size), XR_OS_IO_OK);
        ASSERT_NULL(bytes);
        ASSERT_EQ_UINT(size, 0);
        ASSERT_NE(xr_os_io_touch(&fs_policy, link_path), XR_OS_IO_OK);
        ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, link_path), 0);
    }
    ASSERT_EQ_INT(xr_os_io_remove(&fs_policy, target), 0);
}

// Regression: `xray build --native` could not create its AOT cache under macOS
// /tmp or /var/folders, because both are symlinks to /private/... and mkdir's
// already-exists recovery used to classify the link with lstat. Ensuring a
// directory has to resolve its final component -- an interior symlink is one
// ordinary path resolution follows anyway -- while the leaf no-follow rule that
// fs_symlink_is_not_followed pins stays exactly as strict as it was.
TEST(fs_mkdir_accepts_symlinked_directory) {
    char target[XR_PATH_MAX];
    char link_path[XR_PATH_MAX];
    char nested[XR_PATH_MAX];
    char nested_real[XR_PATH_MAX];
    ASSERT_TRUE(test_path(target, sizeof(target), "mkdir-target.dir"));
    ASSERT_TRUE(test_path(link_path, sizeof(link_path), "mkdir-link.dir"));
    ASSERT_TRUE(test_path(nested, sizeof(nested), "mkdir-link.dir/nested"));
    ASSERT_TRUE(test_path(nested_real, sizeof(nested_real), "mkdir-target.dir/nested"));

    // Observe everything first, then clean up, then assert. A failing assertion
    // returns from the test, so asserting with directories still on disk would
    // strand test_root and cascade into a misleading fs_remove_private_root
    // failure that buries the real one.
    XrFsStat link_stat = {0};
    int link_stat_rc = -1;
    bool link_reads_as_dir = true;
    int ensure_link = -1;
    int ensure_nested = -1;
    int ensure_nested_again = -1;
    bool landed_on_target = false;

    int ensure_target = xr_os_io_mkdir(&fs_policy, target, 0755);
    bool target_is_dir = fs_query(xr_os_io_is_dir, target);
    bool linked = create_dir_symlink(link_path, target);
    if (linked) {
        link_stat_rc = xr_os_io_stat(&fs_policy, link_path, &link_stat);
        link_reads_as_dir = fs_query(xr_os_io_is_dir, link_path);
        ensure_link = xr_os_io_mkdir(&fs_policy, link_path, 0755);
        ensure_nested = xr_os_io_mkdir(&fs_policy, nested, 0755);
        ensure_nested_again = xr_os_io_mkdir(&fs_policy, nested, 0755);
        landed_on_target = fs_query(xr_os_io_is_dir, nested_real);
        remove_dir(nested_real);
        remove_dir_symlink(link_path);
    }
    remove_dir(target);

    ASSERT_EQ_INT(ensure_target, 0);
    ASSERT_TRUE(target_is_dir);
    if (linked) {
        // The link is still classified as a leaf that is not followed.
        ASSERT_EQ_INT(link_stat_rc, 0);
        ASSERT_EQ_INT(link_stat.kind, XR_FS_OTHER);
        ASSERT_FALSE(link_reads_as_dir);
        // ...yet ensuring it succeeds, because it resolves to a directory.
        ASSERT_EQ_INT(ensure_link, 0);
        // And a directory can be created through it, landing in the real
        // target -- which is what directory component walks require.
        ASSERT_EQ_INT(ensure_nested, 0);
        ASSERT_EQ_INT(ensure_nested_again, 0);  // idempotent second pass
        ASSERT_TRUE(landed_on_target);
    }
    ASSERT_FALSE(fs_query(xr_os_io_exists, target));
}

TEST(fs_mkdir_rejects_links_that_miss_a_directory) {
    static const uint8_t payload[] = {'f', 'i', 'l', 'e'};
    char file_target[XR_PATH_MAX];
    char file_link[XR_PATH_MAX];
    char dangling[XR_PATH_MAX];
    char missing[XR_PATH_MAX];
    ASSERT_TRUE(test_path(file_target, sizeof(file_target), "mkdir-file.obj"));
    ASSERT_TRUE(test_path(file_link, sizeof(file_link), "mkdir-file-link.obj"));
    ASSERT_TRUE(test_path(dangling, sizeof(dangling), "mkdir-dangling.dir"));
    ASSERT_TRUE(test_path(missing, sizeof(missing), "mkdir-absent.dir"));
    ASSERT_EQ_INT(xr_os_io_write_new_file_sync(&fs_policy, file_target, payload, sizeof(payload)), 0);

    // Observe, clean up, then assert -- see fs_mkdir_accepts_symlinked_directory.
    int ensure_file_link = -1;
    int remove_file_link = 0;
    int ensure_dangling = -1;
    bool file_linked = create_file_symlink(file_link, file_target);
    if (file_linked) {
        ensure_file_link = xr_os_io_mkdir(&fs_policy, file_link, 0755);
        remove_file_link = xr_os_io_remove(&fs_policy, file_link);
    }
    bool dangling_linked = create_dir_symlink(dangling, missing);
    if (dangling_linked) {
        ensure_dangling = xr_os_io_mkdir(&fs_policy, dangling, 0755);
        remove_dir_symlink(dangling);
    }
    int ensure_plain_file = xr_os_io_mkdir(&fs_policy, file_target, 0755);
    int remove_file_target = xr_os_io_remove(&fs_policy, file_target);

    // A link that lands on a regular file is not a directory.
    if (file_linked) {
        ASSERT_NE(ensure_file_link, XR_OS_IO_OK);
        ASSERT_EQ_INT(remove_file_link, 0);
    }
    // Neither is one that resolves to nothing at all.
    if (dangling_linked) {
        ASSERT_NE(ensure_dangling, XR_OS_IO_OK);
    }
    // A plain regular file is still refused.
    ASSERT_NE(ensure_plain_file, XR_OS_IO_OK);
    ASSERT_EQ_INT(remove_file_target, 0);
}

TEST(fs_remove_private_root) {
    remove_test_root();
    ASSERT_FALSE(fs_query(xr_os_io_exists, test_root));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Atomic filesystem primitives");
RUN_TEST(fs_private_root);
RUN_TEST(fs_complete_publish);
RUN_TEST(fs_existing_publish_does_not_replace);
RUN_TEST(fs_oversize_read_is_rejected);
RUN_TEST(fs_touch_updates_regular_file);
RUN_TEST(fs_symlink_is_not_followed);
RUN_TEST(fs_mkdir_accepts_symlinked_directory);
RUN_TEST(fs_mkdir_rejects_links_that_miss_a_directory);
RUN_TEST(fs_remove_private_root);
TEST_MAIN_END()
