/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_xi_prefix_checks.inc.c - Initializer prefix membership boundaries
 */

#include "program/xr_program_xi_prefix.h"

static void assert_initializer_prefix_membership(void) {
    XiValue values[9] = {0};
    XiValue *order[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        order[i] = &values[i];
        values[i].id = 100u - i;
    }
    XiBlock block = {0};
    block.values = order;
    block.nvalues = 8u;
    for (uint32_t limit = 0u; limit <= 8u; ++limit) {
        for (uint32_t start = 0u; start <= 9u; ++start) {
            for (uint32_t target = 0u; target < 9u; ++target) {
                uint32_t cursor = start;
                ASSERT_EQ_INT(xr_program_xi_prefix_contains(&block, limit, &values[target], &cursor),
                              target < limit);
                if (target < limit) ASSERT_EQ_UINT(cursor, target);
                else ASSERT_EQ_UINT(cursor, start);
            }
        }
    }
    const uint32_t sequence[] = {7, 7, 0, 6, 1, 5, 2, 4, 3, 8, 0};
    uint32_t cursor = UINT32_MAX;
    for (uint32_t i = 0u; i < XR_COUNTOF(sequence); ++i) {
        uint32_t target = sequence[i];
        ASSERT_EQ_INT(xr_program_xi_prefix_contains(&block, 8u, &values[target], &cursor), target < 8u);
    }
    ASSERT_FALSE(xr_program_xi_prefix_contains(&block, 9u, &values[0], &cursor));
    ASSERT_FALSE(xr_program_xi_prefix_contains(&block, 8u, NULL, &cursor));
    ASSERT_FALSE(xr_program_xi_prefix_contains(&block, 8u, &values[0], NULL));
    ASSERT_FALSE(xr_program_xi_prefix_contains(NULL, 8u, &values[0], &cursor));
    block.values = NULL;
    ASSERT_FALSE(xr_program_xi_prefix_contains(&block, 8u, &values[0], &cursor));
}
