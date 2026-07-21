/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_test_yield.c - Standalone AOT provider for the internal test_yield module
 */

#include "xaot_coro.h"

#include <stdatomic.h>

#include "../os/os_thread.h"

/* Match the VM test module's process-wide atomic counter. */
static _Atomic int64_t g_aot_test_yield_counter = 0;

int64_t xr_aot_test_yield_simple(void) {
    return 42;
}

int64_t xr_aot_test_yield_add(int64_t a, int64_t b) {
    return a + b;
}

int64_t xr_aot_test_yield_sync(void) {
    return 100;
}

int64_t xr_aot_test_yield_blocking_sleep(int64_t milliseconds) {
    if (milliseconds < 0)
        milliseconds = 0;
    if (milliseconds > 1000)
        milliseconds = 1000;
    xr_thread_sleep_ms((unsigned int) milliseconds);
    return milliseconds;
}

void xr_aot_test_yield_counter_inc(void) {
    atomic_fetch_add_explicit(&g_aot_test_yield_counter, 1, memory_order_relaxed);
}

int64_t xr_aot_test_yield_counter_get(void) {
    return atomic_load_explicit(&g_aot_test_yield_counter, memory_order_acquire);
}

int64_t xr_aot_test_yield_counter_reset(void) {
    return atomic_exchange_explicit(&g_aot_test_yield_counter, 0, memory_order_acq_rel);
}
