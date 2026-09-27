/* Exact decimal witnesses exercise target rounding without a host float oracle. */
#ifndef XIR_DECIMAL_CASES_H
#define XIR_DECIMAL_CASES_H
#include "shared/xr_decimal_float.h"
#include <fenv.h>
static void decimal_vectors(void) {
    static const struct { const char *text; uint32_t width; uint64_t expected; } cases[] = {
#define XIR_DECIMAL(text, width, expected) {text, width, expected},
#include "xir_decimal_vectors.def"
#undef XIR_DECIMAL
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint64_t actual = UINT64_MAX;
        CHECK(xr_decimal_float_parse(cases[i].text, strlen(cases[i].text), cases[i].width, &actual));
        if (actual != cases[i].expected) fprintf(stderr, "decimal witness %zu width %u: %llx != %llx\n",
            i, cases[i].width, (unsigned long long) actual, (unsigned long long) cases[i].expected);
        CHECK(actual == cases[i].expected);
    }
}
static void decimal_cases(void) {
    static const char *invalid[] = {"", "+", "-", ".", "e1", "1e", "1e+", "1e-", "1.2.3", "_1.0",
        "1_.0", "1._0", "1.0_", "1__0.0", "1e_1", "1e1_", "1e1__0", " 1.0", "1.0 ", "nan", "inf", "0x1p0"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        uint64_t result = UINT64_MAX;
        CHECK(!xr_decimal_float_parse(invalid[i], strlen(invalid[i]), 64, &result) && result == 0);
    }
    uint64_t result = UINT64_MAX;
    CHECK(!xr_decimal_float_parse(NULL, 1, 64, &result) && result == 0);
    CHECK(!xr_decimal_float_parse("1", (size_t) INT32_MAX + 1, 64, &result) && result == 0);
    CHECK(!xr_decimal_float_parse("1", 1, 16, &result) && result == 0);
    CHECK(!xr_decimal_float_parse("1", 1, 64, NULL));
    CHECK(xr_decimal_float_parse("1.25trailing", 4, 32, &result) && result == UINT64_C(0x3fa00000));
    CHECK(!xr_decimal_float_parse("1.\0x", 4, 32, &result) && result == 0);
    int previous = fegetround();
    static const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]); ++i) {
        CHECK(fesetround(modes[i]) == 0);
        CHECK(feclearexcept(FE_ALL_EXCEPT) == 0);
        CHECK(feraiseexcept(FE_INEXACT) == 0);
        int before = fetestexcept(FE_ALL_EXCEPT);
        decimal_vectors();
        CHECK(fetestexcept(FE_ALL_EXCEPT) == before && fegetround() == modes[i]);
    }
    CHECK(fesetround(previous) == 0);
    CHECK(feclearexcept(FE_ALL_EXCEPT) == 0);
}
#endif // XIR_DECIMAL_CASES_H
