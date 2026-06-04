/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_handoff_budget.c - Unit tests for bounded handoff M allocation
 */

#include "../test_framework.h"
#include "coro/xworker.h"
#include <stdatomic.h>
#include <string.h>

TEST(reserve_handoff_m_stops_at_cap) {
    XrRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.worker_count = 2;
    runtime.handoff_max_m = 4;
    atomic_store(&runtime.m_count, 2);

    ASSERT_EQ_INT(xr_runtime_reserve_handoff_m_id(&runtime), 2);
    ASSERT_EQ_INT(xr_runtime_reserve_handoff_m_id(&runtime), 3);
    ASSERT_EQ_INT(xr_runtime_reserve_handoff_m_id(&runtime), -1);
    ASSERT_EQ_INT(atomic_load(&runtime.m_count), 4);
}

TEST(reserve_handoff_m_rejects_null_runtime) {
    ASSERT_EQ_INT(xr_runtime_reserve_handoff_m_id(NULL), -1);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Handoff Budget");
RUN_TEST(reserve_handoff_m_stops_at_cap);
RUN_TEST(reserve_handoff_m_rejects_null_runtime);

TEST_MAIN_END()
