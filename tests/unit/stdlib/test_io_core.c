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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("IO Core - readLines");
RUN_TEST(io_core_read_lines_empty_file_has_no_lines);
RUN_TEST(io_core_read_lines_drops_only_trailing_newline_record);
RUN_TEST(io_core_read_lines_keeps_middle_empty_lines);
RUN_TEST(io_core_read_lines_trims_trailing_carriage_returns);
RUN_TEST(io_core_read_lines_rejects_invalid_callback);

TEST_MAIN_END()
