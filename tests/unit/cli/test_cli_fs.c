/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_fs.c - Unit tests for xcli_fs module
 *
 * Tests file I/O, path queries, file list, directory traversal,
 * and safe parsing helpers.
 */

#include "../test_framework.h"
#include "app/cli/xcli_fs.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <windows.h>
#define test_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define test_mkdir(p) mkdir(p, 0755)
#endif

/* ========== Helper: Create temp directory tree ========== */

static char g_tmpdir[256];

static void make_tmp_tree(void) {
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    GetTempPathA(sizeof(tmpdir), tmpdir);
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%sxr_test_cli_fs", tmpdir);
    _mkdir(g_tmpdir);
#else
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/xr_test_cli_fs_XXXXXX");
    char *r = mkdtemp(g_tmpdir);
    (void)r;
#endif

    /* Create structure:
     *   g_tmpdir/
     *     hello.xr
     *     readme.txt
     *     .hidden/
     *       secret.xr
     *     sub/
     *       foo.xr
     *       bar.xr
     *     node_modules/
     *       pkg.xr
     *     build/
     *       out.xr
     */
    char path[512];

    snprintf(path, sizeof(path), "%s/hello.xr", g_tmpdir);
    FILE *f = fopen(path, "wb"); fprintf(f, "print(1)\n"); fclose(f);

    snprintf(path, sizeof(path), "%s/readme.txt", g_tmpdir);
    f = fopen(path, "wb"); fprintf(f, "doc\n"); fclose(f);

    snprintf(path, sizeof(path), "%s/.hidden", g_tmpdir);
    test_mkdir(path);
    snprintf(path, sizeof(path), "%s/.hidden/secret.xr", g_tmpdir);
    f = fopen(path, "wb"); fprintf(f, "hidden\n"); fclose(f);

    snprintf(path, sizeof(path), "%s/sub", g_tmpdir);
    test_mkdir(path);
    snprintf(path, sizeof(path), "%s/sub/foo.xr", g_tmpdir);
    f = fopen(path, "wb"); fprintf(f, "foo\n"); fclose(f);
    snprintf(path, sizeof(path), "%s/sub/bar.xr", g_tmpdir);
    f = fopen(path, "wb"); fprintf(f, "bar\n"); fclose(f);

    snprintf(path, sizeof(path), "%s/node_modules", g_tmpdir);
    test_mkdir(path);
    snprintf(path, sizeof(path), "%s/node_modules/pkg.xr", g_tmpdir);
    f = fopen(path, "wb"); fprintf(f, "pkg\n"); fclose(f);

    snprintf(path, sizeof(path), "%s/build", g_tmpdir);
    test_mkdir(path);
    snprintf(path, sizeof(path), "%s/build/out.xr", g_tmpdir);
    f = fopen(path, "wb"); fprintf(f, "out\n"); fclose(f);
}

static void rm_rf(const char *dir) {
    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" >nul 2>&1", dir);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
#endif
    (void)system(cmd);
}

/* ========== File I/O Tests ========== */

TEST(read_file_success) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/hello.xr", g_tmpdir);
    char *content = xr_cli_read_file(path);
    ASSERT_NOT_NULL(content);
    ASSERT_STR_EQ(content, "print(1)\n");
    xr_free(content);
    rm_rf(g_tmpdir);
}

TEST(read_file_nonexistent) {
    char *content = xr_cli_read_file("/tmp/xr_test_nonexistent_file_12345.xr");
    ASSERT_NULL(content);
}

TEST(write_file_and_read_back) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/write_test.txt", g_tmpdir);
    int rc = xr_cli_write_file(path, "hello world\n");
    ASSERT_EQ_INT(rc, 0);
    char *content = xr_cli_read_file(path);
    ASSERT_NOT_NULL(content);
    ASSERT_STR_EQ(content, "hello world\n");
    xr_free(content);
    rm_rf(g_tmpdir);
}

TEST(write_file_atomic_and_read_back) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/atomic_test.txt", g_tmpdir);
    int rc = xr_cli_write_file_atomic(path, "atomic content\n");
    ASSERT_EQ_INT(rc, 0);
    char *content = xr_cli_read_file(path);
    ASSERT_NOT_NULL(content);
    ASSERT_STR_EQ(content, "atomic content\n");
    xr_free(content);
    rm_rf(g_tmpdir);
}

/* ========== Path Query Tests ========== */

TEST(file_exists_true) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/hello.xr", g_tmpdir);
    ASSERT_TRUE(xr_cli_file_exists(path));
    rm_rf(g_tmpdir);
}

TEST(file_exists_false) {
    ASSERT_FALSE(xr_cli_file_exists("/tmp/xr_test_nonexistent_12345"));
}

TEST(is_directory_true) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/sub", g_tmpdir);
    ASSERT_TRUE(xr_cli_is_directory(path));
    rm_rf(g_tmpdir);
}

TEST(is_directory_false_on_file) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/hello.xr", g_tmpdir);
    ASSERT_FALSE(xr_cli_is_directory(path));
    rm_rf(g_tmpdir);
}

TEST(is_xr_file_true) {
    ASSERT_TRUE(xr_cli_is_xr_file("hello.xr"));
    ASSERT_TRUE(xr_cli_is_xr_file("path/to/test.xr"));
}

TEST(is_xr_file_false) {
    ASSERT_FALSE(xr_cli_is_xr_file("readme.txt"));
    ASSERT_FALSE(xr_cli_is_xr_file("xr"));
    ASSERT_FALSE(xr_cli_is_xr_file(".xr"));
}

/* ========== File List Tests ========== */

TEST(filelist_init_empty) {
    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    ASSERT_EQ_INT(fl.count, 0);
    ASSERT_NULL(fl.paths);
    xr_cli_filelist_free(&fl);
}

TEST(filelist_add_and_count) {
    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    xr_cli_filelist_add(&fl, "/a.xr");
    xr_cli_filelist_add(&fl, "/b.xr");
    xr_cli_filelist_add(&fl, "/c.xr");
    ASSERT_EQ_INT(fl.count, 3);
    ASSERT_STR_EQ(fl.paths[0], "/a.xr");
    ASSERT_STR_EQ(fl.paths[1], "/b.xr");
    ASSERT_STR_EQ(fl.paths[2], "/c.xr");
    xr_cli_filelist_free(&fl);
}

TEST(filelist_sort) {
    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    xr_cli_filelist_add(&fl, "/z.xr");
    xr_cli_filelist_add(&fl, "/a.xr");
    xr_cli_filelist_add(&fl, "/m.xr");
    xr_cli_filelist_sort(&fl);
    ASSERT_STR_EQ(fl.paths[0], "/a.xr");
    ASSERT_STR_EQ(fl.paths[1], "/m.xr");
    ASSERT_STR_EQ(fl.paths[2], "/z.xr");
    xr_cli_filelist_free(&fl);
}

/* ========== Directory Traversal Tests ========== */

TEST(collect_single_file) {
    make_tmp_tree();
    char path[512];
    snprintf(path, sizeof(path), "%s/hello.xr", g_tmpdir);

    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    XrCliWalkOpts opts = xr_cli_walk_defaults();

    int rc = xr_cli_collect_files(path, &opts, &fl);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(fl.count, 1);

    xr_cli_filelist_free(&fl);
    rm_rf(g_tmpdir);
}

TEST(collect_directory_xr_only) {
    make_tmp_tree();

    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    XrCliWalkOpts opts = xr_cli_walk_defaults();

    int rc = xr_cli_collect_files(g_tmpdir, &opts, &fl);
    ASSERT_EQ_INT(rc, 0);

    /* Should find: hello.xr, sub/foo.xr, sub/bar.xr = 3 files.
     * Should NOT find: .hidden/secret.xr, node_modules/pkg.xr,
     * build/out.xr, readme.txt */
    ASSERT_EQ_INT(fl.count, 3);

    /* Verify ignore rules worked */
    for (int i = 0; i < fl.count; i++) {
        ASSERT_NULL(strstr(fl.paths[i], ".hidden"));
        ASSERT_NULL(strstr(fl.paths[i], "node_modules"));
        ASSERT_NULL(strstr(fl.paths[i], "build"));
        ASSERT_NULL(strstr(fl.paths[i], "readme.txt"));
    }

    xr_cli_filelist_free(&fl);
    rm_rf(g_tmpdir);
}

TEST(collect_skips_hidden) {
    make_tmp_tree();

    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    XrCliWalkOpts opts = xr_cli_walk_defaults();
    opts.skip_hidden = true;

    int rc = xr_cli_collect_files(g_tmpdir, &opts, &fl);
    ASSERT_EQ_INT(rc, 0);

    for (int i = 0; i < fl.count; i++) {
        ASSERT_NULL(strstr(fl.paths[i], ".hidden"));
    }

    xr_cli_filelist_free(&fl);
    rm_rf(g_tmpdir);
}

TEST(collect_nonexistent_returns_error) {
    XrCliFileList fl;
    xr_cli_filelist_init(&fl);
    XrCliWalkOpts opts = xr_cli_walk_defaults();

    int rc = xr_cli_collect_files("/tmp/xr_does_not_exist_98765", &opts, &fl);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_EQ_INT(fl.count, 0);

    xr_cli_filelist_free(&fl);
}

/* ========== Safe Parsing Tests ========== */

TEST(parse_int_valid) {
    int val = 0;
    ASSERT_TRUE(xr_cli_parse_int("42", &val));
    ASSERT_EQ_INT(val, 42);

    ASSERT_TRUE(xr_cli_parse_int("-7", &val));
    ASSERT_EQ_INT(val, -7);

    ASSERT_TRUE(xr_cli_parse_int("0", &val));
    ASSERT_EQ_INT(val, 0);
}

TEST(parse_int_invalid) {
    int val = 0;
    ASSERT_FALSE(xr_cli_parse_int("abc", &val));
    ASSERT_FALSE(xr_cli_parse_int("", &val));
    ASSERT_FALSE(xr_cli_parse_int("12x", &val));
}

TEST(parse_int_null_rejected) {
    int val = 0;
    ASSERT_FALSE(xr_cli_parse_int(NULL, &val));
}

TEST(parse_port_valid) {
    int port = 0;
    ASSERT_TRUE(xr_cli_parse_port("8080", &port));
    ASSERT_EQ_INT(port, 8080);

    ASSERT_TRUE(xr_cli_parse_port("0", &port));
    ASSERT_EQ_INT(port, 0);

    ASSERT_TRUE(xr_cli_parse_port("65535", &port));
    ASSERT_EQ_INT(port, 65535);
}

TEST(parse_port_invalid) {
    int port = 0;
    ASSERT_FALSE(xr_cli_parse_port("-1", &port));
    ASSERT_FALSE(xr_cli_parse_port("65536", &port));
    ASSERT_FALSE(xr_cli_parse_port("abc", &port));
    ASSERT_FALSE(xr_cli_parse_port("", &port));
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("File I/O");
    RUN_TEST(read_file_success);
    RUN_TEST(read_file_nonexistent);
    RUN_TEST(write_file_and_read_back);
    RUN_TEST(write_file_atomic_and_read_back);

    RUN_TEST_SUITE("Path Queries");
    RUN_TEST(file_exists_true);
    RUN_TEST(file_exists_false);
    RUN_TEST(is_directory_true);
    RUN_TEST(is_directory_false_on_file);
    RUN_TEST(is_xr_file_true);
    RUN_TEST(is_xr_file_false);

    RUN_TEST_SUITE("File List");
    RUN_TEST(filelist_init_empty);
    RUN_TEST(filelist_add_and_count);
    RUN_TEST(filelist_sort);

    RUN_TEST_SUITE("Directory Traversal");
    RUN_TEST(collect_single_file);
    RUN_TEST(collect_directory_xr_only);
    RUN_TEST(collect_skips_hidden);
    RUN_TEST(collect_nonexistent_returns_error);

    RUN_TEST_SUITE("Safe Parsing");
    RUN_TEST(parse_int_valid);
    RUN_TEST(parse_int_invalid);
    RUN_TEST(parse_int_null_rejected);
    RUN_TEST(parse_port_valid);
    RUN_TEST(parse_port_invalid);

TEST_MAIN_END()
