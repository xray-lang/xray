/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_io_core.c - Unit tests for runtime-neutral IO core helpers
 */

#include "../test_framework.h"
#include "shared/xr_io_core.h"

#include <stdlib.h>
#include <string.h>

typedef struct IoLineCollector {
    size_t count;
    char lines[8][32];
    size_t lens[8];
} IoLineCollector;

static bool io_core_collect_line(void *ctx, const char *data, size_t len) {
    IoLineCollector *collector = (IoLineCollector *) ctx;
    if (collector->count >= 8 || len >= sizeof(collector->lines[0]))
        return false;
    memcpy(collector->lines[collector->count], data, len);
    collector->lines[collector->count][len] = '\0';
    collector->lens[collector->count] = len;
    collector->count++;
    return true;
}

static bool io_core_collect(const char *input, IoLineCollector *collector) {
    memset(collector, 0, sizeof(*collector));
    return xr_io_core_read_lines_each(input, strlen(input), io_core_collect_line, collector);
}

typedef struct IoMkdirpFake {
    char dirs[16][64];
    size_t dir_count;
    char calls[16][64];
    size_t call_count;
    const char *blocked_path;
} IoMkdirpFake;

static bool io_mkdirp_fake_has_dir(const IoMkdirpFake *fake, const char *path) {
    for (size_t i = 0; i < fake->dir_count; i++) {
        if (strcmp(fake->dirs[i], path) == 0)
            return true;
    }
    return false;
}

static bool io_mkdirp_fake_add_dir(IoMkdirpFake *fake, const char *path) {
    if (fake->dir_count >= 16 || strlen(path) >= sizeof(fake->dirs[0]))
        return false;
    strcpy(fake->dirs[fake->dir_count++], path);
    return true;
}

static int io_mkdirp_fake_mkdir(void *ctx, const char *path) {
    IoMkdirpFake *fake = (IoMkdirpFake *) ctx;
    if (fake->call_count < 16 && strlen(path) < sizeof(fake->calls[0]))
        strcpy(fake->calls[fake->call_count++], path);
    if (fake->blocked_path && strcmp(fake->blocked_path, path) == 0)
        return -1;
    if (io_mkdirp_fake_has_dir(fake, path))
        return -1;
    return io_mkdirp_fake_add_dir(fake, path) ? 0 : -1;
}

static bool io_mkdirp_fake_is_dir(void *ctx, const char *path) {
    return io_mkdirp_fake_has_dir((IoMkdirpFake *) ctx, path);
}

static bool io_mkdirp_fake_run(IoMkdirpFake *fake, char *path) {
    return xr_io_core_mkdirp(path, io_mkdirp_fake_mkdir, io_mkdirp_fake_is_dir, fake);
}

typedef struct IoCopyFake {
    const char *src;
    size_t src_len;
    size_t read_pos;
    char dst[64];
    size_t dst_len;
    size_t max_write;
    bool read_error;
} IoCopyFake;

static size_t io_copy_fake_read(void *ctx, void *buf, size_t cap) {
    IoCopyFake *fake = (IoCopyFake *) ctx;
    if (fake->read_pos >= fake->src_len)
        return 0;
    size_t n = fake->src_len - fake->read_pos;
    if (n > cap)
        n = cap;
    memcpy(buf, fake->src + fake->read_pos, n);
    fake->read_pos += n;
    return n;
}

static size_t io_copy_fake_write(void *ctx, const void *buf, size_t len) {
    IoCopyFake *fake = (IoCopyFake *) ctx;
    size_t n = len;
    if (fake->max_write != 0 && n > fake->max_write)
        n = fake->max_write;
    if (fake->dst_len + n > sizeof(fake->dst))
        n = sizeof(fake->dst) - fake->dst_len;
    memcpy(fake->dst + fake->dst_len, buf, n);
    fake->dst_len += n;
    return n;
}

static bool io_copy_fake_error(void *ctx) {
    return ((IoCopyFake *) ctx)->read_error;
}

typedef struct IoReadFake {
    const char *src;
    size_t src_len;
    size_t pos;
    bool seek_end_fails;
    bool seek_start_fails;
    bool read_error;
    bool alloc_fails;
    bool has_tell_override;
    long tell_override;
    size_t alloc_count;
    size_t free_count;
} IoReadFake;

static bool io_read_fake_seek_end(void *ctx) {
    IoReadFake *fake = (IoReadFake *) ctx;
    if (fake->seek_end_fails)
        return false;
    fake->pos = fake->src_len;
    return true;
}

static long io_read_fake_tell(void *ctx) {
    IoReadFake *fake = (IoReadFake *) ctx;
    if (fake->has_tell_override)
        return fake->tell_override;
    return (long) fake->pos;
}

static bool io_read_fake_seek_start(void *ctx) {
    IoReadFake *fake = (IoReadFake *) ctx;
    if (fake->seek_start_fails)
        return false;
    fake->pos = 0;
    return true;
}

static size_t io_read_fake_read(void *ctx, void *buf, size_t cap) {
    IoReadFake *fake = (IoReadFake *) ctx;
    size_t n = fake->src_len - fake->pos;
    if (n > cap)
        n = cap;
    if (n > 0)
        memcpy(buf, fake->src + fake->pos, n);
    fake->pos += n;
    return n;
}

static bool io_read_fake_error(void *ctx) {
    return ((IoReadFake *) ctx)->read_error;
}

static void *io_read_fake_alloc(void *ctx, size_t size) {
    IoReadFake *fake = (IoReadFake *) ctx;
    fake->alloc_count++;
    if (fake->alloc_fails)
        return NULL;
    return malloc(size);
}

static void io_read_fake_free(void *ctx, void *ptr) {
    IoReadFake *fake = (IoReadFake *) ctx;
    fake->free_count++;
    free(ptr);
}

TEST(io_core_read_lines_empty_file_has_no_lines) {
    IoLineCollector collector;
    ASSERT(io_core_collect("", &collector));
    ASSERT_EQ_UINT(collector.count, 0);
}

TEST(io_core_read_lines_drops_only_trailing_newline_record) {
    IoLineCollector collector;
    ASSERT(io_core_collect("alpha\n", &collector));
    ASSERT_EQ_UINT(collector.count, 1);
    ASSERT_STR_EQ(collector.lines[0], "alpha");

    ASSERT(io_core_collect("alpha\r\nbeta\r\n", &collector));
    ASSERT_EQ_UINT(collector.count, 2);
    ASSERT_STR_EQ(collector.lines[0], "alpha");
    ASSERT_STR_EQ(collector.lines[1], "beta");
}

TEST(io_core_read_lines_keeps_middle_empty_lines) {
    IoLineCollector collector;
    ASSERT(io_core_collect("a\n\nb", &collector));
    ASSERT_EQ_UINT(collector.count, 3);
    ASSERT_STR_EQ(collector.lines[0], "a");
    ASSERT_EQ_UINT(collector.lens[1], 0);
    ASSERT_STR_EQ(collector.lines[2], "b");
}

TEST(io_core_read_lines_trims_trailing_carriage_returns) {
    IoLineCollector collector;
    ASSERT(io_core_collect("a\r\r", &collector));
    ASSERT_EQ_UINT(collector.count, 1);
    ASSERT_STR_EQ(collector.lines[0], "a");

    ASSERT(io_core_collect("\r\n", &collector));
    ASSERT_EQ_UINT(collector.count, 1);
    ASSERT_EQ_UINT(collector.lens[0], 0);
}

TEST(io_core_read_lines_rejects_invalid_callback) {
    ASSERT(!xr_io_core_read_lines_each("x", 1, NULL, NULL));
    ASSERT(!xr_io_core_read_lines_each(NULL, 1, io_core_collect_line, NULL));
}

TEST(io_core_mkdirp_rejects_empty_path) {
    IoMkdirpFake fake = {0};
    char path[] = "";
    ASSERT(!io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.call_count, 0);
}

TEST(io_core_mkdirp_creates_nested_directories) {
    IoMkdirpFake fake = {0};
    char path[] = "a/b/c";
    ASSERT(io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.dir_count, 3);
    ASSERT_STR_EQ(fake.dirs[0], "a");
    ASSERT_STR_EQ(fake.dirs[1], "a/b");
    ASSERT_STR_EQ(fake.dirs[2], "a/b/c");
}

TEST(io_core_mkdirp_existing_final_directory_succeeds) {
    IoMkdirpFake fake = {0};
    ASSERT(io_mkdirp_fake_add_dir(&fake, "a"));
    ASSERT(io_mkdirp_fake_add_dir(&fake, "a/b"));
    char path[] = "a/b";
    ASSERT(io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.dir_count, 2);
    ASSERT_EQ_UINT(fake.call_count, 2);
    ASSERT_STR_EQ(fake.calls[0], "a");
    ASSERT_STR_EQ(fake.calls[1], "a/b");
}

TEST(io_core_mkdirp_trims_trailing_separators) {
    IoMkdirpFake fake = {0};
    char path[] = "a/b///";
    ASSERT(io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.dir_count, 2);
    ASSERT_STR_EQ(fake.dirs[0], "a");
    ASSERT_STR_EQ(fake.dirs[1], "a/b");
}

TEST(io_core_mkdirp_handles_root_path) {
    IoMkdirpFake fake = {0};
    ASSERT(io_mkdirp_fake_add_dir(&fake, "/"));
    char path[] = "/";
    ASSERT(io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.call_count, 0);
}

TEST(io_core_mkdirp_handles_backslash_separators) {
    IoMkdirpFake fake = {0};
    char path[] = "a\\b\\c";
    ASSERT(io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.dir_count, 3);
    ASSERT_STR_EQ(fake.dirs[0], "a");
    ASSERT_STR_EQ(fake.dirs[1], "a\\b");
    ASSERT_STR_EQ(fake.dirs[2], "a\\b\\c");
}

TEST(io_core_mkdirp_fails_on_blocked_intermediate) {
    IoMkdirpFake fake = {.blocked_path = "a/b"};
    char path[] = "a/b/c";
    ASSERT(!io_mkdirp_fake_run(&fake, path));
    ASSERT_EQ_UINT(fake.dir_count, 1);
    ASSERT_STR_EQ(fake.dirs[0], "a");
}

TEST(io_core_copy_stream_copies_multiple_chunks) {
    char buf[4];
    IoCopyFake fake = {.src = "abcdefghij", .src_len = 10};
    ASSERT(xr_io_core_copy_stream(&fake, io_copy_fake_read, io_copy_fake_write, io_copy_fake_error,
                                  buf, sizeof(buf)));
    ASSERT_EQ_UINT(fake.read_pos, 10);
    ASSERT_EQ_UINT(fake.dst_len, 10);
    ASSERT_MEM_EQ(fake.dst, "abcdefghij", 10);
}

TEST(io_core_copy_stream_handles_exact_chunk_eof) {
    char buf[4];
    IoCopyFake fake = {.src = "abcdefgh", .src_len = 8};
    ASSERT(xr_io_core_copy_stream(&fake, io_copy_fake_read, io_copy_fake_write, io_copy_fake_error,
                                  buf, sizeof(buf)));
    ASSERT_EQ_UINT(fake.dst_len, 8);
    ASSERT_MEM_EQ(fake.dst, "abcdefgh", 8);
}

TEST(io_core_copy_stream_rejects_short_write) {
    char buf[4];
    IoCopyFake fake = {.src = "abcde", .src_len = 5, .max_write = 3};
    ASSERT_FALSE(xr_io_core_copy_stream(&fake, io_copy_fake_read, io_copy_fake_write,
                                        io_copy_fake_error, buf, sizeof(buf)));
}

TEST(io_core_copy_stream_rejects_read_error) {
    char buf[4];
    IoCopyFake fake = {.src = "abc", .src_len = 3, .read_error = true};
    ASSERT_FALSE(xr_io_core_copy_stream(&fake, io_copy_fake_read, io_copy_fake_write,
                                        io_copy_fake_error, buf, sizeof(buf)));
}

TEST(io_core_copy_stream_rejects_invalid_callbacks_or_buffer) {
    char buf[4];
    IoCopyFake fake = {.src = "abc", .src_len = 3};
    ASSERT_FALSE(xr_io_core_copy_stream(&fake, NULL, io_copy_fake_write, io_copy_fake_error, buf,
                                        sizeof(buf)));
    ASSERT_FALSE(xr_io_core_copy_stream(&fake, io_copy_fake_read, NULL, io_copy_fake_error, buf,
                                        sizeof(buf)));
    ASSERT_FALSE(xr_io_core_copy_stream(&fake, io_copy_fake_read, io_copy_fake_write,
                                        io_copy_fake_error, NULL, sizeof(buf)));
    ASSERT_FALSE(xr_io_core_copy_stream(&fake, io_copy_fake_read, io_copy_fake_write,
                                        io_copy_fake_error, buf, 0));
}

TEST(io_core_read_sized_stream_alloc_reads_and_nul_terminates) {
    IoReadFake fake = {.src = "hello", .src_len = 5};
    size_t len = 99;
    char *buf = xr_io_core_read_sized_stream_alloc(
        &fake, io_read_fake_seek_end, io_read_fake_tell, io_read_fake_seek_start, io_read_fake_read,
        io_read_fake_error, io_read_fake_alloc, io_read_fake_free, &fake, 16, &len);

    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_UINT(len, 5);
    ASSERT_MEM_EQ(buf, "hello", 5);
    ASSERT_EQ_INT(buf[5], '\0');
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.free_count, 0);
    free(buf);
}

TEST(io_core_read_sized_stream_alloc_accepts_empty_stream) {
    IoReadFake fake = {.src = "", .src_len = 0};
    size_t len = 99;
    char *buf = xr_io_core_read_sized_stream_alloc(
        &fake, io_read_fake_seek_end, io_read_fake_tell, io_read_fake_seek_start, io_read_fake_read,
        io_read_fake_error, io_read_fake_alloc, io_read_fake_free, &fake, 16, &len);

    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_EQ_INT(buf[0], '\0');
    free(buf);
}

TEST(io_core_read_sized_stream_alloc_rejects_too_large_stream) {
    IoReadFake fake = {.src = "hello", .src_len = 5};
    size_t len = 99;
    char *buf = xr_io_core_read_sized_stream_alloc(
        &fake, io_read_fake_seek_end, io_read_fake_tell, io_read_fake_seek_start, io_read_fake_read,
        io_read_fake_error, io_read_fake_alloc, io_read_fake_free, &fake, 4, &len);

    ASSERT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_EQ_UINT(fake.alloc_count, 0);
}

TEST(io_core_read_sized_stream_alloc_releases_on_read_error) {
    IoReadFake fake = {.src = "hello", .src_len = 5, .read_error = true};
    size_t len = 99;
    char *buf = xr_io_core_read_sized_stream_alloc(
        &fake, io_read_fake_seek_end, io_read_fake_tell, io_read_fake_seek_start, io_read_fake_read,
        io_read_fake_error, io_read_fake_alloc, io_read_fake_free, &fake, 16, &len);

    ASSERT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.free_count, 1);
}

TEST(io_core_dot_dir_entry_recognizes_reserved_names) {
    ASSERT(xr_io_core_is_dot_dir_entry("."));
    ASSERT(xr_io_core_is_dot_dir_entry(".."));
    ASSERT_FALSE(xr_io_core_is_dot_dir_entry(""));
    ASSERT_FALSE(xr_io_core_is_dot_dir_entry(NULL));
    ASSERT_FALSE(xr_io_core_is_dot_dir_entry("..."));
    ASSERT_FALSE(xr_io_core_is_dot_dir_entry(".hidden"));
}

TEST(io_core_join_child_path_uses_explicit_separator) {
    char path[32];
    ASSERT(xr_io_core_join_child_path("root", '/', "leaf", path, sizeof(path)));
    ASSERT_STR_EQ(path, "root/leaf");
    ASSERT(xr_io_core_join_child_path("root", '\\', "leaf", path, sizeof(path)));
    ASSERT_STR_EQ(path, "root\\leaf");
}

TEST(io_core_join_child_path_rejects_empty_or_truncated_paths) {
    char path[8];
    size_t len = 0;
    ASSERT(!xr_io_core_join_child_len("", "leaf", &len));
    ASSERT(!xr_io_core_join_child_len("root", "", &len));
    ASSERT(!xr_io_core_join_child_path("root", '/', "leaf", path, sizeof(path)));
    ASSERT(xr_io_core_join_child_path("a", '/', "b", path, sizeof(path)));
    ASSERT_STR_EQ(path, "a/b");
}

TEST(io_core_relative_path_from_base_trims_one_separator) {
    const char *rel = xr_io_core_relative_path_from_base("root/child/file", strlen("root"));
    ASSERT_STR_EQ(rel, "child/file");
    rel = xr_io_core_relative_path_from_base("root\\child", strlen("root"));
    ASSERT_STR_EQ(rel, "child");
    rel = xr_io_core_relative_path_from_base("root", strlen("root"));
    ASSERT_STR_EQ(rel, "");
}

TEST(io_core_temp_template_uses_explicit_separator_and_stem) {
    char path[64];
    ASSERT(xr_io_core_temp_template("/tmp", '/', "xray_XXXXXX", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/xray_XXXXXX");
    ASSERT(xr_io_core_temp_template("C:\\Temp", '\\', "xray_XXXXXX", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:\\Temp\\xray_XXXXXX");
}

TEST(io_core_temp_template_rejects_invalid_or_truncated_output) {
    char path[8];
    ASSERT(!xr_io_core_temp_template("", '/', "xray_XXXXXX", path, sizeof(path)));
    ASSERT(!xr_io_core_temp_template("/tmp", '/', "", path, sizeof(path)));
    ASSERT(!xr_io_core_temp_template("/tmp", '/', "xray_XXXXXX", path, sizeof(path)));
}

TEST(io_core_stat_field_names_are_shared_schema) {
    ASSERT_EQ_INT(XR_IO_CORE_STAT_FIELD_COUNT, 10);
    ASSERT_EQ_INT(XR_IO_CORE_STAT_SIZE, 0);
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_SIZE], "size");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_MODE], "mode");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_MTIME], "mtime");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_ATIME], "atime");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_CTIME], "ctime");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_UID], "uid");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_GID], "gid");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_IS_FILE], "isFile");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_IS_DIR], "isDir");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_IS_SYMLINK], "isSymlink");
}

TEST(io_core_stat_fields_normalize_mode_and_preserve_metadata) {
    XrIoCoreStatFields fields =
        xr_io_core_stat_fields(12, 0100644, 1, 2, 3, 501, 20, true, false, true);
    ASSERT_EQ_INT(fields.size, 12);
    ASSERT_EQ_INT(fields.mode, 0644);
    ASSERT_EQ_INT(fields.mtime, 1);
    ASSERT_EQ_INT(fields.atime, 2);
    ASSERT_EQ_INT(fields.ctime, 3);
    ASSERT_EQ_INT(fields.uid, 501);
    ASSERT_EQ_INT(fields.gid, 20);
    ASSERT_TRUE(fields.is_file);
    ASSERT_FALSE(fields.is_dir);
    ASSERT_TRUE(fields.is_symlink);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("IO Core - readLines");
RUN_TEST(io_core_read_lines_empty_file_has_no_lines);
RUN_TEST(io_core_read_lines_drops_only_trailing_newline_record);
RUN_TEST(io_core_read_lines_keeps_middle_empty_lines);
RUN_TEST(io_core_read_lines_trims_trailing_carriage_returns);
RUN_TEST(io_core_read_lines_rejects_invalid_callback);

RUN_TEST_SUITE("IO Core - mkdirp");
RUN_TEST(io_core_mkdirp_rejects_empty_path);
RUN_TEST(io_core_mkdirp_creates_nested_directories);
RUN_TEST(io_core_mkdirp_existing_final_directory_succeeds);
RUN_TEST(io_core_mkdirp_trims_trailing_separators);
RUN_TEST(io_core_mkdirp_handles_root_path);
RUN_TEST(io_core_mkdirp_handles_backslash_separators);
RUN_TEST(io_core_mkdirp_fails_on_blocked_intermediate);

RUN_TEST_SUITE("IO Core - copy stream");
RUN_TEST(io_core_copy_stream_copies_multiple_chunks);
RUN_TEST(io_core_copy_stream_handles_exact_chunk_eof);
RUN_TEST(io_core_copy_stream_rejects_short_write);
RUN_TEST(io_core_copy_stream_rejects_read_error);
RUN_TEST(io_core_copy_stream_rejects_invalid_callbacks_or_buffer);

RUN_TEST_SUITE("IO Core - sized read");
RUN_TEST(io_core_read_sized_stream_alloc_reads_and_nul_terminates);
RUN_TEST(io_core_read_sized_stream_alloc_accepts_empty_stream);
RUN_TEST(io_core_read_sized_stream_alloc_rejects_too_large_stream);
RUN_TEST(io_core_read_sized_stream_alloc_releases_on_read_error);

RUN_TEST_SUITE("IO Core - dir walk paths");
RUN_TEST(io_core_dot_dir_entry_recognizes_reserved_names);
RUN_TEST(io_core_join_child_path_uses_explicit_separator);
RUN_TEST(io_core_join_child_path_rejects_empty_or_truncated_paths);
RUN_TEST(io_core_relative_path_from_base_trims_one_separator);
RUN_TEST(io_core_temp_template_uses_explicit_separator_and_stem);
RUN_TEST(io_core_temp_template_rejects_invalid_or_truncated_output);

RUN_TEST_SUITE("IO Core - stat");
RUN_TEST(io_core_stat_field_names_are_shared_schema);
RUN_TEST(io_core_stat_fields_normalize_mode_and_preserve_metadata);

TEST_MAIN_END()
