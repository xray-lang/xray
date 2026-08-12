/* Freestanding shared numeric-neg owner KAT. */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

TEST(freestanding_numeric_neg_owner_preserves_scalar_edges) {
    XrNumericNegResult integer = xrt_numeric_neg_eval(XR_NUMERIC_NEG_I64, INT64_MIN, 0.0);
    ASSERT_EQ_INT(integer.i64, INT64_MIN);

    uint64_t bits = UINT64_C(0x7ff8000000001234);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    XrNumericNegResult floating = xrt_numeric_neg_eval(XR_NUMERIC_NEG_F64, 0, value);
    uint64_t result_bits = 0;
    memcpy(&result_bits, &floating.f64, sizeof(result_bits));
    ASSERT_EQ_UINT(result_bits, UINT64_C(0xfff8000000001234));

    ASSERT_EQ_INT(XR_TO_INT(xrt_neg(XR_FROM_INT(INT64_MIN))), INT64_MIN);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Numeric Neg Owner");
RUN_TEST(freestanding_numeric_neg_owner_preserves_scalar_edges);
TEST_MAIN_END()
