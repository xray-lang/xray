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

#include <stdio.h>
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

typedef struct IoWriteAllFake {
    char dst[64];
    size_t dst_len;
    size_t max_chunk;
    bool write_error;
    bool stall;
    size_t call_count;
} IoWriteAllFake;

typedef struct IoTouchFake {
    bool update_ok;
    bool create_ok;
    size_t update_count;
    size_t create_count;
    char updated_path[32];
    char created_path[32];
} IoTouchFake;

typedef struct IoRemoveAllNode {
    const char *path;
    XrIoCorePathKind kind;
    const char *entries[8];
    size_t entry_count;
} IoRemoveAllNode;

typedef struct IoRemoveAllFake {
    IoRemoveAllNode nodes[8];
    size_t node_count;
    const char *fail_path;
    char log[16][64];
    size_t log_count;
    size_t alloc_count;
    size_t free_count;
} IoRemoveAllFake;

typedef struct IoDirCollectFake {
    char entries[16][64];
    size_t entry_count;
    bool fail_on_emit;
} IoDirCollectFake;

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

static size_t io_write_all_fake_write(void *ctx, const void *buf, size_t len) {
    IoWriteAllFake *fake = (IoWriteAllFake *) ctx;
    fake->call_count++;
    if (fake->stall)
        return 0;
    size_t n = len;
    if (fake->max_chunk != 0 && n > fake->max_chunk)
        n = fake->max_chunk;
    if (fake->dst_len + n > sizeof(fake->dst))
        n = sizeof(fake->dst) - fake->dst_len;
    memcpy(fake->dst + fake->dst_len, buf, n);
    fake->dst_len += n;
    return n;
}

static bool io_write_all_fake_error(void *ctx) {
    return ((IoWriteAllFake *) ctx)->write_error;
}

static bool io_touch_fake_update(void *ctx, const char *path) {
    IoTouchFake *fake = (IoTouchFake *) ctx;
    fake->update_count++;
    strcpy(fake->updated_path, path);
    return fake->update_ok;
}

static bool io_touch_fake_create(void *ctx, const char *path) {
    IoTouchFake *fake = (IoTouchFake *) ctx;
    fake->create_count++;
    strcpy(fake->created_path, path);
    return fake->create_ok;
}

static const IoRemoveAllNode *io_remove_all_fake_find(const IoRemoveAllFake *fake,
                                                      const char *path) {
    for (size_t i = 0; i < fake->node_count; i++) {
        if (strcmp(fake->nodes[i].path, path) == 0)
            return &fake->nodes[i];
    }
    return NULL;
}

static XrIoCorePathKind io_remove_all_fake_kind(void *ctx, const char *path) {
    const IoRemoveAllNode *node = io_remove_all_fake_find((const IoRemoveAllFake *) ctx, path);
    return node ? node->kind : XR_IO_CORE_PATH_MISSING;
}

static bool io_remove_all_fake_for_each(void *ctx, const char *path, XrIoCoreDirEntryFn visit,
                                        void *visit_ctx) {
    const IoRemoveAllNode *node = io_remove_all_fake_find((const IoRemoveAllFake *) ctx, path);
    if (!node || node->kind != XR_IO_CORE_PATH_DIR)
        return false;
    for (size_t i = 0; i < node->entry_count; i++) {
        if (!visit(visit_ctx, node->entries[i]))
            return false;
    }
    return true;
}

static bool io_remove_all_fake_log(IoRemoveAllFake *fake, const char *kind, const char *path) {
    if (fake->log_count >= 16)
        return false;
    snprintf(fake->log[fake->log_count++], sizeof(fake->log[0]), "%s:%s", kind, path);
    return !fake->fail_path || strcmp(fake->fail_path, path) != 0;
}

static bool io_remove_all_fake_remove_leaf(void *ctx, const char *path) {
    return io_remove_all_fake_log((IoRemoveAllFake *) ctx, "leaf", path);
}

static bool io_remove_all_fake_remove_dir(void *ctx, const char *path) {
    return io_remove_all_fake_log((IoRemoveAllFake *) ctx, "dir", path);
}

static void *io_remove_all_fake_alloc(void *ctx, size_t size) {
    IoRemoveAllFake *fake = (IoRemoveAllFake *) ctx;
    fake->alloc_count++;
    return malloc(size);
}

static void io_remove_all_fake_free(void *ctx, void *ptr) {
    IoRemoveAllFake *fake = (IoRemoveAllFake *) ctx;
    fake->free_count++;
    free(ptr);
}

static XrIoCoreRemoveAllOps io_remove_all_fake_ops(IoRemoveAllFake *fake) {
    XrIoCoreRemoveAllOps ops = {
        .kind = io_remove_all_fake_kind,
        .for_each_entry = io_remove_all_fake_for_each,
        .remove_leaf = io_remove_all_fake_remove_leaf,
        .remove_dir = io_remove_all_fake_remove_dir,
        .alloc = io_remove_all_fake_alloc,
        .free = io_remove_all_fake_free,
        .alloc_ctx = fake,
        .sep = '/',
    };
    return ops;
}

static XrIoCoreReadDirOps io_read_dir_fake_ops(IoRemoveAllFake *fake) {
    XrIoCoreReadDirOps ops = {
        .for_each_entry = io_remove_all_fake_for_each,
        .kind = io_remove_all_fake_kind,
        .alloc = io_remove_all_fake_alloc,
        .free = io_remove_all_fake_free,
        .alloc_ctx = fake,
        .sep = '/',
        .max_depth = XR_IO_CORE_READ_DIR_MAX_DEPTH,
    };
    return ops;
}

static bool io_dir_collect_emit(void *ctx, const char *path) {
    IoDirCollectFake *collector = (IoDirCollectFake *) ctx;
    if (collector->fail_on_emit)
        return false;
    if (collector->entry_count >= 16 || strlen(path) >= sizeof(collector->entries[0]))
        return false;
    strcpy(collector->entries[collector->entry_count++], path);
    return true;
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

typedef struct IoReadAllFake {
    const char *src;
    size_t src_len;
    size_t pos;
    bool read_error;
    bool alloc_fails;
    bool realloc_fails;
    size_t alloc_count;
    size_t realloc_count;
    size_t free_count;
} IoReadAllFake;

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

static size_t io_read_all_fake_read(void *ctx, void *buf, size_t cap) {
    IoReadAllFake *fake = (IoReadAllFake *) ctx;
    if (fake->pos >= fake->src_len)
        return 0;
    size_t n = fake->src_len - fake->pos;
    if (n > cap)
        n = cap;
    if (n > 0)
        memcpy(buf, fake->src + fake->pos, n);
    fake->pos += n;
    return n;
}

static bool io_read_all_fake_error(void *ctx) {
    return ((IoReadAllFake *) ctx)->read_error;
}

static void *io_read_all_fake_alloc(void *ctx, size_t size) {
    IoReadAllFake *fake = (IoReadAllFake *) ctx;
    fake->alloc_count++;
    if (fake->alloc_fails)
        return NULL;
    return malloc(size);
}

static void *io_read_all_fake_realloc(void *ctx, void *ptr, size_t size) {
    IoReadAllFake *fake = (IoReadAllFake *) ctx;
    fake->realloc_count++;
    if (fake->realloc_fails)
        return NULL;
    return realloc(ptr, size);
}

static void io_read_all_fake_free(void *ctx, void *ptr) {
    IoReadAllFake *fake = (IoReadAllFake *) ctx;
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

TEST(io_core_write_all_retries_short_writes) {
    IoWriteAllFake fake = {.max_chunk = 2};
    ASSERT_TRUE(
        xr_io_core_write_all(&fake, io_write_all_fake_write, io_write_all_fake_error, "abcdef", 6));
    ASSERT_EQ_UINT(fake.call_count, 3);
    ASSERT_EQ_UINT(fake.dst_len, 6);
    ASSERT_MEM_EQ(fake.dst, "abcdef", 6);
}

TEST(io_core_write_all_accepts_zero_length_without_data) {
    IoWriteAllFake fake = {.stall = true};
    ASSERT_TRUE(
        xr_io_core_write_all(&fake, io_write_all_fake_write, io_write_all_fake_error, NULL, 0));
    ASSERT_EQ_UINT(fake.call_count, 0);
}

TEST(io_core_write_all_rejects_error_after_full_write) {
    IoWriteAllFake fake = {.write_error = true};
    ASSERT_FALSE(
        xr_io_core_write_all(&fake, io_write_all_fake_write, io_write_all_fake_error, "abc", 3));
    ASSERT_EQ_UINT(fake.dst_len, 3);
}

TEST(io_core_write_all_rejects_no_progress_and_invalid_args) {
    IoWriteAllFake fake = {.stall = true};
    ASSERT_FALSE(
        xr_io_core_write_all(&fake, io_write_all_fake_write, io_write_all_fake_error, "abc", 3));
    ASSERT_FALSE(xr_io_core_write_all(&fake, NULL, io_write_all_fake_error, "abc", 3));
    ASSERT_FALSE(
        xr_io_core_write_all(&fake, io_write_all_fake_write, io_write_all_fake_error, NULL, 1));
}

TEST(io_core_touch_prefers_timestamp_update) {
    IoTouchFake fake = {.update_ok = true, .create_ok = true};
    ASSERT_TRUE(xr_io_core_touch("file.txt", io_touch_fake_update, io_touch_fake_create, &fake));
    ASSERT_EQ_UINT(fake.update_count, 1);
    ASSERT_EQ_UINT(fake.create_count, 0);
    ASSERT_STR_EQ(fake.updated_path, "file.txt");
}

TEST(io_core_touch_creates_when_update_fails) {
    IoTouchFake fake = {.update_ok = false, .create_ok = true};
    ASSERT_TRUE(xr_io_core_touch("new.txt", io_touch_fake_update, io_touch_fake_create, &fake));
    ASSERT_EQ_UINT(fake.update_count, 1);
    ASSERT_EQ_UINT(fake.create_count, 1);
    ASSERT_STR_EQ(fake.created_path, "new.txt");
}

TEST(io_core_touch_rejects_create_failure_and_invalid_args) {
    IoTouchFake fake = {.update_ok = false, .create_ok = false};
    ASSERT_FALSE(
        xr_io_core_touch("blocked.txt", io_touch_fake_update, io_touch_fake_create, &fake));
    ASSERT_EQ_UINT(fake.update_count, 1);
    ASSERT_EQ_UINT(fake.create_count, 1);
    ASSERT_FALSE(xr_io_core_touch(NULL, io_touch_fake_update, io_touch_fake_create, &fake));
    ASSERT_FALSE(xr_io_core_touch("x", NULL, io_touch_fake_create, &fake));
    ASSERT_FALSE(xr_io_core_touch("x", io_touch_fake_update, NULL, &fake));
}

TEST(io_core_remove_all_recurses_depth_first_and_skips_dot_entries) {
    IoRemoveAllFake fake = {
        .nodes =
            {
                {.path = "root",
                 .kind = XR_IO_CORE_PATH_DIR,
                 .entries = {".", "..", "a", "sub"},
                 .entry_count = 4},
                {.path = "root/a", .kind = XR_IO_CORE_PATH_LEAF},
                {.path = "root/sub",
                 .kind = XR_IO_CORE_PATH_DIR,
                 .entries = {"b"},
                 .entry_count = 1},
                {.path = "root/sub/b", .kind = XR_IO_CORE_PATH_LEAF},
            },
        .node_count = 4,
    };
    XrIoCoreRemoveAllOps ops = io_remove_all_fake_ops(&fake);

    ASSERT_TRUE(xr_io_core_remove_all("root", &ops, &fake));
    ASSERT_EQ_UINT(fake.log_count, 4);
    ASSERT_STR_EQ(fake.log[0], "leaf:root/a");
    ASSERT_STR_EQ(fake.log[1], "leaf:root/sub/b");
    ASSERT_STR_EQ(fake.log[2], "dir:root/sub");
    ASSERT_STR_EQ(fake.log[3], "dir:root");
    ASSERT_EQ_UINT(fake.alloc_count, 3);
    ASSERT_EQ_UINT(fake.free_count, 3);
}

TEST(io_core_remove_all_handles_leaf_and_missing_paths) {
    IoRemoveAllFake fake = {
        .nodes = {{.path = "file", .kind = XR_IO_CORE_PATH_LEAF}},
        .node_count = 1,
    };
    XrIoCoreRemoveAllOps ops = io_remove_all_fake_ops(&fake);

    ASSERT_TRUE(xr_io_core_remove_all("file", &ops, &fake));
    ASSERT_EQ_UINT(fake.log_count, 1);
    ASSERT_STR_EQ(fake.log[0], "leaf:file");
    ASSERT_FALSE(xr_io_core_remove_all("missing", &ops, &fake));
}

TEST(io_core_remove_all_reports_child_failure_but_still_visits_parent_dirs) {
    IoRemoveAllFake fake = {
        .nodes =
            {
                {.path = "root", .kind = XR_IO_CORE_PATH_DIR, .entries = {"sub"}, .entry_count = 1},
                {.path = "root/sub",
                 .kind = XR_IO_CORE_PATH_DIR,
                 .entries = {"bad"},
                 .entry_count = 1},
                {.path = "root/sub/bad", .kind = XR_IO_CORE_PATH_LEAF},
            },
        .node_count = 3,
        .fail_path = "root/sub/bad",
    };
    XrIoCoreRemoveAllOps ops = io_remove_all_fake_ops(&fake);

    ASSERT_FALSE(xr_io_core_remove_all("root", &ops, &fake));
    ASSERT_EQ_UINT(fake.log_count, 3);
    ASSERT_STR_EQ(fake.log[0], "leaf:root/sub/bad");
    ASSERT_STR_EQ(fake.log[1], "dir:root/sub");
    ASSERT_STR_EQ(fake.log[2], "dir:root");
}

TEST(io_core_read_dir_filters_dot_entries) {
    IoRemoveAllFake fake = {
        .nodes = {{.path = "root",
                   .kind = XR_IO_CORE_PATH_DIR,
                   .entries = {".", "..", "a.txt", "b.txt"},
                   .entry_count = 4}},
        .node_count = 1,
    };
    IoDirCollectFake collector = {0};

    ASSERT_TRUE(xr_io_core_read_dir("root", io_remove_all_fake_for_each, &fake, io_dir_collect_emit,
                                    &collector));
    ASSERT_EQ_UINT(collector.entry_count, 2);
    ASSERT_STR_EQ(collector.entries[0], "a.txt");
    ASSERT_STR_EQ(collector.entries[1], "b.txt");
}

TEST(io_core_read_dir_recursive_emits_relative_depth_first_paths) {
    IoRemoveAllFake fake = {
        .nodes =
            {
                {.path = "root",
                 .kind = XR_IO_CORE_PATH_DIR,
                 .entries = {".", "a.txt", "sub", "link"},
                 .entry_count = 4},
                {.path = "root/a.txt", .kind = XR_IO_CORE_PATH_LEAF},
                {.path = "root/sub",
                 .kind = XR_IO_CORE_PATH_DIR,
                 .entries = {"b.txt"},
                 .entry_count = 1},
                {.path = "root/sub/b.txt", .kind = XR_IO_CORE_PATH_LEAF},
                {.path = "root/link", .kind = XR_IO_CORE_PATH_LEAF},
            },
        .node_count = 5,
    };
    XrIoCoreReadDirOps ops = io_read_dir_fake_ops(&fake);
    IoDirCollectFake collector = {0};

    ASSERT_TRUE(
        xr_io_core_read_dir_recursive("root", &ops, &fake, io_dir_collect_emit, &collector));
    ASSERT_EQ_UINT(collector.entry_count, 4);
    ASSERT_STR_EQ(collector.entries[0], "a.txt");
    ASSERT_STR_EQ(collector.entries[1], "sub");
    ASSERT_STR_EQ(collector.entries[2], "sub/b.txt");
    ASSERT_STR_EQ(collector.entries[3], "link");
    ASSERT_EQ_UINT(fake.alloc_count, 4);
    ASSERT_EQ_UINT(fake.free_count, 4);
}

TEST(io_core_read_dir_recursive_treats_missing_root_as_empty) {
    IoRemoveAllFake fake = {0};
    XrIoCoreReadDirOps ops = io_read_dir_fake_ops(&fake);
    IoDirCollectFake collector = {0};

    ASSERT_TRUE(
        xr_io_core_read_dir_recursive("missing", &ops, &fake, io_dir_collect_emit, &collector));
    ASSERT_EQ_UINT(collector.entry_count, 0);
}

TEST(io_core_read_dir_recursive_obeys_depth_limit) {
    IoRemoveAllFake fake = {
        .nodes =
            {
                {.path = "root", .kind = XR_IO_CORE_PATH_DIR, .entries = {"sub"}, .entry_count = 1},
                {.path = "root/sub",
                 .kind = XR_IO_CORE_PATH_DIR,
                 .entries = {"deep.txt"},
                 .entry_count = 1},
                {.path = "root/sub/deep.txt", .kind = XR_IO_CORE_PATH_LEAF},
            },
        .node_count = 3,
    };
    XrIoCoreReadDirOps ops = io_read_dir_fake_ops(&fake);
    ops.max_depth = 1;
    IoDirCollectFake collector = {0};

    ASSERT_TRUE(
        xr_io_core_read_dir_recursive("root", &ops, &fake, io_dir_collect_emit, &collector));
    ASSERT_EQ_UINT(collector.entry_count, 1);
    ASSERT_STR_EQ(collector.entries[0], "sub");
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

TEST(io_core_read_all_stream_alloc_grows_and_nul_terminates) {
    IoReadAllFake fake = {.src = "abcdefghij", .src_len = 10};
    size_t len = 99;
    char *buf = xr_io_core_read_all_stream_alloc(
        &fake, io_read_all_fake_read, io_read_all_fake_error, io_read_all_fake_alloc,
        io_read_all_fake_realloc, io_read_all_fake_free, &fake, 4, 64, &len);

    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_UINT(len, 10);
    ASSERT_MEM_EQ(buf, "abcdefghij", 10);
    ASSERT_EQ_INT(buf[10], '\0');
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.realloc_count, 2);
    ASSERT_EQ_UINT(fake.free_count, 0);
    free(buf);
}

TEST(io_core_read_all_stream_alloc_handles_exact_chunk_eof) {
    IoReadAllFake fake = {.src = "abcd", .src_len = 4};
    size_t len = 99;
    char *buf = xr_io_core_read_all_stream_alloc(
        &fake, io_read_all_fake_read, io_read_all_fake_error, io_read_all_fake_alloc,
        io_read_all_fake_realloc, io_read_all_fake_free, &fake, 4, 8, &len);

    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_UINT(len, 4);
    ASSERT_MEM_EQ(buf, "abcd", 4);
    ASSERT_EQ_INT(buf[4], '\0');
    ASSERT_EQ_UINT(fake.realloc_count, 1);
    free(buf);
}

TEST(io_core_read_all_stream_alloc_rejects_read_error_and_releases) {
    IoReadAllFake fake = {.src = "abc", .src_len = 3, .read_error = true};
    size_t len = 99;
    char *buf = xr_io_core_read_all_stream_alloc(
        &fake, io_read_all_fake_read, io_read_all_fake_error, io_read_all_fake_alloc,
        io_read_all_fake_realloc, io_read_all_fake_free, &fake, 4, 64, &len);

    ASSERT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.free_count, 1);
}

TEST(io_core_read_all_stream_alloc_rejects_stream_past_max_and_releases) {
    IoReadAllFake fake = {.src = "abcde", .src_len = 5};
    size_t len = 99;
    char *buf = xr_io_core_read_all_stream_alloc(
        &fake, io_read_all_fake_read, io_read_all_fake_error, io_read_all_fake_alloc,
        io_read_all_fake_realloc, io_read_all_fake_free, &fake, 4, 4, &len);

    ASSERT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.realloc_count, 0);
    ASSERT_EQ_UINT(fake.free_count, 1);
}

TEST(io_core_read_all_stream_alloc_rejects_realloc_failure_and_releases) {
    IoReadAllFake fake = {.src = "abcde", .src_len = 5, .realloc_fails = true};
    size_t len = 99;
    char *buf = xr_io_core_read_all_stream_alloc(
        &fake, io_read_all_fake_read, io_read_all_fake_error, io_read_all_fake_alloc,
        io_read_all_fake_realloc, io_read_all_fake_free, &fake, 4, 8, &len);

    ASSERT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_EQ_UINT(fake.alloc_count, 1);
    ASSERT_EQ_UINT(fake.realloc_count, 1);
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

TEST(io_core_path_result_view_keeps_length_and_buffer) {
    XrIoCorePathView view;
    const char path[] = "/tmp/xray-target-extra";
    ASSERT(xr_io_core_path_result_view(path, 16, &view));
    ASSERT_EQ_PTR(view.data, path);
    ASSERT_EQ_UINT(view.len, 16);
    ASSERT_MEM_EQ(view.data, "/tmp/xray-target", 16);
}

TEST(io_core_path_result_view_strips_windows_extended_prefix) {
    XrIoCorePathView view;
    const char path[] = "\\\\?\\C:\\Temp\\xray.txt";
    ASSERT(xr_io_core_path_result_view(path, strlen(path), &view));
    ASSERT_EQ_PTR(view.data, path + 4);
    ASSERT_EQ_UINT(view.len, strlen("C:\\Temp\\xray.txt"));
    ASSERT_MEM_EQ(view.data, "C:\\Temp\\xray.txt", view.len);
}

TEST(io_core_path_result_cstr_view_uses_c_string_length) {
    XrIoCorePathView view;
    const char path[] = "relative/path";
    ASSERT(xr_io_core_path_result_cstr_view(path, &view));
    ASSERT_EQ_PTR(view.data, path);
    ASSERT_EQ_UINT(view.len, strlen(path));
}

TEST(io_core_path_result_view_rejects_invalid_args_and_resets_output) {
    XrIoCorePathView view = {.data = "old", .len = 3};
    ASSERT_FALSE(xr_io_core_path_result_view(NULL, 0, &view));
    ASSERT_NULL(view.data);
    ASSERT_EQ_UINT(view.len, 0);
    ASSERT_FALSE(xr_io_core_path_result_view("x", 1, NULL));
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

TEST(io_core_chmod_mode_accepts_nonnegative_int_range) {
    int mode = -1;
    ASSERT_TRUE(xr_io_core_chmod_mode(0644, &mode));
    ASSERT_EQ_INT(mode, 0644);
    ASSERT_TRUE(xr_io_core_chmod_mode(0, &mode));
    ASSERT_EQ_INT(mode, 0);
    ASSERT_TRUE(xr_io_core_chmod_mode(INT_MAX, &mode));
    ASSERT_EQ_INT(mode, INT_MAX);
}

TEST(io_core_chmod_mode_rejects_negative_overflow_and_invalid_output) {
    int mode = 123;
    ASSERT_FALSE(xr_io_core_chmod_mode(-1, &mode));
    ASSERT_EQ_INT(mode, 0);
    ASSERT_FALSE(xr_io_core_chmod_mode((int64_t) INT_MAX + 1, &mode));
    ASSERT_EQ_INT(mode, 0);
    ASSERT_FALSE(xr_io_core_chmod_mode(0644, NULL));
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

RUN_TEST_SUITE("IO Core - write all");
RUN_TEST(io_core_write_all_retries_short_writes);
RUN_TEST(io_core_write_all_accepts_zero_length_without_data);
RUN_TEST(io_core_write_all_rejects_error_after_full_write);
RUN_TEST(io_core_write_all_rejects_no_progress_and_invalid_args);

RUN_TEST_SUITE("IO Core - touch");
RUN_TEST(io_core_touch_prefers_timestamp_update);
RUN_TEST(io_core_touch_creates_when_update_fails);
RUN_TEST(io_core_touch_rejects_create_failure_and_invalid_args);

RUN_TEST_SUITE("IO Core - remove all");
RUN_TEST(io_core_remove_all_recurses_depth_first_and_skips_dot_entries);
RUN_TEST(io_core_remove_all_handles_leaf_and_missing_paths);
RUN_TEST(io_core_remove_all_reports_child_failure_but_still_visits_parent_dirs);

RUN_TEST_SUITE("IO Core - read dir");
RUN_TEST(io_core_read_dir_filters_dot_entries);
RUN_TEST(io_core_read_dir_recursive_emits_relative_depth_first_paths);
RUN_TEST(io_core_read_dir_recursive_treats_missing_root_as_empty);
RUN_TEST(io_core_read_dir_recursive_obeys_depth_limit);

RUN_TEST_SUITE("IO Core - sized read");
RUN_TEST(io_core_read_sized_stream_alloc_reads_and_nul_terminates);
RUN_TEST(io_core_read_sized_stream_alloc_accepts_empty_stream);
RUN_TEST(io_core_read_sized_stream_alloc_rejects_too_large_stream);
RUN_TEST(io_core_read_sized_stream_alloc_releases_on_read_error);

RUN_TEST_SUITE("IO Core - read all stream");
RUN_TEST(io_core_read_all_stream_alloc_grows_and_nul_terminates);
RUN_TEST(io_core_read_all_stream_alloc_handles_exact_chunk_eof);
RUN_TEST(io_core_read_all_stream_alloc_rejects_read_error_and_releases);
RUN_TEST(io_core_read_all_stream_alloc_rejects_stream_past_max_and_releases);
RUN_TEST(io_core_read_all_stream_alloc_rejects_realloc_failure_and_releases);

RUN_TEST_SUITE("IO Core - dir walk paths");
RUN_TEST(io_core_dot_dir_entry_recognizes_reserved_names);
RUN_TEST(io_core_join_child_path_uses_explicit_separator);
RUN_TEST(io_core_join_child_path_rejects_empty_or_truncated_paths);
RUN_TEST(io_core_relative_path_from_base_trims_one_separator);
RUN_TEST(io_core_temp_template_uses_explicit_separator_and_stem);
RUN_TEST(io_core_temp_template_rejects_invalid_or_truncated_output);

RUN_TEST_SUITE("IO Core - path result view");
RUN_TEST(io_core_path_result_view_keeps_length_and_buffer);
RUN_TEST(io_core_path_result_view_strips_windows_extended_prefix);
RUN_TEST(io_core_path_result_cstr_view_uses_c_string_length);
RUN_TEST(io_core_path_result_view_rejects_invalid_args_and_resets_output);

RUN_TEST_SUITE("IO Core - stat");
RUN_TEST(io_core_stat_field_names_are_shared_schema);
RUN_TEST(io_core_stat_fields_normalize_mode_and_preserve_metadata);

RUN_TEST_SUITE("IO Core - chmod");
RUN_TEST(io_core_chmod_mode_accepts_nonnegative_int_range);
RUN_TEST(io_core_chmod_mode_rejects_negative_overflow_and_invalid_output);

TEST_MAIN_END()
