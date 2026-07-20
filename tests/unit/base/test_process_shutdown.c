/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_process_shutdown.c - task 218 defense line 4: xr_process_shutdown()
 * must release process-wide registries safely and idempotently.
 */

#include "../test_framework.h"
#include "runtime/value/xtype.h"
#include "runtime/xr_process_shutdown.h"
#include <stdint.h>

/* Defined with external linkage in xtype.c; not part of the public header. */
XrTypePool *xr_type_get_current_pool(void);

TEST(process_shutdown_clears_current_type_pool) {
    xr_type_global_init();
    /* Borrow a sentinel current type pool, as the analyzer would. */
    xr_type_set_current_pool((XrTypePool *) (uintptr_t) 0x1, NULL);
    xr_process_shutdown();
    /* The borrowed pool pointer must be cleared — no stale cross-lifetime
     * borrow survives process shutdown. */
    ASSERT_NULL(xr_type_get_current_pool());
}

TEST(process_shutdown_is_idempotent) {
    xr_process_shutdown();
    xr_process_shutdown();
    ASSERT_NULL(xr_type_get_current_pool());
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("process shutdown (task 218 line 4)");
RUN_TEST(process_shutdown_clears_current_type_pool);
RUN_TEST(process_shutdown_is_idempotent);
TEST_MAIN_END()
