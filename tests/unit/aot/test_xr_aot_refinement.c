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
#include "../../../src/aot/refine/xr_aot_representation_refinement.h"
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
#include "../../../src/ir/xi_opt.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
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

typedef struct RepresentationFixture {
    XiFunc *function;
    XiBlock *entry;
    XiValue *native_constant;
    XiValue *tagged_call;
    XiValue *rhs;
    XiValue *sum;
    XrTargetProfile *target_profile;
    XrTargetPlan *target_plan;
} RepresentationFixture;

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
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
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

static RepresentationFixture representation_fixture_create(void) {
    RepresentationFixture fixture = {0};
    fixture.function = xi_func_new("representation_refinement", &scalar_int);
    REQUIRE(fixture.function != NULL);
    fixture.entry = xi_block_new(fixture.function);
    REQUIRE(fixture.entry != NULL);
    fixture.native_constant =
        xi_const_int(fixture.function, fixture.entry, 40, &scalar_int);
    fixture.tagged_call = xi_value_new(fixture.function, fixture.entry,
                                       XI_CALL_BUILTIN, &scalar_int, 1);
    REQUIRE(fixture.native_constant != NULL && fixture.tagged_call != NULL);
    fixture.tagged_call->aux = (void *) "refinement_dynamic_int";
    fixture.tagged_call->args[0] = fixture.native_constant;
    fixture.rhs = xi_const_int(fixture.function, fixture.entry, 2, &scalar_int);
    fixture.sum = xi_value_new(fixture.function, fixture.entry, XI_ADD,
                               &scalar_int, 2);
    REQUIRE(fixture.rhs != NULL && fixture.sum != NULL);
    fixture.sum->args[0] = fixture.tagged_call;
    fixture.sum->args[1] = fixture.rhs;
    xi_block_set_return(fixture.entry, fixture.sum);
    fixture.function->stage = XI_STAGE_OPTIMIZED;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(fixture.function, error,
                                              sizeof(error)));
    fixture.target_profile = build_target_profile();
    REQUIRE(xr_target_plan_build(fixture.function->semantic_plan,
                                 fixture.target_profile, &fixture.target_plan,
                                 error, sizeof(error)));
    return fixture;
}

static void representation_fixture_free(RepresentationFixture *fixture) {
    xr_target_plan_free(fixture->target_plan);
    xr_target_profile_free(fixture->target_profile);
    xi_func_free(fixture->function);
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

static XrAotRefinementPlan *build_representation_plan(
    const RepresentationFixture *fixture, XrAotRefinementDiagnostic *diag) {
    XrAotRefinementPlan *plan = NULL;
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    REQUIRE(xr_aot_representation_refinement_build(
        fixture->function, fixture->target_plan, &policy, &plan, diag));
    REQUIRE(plan != NULL);
    return plan;
}

static void test_representation_adapters_are_immutable_and_consumable(void) {
    RepresentationFixture fixture = representation_fixture_create();
    uint32_t value_count = fixture.function->next_value_id;
    uint32_t block_value_count = fixture.entry->nvalues;
    XiValue *tagged_arg = fixture.tagged_call->args[0];
    XiValue *sum_arg = fixture.sum->args[0];
    uint8_t constant_rep = fixture.native_constant->rep;
    uint8_t call_rep = fixture.tagged_call->rep;
    XrFingerprint semantic_before =
        xr_semantic_plan_fingerprint(fixture.function->semantic_plan);

    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.frozen && view.verified && view.record_count == 2);
    REQUIRE(view.records[0].transform_kind ==
            XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER);
    REQUIRE(view.records[0].decision == XR_AOT_REFINEMENT_APPLIED);
    REQUIRE(view.records[0].representation_adapter.adapter_kind ==
            XR_AOT_REP_ADAPTER_BOX);
    REQUIRE(view.records[0].representation_adapter.output_rep_kind ==
            XR_MACHINE_REP_DYN_VALUE);
    REQUIRE(view.records[1].representation_adapter.adapter_kind ==
            XR_AOT_REP_ADAPTER_UNBOX);
    REQUIRE(view.records[1].representation_adapter.input_rep_kind ==
            XR_MACHINE_REP_DYN_VALUE);

    REQUIRE(fixture.function->next_value_id == value_count);
    REQUIRE(fixture.entry->nvalues == block_value_count);
    REQUIRE(fixture.tagged_call->args[0] == tagged_arg);
    REQUIRE(fixture.sum->args[0] == sum_arg);
    REQUIRE(fixture.native_constant->rep == constant_rep);
    REQUIRE(fixture.tagged_call->rep == call_rep);
    REQUIRE(xr_fingerprint_equal(
        semantic_before,
        xr_semantic_plan_fingerprint(fixture.function->semantic_plan)));

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    REQUIRE(xr_aot_representation_refinement_verify(
        &view, fixture.function, fixture.target_plan, &policy, &diag));
    XrAotBackendStats stats = {0};
    XrAotNullBackend null_backend = {0};
    REQUIRE(xr_aot_backend_run(&view, fixture.target_plan,
                               xr_aot_null_backend_interface(), &null_backend,
                               &stats, &diag));
    REQUIRE(stats.visited == 2 && stats.applied == 2 && stats.refused == 0);
    char emission[2048];
    XrAotTestBackend backend = {
        .buffer = emission,
        .capacity = sizeof(emission),
    };
    REQUIRE(xr_aot_backend_run(&view, fixture.target_plan,
                               xr_aot_test_backend_interface(), &backend,
                               &stats, &diag));
    REQUIRE(strstr(emission, "transform=representation-adapter") != NULL);
    REQUIRE(strstr(emission, "kind=1") != NULL);
    REQUIRE(strstr(emission, "kind=2") != NULL);

    xr_aot_refinement_plan_free(plan);
    representation_fixture_free(&fixture);
}

static void test_representation_record_mutations_fail_closed(void) {
    RepresentationFixture fixture = representation_fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView original = xr_aot_refinement_plan_view(plan);
    XrAotTransformationRecord records[2];
    memcpy(records, original.records, sizeof(records));
    XrAotRefinementPlanView mutated = original;
    mutated.records = records;

    records[0].input_state.generation[XR_AOT_INV_VALUES]++;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_STALE_EVIDENCE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.source_operation_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.source_type_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.use_operation_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.input_rep_kind = XR_MACHINE_REP_F64;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.layout_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_LAYOUT);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    xr_aot_refinement_plan_free(plan);
    representation_fixture_free(&fixture);
}

static void test_enum_descriptor_adapter_refuses_without_layout_family(void) {
    RepresentationFixture fixture = representation_fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *native_plan = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView native_view =
        xr_aot_refinement_plan_view(native_plan);
    const XrAotRepresentationAdapterRecord *native =
        &native_view.records[0].representation_adapter;
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(fixture.target_plan, &diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol =
        xr_aot_refinement_representation_protocol(27903);
    XrAotRepresentationAdapterRequest request = {
        .source_value = native->source_value,
        .use_operation = native->use_operation,
        .use_operand = native->use_operand,
        .adapter_kind = XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX,
        .input_rep_kind = native->input_rep_kind,
        .output_rep_kind = native->output_rep_kind,
        .layout = native->layout,
    };
    REQUIRE(!xr_aot_refinement_add_representation_adapter(
        builder, &protocol, fixture.target_plan, &request, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    xr_aot_refinement_builder_free(builder);
    xr_aot_refinement_plan_free(native_plan);
    representation_fixture_free(&fixture);
}

static void test_live_source_use_and_type_mutations_are_rederived(void) {
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    XrAotRefinementDiagnostic diag = {0};

    RepresentationFixture source = representation_fixture_create();
    XrAotRefinementPlan *source_plan = build_representation_plan(&source, &diag);
    XrAotRefinementPlanView source_view =
        xr_aot_refinement_plan_view(source_plan);
    source.native_constant->op = XI_PARAM;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &source_view, source.function, source.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    xr_aot_refinement_plan_free(source_plan);
    representation_fixture_free(&source);

    RepresentationFixture use = representation_fixture_create();
    XrAotRefinementPlan *use_plan = build_representation_plan(&use, &diag);
    XrAotRefinementPlanView use_view = xr_aot_refinement_plan_view(use_plan);
    use.tagged_call->op = XI_CONST;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &use_view, use.function, use.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    xr_aot_refinement_plan_free(use_plan);
    representation_fixture_free(&use);

    static XrType scalar_float = {
        .kind = XR_KIND_FLOAT,
        .id = 2,
        .scalar_rep = XR_NATIVE_F64,
        .frozen = true,
    };
    RepresentationFixture type = representation_fixture_create();
    XrAotRefinementPlan *type_plan = build_representation_plan(&type, &diag);
    XrAotRefinementPlanView type_view = xr_aot_refinement_plan_view(type_plan);
    type.native_constant->type = &scalar_float;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &type_view, type.function, type.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    xr_aot_refinement_plan_free(type_plan);
    representation_fixture_free(&type);
}

int main(void) {
    test_scalar_direct_call_refuses_without_baseline_change();
    test_stale_state_and_baseline_mutations_fail_closed();
    test_machine_readable_invalidation_is_functional();
    test_null_and_test_backends_cover_refusal();
    test_representation_adapters_are_immutable_and_consumable();
    test_representation_record_mutations_fail_closed();
    test_enum_descriptor_adapter_refuses_without_layout_family();
    test_live_source_use_and_type_mutations_are_rederived();
    printf("TargetPlan-native AOT refinement tests passed\n");
    return 0;
}
