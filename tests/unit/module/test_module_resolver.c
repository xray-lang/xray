/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_resolver.c - Unit tests for unified module resolver
 */

#include "../test_framework.h"
#include "module/xmodule_resolver.h"
#include "base/xhashmap.h"
#include "base/xmalloc.h"
#include "base/xfileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ========== Test Fixtures ========== */

static char g_tmpdir[512];

/* Create a temporary directory tree for file-resolution tests. */
static void setup_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/xray_test_resolver_XXXXXX");
    char *d = mkdtemp(g_tmpdir);
    ASSERT_NOT_NULL(d);
}

/* Recursively remove the temp directory. */
static void teardown_tmpdir(void) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    (void) system(cmd);
}

/* Create a file under g_tmpdir.  Parent dirs are created automatically. */
static void create_file(const char *rel_path, const char *content) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", g_tmpdir, rel_path);

    /* Ensure parent dirs exist */
    char dir[1024];
    strncpy(dir, full, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char mkd[1100];
        snprintf(mkd, sizeof(mkd), "mkdir -p %s", dir);
        (void) system(mkd);
    }

    FILE *f = fopen(full, "w");
    if (!f) {
        fprintf(stderr, "create_file: cannot create %s\n", full);
        abort();
    }
    if (content)
        fputs(content, f);
    fclose(f);
}

/* Build an absolute importer path from a relative path under g_tmpdir. */
static void make_importer(char *buf, size_t bufsz, const char *rel) {
    snprintf(buf, bufsz, "%s/%s", g_tmpdir, rel);
}

/* ========== Lifecycle Tests ========== */

TEST(resolver_new_free) {
    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    ASSERT_NOT_NULL(r);
    xr_module_resolver_free(r);
}

/* ========== Stdlib Resolution Tests ========== */

TEST(resolve_bare_stdlib_known) {
    XrHashMap *loaders = xr_hashmap_new();
    xr_hashmap_set(loaders, "time", (void *) 1);
    xr_hashmap_set(loaders, "math", (void *) 1);

    XrModuleResolverConfig cfg = {.native_loaders = loaders, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "time", true, NULL, &mid, &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(mid.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(mid.canonical, "time");
    ASSERT_NULL(mid.source_path);
    xr_module_id_cleanup(&mid);

    rc = xr_module_resolver_resolve(r, "math", true, NULL, &mid, &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(mid.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(mid.canonical, "math");
    xr_module_id_cleanup(&mid);

    xr_module_resolver_free(r);
    xr_hashmap_free(loaders);
}

TEST(resolve_bare_stdlib_unknown) {
    XrHashMap *loaders = xr_hashmap_new();
    xr_hashmap_set(loaders, "time", (void *) 1);

    XrModuleResolverConfig cfg = {.native_loaders = loaders, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "nosuchmodule", true, NULL, &mid, &err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_NOT_NULL(err);
    xr_free(err);

    xr_module_resolver_free(r);
    xr_hashmap_free(loaders);
}

/* ========== Relative File Resolution Tests ========== */

TEST(resolve_relative_file) {
    setup_tmpdir();
    create_file("src/main.xr", "// entry");
    create_file("src/utils.xr", "// utils");

    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    char importer[1024];
    make_importer(importer, sizeof(importer), "src/main.xr");

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "./utils", false, importer, &mid, &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(mid.kind, XR_MOD_FILE);
    ASSERT_NOT_NULL(mid.source_path);
    /* Should end with /src/utils.xr */
    ASSERT_NOT_NULL(strstr(mid.source_path, "utils.xr"));
    xr_module_id_cleanup(&mid);

    xr_module_resolver_free(r);
    teardown_tmpdir();
}

TEST(resolve_relative_dir_index) {
    setup_tmpdir();
    create_file("src/main.xr", "// entry");
    create_file("src/models/index.xr", "// models index");

    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    char importer[1024];
    make_importer(importer, sizeof(importer), "src/main.xr");

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "./models", false, importer, &mid, &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(mid.kind, XR_MOD_FILE);
    ASSERT_NOT_NULL(mid.source_path);
    ASSERT_NOT_NULL(strstr(mid.source_path, "models/index.xr"));
    xr_module_id_cleanup(&mid);

    xr_module_resolver_free(r);
    teardown_tmpdir();
}

TEST(resolve_relative_not_found) {
    setup_tmpdir();
    create_file("src/main.xr", "// entry");

    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    char importer[1024];
    make_importer(importer, sizeof(importer), "src/main.xr");

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "./nonexist", false, importer, &mid, &err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_NOT_NULL(err);
    xr_free(err);

    xr_module_resolver_free(r);
    teardown_tmpdir();
}

TEST(resolve_parent_relative) {
    setup_tmpdir();
    create_file("lib/helper.xr", "// helper");
    create_file("src/deep/nested.xr", "// nested");

    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    char importer[1024];
    make_importer(importer, sizeof(importer), "src/deep/nested.xr");

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "../../lib/helper", false, importer, &mid, &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(mid.kind, XR_MOD_FILE);
    ASSERT_NOT_NULL(strstr(mid.source_path, "helper.xr"));
    xr_module_id_cleanup(&mid);

    xr_module_resolver_free(r);
    teardown_tmpdir();
}

/* ========== Cache Tests ========== */

TEST(resolve_caches_results) {
    setup_tmpdir();
    create_file("src/main.xr", "// entry");
    create_file("src/utils.xr", "// utils");

    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    char importer[1024];
    make_importer(importer, sizeof(importer), "src/main.xr");

    XrModuleId mid1, mid2;
    char *err = NULL;
    int rc1 = xr_module_resolver_resolve(r, "./utils", false, importer, &mid1, &err);
    ASSERT_EQ_INT(rc1, 0);

    int rc2 = xr_module_resolver_resolve(r, "./utils", false, importer, &mid2, &err);
    ASSERT_EQ_INT(rc2, 0);

    /* Both should resolve to the same canonical path */
    ASSERT_STR_EQ(mid1.source_path, mid2.source_path);

    xr_module_id_cleanup(&mid1);
    xr_module_id_cleanup(&mid2);
    xr_module_resolver_free(r);
    teardown_tmpdir();
}

/* ========== XrModuleId Cleanup Tests ========== */

TEST(module_id_cleanup_null_safe) {
    XrModuleId mid = {.kind = XR_MOD_STDLIB, .canonical = NULL, .source_path = NULL};
    xr_module_id_cleanup(&mid);
    ASSERT_NULL(mid.canonical);
    ASSERT_NULL(mid.source_path);

    xr_module_id_cleanup(NULL);
}

/* ========== Quoted Non-Relative (Project-Relative) Tests ========== */

TEST(resolve_quoted_single_segment) {
    setup_tmpdir();
    create_file("src/main.xr", "// entry");
    create_file("src/config.xr", "// config");

    XrModuleResolverConfig cfg = {.native_loaders = NULL, .stdlib_path = NULL, .lockfile = NULL};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);

    char importer[1024];
    make_importer(importer, sizeof(importer), "src/main.xr");

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(r, "config", false, importer, &mid, &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(mid.kind, XR_MOD_FILE);
    ASSERT_NOT_NULL(strstr(mid.source_path, "config.xr"));
    xr_module_id_cleanup(&mid);

    xr_module_resolver_free(r);
    teardown_tmpdir();
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Lifecycle");
RUN_TEST(resolver_new_free);

RUN_TEST_SUITE("Stdlib Resolution");
RUN_TEST(resolve_bare_stdlib_known);
RUN_TEST(resolve_bare_stdlib_unknown);

RUN_TEST_SUITE("Relative File Resolution");
RUN_TEST(resolve_relative_file);
RUN_TEST(resolve_relative_dir_index);
RUN_TEST(resolve_relative_not_found);
RUN_TEST(resolve_parent_relative);

RUN_TEST_SUITE("Cache");
RUN_TEST(resolve_caches_results);

RUN_TEST_SUITE("Module ID");
RUN_TEST(module_id_cleanup_null_safe);

RUN_TEST_SUITE("Project-Relative Resolution");
RUN_TEST(resolve_quoted_single_segment);

TEST_MAIN_END()
