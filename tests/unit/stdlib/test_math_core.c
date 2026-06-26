/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_math_core.c - Unit tests for runtime-neutral math core helpers
 */

#include "../test_framework.h"
#include "shared/xr_math_core.h"
#include <stdint.h>
#include <string.h>

typedef struct MathRandomSeq {
    const uint64_t *values;
    size_t count;
    size_t index;
} MathRandomSeq;

static void math_test_random_bytes(void *ctx, unsigned char *buf, size_t len) {
    MathRandomSeq *seq = (MathRandomSeq *) ctx;
    uint64_t value = 0;
    if (seq && seq->index < seq->count)
        value = seq->values[seq->index++];
    memset(buf, 0, len);
    memcpy(buf, &value, len < sizeof(value) ? len : sizeof(value));
}

TEST(math_core_random_f64_uses_top_53_bits) {
    const uint64_t values[] = {UINT64_C(1) << 63};
    MathRandomSeq seq = {values, 1, 0};
    double out = xr_math_core_random_f64(math_test_random_bytes, &seq);
    ASSERT_FLOAT_EQ(out, 0.5, 0.000000000001);
    ASSERT_EQ_UINT(seq.index, 1);
}

TEST(math_core_random_i64_equal_and_swapped_range) {
    const uint64_t values[] = {UINT64_C(13)};
    MathRandomSeq seq = {values, 1, 0};
    ASSERT_EQ_INT(xr_math_core_random_i64(math_test_random_bytes, &seq, 7, 7), 7);
    ASSERT_EQ_UINT(seq.index, 0);
    ASSERT_EQ_INT(xr_math_core_random_i64(math_test_random_bytes, &seq, 10, 1), 4);
    ASSERT_EQ_UINT(seq.index, 1);
}

TEST(math_core_random_i64_rejects_modulo_bias_tail) {
    const uint64_t values[] = {UINT64_C(0), UINT64_C(15)};
    MathRandomSeq seq = {values, 2, 0};
    ASSERT_EQ_INT(xr_math_core_random_i64(math_test_random_bytes, &seq, 10, 19), 15);
    ASSERT_EQ_UINT(seq.index, 2);
}

TEST(math_core_random_i64_full_range_wraps_from_unsigned) {
    const uint64_t values[] = {UINT64_C(0)};
    MathRandomSeq seq = {values, 1, 0};
    ASSERT_EQ_INT(xr_math_core_random_i64(math_test_random_bytes, &seq, INT64_MIN, INT64_MAX),
                  INT64_MIN);
    ASSERT_EQ_UINT(seq.index, 1);
}

TEST(math_core_int_argument_rules) {
    ASSERT_EQ_INT(xr_math_core_int_arg_or(true, 42, 0), 42);
    ASSERT_EQ_INT(xr_math_core_int_arg_or(false, 42, 0), 0);
    ASSERT_EQ_INT(xr_math_core_int_arg_or(false, -7, 99), 99);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Math Core - Random");
RUN_TEST(math_core_random_f64_uses_top_53_bits);
RUN_TEST(math_core_random_i64_equal_and_swapped_range);
RUN_TEST(math_core_random_i64_rejects_modulo_bias_tail);
RUN_TEST(math_core_random_i64_full_range_wraps_from_unsigned);
RUN_TEST(math_core_int_argument_rules);

TEST_MAIN_END()
