/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_bits_wide.c - Strict C11 compilation and independent wide-product vectors
 */
#include "shared/xr_bits_core.h"
#include <stdio.h>

int main(void) {
    const struct { uint64_t a, b, high; } unsigned_cases[] = {
        {0, UINT64_MAX, 0}, {1, UINT64_MAX, 0},
        {UINT64_MAX, UINT64_MAX, UINT64_MAX - 1},
        {UINT64_C(0x8000000000000000), 2, 1},
        {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210), UINT64_C(0x0121fa00ad77d742)},
        {UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555), UINT64_C(0x38e38e38e38e38e3)},
    };
    const struct { int64_t a, b, high; } signed_cases[] = {
        {0, INT64_MIN, 0}, {1, -1, -1}, {-1, -1, 0},
        {INT64_MIN, INT64_MIN, INT64_C(4611686018427387904)},
        {INT64_MIN, INT64_MAX, -INT64_C(4611686018427387904)},
        {INT64_MAX, INT64_MAX, INT64_C(4611686018427387903)},
        {INT64_MIN, -1, 0},
        {-INT64_C(0x0123456789abcdef), INT64_C(0x76543210fedcba98), -INT64_C(37895533524870825)},
    };
    for (size_t n = 0; n < sizeof(unsigned_cases) / sizeof(unsigned_cases[0]); ++n) {
        uint64_t a = unsigned_cases[n].a, b = unsigned_cases[n].b;
        if (xr_bits_umulh64(a, b) != unsigned_cases[n].high ||
            xr_bits_umulh64(b, a) != unsigned_cases[n].high) {
            fprintf(stderr, "unsigned vector %zu failed\n", n);
            return 1;
        }
    }
    for (size_t n = 0; n < sizeof(signed_cases) / sizeof(signed_cases[0]); ++n) {
        int64_t a = signed_cases[n].a, b = signed_cases[n].b;
        if (xr_bits_smulh64(a, b) != signed_cases[n].high ||
            xr_bits_smulh64(b, a) != signed_cases[n].high) {
            fprintf(stderr, "signed vector %zu failed\n", n);
            return 1;
        }
    }
    puts("Wide multiply signed and unsigned vectors passed");
    return 0;
}
