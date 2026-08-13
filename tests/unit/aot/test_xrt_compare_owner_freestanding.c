/* Freestanding compare semantic-owner KAT: the no-libc prelude answers the six
 * relations through the shared owner, on the same lanes as every other profile. */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"
#include <math.h>

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

static const XrCompareKind ALL_KINDS[6] = {XR_COMPARE_EQ, XR_COMPARE_NE, XR_COMPARE_LT,
                                           XR_COMPARE_LE, XR_COMPARE_GT, XR_COMPARE_GE};

TEST(freestanding_lane_adapters_answer_the_owner) {
    for (int i = 0; i < 6; i++) {
        XrCompareKind kind = ALL_KINDS[i];
        ASSERT_EQ_INT((int) xr_compare_i64_core(kind, -3, 4), (int) xrt_compare_i64(kind, -3, 4));
        ASSERT_EQ_INT((int) xr_compare_u64_core(kind, UINT64_MAX, 1),
                      (int) xrt_compare_u64(kind, UINT64_MAX, 1));
        ASSERT_EQ_INT((int) xr_compare_f64_core(kind, 1.5, 2.5),
                      (int) xrt_compare_f64(kind, 1.5, 2.5));
        ASSERT_EQ_INT((int) xr_compare_ordering_core(kind, -1),
                      (int) xrt_compare_ordering(kind, -1));
        ASSERT_EQ_INT((int) xr_compare_equal_core(kind, true), (int) xrt_compare_equal(kind, true));
        ASSERT_EQ_INT((int) xr_compare_route_core(kind, XR_COMPARE_OPERAND_INT,
                                                  XR_COMPARE_OPERAND_FLOAT),
                      (int) xrt_compare_route(kind, XR_COMPARE_OPERAND_INT,
                                              XR_COMPARE_OPERAND_FLOAT));
    }
}

TEST(freestanding_tagged_equality_answers_each_relation) {
    XrValue seven = XR_FROM_INT(7);
    XrValue eight = XR_FROM_INT(8);
    XrValue truth = XR_FROM_BOOL(1);
    XrValue falsehood = XR_FROM_BOOL(0);
    ASSERT_TRUE(xrt_eq(seven, seven));
    ASSERT_FALSE(xrt_eq(seven, eight));
    ASSERT_TRUE(xrt_compare_tagged_equal(XR_COMPARE_NE, seven, eight));
    ASSERT_FALSE(xrt_compare_tagged_equal(XR_COMPARE_NE, seven, seven));
    ASSERT_TRUE(xrt_eq(truth, truth));
    ASSERT_FALSE(xrt_eq(truth, falsehood));
    /* An order relation over a domain that only knows equality is false. */
    ASSERT_FALSE(xrt_compare_tagged_equal(XR_COMPARE_LT, truth, falsehood));
}

TEST(freestanding_tagged_equality_separates_tag_classes) {
    XrValue integer = XR_FROM_INT(1);
    XrValue floating = XR_FROM_FLOAT(1.0);
    XrValue nothing = XR_NULL_VAL;
    ASSERT_FALSE(xrt_eq(integer, floating));
    ASSERT_TRUE(xrt_compare_tagged_equal(XR_COMPARE_NE, integer, floating));
    ASSERT_FALSE(xrt_eq(integer, nothing));
    ASSERT_TRUE(xrt_eq(nothing, nothing));
}

TEST(freestanding_tagged_order_uses_the_integer_and_double_lanes) {
    XrValue small = XR_FROM_INT(-5);
    XrValue large = XR_FROM_INT(9);
    ASSERT_TRUE(xrt_lt(small, large));
    ASSERT_FALSE(xrt_lt(large, small));
    ASSERT_TRUE(xrt_le(small, small));
    ASSERT_TRUE(xrt_compare_tagged_order(XR_COMPARE_GT, large, small));
    ASSERT_TRUE(xrt_compare_tagged_order(XR_COMPARE_GE, large, large));

    XrValue half = XR_FROM_FLOAT(0.5);
    XrValue one_and_a_half = XR_FROM_FLOAT(1.5);
    ASSERT_TRUE(xrt_lt(half, one_and_a_half));
    ASSERT_TRUE(xrt_le(one_and_a_half, one_and_a_half));
    ASSERT_FALSE(xrt_lt(one_and_a_half, half));
}

TEST(freestanding_order_against_nan_is_always_false) {
    XrValue nan_value = XR_FROM_FLOAT((double) NAN);
    XrValue one = XR_FROM_FLOAT(1.0);
    ASSERT_FALSE(xrt_lt(nan_value, one));
    ASSERT_FALSE(xrt_le(nan_value, one));
    ASSERT_FALSE(xrt_lt(one, nan_value));
    ASSERT_FALSE(xrt_le(one, nan_value));
    ASSERT_FALSE(xrt_eq(nan_value, nan_value));
    ASSERT_TRUE(xrt_compare_tagged_equal(XR_COMPARE_NE, nan_value, nan_value));
}

TEST(freestanding_native_relation_keeps_the_operand_type) {
    uint64_t top = UINT64_C(0x8000000000000000);
    uint64_t five = 5;
    ASSERT_TRUE(xrt_compare_native(GT, top, five));
    ASSERT_FALSE(xrt_compare_native(LT, top, five));
    ASSERT_TRUE(xrt_compare_native(LT, (int64_t) top, (int64_t) five));
    ASSERT_TRUE(xrt_compare_native(EQ, 3.5, 3.5));
    ASSERT_TRUE(xrt_compare_native(NE, 3.5, 4.5));
    ASSERT_TRUE(xrt_compare_native(LE, 3.5, 3.5));
    ASSERT_TRUE(xrt_compare_native(GE, 4.5, 3.5));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Compare Owner");
RUN_TEST(freestanding_lane_adapters_answer_the_owner);
RUN_TEST(freestanding_tagged_equality_answers_each_relation);
RUN_TEST(freestanding_tagged_equality_separates_tag_classes);
RUN_TEST(freestanding_tagged_order_uses_the_integer_and_double_lanes);
RUN_TEST(freestanding_order_against_nan_is_always_false);
RUN_TEST(freestanding_native_relation_keeps_the_operand_type);
TEST_MAIN_END()
