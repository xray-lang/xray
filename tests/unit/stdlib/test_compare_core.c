/* Known-answer tests for the shared equality and order relations. */

#include "../test_framework.h"
#include "shared/xr_compare_core.h"
#include <math.h>

#define OWNER_APPLY_I64(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_I64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_SEMANTIC_PLAN,   \
                               (kind), (a), (b))
#define OWNER_APPLY_U64(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_U64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_SEMANTIC_PLAN,   \
                               (kind), (a), (b))
#define OWNER_APPLY_F64(kind, a, b)                                                                \
    XR_COMPARE_OWNER_APPLY_F64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_SEMANTIC_PLAN,   \
                               (kind), (a), (b))
#define OWNER_ROUTE(kind, left, right)                                                             \
    XR_COMPARE_OWNER_ROUTE(XR_SEM_OWNER_ID_SHARED_COMPARE_HI, XR_SEM_OWNER_ID_SHARED_COMPARE_LO,   \
                           XR_SEM_CONSUMER_SEMANTIC_PLAN, (kind), (left), (right))

static const XrCompareKind ALL_KINDS[6] = {XR_COMPARE_EQ, XR_COMPARE_NE, XR_COMPARE_LT,
                                           XR_COMPARE_LE, XR_COMPARE_GT, XR_COMPARE_GE};

TEST(signed_lane_answers_every_relation) {
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_EQ, 7, 7));
    ASSERT_FALSE(xr_compare_i64_core(XR_COMPARE_EQ, 7, 8));
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_NE, 7, 8));
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_LT, -1, 0));
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_LE, 7, 7));
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_GT, 0, -1));
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_GE, 7, 7));
    /* The extremes order by sign, not by bit pattern. */
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_LT, INT64_MIN, INT64_MAX));
    ASSERT_FALSE(xr_compare_i64_core(XR_COMPARE_GT, INT64_MIN, INT64_MAX));
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_LT, INT64_MIN, 0));
}

TEST(unsigned_lane_orders_top_bit_set_operands_above_zero) {
    uint64_t top = UINT64_C(0x8000000000000000);
    uint64_t ones = UINT64_MAX;
    ASSERT_TRUE(xr_compare_u64_core(XR_COMPARE_GT, top, UINT64_C(5)));
    ASSERT_TRUE(xr_compare_u64_core(XR_COMPARE_LT, UINT64_C(5), top));
    ASSERT_TRUE(xr_compare_u64_core(XR_COMPARE_GT, ones, top));
    ASSERT_TRUE(xr_compare_u64_core(XR_COMPARE_GE, ones, ones));
    ASSERT_TRUE(xr_compare_u64_core(XR_COMPARE_EQ, top, top));
    ASSERT_TRUE(xr_compare_u64_core(XR_COMPARE_NE, top, ones));
    /* The same bits read on the signed lane order the other way. */
    ASSERT_TRUE(xr_compare_i64_core(XR_COMPARE_LT, (int64_t) top, (int64_t) 5));
}

TEST(float_lane_is_ieee_and_never_matches_nan) {
    double nan_value = (double) NAN;
    for (int i = 0; i < 6; i++) {
        XrCompareKind kind = ALL_KINDS[i];
        bool expected = (kind == XR_COMPARE_NE);
        ASSERT_EQ_INT((int) expected, (int) xr_compare_f64_core(kind, nan_value, nan_value));
        ASSERT_EQ_INT((int) expected, (int) xr_compare_f64_core(kind, nan_value, 1.0));
        ASSERT_EQ_INT((int) expected, (int) xr_compare_f64_core(kind, 1.0, nan_value));
    }
    ASSERT_TRUE(xr_compare_f64_core(XR_COMPARE_EQ, 0.0, -0.0));
    ASSERT_FALSE(xr_compare_f64_core(XR_COMPARE_NE, 0.0, -0.0));
    ASSERT_TRUE(xr_compare_f64_core(XR_COMPARE_LT, -1.0, 1.0));
    ASSERT_TRUE(xr_compare_f64_core(XR_COMPARE_GE, 2.0, 2.0));
}

TEST(three_way_lane_turns_a_comparator_answer_into_each_relation) {
    ASSERT_TRUE(xr_compare_ordering_core(XR_COMPARE_EQ, 0));
    ASSERT_FALSE(xr_compare_ordering_core(XR_COMPARE_EQ, -1));
    ASSERT_TRUE(xr_compare_ordering_core(XR_COMPARE_NE, 3));
    ASSERT_TRUE(xr_compare_ordering_core(XR_COMPARE_LT, -42));
    ASSERT_TRUE(xr_compare_ordering_core(XR_COMPARE_LE, 0));
    ASSERT_TRUE(xr_compare_ordering_core(XR_COMPARE_GT, 42));
    ASSERT_TRUE(xr_compare_ordering_core(XR_COMPARE_GE, 0));
    ASSERT_FALSE(xr_compare_ordering_core(XR_COMPARE_GT, 0));
}

TEST(equality_only_lane_refuses_to_order) {
    ASSERT_TRUE(xr_compare_equal_core(XR_COMPARE_EQ, true));
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_EQ, false));
    ASSERT_TRUE(xr_compare_equal_core(XR_COMPARE_NE, false));
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_NE, true));
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_LT, true));
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_LE, true));
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_GT, true));
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_GE, true));
}

TEST(address_lane_compares_identity) {
    int storage[2];
    ASSERT_TRUE(xr_compare_ptr_core(XR_COMPARE_EQ, &storage[0], &storage[0]));
    ASSERT_TRUE(xr_compare_ptr_core(XR_COMPARE_NE, &storage[0], &storage[1]));
    ASSERT_TRUE(xr_compare_ptr_core(XR_COMPARE_LT, &storage[0], &storage[1]));
    ASSERT_TRUE(xr_compare_ptr_core(XR_COMPARE_GE, &storage[1], &storage[0]));
    ASSERT_TRUE(xr_compare_ptr_core(XR_COMPARE_EQ, NULL, NULL));
}

TEST(an_integer_never_equals_a_float_but_still_orders_against_one) {
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_UNRELATED,
                  (int) xr_compare_route_core(XR_COMPARE_EQ, XR_COMPARE_OPERAND_INT,
                                              XR_COMPARE_OPERAND_FLOAT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_UNRELATED,
                  (int) xr_compare_route_core(XR_COMPARE_NE, XR_COMPARE_OPERAND_FLOAT,
                                              XR_COMPARE_OPERAND_INT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_F64,
                  (int) xr_compare_route_core(XR_COMPARE_LT, XR_COMPARE_OPERAND_INT,
                                              XR_COMPARE_OPERAND_FLOAT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_F64,
                  (int) xr_compare_route_core(XR_COMPARE_GE, XR_COMPARE_OPERAND_FLOAT,
                                              XR_COMPARE_OPERAND_INT));
    /* The unrelated route answers false for equality and true for inequality. */
    ASSERT_FALSE(xr_compare_equal_core(XR_COMPARE_EQ, false));
    ASSERT_TRUE(xr_compare_equal_core(XR_COMPARE_NE, false));
}

TEST(an_integer_and_a_big_integer_share_a_lossless_type) {
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_BIGINT,
                  (int) xr_compare_route_core(XR_COMPARE_EQ, XR_COMPARE_OPERAND_BIGINT,
                                              XR_COMPARE_OPERAND_BIGINT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_BIGINT_INT,
                  (int) xr_compare_route_core(XR_COMPARE_EQ, XR_COMPARE_OPERAND_BIGINT,
                                              XR_COMPARE_OPERAND_INT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_INT_BIGINT,
                  (int) xr_compare_route_core(XR_COMPARE_LT, XR_COMPARE_OPERAND_INT,
                                              XR_COMPARE_OPERAND_BIGINT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_OTHER,
                  (int) xr_compare_route_core(XR_COMPARE_EQ, XR_COMPARE_OPERAND_BIGINT,
                                              XR_COMPARE_OPERAND_FLOAT));
}

TEST(scalar_routes_pick_their_own_lane) {
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_I64,
                  (int) xr_compare_route_core(XR_COMPARE_EQ, XR_COMPARE_OPERAND_INT,
                                              XR_COMPARE_OPERAND_INT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_F64,
                  (int) xr_compare_route_core(XR_COMPARE_EQ, XR_COMPARE_OPERAND_FLOAT,
                                              XR_COMPARE_OPERAND_FLOAT));
    ASSERT_EQ_INT(XR_COMPARE_ROUTE_OTHER,
                  (int) xr_compare_route_core(XR_COMPARE_LT, XR_COMPARE_OPERAND_OTHER,
                                              XR_COMPARE_OPERAND_INT));
}

TEST(mirroring_a_relation_swaps_the_operand_it_reads_from) {
    ASSERT_EQ_INT(XR_COMPARE_GT, (int) xr_compare_kind_mirrored_core(XR_COMPARE_LT));
    ASSERT_EQ_INT(XR_COMPARE_GE, (int) xr_compare_kind_mirrored_core(XR_COMPARE_LE));
    ASSERT_EQ_INT(XR_COMPARE_LT, (int) xr_compare_kind_mirrored_core(XR_COMPARE_GT));
    ASSERT_EQ_INT(XR_COMPARE_LE, (int) xr_compare_kind_mirrored_core(XR_COMPARE_GE));
    ASSERT_EQ_INT(XR_COMPARE_EQ, (int) xr_compare_kind_mirrored_core(XR_COMPARE_EQ));
    ASSERT_EQ_INT(XR_COMPARE_NE, (int) xr_compare_kind_mirrored_core(XR_COMPARE_NE));
    ASSERT_TRUE(xr_compare_kind_is_equality_core(XR_COMPARE_EQ));
    ASSERT_TRUE(xr_compare_kind_is_equality_core(XR_COMPARE_NE));
    ASSERT_FALSE(xr_compare_kind_is_equality_core(XR_COMPARE_LT));
    /* Mirroring twice is the relation you started with. */
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ_INT((int) ALL_KINDS[i],
                      (int) xr_compare_kind_mirrored_core(
                          xr_compare_kind_mirrored_core(ALL_KINDS[i])));
    }
}

TEST(owner_adapters_answer_the_same_as_the_kernel) {
    for (int i = 0; i < 6; i++) {
        XrCompareKind kind = ALL_KINDS[i];
        ASSERT_EQ_INT((int) xr_compare_i64_core(kind, -3, 4), (int) OWNER_APPLY_I64(kind, -3, 4));
        ASSERT_EQ_INT((int) xr_compare_u64_core(kind, UINT64_MAX, 1),
                      (int) OWNER_APPLY_U64(kind, UINT64_MAX, 1));
        ASSERT_EQ_INT((int) xr_compare_f64_core(kind, 1.5, 1.5),
                      (int) OWNER_APPLY_F64(kind, 1.5, 1.5));
        ASSERT_EQ_INT((int) xr_compare_route_core(kind, XR_COMPARE_OPERAND_INT,
                                                  XR_COMPARE_OPERAND_FLOAT),
                      (int) OWNER_ROUTE(kind, XR_COMPARE_OPERAND_INT, XR_COMPARE_OPERAND_FLOAT));
    }
}

TEST(the_native_relation_form_matches_the_lane_functions) {
    int64_t a = -3;
    int64_t b = 4;
    uint64_t ua = UINT64_MAX;
    uint64_t ub = 1;
    double fa = 1.5;
    double fb = 2.5;
#define NATIVE(relation, x, y)                                                                     \
    XR_COMPARE_OWNER_APPLY_NATIVE(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                               \
                                  XR_SEM_OWNER_ID_SHARED_COMPARE_LO,                               \
                                  XR_SEM_CONSUMER_SEMANTIC_PLAN, relation, (x), (y))
    ASSERT_EQ_INT((int) xr_compare_i64_core(XR_COMPARE_EQ, a, b), (int) NATIVE(EQ, a, b));
    ASSERT_EQ_INT((int) xr_compare_i64_core(XR_COMPARE_NE, a, b), (int) NATIVE(NE, a, b));
    ASSERT_EQ_INT((int) xr_compare_i64_core(XR_COMPARE_LT, a, b), (int) NATIVE(LT, a, b));
    ASSERT_EQ_INT((int) xr_compare_i64_core(XR_COMPARE_LE, a, b), (int) NATIVE(LE, a, b));
    ASSERT_EQ_INT((int) xr_compare_i64_core(XR_COMPARE_GT, a, b), (int) NATIVE(GT, a, b));
    ASSERT_EQ_INT((int) xr_compare_i64_core(XR_COMPARE_GE, a, b), (int) NATIVE(GE, a, b));
    /* The native form keeps the operand type, so unsigned operands stay
     * unsigned instead of being read on the signed lane. */
    ASSERT_EQ_INT((int) xr_compare_u64_core(XR_COMPARE_GT, ua, ub), (int) NATIVE(GT, ua, ub));
    ASSERT_EQ_INT((int) xr_compare_f64_core(XR_COMPARE_LT, fa, fb), (int) NATIVE(LT, fa, fb));
#undef NATIVE
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Compare Core");
RUN_TEST(signed_lane_answers_every_relation);
RUN_TEST(unsigned_lane_orders_top_bit_set_operands_above_zero);
RUN_TEST(float_lane_is_ieee_and_never_matches_nan);
RUN_TEST(three_way_lane_turns_a_comparator_answer_into_each_relation);
RUN_TEST(equality_only_lane_refuses_to_order);
RUN_TEST(address_lane_compares_identity);
RUN_TEST(an_integer_never_equals_a_float_but_still_orders_against_one);
RUN_TEST(an_integer_and_a_big_integer_share_a_lossless_type);
RUN_TEST(scalar_routes_pick_their_own_lane);
RUN_TEST(mirroring_a_relation_swaps_the_operand_it_reads_from);
RUN_TEST(owner_adapters_answer_the_same_as_the_kernel);
RUN_TEST(the_native_relation_form_matches_the_lane_functions);
TEST_MAIN_END()
