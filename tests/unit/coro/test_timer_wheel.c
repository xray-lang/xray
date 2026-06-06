/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_timer_wheel.c - Unit tests for coroutine timer wheel cancellation
 */

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "coro/xtimer_wheel.h"
#include "coro/xworker.h"
#include <stdatomic.h>
#include <string.h>

static void timer_test_noop(void *arg) {
    (void) arg;
}

TEST(cancel_queue_drains_clustered_remote_timers) {
    enum {
        TIMER_COUNT = 8192
    };

    XrRuntime runtime;
    XrWorker worker;
    memset(&runtime, 0, sizeof(runtime));
    memset(&worker, 0, sizeof(worker));
    runtime.worker_count = 1;
    runtime.workers = &worker;

    XrTimerWheel *tw = xr_timer_wheel_create(&runtime, 0);
    ASSERT_NOT_NULL(tw);

    XrTWheelTimer *timers = (XrTWheelTimer *) xr_calloc(TIMER_COUNT, sizeof(XrTWheelTimer));
    ASSERT_NOT_NULL(timers);

    int64_t deadline = xr_monotonic_ticks() + 60000;
    for (int i = 0; i < TIMER_COUNT; i++) {
        timers[i].slot = XR_TW_SLOT_INACTIVE;
        atomic_store_explicit(&timers[i].state, XR_TIMER_STATE_ACTIVE, memory_order_relaxed);
        atomic_store_explicit(&timers[i].cancel_next, NULL, memory_order_relaxed);
        ASSERT_TRUE(xr_twheel_set_timer(tw, &timers[i], timer_test_noop, NULL, deadline));
    }
    ASSERT_EQ_INT(tw->nto, TIMER_COUNT);
    ASSERT_EQ_INT(tw->later.nto, TIMER_COUNT);

    for (int i = 0; i < TIMER_COUNT; i++) {
        xr_timer_queue_cancel(tw, &timers[i]);
    }
    ASSERT_TRUE(xr_timer_cancel_pending(tw));

    int processed = xr_timer_process_canceled_queue(tw);
    ASSERT_EQ_INT(processed, TIMER_COUNT);
    ASSERT_EQ_INT(tw->nto, 0);
    ASSERT_EQ_INT(tw->soon.nto, 0);
    ASSERT_EQ_INT(tw->later.nto, 0);
    ASSERT_FALSE(xr_timer_cancel_pending(tw));

    for (int i = 0; i < TIMER_COUNT; i++) {
        ASSERT_EQ_INT(timers[i].slot, XR_TW_SLOT_INACTIVE);
        ASSERT_EQ_INT(atomic_load_explicit(&timers[i].state, memory_order_acquire),
                      XR_TIMER_STATE_ACTIVE);
    }

    xr_free(timers);
    xr_timer_wheel_destroy(tw);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Timer Wheel");
RUN_TEST(cancel_queue_drains_clustered_remote_timers);

TEST_MAIN_END()
