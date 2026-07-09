/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_http_buffer.c - Unit tests for XrHttpBuffer (self-growing HTTP buffer)
 *
 * KEY CONCEPT:
 *   Tests buffer init/free, reserve/advance/consume pattern,
 *   auto-compact, thread-local recycle pool acquire/release.
 */

#include "../test_framework.h"
#include "../../../stdlib/http/http_buffer.h"
#include <string.h>

/* ========== Init / Free ========== */

TEST(http_buffer_init_free) {
    XrHttpBuffer buf;
    ASSERT_TRUE(http_buffer_init(&buf, 256));
    ASSERT_NOT_NULL(buf._base);
    ASSERT_NOT_NULL(buf.bytes);
    ASSERT_EQ_INT((int) buf.size, 0);
    ASSERT_TRUE(buf.capacity >= 256);
    http_buffer_free(&buf);
    ASSERT_TRUE(buf._base == NULL);
}

/* ========== Reserve and Advance ========== */

TEST(http_buffer_reserve_advance) {
    XrHttpBuffer buf;
    http_buffer_init(&buf, 64);

    char *wp = http_buffer_reserve(&buf, 32);
    ASSERT_NOT_NULL(wp);
    memcpy(wp, "Hello, World!", 13);
    http_buffer_advance(&buf, 13);

    ASSERT_EQ_INT((int) buf.size, 13);
    ASSERT_EQ_INT(memcmp(buf.bytes, "Hello, World!", 13), 0);

    http_buffer_free(&buf);
}

/* ========== Consume ========== */

TEST(http_buffer_consume) {
    XrHttpBuffer buf;
    http_buffer_init(&buf, 64);

    char *wp = http_buffer_reserve(&buf, 10);
    memcpy(wp, "0123456789", 10);
    http_buffer_advance(&buf, 10);

    http_buffer_consume(&buf, 4);
    ASSERT_EQ_INT((int) buf.size, 6);
    ASSERT_EQ_INT(memcmp(buf.bytes, "456789", 6), 0);

    http_buffer_free(&buf);
}

/* ========== Auto-Compact ========== */

TEST(http_buffer_auto_compact) {
    XrHttpBuffer buf;
    http_buffer_init(&buf, 64);

    // Fill buffer
    char *wp = http_buffer_reserve(&buf, 48);
    memset(wp, 'A', 48);
    http_buffer_advance(&buf, 48);

    // Consume more than half capacity -> triggers auto-compact
    http_buffer_consume(&buf, 40);
    ASSERT_EQ_INT((int) buf.size, 8);
    // After auto-compact, bytes should be at _base
    ASSERT_TRUE(buf.bytes == buf._base);

    http_buffer_free(&buf);
}

/* ========== Growth ========== */

TEST(http_buffer_growth) {
    XrHttpBuffer buf;
    http_buffer_init(&buf, 32);

    // Write more than initial capacity
    for (int i = 0; i < 100; i++) {
        char *wp = http_buffer_reserve(&buf, 16);
        ASSERT_NOT_NULL(wp);
        memset(wp, 'X', 16);
        http_buffer_advance(&buf, 16);
    }

    ASSERT_EQ_INT((int) buf.size, 1600);
    ASSERT_TRUE(buf.capacity >= 1600);

    http_buffer_free(&buf);
}

/* ========== Reset ========== */

TEST(http_buffer_reset) {
    XrHttpBuffer buf;
    http_buffer_init(&buf, 128);

    char *wp = http_buffer_reserve(&buf, 64);
    memset(wp, 'Z', 64);
    http_buffer_advance(&buf, 64);
    http_buffer_consume(&buf, 32);

    http_buffer_reset(&buf);
    ASSERT_EQ_INT((int) buf.size, 0);
    ASSERT_TRUE(buf.bytes == buf._base);

    http_buffer_free(&buf);
}

/* ========== Thread-Local Recycle Pool ========== */

TEST(http_buffer_acquire_release) {
    XrHttpBuffer *buf = http_buffer_acquire(256);
    ASSERT_NOT_NULL(buf);
    ASSERT_NOT_NULL(buf->bytes);
    ASSERT_TRUE(buf->capacity >= 256);
    ASSERT_EQ_INT((int) buf->size, 0);

    // Write some data
    char *wp = http_buffer_reserve(buf, 10);
    memcpy(wp, "test data!", 10);
    http_buffer_advance(buf, 10);

    http_buffer_release(buf);

    // Acquire again - should reuse from TLS pool
    XrHttpBuffer *buf2 = http_buffer_acquire(256);
    ASSERT_NOT_NULL(buf2);
    ASSERT_EQ_INT((int) buf2->size, 0);

    http_buffer_release(buf2);
    http_buffer_pool_cleanup();
}

/* ========== Multiple Acquires ========== */

TEST(http_buffer_multiple_acquires) {
    XrHttpBuffer *bufs[32];
    for (int i = 0; i < 32; i++) {
        bufs[i] = http_buffer_acquire(128);
        ASSERT_NOT_NULL(bufs[i]);
    }

    for (int i = 0; i < 32; i++) {
        http_buffer_release(bufs[i]);
    }

    http_buffer_pool_cleanup();
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("HttpBuffer - Init/Free");
RUN_TEST(http_buffer_init_free);

RUN_TEST_SUITE("HttpBuffer - Reserve/Advance");
RUN_TEST(http_buffer_reserve_advance);

RUN_TEST_SUITE("HttpBuffer - Consume");
RUN_TEST(http_buffer_consume);

RUN_TEST_SUITE("HttpBuffer - Auto-Compact");
RUN_TEST(http_buffer_auto_compact);

RUN_TEST_SUITE("HttpBuffer - Growth");
RUN_TEST(http_buffer_growth);

RUN_TEST_SUITE("HttpBuffer - Reset");
RUN_TEST(http_buffer_reset);

RUN_TEST_SUITE("HttpBuffer - Thread-Local Pool");
RUN_TEST(http_buffer_acquire_release);
RUN_TEST(http_buffer_multiple_acquires);

TEST_MAIN_END()
