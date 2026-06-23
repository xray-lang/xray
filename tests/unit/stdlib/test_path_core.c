/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_path_core.c - Unit tests for runtime-neutral path core helpers
 */

#include "../test_framework.h"
#include "shared/xr_path_core.h"

TEST(path_core_constants_match_target) {
    ASSERT_STR_EQ(xr_path_core_sep_str(), "/");
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(xr_path_core_delimiter_str(), ";");
#else
    ASSERT_STR_EQ(xr_path_core_delimiter_str(), ":");
#endif
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("path core");
RUN_TEST(path_core_constants_match_target);
TEST_MAIN_END()
