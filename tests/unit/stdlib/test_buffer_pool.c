/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_buffer_pool.c - Unit tests for XrNetBuffer (self-growing network buffer)
 *
 * KEY CONCEPT:
 *   Tests buffer init/free, reserve/advance/consume pattern,
 *   auto-compact, TLS recycle pool acquire/release.
 */

#include "../test_framework.h"
#include "../../../stdlib/net/xnetbuf.h"
#include <string.h>

/* ========== Init / Free ========== */

TEST(netbuf_init_free) {
    XrNetBuffer buf;
    ASSERT_TRUE(xr_netbuf_init(&buf, 256));
    ASSERT_NOT_NULL(buf._base);
    ASSERT_NOT_NULL(buf.bytes);
    ASSERT_EQ_INT((int) buf.size, 0);
    ASSERT_TRUE(buf.capacity >= 256);
    xr_netbuf_free(&buf);
    ASSERT_TRUE(buf._base == NULL);
}

/* ========== Reserve and Advance ========== */

TEST(netbuf_reserve_advance) {
    XrNetBuffer buf;
    xr_netbuf_init(&buf, 64);

    char *wp = xr_netbuf_reserve(&buf, 32);
    ASSERT_NOT_NULL(wp);
    memcpy(wp, "Hello, World!", 13);
    xr_netbuf_advance(&buf, 13);

    ASSERT_EQ_INT((int) buf.size, 13);
    ASSERT_EQ_INT(memcmp(buf.bytes, "Hello, World!", 13), 0);

    xr_netbuf_free(&buf);
}

/* ========== Consume ========== */

TEST(netbuf_consume) {
    XrNetBuffer buf;
    xr_netbuf_init(&buf, 64);

    char *wp = xr_netbuf_reserve(&buf, 10);
    memcpy(wp, "0123456789", 10);
    xr_netbuf_advance(&buf, 10);

    xr_netbuf_consume(&buf, 4);
    ASSERT_EQ_INT((int) buf.size, 6);
    ASSERT_EQ_INT(memcmp(buf.bytes, "456789", 6), 0);

    xr_netbuf_free(&buf);
}

/* ========== Auto-Compact ========== */

TEST(netbuf_auto_compact) {
    XrNetBuffer buf;
    xr_netbuf_init(&buf, 64);

    // Fill buffer
    char *wp = xr_netbuf_reserve(&buf, 48);
    memset(wp, 'A', 48);
    xr_netbuf_advance(&buf, 48);

    // Consume more than half capacity -> triggers auto-compact
    xr_netbuf_consume(&buf, 40);
    ASSERT_EQ_INT((int) buf.size, 8);
    // After auto-compact, bytes should be at _base
    ASSERT_TRUE(buf.bytes == buf._base);

    xr_netbuf_free(&buf);
}

/* ========== Growth ========== */

TEST(netbuf_growth) {
    XrNetBuffer buf;
    xr_netbuf_init(&buf, 32);

    // Write more than initial capacity
    for (int i = 0; i < 100; i++) {
        char *wp = xr_netbuf_reserve(&buf, 16);
        ASSERT_NOT_NULL(wp);
        memset(wp, 'X', 16);
        xr_netbuf_advance(&buf, 16);
    }

    ASSERT_EQ_INT((int) buf.size, 1600);
    ASSERT_TRUE(buf.capacity >= 1600);

    xr_netbuf_free(&buf);
}

/* ========== Reset ========== */

TEST(netbuf_reset) {
    XrNetBuffer buf;
    xr_netbuf_init(&buf, 128);

    char *wp = xr_netbuf_reserve(&buf, 64);
    memset(wp, 'Z', 64);
    xr_netbuf_advance(&buf, 64);
    xr_netbuf_consume(&buf, 32);

    xr_netbuf_reset(&buf);
    ASSERT_EQ_INT((int) buf.size, 0);
    ASSERT_TRUE(buf.bytes == buf._base);

    xr_netbuf_free(&buf);
}

/* ========== TLS Recycle Pool ========== */

TEST(netbuf_acquire_release) {
    XrNetBuffer *buf = xr_netbuf_acquire(256);
    ASSERT_NOT_NULL(buf);
    ASSERT_NOT_NULL(buf->bytes);
    ASSERT_TRUE(buf->capacity >= 256);
    ASSERT_EQ_INT((int) buf->size, 0);

    // Write some data
    char *wp = xr_netbuf_reserve(buf, 10);
    memcpy(wp, "test data!", 10);
    xr_netbuf_advance(buf, 10);

    xr_netbuf_release(buf);

    // Acquire again - should reuse from TLS pool
    XrNetBuffer *buf2 = xr_netbuf_acquire(256);
    ASSERT_NOT_NULL(buf2);
    ASSERT_EQ_INT((int) buf2->size, 0);

    xr_netbuf_release(buf2);
    xr_netbuf_pool_cleanup();
}

/* ========== Multiple Acquires ========== */

TEST(netbuf_multiple_acquires) {
    XrNetBuffer *bufs[32];
    for (int i = 0; i < 32; i++) {
        bufs[i] = xr_netbuf_acquire(128);
        ASSERT_NOT_NULL(bufs[i]);
    }

    for (int i = 0; i < 32; i++) {
        xr_netbuf_release(bufs[i]);
    }

    xr_netbuf_pool_cleanup();
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("NetBuffer - Init/Free");
RUN_TEST(netbuf_init_free);

RUN_TEST_SUITE("NetBuffer - Reserve/Advance");
RUN_TEST(netbuf_reserve_advance);

RUN_TEST_SUITE("NetBuffer - Consume");
RUN_TEST(netbuf_consume);

RUN_TEST_SUITE("NetBuffer - Auto-Compact");
RUN_TEST(netbuf_auto_compact);

RUN_TEST_SUITE("NetBuffer - Growth");
RUN_TEST(netbuf_growth);

RUN_TEST_SUITE("NetBuffer - Reset");
RUN_TEST(netbuf_reset);

RUN_TEST_SUITE("NetBuffer - TLS Pool");
RUN_TEST(netbuf_acquire_release);
RUN_TEST(netbuf_multiple_acquires);

TEST_MAIN_END()
