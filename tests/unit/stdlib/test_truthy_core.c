/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_truthy_core.c - Unit tests for runtime-neutral truthiness rules
 */

#include "../test_framework.h"
#include "shared/xr_truthy_core.h"

TEST(truthy_core_scalars_match_language) {
    ASSERT(!xr_truthy_core_eval(XR_TRUTHY_CORE_NULL, 0, 0.0, 0));
    ASSERT(!xr_truthy_core_eval(XR_TRUTHY_CORE_BOOL, 0, 0.0, 0));
    ASSERT(xr_truthy_core_eval(XR_TRUTHY_CORE_BOOL, 1, 0.0, 0));
    ASSERT(!xr_truthy_core_eval(XR_TRUTHY_CORE_INT, 0, 0.0, 0));
    ASSERT(xr_truthy_core_eval(XR_TRUTHY_CORE_INT, -1, 0.0, 0));
    ASSERT(!xr_truthy_core_eval(XR_TRUTHY_CORE_FLOAT, 0, 0.0, 0));
    ASSERT(xr_truthy_core_eval(XR_TRUTHY_CORE_FLOAT, 0, -0.25, 0));
}

TEST(truthy_core_sized_values_use_length) {
    ASSERT(!xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0, 0));
    ASSERT(xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0, 1));
    ASSERT(xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0, -1));
}

TEST(truthy_core_unknown_objects_are_truthy) {
    ASSERT(xr_truthy_core_eval(XR_TRUTHY_CORE_OBJECT, 0, 0.0, 0));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Truthy Core");
RUN_TEST(truthy_core_scalars_match_language);
RUN_TEST(truthy_core_sized_values_use_length);
RUN_TEST(truthy_core_unknown_objects_are_truthy);

TEST_MAIN_END()
