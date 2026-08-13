/* Known-answer tests for the shared target vector queries. */

#include "../test_framework.h"
#include "aot/xaot_link.h"
#include "shared/xr_target_simd_core.h"

#define OWNER_QUERY(kind, selection, features)                                                     \
    XR_TARGET_SIMD_QUERY_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_TARGET_SIMD_QUERY_HI,                  \
                                     XR_SEM_OWNER_ID_SHARED_TARGET_SIMD_QUERY_LO,                  \
                                     XR_SEM_CONSUMER_SEMANTIC_PLAN, (kind), (selection),           \
                                     (features))

static int32_t static_bytes(uint32_t features) {
    XrTargetSimdQueryResult result = xr_target_simd_query_core(
        XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_SELECTION_STATIC, features);
    return result.width_source == (uint8_t) XR_TARGET_SIMD_WIDTH_STATIC ? result.bytes : -1;
}

static int answer(XrTargetSimdQueryKind kind, XrTargetSimdSelection selection, uint32_t features) {
    XrTargetSimdQueryResult result =
        xr_target_simd_query_core(kind, (uint8_t) selection, features);
    return result.status == (uint8_t) XR_TARGET_SIMD_QUERY_OK ? (int) result.answer : -1;
}

TEST(a_static_target_carries_the_width_of_its_widest_feature) {
    ASSERT_EQ_INT(16, (int) static_bytes(0));
    ASSERT_EQ_INT(16, (int) static_bytes(UINT32_C(1) << 0));
    ASSERT_EQ_INT(32, (int) static_bytes(XR_TARGET_SIMD_FEATURE_WIDE_256));
    ASSERT_EQ_INT(64, (int) static_bytes(XR_TARGET_SIMD_FEATURE_WIDE_512));
    /* Carrying both means the machine has the wider register file. */
    ASSERT_EQ_INT(64, (int) static_bytes(XR_TARGET_SIMD_FEATURE_WIDE_256 |
                                         XR_TARGET_SIMD_FEATURE_WIDE_512));
    /* An unrelated feature never widens the portable baseline. */
    ASSERT_EQ_INT(16, (int) static_bytes(UINT32_C(1) << 6));
}

TEST(a_run_time_width_is_reported_as_a_source_not_a_number) {
    XrTargetSimdQueryResult dispatch = xr_target_simd_query_core(
        XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_SELECTION_DISPATCH,
        XR_TARGET_SIMD_FEATURE_WIDE_512);
    ASSERT_EQ_INT((int) XR_TARGET_SIMD_QUERY_OK, (int) dispatch.status);
    ASSERT_EQ_INT((int) XR_TARGET_SIMD_WIDTH_RUNTIME_DISPATCH, (int) dispatch.width_source);
    ASSERT_EQ_INT(0, (int) dispatch.bytes);

    XrTargetSimdQueryResult scalable = xr_target_simd_query_core(
        XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_SELECTION_SCALABLE, 0);
    ASSERT_EQ_INT((int) XR_TARGET_SIMD_WIDTH_RUNTIME_SCALABLE, (int) scalable.width_source);
    ASSERT_EQ_INT(0, (int) scalable.bytes);
}

TEST(acceleration_follows_the_features_and_ignores_the_selection) {
    ASSERT_EQ_INT(0, answer(XR_TARGET_SIMD_QUERY_ACCELERATED,
                            XR_TARGET_SIMD_SELECTION_STATIC, 0));
    ASSERT_EQ_INT(1, answer(XR_TARGET_SIMD_QUERY_ACCELERATED,
                            XR_TARGET_SIMD_SELECTION_STATIC, UINT32_C(1) << 1));
    ASSERT_EQ_INT(1, answer(XR_TARGET_SIMD_QUERY_ACCELERATED,
                            XR_TARGET_SIMD_SELECTION_DISPATCH,
                            XR_TARGET_SIMD_FEATURE_WIDE_256));
    /* A dispatch build with no declared feature has no vector hardware to
     * dispatch between. */
    ASSERT_EQ_INT(0, answer(XR_TARGET_SIMD_QUERY_ACCELERATED,
                            XR_TARGET_SIMD_SELECTION_DISPATCH, 0));
}

TEST(run_time_selection_and_scalability_are_different_questions) {
    ASSERT_EQ_INT(0, answer(XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED,
                            XR_TARGET_SIMD_SELECTION_STATIC,
                            XR_TARGET_SIMD_FEATURE_WIDE_512));
    ASSERT_EQ_INT(1, answer(XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED,
                            XR_TARGET_SIMD_SELECTION_DISPATCH, 0));
    ASSERT_EQ_INT(1, answer(XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED,
                            XR_TARGET_SIMD_SELECTION_SCALABLE, 0));
    /* Only a scalable register answers the narrower question. */
    ASSERT_EQ_INT(0, answer(XR_TARGET_SIMD_QUERY_SCALABLE,
                            XR_TARGET_SIMD_SELECTION_DISPATCH, 0));
    ASSERT_EQ_INT(1, answer(XR_TARGET_SIMD_QUERY_SCALABLE,
                            XR_TARGET_SIMD_SELECTION_SCALABLE, 0));
    ASSERT_EQ_INT(0, answer(XR_TARGET_SIMD_QUERY_SCALABLE,
                            XR_TARGET_SIMD_SELECTION_STATIC, 0));
}

TEST(an_undeclared_selection_or_kind_fails_closed) {
    XrTargetSimdQueryResult bad_selection = xr_target_simd_query_core(
        XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_SELECTION_COUNT, 0);
    ASSERT_EQ_INT((int) XR_TARGET_SIMD_QUERY_INVALID_SELECTION, (int) bad_selection.status);
    ASSERT_EQ_INT(0, (int) bad_selection.bytes);

    XrTargetSimdQueryResult bad_kind = xr_target_simd_query_core(
        (XrTargetSimdQueryKind) XR_TARGET_SIMD_QUERY_KIND_COUNT,
        (uint8_t) XR_TARGET_SIMD_SELECTION_STATIC, 0);
    ASSERT_EQ_INT((int) XR_TARGET_SIMD_QUERY_INVALID_KIND, (int) bad_kind.status);
    ASSERT_EQ_INT(0, (int) bad_kind.answer);
}

TEST(the_portable_baseline_is_the_machine_bytecode_executes) {
    XrTargetSimdQueryResult bytes =
        OWNER_QUERY(XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_BASELINE_SELECTION,
                    XR_TARGET_SIMD_BASELINE_FEATURES);
    ASSERT_EQ_INT(XR_TARGET_SIMD_BASELINE_BYTES, (int) bytes.bytes);
    ASSERT_TRUE(xr_target_simd_query_is_baseline_core(XR_TARGET_SIMD_QUERY_BYTES, bytes));

    XrTargetSimdQueryResult accelerated =
        OWNER_QUERY(XR_TARGET_SIMD_QUERY_ACCELERATED, (uint8_t) XR_TARGET_SIMD_BASELINE_SELECTION,
                    XR_TARGET_SIMD_BASELINE_FEATURES);
    ASSERT_TRUE(xr_target_simd_query_is_baseline_core(XR_TARGET_SIMD_QUERY_ACCELERATED,
                                                      accelerated));
    XrTargetSimdQueryResult selected = OWNER_QUERY(XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED,
                                                   (uint8_t) XR_TARGET_SIMD_BASELINE_SELECTION,
                                                   XR_TARGET_SIMD_BASELINE_FEATURES);
    ASSERT_TRUE(xr_target_simd_query_is_baseline_core(XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED,
                                                      selected));
    XrTargetSimdQueryResult scalable =
        OWNER_QUERY(XR_TARGET_SIMD_QUERY_SCALABLE, (uint8_t) XR_TARGET_SIMD_BASELINE_SELECTION,
                    XR_TARGET_SIMD_BASELINE_FEATURES);
    ASSERT_TRUE(xr_target_simd_query_is_baseline_core(XR_TARGET_SIMD_QUERY_SCALABLE, scalable));

    /* Any other machine is not the baseline. */
    XrTargetSimdQueryResult wide = xr_target_simd_query_core(
        XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_SELECTION_STATIC,
        XR_TARGET_SIMD_FEATURE_WIDE_256);
    ASSERT_FALSE(xr_target_simd_query_is_baseline_core(XR_TARGET_SIMD_QUERY_BYTES, wide));
    XrTargetSimdQueryResult dispatch_bytes = xr_target_simd_query_core(
        XR_TARGET_SIMD_QUERY_BYTES, (uint8_t) XR_TARGET_SIMD_SELECTION_DISPATCH, 0);
    ASSERT_FALSE(xr_target_simd_query_is_baseline_core(XR_TARGET_SIMD_QUERY_BYTES,
                                                       dispatch_bytes));
}

TEST(the_owner_adapter_answers_the_same_as_the_kernel) {
    static const XrTargetSimdSelection SELECTIONS[3] = {XR_TARGET_SIMD_SELECTION_STATIC,
                                                        XR_TARGET_SIMD_SELECTION_SCALABLE,
                                                        XR_TARGET_SIMD_SELECTION_DISPATCH};
    static const uint32_t FEATURES[4] = {0, UINT32_C(1) << 0, XR_TARGET_SIMD_FEATURE_WIDE_256,
                                         XR_TARGET_SIMD_FEATURE_WIDE_512};
    for (int s = 0; s < 3; s++) {
        for (int fi = 0; fi < 4; fi++) {
            uint8_t selection = (uint8_t) SELECTIONS[s];
            uint32_t features = FEATURES[fi];
            XrTargetSimdQueryResult direct =
                xr_target_simd_query_core(XR_TARGET_SIMD_QUERY_BYTES, selection, features);
            XrTargetSimdQueryResult applied =
                OWNER_QUERY(XR_TARGET_SIMD_QUERY_BYTES, selection, features);
            ASSERT_EQ_INT((int) direct.status, (int) applied.status);
            ASSERT_EQ_INT((int) direct.width_source, (int) applied.width_source);
            ASSERT_EQ_INT((int) direct.bytes, (int) applied.bytes);
            ASSERT_EQ_INT((int) xr_target_simd_width_source_core(selection),
                          (int) applied.width_source);
            if (applied.width_source == (uint8_t) XR_TARGET_SIMD_WIDTH_STATIC)
                ASSERT_EQ_INT((int) xr_target_simd_static_bytes_core(features),
                              (int) applied.bytes);
        }
    }
}

/* The answers AOT CGen folded by hand from the link target before this owner
 * existed. Kept here as the oracle so the migration is checked against what it
 * replaced, over every target the AOT manifest can describe. */
static uint8_t retired_cgen_selection(XaotSimdMode mode) {
    if (mode == XAOT_SIMD_SVE)
        return (uint8_t) XR_TARGET_SIMD_SELECTION_SCALABLE;
    if (mode == XAOT_SIMD_DISPATCH)
        return (uint8_t) XR_TARGET_SIMD_SELECTION_DISPATCH;
    return (uint8_t) XR_TARGET_SIMD_SELECTION_STATIC;
}

TEST(the_owner_reproduces_every_answer_cgen_used_to_fold_by_hand) {
    static const XaotSimdMode MODES[11] = {
        XAOT_SIMD_AUTO,  XAOT_SIMD_SCALAR, XAOT_SIMD_NATIVE, XAOT_SIMD_NEON,
        XAOT_SIMD_SSE2,  XAOT_SIMD_AVX2,   XAOT_SIMD_AVX512, XAOT_SIMD_VSX,
        XAOT_SIMD_LSX,   XAOT_SIMD_SVE,    XAOT_SIMD_DISPATCH};
    static const uint32_t FEATURES[8] = {
        0,
        XAOT_SIMD_FEATURE_NEON,
        XAOT_SIMD_FEATURE_SSE2,
        XAOT_SIMD_FEATURE_SSE2 | XAOT_SIMD_FEATURE_AVX2,
        XAOT_SIMD_FEATURE_SSE2 | XAOT_SIMD_FEATURE_AVX2 | XAOT_SIMD_FEATURE_AVX512,
        XAOT_SIMD_FEATURE_SVE,
        XAOT_SIMD_FEATURE_VSX,
        XAOT_SIMD_FEATURE_LSX};
    for (int mi = 0; mi < 11; mi++) {
        for (int fi = 0; fi < 8; fi++) {
            XaotSimdMode mode = MODES[mi];
            uint32_t features = FEATURES[fi];
            uint8_t selection = retired_cgen_selection(mode);

            bool retired_runtime_dispatch =
                mode == XAOT_SIMD_DISPATCH || mode == XAOT_SIMD_SVE;
            int retired_bytes = (features & XAOT_SIMD_FEATURE_AVX512) != 0   ? 64
                                : (features & XAOT_SIMD_FEATURE_AVX2) != 0 ? 32
                                                                           : 16;
            bool retired_accelerated = features != 0;
            bool retired_selected = mode == XAOT_SIMD_DISPATCH || mode == XAOT_SIMD_SVE;
            bool retired_scalable = mode == XAOT_SIMD_SVE;

            XrTargetSimdQueryResult bytes =
                xr_target_simd_query_core(XR_TARGET_SIMD_QUERY_BYTES, selection, features);
            ASSERT_EQ_INT((int) XR_TARGET_SIMD_QUERY_OK, (int) bytes.status);
            if (retired_runtime_dispatch) {
                ASSERT_EQ_INT((int) (mode == XAOT_SIMD_SVE
                                         ? XR_TARGET_SIMD_WIDTH_RUNTIME_SCALABLE
                                         : XR_TARGET_SIMD_WIDTH_RUNTIME_DISPATCH),
                              (int) bytes.width_source);
            } else {
                ASSERT_EQ_INT((int) XR_TARGET_SIMD_WIDTH_STATIC, (int) bytes.width_source);
                ASSERT_EQ_INT(retired_bytes, (int) bytes.bytes);
            }
            ASSERT_EQ_INT(retired_accelerated ? 1 : 0,
                          answer(XR_TARGET_SIMD_QUERY_ACCELERATED,
                                 (XrTargetSimdSelection) selection, features));
            ASSERT_EQ_INT(retired_selected ? 1 : 0,
                          answer(XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED,
                                 (XrTargetSimdSelection) selection, features));
            ASSERT_EQ_INT(retired_scalable ? 1 : 0,
                          answer(XR_TARGET_SIMD_QUERY_SCALABLE,
                                 (XrTargetSimdSelection) selection, features));
        }
    }
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Target SIMD Core");
RUN_TEST(a_static_target_carries_the_width_of_its_widest_feature);
RUN_TEST(a_run_time_width_is_reported_as_a_source_not_a_number);
RUN_TEST(acceleration_follows_the_features_and_ignores_the_selection);
RUN_TEST(run_time_selection_and_scalability_are_different_questions);
RUN_TEST(an_undeclared_selection_or_kind_fails_closed);
RUN_TEST(the_portable_baseline_is_the_machine_bytecode_executes);
RUN_TEST(the_owner_adapter_answers_the_same_as_the_kernel);
RUN_TEST(the_owner_reproduces_every_answer_cgen_used_to_fold_by_hand);
TEST_MAIN_END()
