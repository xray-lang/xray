/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lockfile.c - Unit tests for lockfile module
 */

#include "../test_framework.h"
#include "module/xlockfile.h"
#include "base/xmalloc.h"
#include <unistd.h>

/* ========== Lockfile Creation Tests ========== */

TEST(lockfile_new) {
    XrLockfile *lock = xr_lockfile_new();
    ASSERT_NOT_NULL(lock);
    ASSERT_EQ_INT(lock->version, 1);
    ASSERT_EQ_INT(lock->package_count, 0);
    xr_lockfile_free(lock);
}

TEST(lockfile_add_package) {
    XrLockfile *lock = xr_lockfile_new();
    ASSERT_NOT_NULL(lock);

    bool ok = xr_lockfile_add_package(lock, "xray/redis", "1.2.3",
                                       "https://pkg.xray-lang.org/xray/redis/1.2.3",
                                       "sha256:abc123");
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(lock->package_count, 1);

    const XrLockedPackage *pkg = xr_lockfile_find(lock, "xray/redis");
    ASSERT_NOT_NULL(pkg);
    ASSERT_STR_EQ(pkg->name, "xray/redis");
    ASSERT_STR_EQ(pkg->version, "1.2.3");
    ASSERT_STR_EQ(pkg->resolved, "https://pkg.xray-lang.org/xray/redis/1.2.3");
    ASSERT_STR_EQ(pkg->checksum, "sha256:abc123");

    xr_lockfile_free(lock);
}

TEST(lockfile_add_duplicate_updates) {
    XrLockfile *lock = xr_lockfile_new();

    xr_lockfile_add_package(lock, "xray/redis", "1.0.0", "", "");
    xr_lockfile_add_package(lock, "xray/redis", "2.0.0", "url2", "sha2");

    ASSERT_EQ_INT(lock->package_count, 1);

    const XrLockedPackage *pkg = xr_lockfile_find(lock, "xray/redis");
    ASSERT_NOT_NULL(pkg);
    ASSERT_STR_EQ(pkg->version, "2.0.0");
    ASSERT_STR_EQ(pkg->resolved, "url2");

    xr_lockfile_free(lock);
}

TEST(lockfile_has) {
    XrLockfile *lock = xr_lockfile_new();
    xr_lockfile_add_package(lock, "xray/redis", "1.0.0", "", "");

    ASSERT_TRUE(xr_lockfile_has(lock, "xray/redis"));
    ASSERT_FALSE(xr_lockfile_has(lock, "xray/http"));

    xr_lockfile_free(lock);
}

TEST(lockfile_remove) {
    XrLockfile *lock = xr_lockfile_new();
    xr_lockfile_add_package(lock, "xray/redis", "1.0.0", "", "");
    xr_lockfile_add_package(lock, "xray/http", "2.0.0", "", "");

    ASSERT_EQ_INT(lock->package_count, 2);

    ASSERT_TRUE(xr_lockfile_remove(lock, "xray/redis"));
    ASSERT_EQ_INT(lock->package_count, 1);
    ASSERT_FALSE(xr_lockfile_has(lock, "xray/redis"));
    ASSERT_TRUE(xr_lockfile_has(lock, "xray/http"));

    // Remove non-existent
    ASSERT_FALSE(xr_lockfile_remove(lock, "xray/redis"));

    xr_lockfile_free(lock);
}

TEST(lockfile_add_dependency) {
    XrLockfile *lock = xr_lockfile_new();
    xr_lockfile_add_package(lock, "xray/redis", "1.0.0", "", "");

    ASSERT_TRUE(xr_lockfile_add_dependency(lock, "xray/redis", "xray/net@^1.0.0"));
    ASSERT_TRUE(xr_lockfile_add_dependency(lock, "xray/redis", "xray/json@^2.0.0"));

    const XrLockedPackage *pkg = xr_lockfile_find(lock, "xray/redis");
    ASSERT_NOT_NULL(pkg);
    ASSERT_EQ_INT(pkg->dep_count, 2);
    ASSERT_STR_EQ(pkg->dependencies[0], "xray/net@^1.0.0");
    ASSERT_STR_EQ(pkg->dependencies[1], "xray/json@^2.0.0");

    // Add dep to non-existent package
    ASSERT_FALSE(xr_lockfile_add_dependency(lock, "nonexist", "dep@1.0"));

    xr_lockfile_free(lock);
}

/* ========== Lockfile Save/Load Tests ========== */

TEST(lockfile_save_and_load) {
    const char *test_path = "/tmp/xray_test_lockfile.lock";

    // Create and save
    XrLockfile *lock = xr_lockfile_new();
    xr_lockfile_add_package(lock, "xray/redis", "1.2.3",
                            "https://pkg.xray-lang.org/dl/redis-1.2.3.tar.gz",
                            "sha256:deadbeef");
    xr_lockfile_add_dependency(lock, "xray/redis", "xray/net@^1.0.0");

    xr_lockfile_add_package(lock, "xray/http", "2.0.0", "", "");

    ASSERT_TRUE(xr_lockfile_save(lock, test_path));
    xr_lockfile_free(lock);

    // Load and verify
    XrLockfile *loaded = xr_lockfile_load(test_path);
    ASSERT_NOT_NULL(loaded);
    ASSERT_EQ_INT(loaded->package_count, 2);

    const XrLockedPackage *redis = xr_lockfile_find(loaded, "xray/redis");
    ASSERT_NOT_NULL(redis);
    ASSERT_STR_EQ(redis->version, "1.2.3");
    ASSERT_STR_EQ(redis->resolved, "https://pkg.xray-lang.org/dl/redis-1.2.3.tar.gz");
    ASSERT_STR_EQ(redis->checksum, "sha256:deadbeef");
    ASSERT_EQ_INT(redis->dep_count, 1);
    ASSERT_STR_EQ(redis->dependencies[0], "xray/net@^1.0.0");

    const XrLockedPackage *http = xr_lockfile_find(loaded, "xray/http");
    ASSERT_NOT_NULL(http);
    ASSERT_STR_EQ(http->version, "2.0.0");

    xr_lockfile_free(loaded);

    // Cleanup
    unlink(test_path);
}

TEST(lockfile_load_nonexistent) {
    XrLockfile *lock = xr_lockfile_load("/tmp/nonexistent_xray_lockfile.lock");
    ASSERT_NULL(lock);
}

/* ========== Edge Cases ========== */

TEST(lockfile_null_args) {
    ASSERT_FALSE(xr_lockfile_add_package(NULL, "pkg", "1.0", "", ""));
    ASSERT_FALSE(xr_lockfile_add_dependency(NULL, "pkg", "dep"));
    ASSERT_NULL(xr_lockfile_find(NULL, "pkg"));
    ASSERT_FALSE(xr_lockfile_has(NULL, "pkg"));
    ASSERT_FALSE(xr_lockfile_remove(NULL, "pkg"));
    ASSERT_FALSE(xr_lockfile_save(NULL, "/tmp/x"));

    // Free NULL should be safe
    xr_lockfile_free(NULL);
}

TEST(lockfile_many_packages) {
    XrLockfile *lock = xr_lockfile_new();
    char name[64];

    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "xray/pkg%d", i);
        ASSERT_TRUE(xr_lockfile_add_package(lock, name, "1.0.0", "", ""));
    }

    ASSERT_EQ_INT(lock->package_count, 100);

    // Verify some lookups
    ASSERT_TRUE(xr_lockfile_has(lock, "xray/pkg0"));
    ASSERT_TRUE(xr_lockfile_has(lock, "xray/pkg99"));
    ASSERT_FALSE(xr_lockfile_has(lock, "xray/pkg100"));

    xr_lockfile_free(lock);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("Lockfile Creation");
    RUN_TEST(lockfile_new);
    RUN_TEST(lockfile_add_package);
    RUN_TEST(lockfile_add_duplicate_updates);
    RUN_TEST(lockfile_has);
    RUN_TEST(lockfile_remove);
    RUN_TEST(lockfile_add_dependency);

    RUN_TEST_SUITE("Lockfile Save/Load");
    RUN_TEST(lockfile_save_and_load);
    RUN_TEST(lockfile_load_nonexistent);

    RUN_TEST_SUITE("Lockfile Edge Cases");
    RUN_TEST(lockfile_null_args);
    RUN_TEST(lockfile_many_packages);

TEST_MAIN_END()
