/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_byte_compare_core.c - Independent byte equality and boundary cases
 */
#include "shared/xr_byte_compare_core.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "byte comparison check failed at line %d\n", __LINE__); return 1; \
} } while (0)

int main(void) {
    unsigned char left[65], right[65];
    struct Cell { unsigned tag; union { unsigned char byte; double number; } value; } cells[65];
    memset(cells, 0x5a, sizeof(cells));
    CHECK(xr_crypto_core_timing_safe_equal(NULL, 0u, 1u, NULL, 0u, 1u));
    CHECK(!xr_crypto_core_timing_safe_equal(NULL, 1u, 1u, NULL, 0u, 1u));
    CHECK(!xr_crypto_core_timing_safe_equal(NULL, 0u, 1u, NULL, 1u, 1u));
    for (unsigned seed = 0u; seed < 256u; ++seed) {
        for (size_t index = 0u; index < sizeof(left); ++index)
            left[index] = (unsigned char)(seed + index * 17u);
        memcpy(right, left, sizeof(left));
        for (size_t index = 0u; index < 65u; ++index)
            cells[index].value.byte = left[index];
        for (size_t length = 0u; length <= sizeof(left); ++length) {
            CHECK(xr_crypto_core_timing_safe_equal(&cells[0].value.byte, length, sizeof(cells[0]),
                                                  left, length, 1u));
            CHECK(xr_crypto_core_timing_safe_equal(&cells[0].value.byte, length, sizeof(cells[0]),
                                                  &cells[0].value.byte, length, sizeof(cells[0])));
            CHECK(xr_crypto_core_timing_safe_equal(left, length, 1u,
                                                  right, length, 1u));
            CHECK(xr_crypto_core_timing_safe_equal(left, length, 1u,
                                                  left, length, 1u));
            if (length != 0u) {
                CHECK(!xr_crypto_core_timing_safe_equal(left, length, 1u,
                                                       right, length - 1u, 1u));
                CHECK(!xr_crypto_core_timing_safe_equal(left, length - 1u, 1u,
                                                       right, length, 1u));
            }
            for (size_t mismatch = 0u; mismatch < length; ++mismatch) {
                right[mismatch] ^= 0x80u;
                CHECK(!xr_crypto_core_timing_safe_equal(&cells[0].value.byte, length, sizeof(cells[0]),
                                                       right, length, 1u));
                CHECK(!xr_crypto_core_timing_safe_equal(left, length, 1u,
                                                       right, length, 1u));
                right[mismatch] ^= 0x80u;
            }
        }
    }
    CHECK(xr_crypto_core_timing_safe_equal(NULL, 0u, 0u, NULL, 0u, 0u));
    CHECK(!xr_crypto_core_timing_safe_equal(left, 1u, 0u, right, 1u, 1u));
    CHECK(!xr_crypto_core_timing_safe_equal(left, 1u, 1u, right, 1u, 0u));
    CHECK(!xr_crypto_core_timing_safe_equal(left, SIZE_MAX, 2u, right, 0u, 1u));
    CHECK(!xr_crypto_core_timing_safe_equal(left, 0u, 1u, right, SIZE_MAX, 2u));
    puts("byte comparison: equality, aliasing, public lengths, every mismatch position PASS");
    return 0;
}
