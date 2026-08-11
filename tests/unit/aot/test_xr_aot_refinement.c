/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_aot_refinement.c - TargetPlan-native AOT refinement KAT
 */

#include "../../../src/aot/refine/xr_aot_refinement.h"
#ifdef XAOT_BUNDLE_H
#error "refinement public API must not expose the legacy XaotBundle"
#endif
#ifdef XGLOBAL_SUMMARY_H
#error "refinement public API must not expose global analyzer evidence"
#endif
#ifdef XI_H
#error "refinement public API must not expose mutable compiler IR"
#endif
#include "../../../src/ir/xi.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/runtime/value/xtype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct RefinementFixture {
    XrSemanticPlan *semantic_plan;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} RefinementFixture;

static XrType scalar_int = {
    .kind = XR_KIND_INT,
    .id = 1,
    .scalar_rep = XR_NATIVE_I64,
    .frozen = true,
};

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("refinement_scalar_baseline", &scalar_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &scalar_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic_plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic_plan, error, sizeof(error)));
    xi_func_free(function);
    return semantic_plan;
}

static XrTargetProfile *build_target_profile(void) {
    XrTargetProfileDraft draft = {0};
    draft.schema_version = XR_TARGET_PLAN_SCHEMA_VERSION;
    draft.architecture = XR_TARGET_ARCH_X86_64;
    draft.operating_system = XR_TARGET_OS_WINDOWS;
    draft.environment = XR_TARGET_ENV_MSVC;
    draft.native_abi = XR_TARGET_ABI_WIN64_X86_64;
    draft.runtime_profile = XR_TARGET_RUNTIME_HOSTED;
    REQUIRE(xr_target_data_layout_init_lp64(&draft.data_layout));
    draft.atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 |
                              XR_TARGET_ATOMIC_WIDTH_16 |
                              XR_TARGET_ATOMIC_WIDTH_32 |
                              XR_TARGET_ATOMIC_WIDTH_64;
    draft.atomic_order_mask = XR_TARGET_ATOMIC_RELAXED |
                              XR_TARGET_ATOMIC_ACQUIRE |
                              XR_TARGET_ATOMIC_RELEASE |
                              XR_TARGET_ATOMIC_ACQ_REL |
                              XR_TARGET_ATOMIC_SEQ_CST;
    draft.float_feature_mask = XR_TARGET_FLOAT_IEEE754 |
                               XR_TARGET_FLOAT_STRICT;
    draft.vector_feature_mask = XR_TARGET_VECTOR_SSE2;
    draft.maximum_vector_bits = 128;
    draft.provider_mask = (UINT64_C(1) << XR_TARGET_PROVIDER_ALLOCATOR) |
                          (UINT64_C(1) << XR_TARGET_PROVIDER_PANIC);
    draft.provider_set_fingerprint.bytes[0] = 0x41;
    draft.object_header_fingerprint.bytes[0] = 0x52;
    draft.runtime_abi_fingerprint.bytes[0] = 0x63;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_freeze(&draft, &profile, error, sizeof(error)));
    return profile;
}

static RefinementFixture fixture_create(void) {
    RefinementFixture fixture = {0};
    fixture.semantic_plan = build_semantic_plan();
    fixture.target_profile = build_target_profile();
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic_plan, fixture.target_profile,
                                 &fixture.target_plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.target_plan));
    REQUIRE(xr_target_plan_completed_family_mask(fixture.target_plan) ==
            XR_TARGET_FAMILY_SCALAR);
    uint32_t call_count = UINT32_MAX;
    REQUIRE(xr_target_plan_calls(fixture.target_plan, &call_count) == NULL);
    REQUIRE(call_count == 0);
    return fixture;
}

static void fixture_free(RefinementFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xr_semantic_plan_free(fixture->semantic_plan);
    memset(fixture, 0, sizeof(*fixture));
}

static XrAotRefinementPlan *build_refused_plan(
    const RefinementFixture *fixture, XrAotRefinementDiagnostic *diag) {
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(fixture->target_plan, diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol = xr_aot_refinement_direct_call_protocol(27901);
    XrAotDirectCallRequest request = {.target_call_index = 0};
    uint32_t decision = 0;
    REQUIRE(xr_aot_refinement_try_direct_call(
        builder, &protocol, fixture->target_plan, &request, &decision, diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_REFUSED);
    REQUIRE(diag->issue == XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE);
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_refinement_builder_freeze(
        builder, fixture->target_plan, &plan, diag));
    xr_aot_refinement_builder_free(builder);
    return plan;
}

static void test_scalar_direct_call_refuses_without_baseline_change(void) {
    RefinementFixture fixture = fixture_create();
    XrFingerprint semantic_before =
        xr_target_plan_semantic_fingerprint(fixture.target_plan);
    XrFingerprint target_before =
        xr_target_plan_fingerprint(fixture.target_plan);
    XrFingerprint profile_before =
        xr_target_profile_fingerprint(fixture.target_profile);
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_refused_plan(&fixture, &diag);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified);
    REQUIRE(view.record_count == 1);
    REQUIRE(view.baseline.completed_family_mask == XR_TARGET_FAMILY_SCALAR);
    REQUIRE(xr_fingerprint_equal(view.baseline.semantic_fingerprint,
                                 semantic_before));
    REQUIRE(xr_fingerprint_equal(view.baseline.target_plan_fingerprint,
                                 target_before));
    REQUIRE(xr_fingerprint_equal(view.baseline.target_profile_fingerprint,
                                 profile_before));
    REQUIRE(view.records[0].decision == XR_AOT_REFINEMENT_REFUSED);
    REQUIRE(view.records[0].diagnostic_issue ==
            XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE);
    REQUIRE(view.records[0].direct_call.target_call_index == 0);
    REQUIRE(memcmp(&view.records[0].input_state,
                   &view.records[0].output_state,
                   sizeof(view.records[0].input_state)) == 0);
    REQUIRE((view.initial_state.available &
             XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET)) == 0);
    REQUIRE(xr_aot_refinement_verify(&view, fixture.target_plan, &diag));
    REQUIRE(xr_fingerprint_equal(target_before,
                                 xr_target_plan_fingerprint(fixture.target_plan)));
    REQUIRE(xr_fingerprint_equal(profile_before,
                                 xr_target_profile_fingerprint(fixture.target_profile)));

    xr_aot_refinement_plan_free(plan);
    fixture_free(&fixture);
}

static void test_stale_state_and_baseline_mutations_fail_closed(void) {
    RefinementFixture fixture = fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_refused_plan(&fixture, &diag);
    XrAotRefinementPlanView original = xr_aot_refinement_plan_view(plan);
    XrAotRefinementPlanView mutated_view = original;
    XrAotTransformationRecord mutated_record = original.records[0];
    mutated_view.records = &mutated_record;

    mutated_record.input_state.generation[XR_AOT_INV_CALL_TARGET]++;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_STALE_EVIDENCE);
    REQUIRE(diag.record_index == 0);

    mutated_record = original.records[0];
    mutated_record.output_state.generation[XR_AOT_INV_VALUES]++;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    mutated_record = original.records[0];
    mutated_record.protocol.requires &=
        ~XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET);
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PASS_PROTOCOL);

    mutated_view = original;
    mutated_view.baseline.semantic_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    mutated_view = original;
    mutated_view.baseline.target_plan_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    mutated_view = original;
    mutated_view.baseline.target_profile_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    mutated_view = original;
    mutated_view.baseline.completed_family_mask |= UINT64_C(1) << 17;
    REQUIRE(!xr_aot_refinement_verify(&mutated_view, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BASELINE_FINGERPRINT);

    xr_aot_refinement_plan_free(plan);
    fixture_free(&fixture);
}

static void test_machine_readable_invalidation_is_functional(void) {
    RefinementFixture fixture = fixture_create();
    XrAotBaselineRef baseline = {0};
    XrAotRefinementDiagnostic diag = {0};
    REQUIRE(xr_aot_refinement_baseline_from_target_plan(
        fixture.target_plan, &baseline, &diag));
    XrAotInvariantState input = xr_aot_refinement_initial_state(&baseline);
    XrAotInvariantState output = {0};
    uint64_t input_generation = input.generation[XR_AOT_INV_VALUES];
    REQUIRE((input.available & XR_AOT_INV_BIT(XR_AOT_INV_VALUES)) != 0);
    REQUIRE(xr_aot_refinement_state_after_invalidation(
        &input, XR_AOT_INV_BIT(XR_AOT_INV_VALUES), &output, &diag));
    REQUIRE((output.available & XR_AOT_INV_BIT(XR_AOT_INV_VALUES)) == 0);
    REQUIRE(output.generation[XR_AOT_INV_VALUES] == input_generation + 1u);
    REQUIRE(input.generation[XR_AOT_INV_VALUES] == input_generation);
    REQUIRE((input.available & XR_AOT_INV_BIT(XR_AOT_INV_VALUES)) != 0);
    XrAotInvariantState before_alias = input;
    REQUIRE(!xr_aot_refinement_state_after_invalidation(
        &input, XR_AOT_INV_BIT(XR_AOT_INV_VALUES), &input, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INVALID_ARGUMENT);
    REQUIRE(memcmp(&input, &before_alias, sizeof(input)) == 0);
    fixture_free(&fixture);
}

static void test_null_and_test_backends_cover_refusal(void) {
    RefinementFixture fixture = fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_refused_plan(&fixture, &diag);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    XrAotBackendStats stats = {0};
    XrAotNullBackend null_backend = {0};
    REQUIRE(xr_aot_backend_run(&view, fixture.target_plan,
                               xr_aot_null_backend_interface(), &null_backend,
                               &stats, &diag));
    REQUIRE(stats.visited == 1 && stats.applied == 0 && stats.refused == 1);
    REQUIRE(memcmp(&stats, &null_backend.stats, sizeof(stats)) == 0);

    char emission[1024];
    XrAotTestBackend test_backend = {
        .buffer = emission,
        .capacity = sizeof(emission),
    };
    REQUIRE(xr_aot_backend_run(&view, fixture.target_plan,
                               xr_aot_test_backend_interface(), &test_backend,
                               &stats, &diag));
    REQUIRE(strstr(emission, "families=0000000000000001") != NULL);
    REQUIRE(strstr(emission, "transform=direct-call decision=refused") != NULL);
    REQUIRE(strstr(emission,
                   "issue=XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE") != NULL);
    REQUIRE(strstr(emission, "end records=1") != NULL);

    XrAotBackendInterface incomplete = *xr_aot_null_backend_interface();
    incomplete.supported_transforms = 0;
    REQUIRE(!xr_aot_backend_run(&view, fixture.target_plan, &incomplete,
                                &null_backend, &stats, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE);

    xr_aot_refinement_plan_free(plan);
    fixture_free(&fixture);
}

int main(void) {
    test_scalar_direct_call_refuses_without_baseline_change();
    test_stale_state_and_baseline_mutations_fail_closed();
    test_machine_readable_invalidation_is_functional();
    test_null_and_test_backends_cover_refusal();
    printf("TargetPlan-native AOT refinement tests passed\n");
    return 0;
}
