/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mpsc_queue.c - Unit tests for lock-free MPSC queue
 *
 * KEY CONCEPT:
 *   Tests MPSC queue init, push, drain, and empty operations
 *   using mock XrCoroutine structs with matching sched_link layout.
 */

#include "../test_framework.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/*
 * Mock XrCoroutine with matching layout for sched_link field.
 * Real layout: XrGCHeader(16) + flags(4) + reductions(4) + sched_link(8)
 * We only need sched_link at offset 24.
 */
struct XrGCHeader {
    struct XrGCHeader *gc_next;
    uint8_t type;
    uint8_t marked;
    uint16_t extra;
    uint32_t objsize;
};

struct XrCoroutine {
    struct XrGCHeader gc;
    _Atomic uint32_t flags;
    int32_t reductions;
    struct XrCoroutine *sched_link;
    /* rest omitted */
    int mock_id;  // test helper
};

#include "coro/xmpsc_queue.h"

// Re-declare functions (they're in xray_core lib)
void xr_mpsc_init(XrMPSCQueue *q);
void xr_mpsc_push(XrMPSCQueue *q, struct XrCoroutine *coro);
struct XrCoroutine *xr_mpsc_drain(XrMPSCQueue *q);
bool xr_mpsc_empty(XrMPSCQueue *q);

// Helper: create a mock coroutine on stack
static struct XrCoroutine mock_coros[16];

static void init_mock_coros(void) {
    memset(mock_coros, 0, sizeof(mock_coros));
    for (int i = 0; i < 16; i++) {
        mock_coros[i].mock_id = i + 1;
    }
}

/* ========== Init ========== */

TEST(mpsc_init) {
    XrMPSCQueue q;
    xr_mpsc_init(&q);
    ASSERT_TRUE(xr_mpsc_empty(&q));
}

/* ========== Push and Drain ========== */

TEST(mpsc_push_single) {
    init_mock_coros();
    XrMPSCQueue q;
    xr_mpsc_init(&q);

    xr_mpsc_push(&q, &mock_coros[0]);
    ASSERT_FALSE(xr_mpsc_empty(&q));

    struct XrCoroutine *list = xr_mpsc_drain(&q);
    ASSERT_NOT_NULL(list);
    ASSERT_EQ_INT(list->mock_id, 1);
    ASSERT_NULL(list->sched_link);
    ASSERT_TRUE(xr_mpsc_empty(&q));
}

TEST(mpsc_push_multiple) {
    init_mock_coros();
    XrMPSCQueue q;
    xr_mpsc_init(&q);

    // Push 3 items
    xr_mpsc_push(&q, &mock_coros[0]);
    xr_mpsc_push(&q, &mock_coros[1]);
    xr_mpsc_push(&q, &mock_coros[2]);

    ASSERT_FALSE(xr_mpsc_empty(&q));

    // Drain returns all as linked list (newest first - Treiber stack)
    struct XrCoroutine *list = xr_mpsc_drain(&q);
    ASSERT_NOT_NULL(list);
    ASSERT_TRUE(xr_mpsc_empty(&q));

    // Count items in list
    int count = 0;
    struct XrCoroutine *cur = list;
    while (cur) {
        count++;
        cur = cur->sched_link;
    }
    ASSERT_EQ_INT(count, 3);
}

TEST(mpsc_drain_empty) {
    XrMPSCQueue q;
    xr_mpsc_init(&q);

    struct XrCoroutine *list = xr_mpsc_drain(&q);
    ASSERT_NULL(list);
}

TEST(mpsc_push_drain_push) {
    init_mock_coros();
    XrMPSCQueue q;
    xr_mpsc_init(&q);

    // First batch
    xr_mpsc_push(&q, &mock_coros[0]);
    xr_mpsc_push(&q, &mock_coros[1]);

    struct XrCoroutine *list1 = xr_mpsc_drain(&q);
    ASSERT_NOT_NULL(list1);
    ASSERT_TRUE(xr_mpsc_empty(&q));

    // Second batch
    xr_mpsc_push(&q, &mock_coros[2]);
    xr_mpsc_push(&q, &mock_coros[3]);
    xr_mpsc_push(&q, &mock_coros[4]);

    struct XrCoroutine *list2 = xr_mpsc_drain(&q);
    ASSERT_NOT_NULL(list2);
    ASSERT_TRUE(xr_mpsc_empty(&q));

    // Count second batch
    int count = 0;
    struct XrCoroutine *cur = list2;
    while (cur) {
        count++;
        cur = cur->sched_link;
    }
    ASSERT_EQ_INT(count, 3);
}

/* ========== Treiber Stack Order ========== */

TEST(mpsc_treiber_order) {
    init_mock_coros();
    XrMPSCQueue q;
    xr_mpsc_init(&q);

    // Push 1, 2, 3 in order
    xr_mpsc_push(&q, &mock_coros[0]);  // id=1
    xr_mpsc_push(&q, &mock_coros[1]);  // id=2
    xr_mpsc_push(&q, &mock_coros[2]);  // id=3

    // Drain: Treiber stack returns newest first (3, 2, 1)
    struct XrCoroutine *list = xr_mpsc_drain(&q);
    ASSERT_EQ_INT(list->mock_id, 3);
    ASSERT_EQ_INT(list->sched_link->mock_id, 2);
    ASSERT_EQ_INT(list->sched_link->sched_link->mock_id, 1);
    ASSERT_NULL(list->sched_link->sched_link->sched_link);
}

/* ========== Repeated Operations ========== */

TEST(mpsc_stress_sequential) {
    init_mock_coros();
    XrMPSCQueue q;
    xr_mpsc_init(&q);

    // Push and drain 10 times
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 5; i++) {
            xr_mpsc_push(&q, &mock_coros[i]);
        }

        struct XrCoroutine *list = xr_mpsc_drain(&q);
        int count = 0;
        while (list) {
            count++;
            list = list->sched_link;
        }
        ASSERT_EQ_INT(count, 5);
        ASSERT_TRUE(xr_mpsc_empty(&q));
    }
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("MPSC Queue - Init");
RUN_TEST(mpsc_init);

RUN_TEST_SUITE("MPSC Queue - Push/Drain");
RUN_TEST(mpsc_push_single);
RUN_TEST(mpsc_push_multiple);
RUN_TEST(mpsc_drain_empty);
RUN_TEST(mpsc_push_drain_push);

RUN_TEST_SUITE("MPSC Queue - Ordering");
RUN_TEST(mpsc_treiber_order);

RUN_TEST_SUITE("MPSC Queue - Stress");
RUN_TEST(mpsc_stress_sequential);

TEST_MAIN_END()
