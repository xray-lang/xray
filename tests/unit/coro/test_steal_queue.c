/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_steal_queue.c - Unit tests for lock-free work-stealing queue
 *
 * KEY CONCEPT:
 *   Tests XrStealQueue (Chase-Lev deque) operations: init, push, pop, steal,
 *   size, empty, capacity limits, and snapshot.
 *   Uses mock coroutine pointers (no real XrCoroutine needed).
 */

#include "../test_framework.h"
#include "coro/xsteal_queue.h"

// Mock coroutine pointers (just unique addresses, never dereferenced)
#define MOCK_CORO(n) ((struct XrCoroutine *) (uintptr_t) (0x1000 + (n)))

/* ========== Lifecycle ========== */

TEST(steal_queue_init_destroy) {
    XrStealQueue q;
    ASSERT_TRUE(xr_steal_queue_init(&q, 0));  // default capacity
    ASSERT_EQ_INT(q.capacity, 256);           // XR_STEAL_QUEUE_DEFAULT_SIZE
    ASSERT_TRUE(xr_steal_queue_empty(&q));
    ASSERT_EQ_INT(xr_steal_queue_size(&q), 0);
    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_init_custom_capacity) {
    XrStealQueue q;
    ASSERT_TRUE(xr_steal_queue_init(&q, 32));
    ASSERT_EQ_INT(q.capacity, 32);
    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_init_rounds_to_power_of_two) {
    XrStealQueue q;
    ASSERT_TRUE(xr_steal_queue_init(&q, 33));
    ASSERT_EQ_INT(q.capacity, 64);  // next power of 2
    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_init_null) {
    ASSERT_FALSE(xr_steal_queue_init(NULL, 32));
}

/* ========== Push / Pop (LIFO) ========== */

TEST(steal_queue_push_pop_single) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    ASSERT_TRUE(xr_steal_queue_push(&q, MOCK_CORO(1)));
    ASSERT_EQ_INT(xr_steal_queue_size(&q), 1);
    ASSERT_FALSE(xr_steal_queue_empty(&q));

    struct XrCoroutine *got = xr_steal_queue_pop(&q);
    ASSERT_EQ_PTR(got, MOCK_CORO(1));
    ASSERT_TRUE(xr_steal_queue_empty(&q));

    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_push_pop_lifo) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    // Push 1, 2, 3
    xr_steal_queue_push(&q, MOCK_CORO(1));
    xr_steal_queue_push(&q, MOCK_CORO(2));
    xr_steal_queue_push(&q, MOCK_CORO(3));
    ASSERT_EQ_INT(xr_steal_queue_size(&q), 3);

    // Pop should be LIFO: 3, 2, 1
    ASSERT_EQ_PTR(xr_steal_queue_pop(&q), MOCK_CORO(3));
    ASSERT_EQ_PTR(xr_steal_queue_pop(&q), MOCK_CORO(2));
    ASSERT_EQ_PTR(xr_steal_queue_pop(&q), MOCK_CORO(1));
    ASSERT_TRUE(xr_steal_queue_empty(&q));

    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_pop_empty) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    ASSERT_NULL(xr_steal_queue_pop(&q));

    xr_steal_queue_destroy(&q);
}

/* ========== Steal (FIFO) ========== */

TEST(steal_queue_steal_single) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    xr_steal_queue_push(&q, MOCK_CORO(1));
    struct XrCoroutine *stolen = xr_steal_queue_steal(&q);
    ASSERT_EQ_PTR(stolen, MOCK_CORO(1));
    ASSERT_TRUE(xr_steal_queue_empty(&q));

    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_steal_fifo) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    // Push 1, 2, 3
    xr_steal_queue_push(&q, MOCK_CORO(1));
    xr_steal_queue_push(&q, MOCK_CORO(2));
    xr_steal_queue_push(&q, MOCK_CORO(3));

    // Steal should be FIFO: 1, 2, 3
    ASSERT_EQ_PTR(xr_steal_queue_steal(&q), MOCK_CORO(1));
    ASSERT_EQ_PTR(xr_steal_queue_steal(&q), MOCK_CORO(2));
    ASSERT_EQ_PTR(xr_steal_queue_steal(&q), MOCK_CORO(3));
    ASSERT_TRUE(xr_steal_queue_empty(&q));

    xr_steal_queue_destroy(&q);
}

TEST(steal_queue_steal_empty) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    ASSERT_NULL(xr_steal_queue_steal(&q));

    xr_steal_queue_destroy(&q);
}

/* ========== Mixed Push / Pop / Steal ========== */

TEST(steal_queue_mixed_ops) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    // Push 1, 2, 3, 4
    xr_steal_queue_push(&q, MOCK_CORO(1));
    xr_steal_queue_push(&q, MOCK_CORO(2));
    xr_steal_queue_push(&q, MOCK_CORO(3));
    xr_steal_queue_push(&q, MOCK_CORO(4));

    // Steal from head: gets 1
    ASSERT_EQ_PTR(xr_steal_queue_steal(&q), MOCK_CORO(1));
    // Pop from tail: gets 4
    ASSERT_EQ_PTR(xr_steal_queue_pop(&q), MOCK_CORO(4));

    ASSERT_EQ_INT(xr_steal_queue_size(&q), 2);

    // Remaining: 2, 3
    ASSERT_EQ_PTR(xr_steal_queue_steal(&q), MOCK_CORO(2));
    ASSERT_EQ_PTR(xr_steal_queue_pop(&q), MOCK_CORO(3));
    ASSERT_TRUE(xr_steal_queue_empty(&q));

    xr_steal_queue_destroy(&q);
}

/* ========== Capacity Full ========== */

TEST(steal_queue_full) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 4);

    // Fill up
    ASSERT_TRUE(xr_steal_queue_push(&q, MOCK_CORO(1)));
    ASSERT_TRUE(xr_steal_queue_push(&q, MOCK_CORO(2)));
    ASSERT_TRUE(xr_steal_queue_push(&q, MOCK_CORO(3)));
    ASSERT_TRUE(xr_steal_queue_push(&q, MOCK_CORO(4)));

    // Should be full
    ASSERT_FALSE(xr_steal_queue_push(&q, MOCK_CORO(5)));

    // Pop one, then push should work
    xr_steal_queue_pop(&q);
    ASSERT_TRUE(xr_steal_queue_push(&q, MOCK_CORO(5)));

    xr_steal_queue_destroy(&q);
}

/* ========== Snapshot ========== */

TEST(steal_queue_snapshot) {
    XrStealQueue q;
    xr_steal_queue_init(&q, 16);

    xr_steal_queue_push(&q, MOCK_CORO(10));
    xr_steal_queue_push(&q, MOCK_CORO(20));
    xr_steal_queue_push(&q, MOCK_CORO(30));

    struct XrCoroutine *buf[8];
    int count = xr_steal_queue_snapshot(&q, buf, 8);
    ASSERT_EQ_INT(count, 3);

    // Snapshot doesn't remove items
    ASSERT_EQ_INT(xr_steal_queue_size(&q), 3);

    xr_steal_queue_destroy(&q);
}

/* ========== NULL Safety ========== */

TEST(steal_queue_null_safety) {
    ASSERT_FALSE(xr_steal_queue_push(NULL, MOCK_CORO(1)));
    ASSERT_NULL(xr_steal_queue_pop(NULL));
    ASSERT_NULL(xr_steal_queue_steal(NULL));
    ASSERT_EQ_INT(xr_steal_queue_size(NULL), 0);
    ASSERT_TRUE(xr_steal_queue_empty(NULL));
    xr_steal_queue_destroy(NULL);  // should not crash
    ASSERT_TRUE(1);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("StealQueue - Lifecycle");
RUN_TEST(steal_queue_init_destroy);
RUN_TEST(steal_queue_init_custom_capacity);
RUN_TEST(steal_queue_init_rounds_to_power_of_two);
RUN_TEST(steal_queue_init_null);

RUN_TEST_SUITE("StealQueue - Push / Pop (LIFO)");
RUN_TEST(steal_queue_push_pop_single);
RUN_TEST(steal_queue_push_pop_lifo);
RUN_TEST(steal_queue_pop_empty);

RUN_TEST_SUITE("StealQueue - Steal (FIFO)");
RUN_TEST(steal_queue_steal_single);
RUN_TEST(steal_queue_steal_fifo);
RUN_TEST(steal_queue_steal_empty);

RUN_TEST_SUITE("StealQueue - Mixed Operations");
RUN_TEST(steal_queue_mixed_ops);

RUN_TEST_SUITE("StealQueue - Capacity");
RUN_TEST(steal_queue_full);

RUN_TEST_SUITE("StealQueue - Snapshot");
RUN_TEST(steal_queue_snapshot);

RUN_TEST_SUITE("StealQueue - NULL Safety");
RUN_TEST(steal_queue_null_safety);

TEST_MAIN_END()
