/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_hash_core.c - Unit tests for runtime-neutral AOT/CGen hash primitives
 */

#include "../test_framework.h"
#include "shared/xr_hash_core.h"

TEST(hash_core_mix_is_stable) {
    ASSERT_EQ_UINT(xr_hash_core_mix_u64(1), UINT64_C(0x5692161d100b05e5));
    ASSERT_EQ_UINT(xr_hash_core_mix_u64(42), UINT64_C(0xa759ea27d4727622));
}

TEST(hash_core_bytes_are_stable) {
    ASSERT_EQ_UINT(xr_hash_core_bytes("", 0), UINT64_C(0x552d3fb62b3c344f));
    ASSERT_EQ_UINT(xr_hash_core_bytes("hello", 5), UINT64_C(0xb7f9172fac45c7e0));
    ASSERT_EQ_UINT(xr_hash_core_bytes("xray", 4), UINT64_C(0x09cd336fe34f97c4));
}

TEST(hash_core_string_hash_is_nonzero_and_stable) {
    ASSERT_EQ_UINT(xr_hash_core_str_hash_bytes("", 0), UINT32_C(725365839));
    ASSERT_EQ_UINT(xr_hash_core_str_hash_bytes("hello", 5), UINT32_C(2890254304));
    ASSERT_EQ_UINT(xr_hash_core_str_hash_bytes("xray", 4), UINT32_C(3813644228));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Hash Core");
RUN_TEST(hash_core_mix_is_stable);
RUN_TEST(hash_core_bytes_are_stable);
RUN_TEST(hash_core_string_hash_is_nonzero_and_stable);

TEST_MAIN_END()
