/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_arithmetic_cases.h - Independent arithmetic bit witnesses
 *
 * KEY CONCEPT:
 *   The host rounding state cannot change the explicit IEEE result.
 */
#ifndef XIR_FLOAT_ARITHMETIC_CASES_H
#define XIR_FLOAT_ARITHMETIC_CASES_H
static void float_arithmetic_cases(void) {
    static const struct {
        uint32_t width; XrXirFloatOperation operation; uint64_t a, b, result;
    } vectors[] = {
#define XIR_FLOAT_ARITHMETIC(width, operation, a, b, result) {width, operation, a, b, result},
#include "xir_float_arithmetic_vectors.def"
#undef XIR_FLOAT_ARITHMETIC
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        uint64_t result = UINT64_MAX;
        CHECK(xr_xir_float_arithmetic(vectors[i].width, vectors[i].operation,
            vectors[i].a, vectors[i].b, &result) == XR_XIR_NUMERIC_OK);
        if (result != vectors[i].result)
            fprintf(stderr, "float arithmetic vector %zu: got %llx expected %llx\n", i,
                (unsigned long long) result, (unsigned long long) vectors[i].result);
        CHECK(result == vectors[i].result);
    }
    uint64_t result = 123;
    CHECK(xr_xir_float_arithmetic(16, XR_XIR_FLOAT_ADD, 0, 0, &result) == XR_XIR_NUMERIC_BAD_ARGUMENT && !result);
    result = 123;
    CHECK(xr_xir_float_arithmetic(32, XR_XIR_FLOAT_ADD, UINT64_C(0x100000000), 0, &result) == XR_XIR_NUMERIC_BAD_ARGUMENT && !result);
    result = 123;
    CHECK(xr_xir_float_arithmetic(32, XR_XIR_FLOAT_DIVIDE, 0, UINT64_C(0x100000000), &result) == XR_XIR_NUMERIC_BAD_ARGUMENT && !result);
    result = 123;
    CHECK(xr_xir_float_arithmetic(64, (XrXirFloatOperation) -1, 0, 0, &result) == XR_XIR_NUMERIC_BAD_ARGUMENT && !result);
    result = 123;
    CHECK(xr_xir_float_arithmetic(64, (XrXirFloatOperation) 4, 0, 0, &result) == XR_XIR_NUMERIC_BAD_ARGUMENT && !result);
    CHECK(xr_xir_float_arithmetic(64, XR_XIR_FLOAT_ADD, 0, 0, NULL) == XR_XIR_NUMERIC_BAD_ARGUMENT);
}
#endif // XIR_FLOAT_ARITHMETIC_CASES_H
