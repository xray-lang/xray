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
#include "../../../src/base/xmalloc.h"
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

typedef struct FailingBackend {
    bool begun;
    bool aborted;
} FailingBackend;

static bool failing_backend_begin(void *context,
                                  const XrAotBaselineRef *baseline,
                                  uint32_t record_count) {
    FailingBackend *backend = (FailingBackend *) context;
    REQUIRE(backend != NULL && baseline != NULL && record_count != 0);
    backend->begun = true;
    return true;
}

static bool failing_backend_visit(void *context, uint32_t index,
                                  const XrAotTransformationRecord *record) {
    FailingBackend *backend = (FailingBackend *) context;
    REQUIRE(backend != NULL && backend->begun && index == 0 && record != NULL);
    return false;
}

static bool failing_backend_finish(void *context) {
    (void) context;
    return false;
}

static void failing_backend_abort(void *context) {
    FailingBackend *backend = (FailingBackend *) context;
    REQUIRE(backend != NULL && backend->begun);
    backend->aborted = true;
}

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
            XR_TARGET_REQUIRED_FAMILIES);
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
    fixture.rhs = xi_value_new(fixture.function, fixture.entry, XI_UNBOX,
                               &scalar_int, 1);
    REQUIRE(fixture.native_constant != NULL && fixture.rhs != NULL);
    fixture.rhs->args[0] = fixture.native_constant;
    fixture.tagged_call = xi_value_new(fixture.function, fixture.entry,
                                       XI_BOX, &scalar_int, 1);
    REQUIRE(fixture.tagged_call != NULL);
    fixture.tagged_call->args[0] = fixture.rhs;
    fixture.sum = xi_value_new(fixture.function, fixture.entry, XI_ADD,
                               &scalar_int, 2);
    REQUIRE(fixture.sum != NULL);
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
    REQUIRE(view.baseline.completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
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
    REQUIRE(strstr(emission, "families=0000000000000007") != NULL);
    REQUIRE(strstr(emission, "transform=direct-call decision=refused") != NULL);
    REQUIRE(strstr(emission,
                   "issue=XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE") != NULL);
    REQUIRE(strstr(emission, "end records=1") != NULL);

    FailingBackend failing = {0};
    XrAotBackendInterface failing_interface = {
        .abi_version = XR_AOT_REFINEMENT_BACKEND_ABI_VERSION,
        .supported_transforms =
            XR_AOT_TRANSFORM_BIT(XR_AOT_TRANSFORM_DIRECT_CALL),
        .begin = failing_backend_begin,
        .visit = failing_backend_visit,
        .finish = failing_backend_finish,
        .abort = failing_backend_abort,
    };
    REQUIRE(!xr_aot_backend_run(&view, fixture.target_plan,
                                 &failing_interface, &failing, &stats,
                                 &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BACKEND_FAILURE);
    REQUIRE(failing.aborted);

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
    XiValue **value_array = fixture.entry->values;
    XiValue *values[4] = {0};
    uint16_t nargs[4] = {0};
    XiValue *args[4][2] = {{0}};
    REQUIRE(block_value_count == 4);
    for (uint32_t i = 0; i < block_value_count; i++) {
        values[i] = fixture.entry->values[i];
        nargs[i] = values[i]->nargs;
        for (uint16_t a = 0; a < nargs[i] && a < 2; a++)
            args[i][a] = values[i]->args[a];
    }
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
    REQUIRE(fixture.entry->values == value_array);
    for (uint32_t i = 0; i < block_value_count; i++) {
        REQUIRE(fixture.entry->values[i] == values[i]);
        REQUIRE(values[i]->nargs == nargs[i]);
        for (uint16_t a = 0; a < nargs[i] && a < 2; a++)
            REQUIRE(values[i]->args[a] == args[i][a]);
    }
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
    REQUIRE(!xr_aot_backend_run(&view, fixture.target_plan,
                                xr_aot_null_backend_interface(),
                                &null_backend, &stats, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE);
    REQUIRE(xr_aot_representation_backend_run(
        &view, fixture.function, fixture.target_plan, &policy,
        xr_aot_null_backend_interface(), &null_backend, &stats, &diag));
    REQUIRE(stats.visited == 2 && stats.applied == 2 && stats.refused == 0);
    char emission[2048];
    XrAotTestBackend backend = {
        .buffer = emission,
        .capacity = sizeof(emission),
    };
    REQUIRE(xr_aot_representation_backend_run(
        &view, fixture.function, fixture.target_plan, &policy,
        xr_aot_test_backend_interface(), &backend, &stats, &diag));
    REQUIRE(strstr(emission, "transform=representation-adapter") != NULL);
    REQUIRE(strstr(emission, "kind=1") != NULL);
    REQUIRE(strstr(emission, "kind=2") != NULL);

    FailingBackend failing = {0};
    XrAotBackendInterface failing_interface = {
        .abi_version = XR_AOT_REFINEMENT_BACKEND_ABI_VERSION,
        .supported_transforms =
            XR_AOT_TRANSFORM_BIT(XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER),
        .begin = failing_backend_begin,
        .visit = failing_backend_visit,
        .finish = failing_backend_finish,
        .abort = failing_backend_abort,
    };
    REQUIRE(!xr_aot_representation_backend_run(
        &view, fixture.function, fixture.target_plan, &policy,
        &failing_interface, &failing, &stats, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_BACKEND_FAILURE);
    REQUIRE(failing.aborted);

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
    records[0].protocol.pass_id++;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    memcpy(records, original.records, sizeof(records));
    records[0].decision = XR_AOT_REFINEMENT_REFUSED;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

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
    records[0].representation_adapter.use_semantic_immediate++;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.input_rep_kind = XR_MACHINE_REP_F64;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.machine_rep_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_REPRESENTATION);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.reserved = 1;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    memcpy(records, original.records, sizeof(records));
    records[0].direct_call.target_call_index = 1;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_STATE);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.layout_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_LAYOUT);

    memcpy(records, original.records, sizeof(records));
    records[0].representation_adapter.fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    memcpy(records, original.records, sizeof(records));
    records[0].fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RECORD_FINGERPRINT);

    mutated = original;
    mutated.fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_PLAN_FINGERPRINT);

    memcpy(records, original.records, sizeof(records));
    XrAotTransformationRecord swap = records[0];
    records[0] = records[1];
    records[1] = swap;
    mutated = original;
    mutated.records = records;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_NONCANONICAL_ORDER);

    records[0] = original.records[0];
    records[1] = original.records[0];
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_DUPLICATE_USE);

    mutated = original;
    mutated.record_count = XR_AOT_REFINEMENT_MAX_RECORDS + 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, fixture.target_plan, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RESOURCE_BUDGET);

    xr_aot_refinement_plan_free(plan);
    representation_fixture_free(&fixture);
}

static void test_independent_verifier_requires_exact_coverage(void) {
    RepresentationFixture fixture = representation_fixture_create();
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *full = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView full_view = xr_aot_refinement_plan_view(full);
    XrAotRefinementPlan *repeat = build_representation_plan(&fixture, &diag);
    XrAotRefinementPlanView repeat_view = xr_aot_refinement_plan_view(repeat);
    REQUIRE(full_view.record_count == repeat_view.record_count);
    REQUIRE(xr_fingerprint_equal(full_view.fingerprint,
                                 repeat_view.fingerprint));
    REQUIRE(memcmp(full_view.records, repeat_view.records,
                   full_view.record_count * sizeof(*full_view.records)) == 0);
    const XrAotRepresentationAdapterRecord *record =
        &full_view.records[0].representation_adapter;
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(fixture.target_plan, &diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol =
        xr_aot_refinement_representation_protocol(27902);
    XrAotRepresentationAdapterRequest request = {
        .source_value = record->source_value,
        .use_operation = record->use_operation,
        .use_block = record->use_block,
        .use_operand = record->use_operand,
        .use_kind = record->use_kind,
        .adapter_kind = record->adapter_kind,
        .input_rep_kind = record->input_rep_kind,
        .output_rep_kind = record->output_rep_kind,
        .layout = record->layout,
        .policy_fingerprint = record->policy_fingerprint,
    };
    uint32_t decision = 0;
    REQUIRE(xr_aot_refinement_try_representation_adapter(
        builder, &protocol, fixture.target_plan, &request, &decision, &diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_APPLIED);
    XrAotRefinementPlan *partial = NULL;
    REQUIRE(xr_aot_refinement_builder_freeze(
        builder, fixture.target_plan, &partial, &diag));
    XrAotRefinementPlanView partial_view =
        xr_aot_refinement_plan_view(partial);
    XiRepPolicy policy = xi_rep_policy_native_boundary();
    REQUIRE(!xr_aot_representation_refinement_verify(
        &partial_view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE);

    XrAotRefinementBuilder *extra_builder =
        xr_aot_refinement_builder_create(fixture.target_plan, &diag);
    REQUIRE(extra_builder != NULL);
    for (uint32_t i = 0; i < full_view.record_count; i++) {
        const XrAotRepresentationAdapterRecord *adapter =
            &full_view.records[i].representation_adapter;
        XrAotRepresentationAdapterRequest exact = {
            .source_value = adapter->source_value,
            .use_operation = adapter->use_operation,
            .use_block = adapter->use_block,
            .use_operand = adapter->use_operand,
            .use_kind = adapter->use_kind,
            .adapter_kind = adapter->adapter_kind,
            .input_rep_kind = adapter->input_rep_kind,
            .output_rep_kind = adapter->output_rep_kind,
            .layout = adapter->layout,
            .policy_fingerprint = adapter->policy_fingerprint,
        };
        REQUIRE(xr_aot_refinement_try_representation_adapter(
            extra_builder, &protocol, fixture.target_plan, &exact, &decision,
            &diag));
    }
    const XrSemanticPlan *semantic = fixture.function->semantic_plan;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(
            semantic, fixture.function->semantic_plan_function_index);
    REQUIRE(semantic_function != NULL);
    uint32_t rhs_value = semantic_function->value_begin + fixture.rhs->id;
    uint32_t sum_value = semantic_function->value_begin + fixture.sum->id;
    uint32_t sum_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation && operation->result_value == sum_value)
            sum_operation = i;
    }
    const XrSemanticOperationRecord *sum =
        xr_semantic_plan_operation(semantic, sum_operation);
    const XrTargetValueRepRecord *rhs_binding =
        xr_target_plan_value_rep(fixture.target_plan, rhs_value);
    const XrTargetMachineRepRecord *rhs_machine =
        rhs_binding ? xr_target_plan_machine_rep(
                          fixture.target_plan, rhs_binding->register_rep)
                    : NULL;
    REQUIRE(sum && rhs_machine);
    XrAotRepresentationAdapterRequest extra = {
        .source_value = rhs_value,
        .use_operation = sum_operation,
        .use_block = sum->block,
        .use_operand = 1,
        .use_kind = XR_AOT_REP_USE_OPERATION,
        .adapter_kind = XR_AOT_REP_ADAPTER_BOX,
        .input_rep_kind = rhs_machine->kind,
        .output_rep_kind = XR_MACHINE_REP_DYN_VALUE,
        .layout = record->layout,
        .policy_fingerprint = record->policy_fingerprint,
    };
    REQUIRE(xr_aot_refinement_try_representation_adapter(
        extra_builder, &protocol, fixture.target_plan, &extra, &decision,
        &diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_APPLIED);
    XrAotRefinementPlan *extra_plan = NULL;
    REQUIRE(xr_aot_refinement_builder_freeze(
        extra_builder, fixture.target_plan, &extra_plan, &diag));
    XrAotRefinementPlanView extra_view =
        xr_aot_refinement_plan_view(extra_plan);
    REQUIRE(xr_aot_refinement_verify(&extra_view, fixture.target_plan, &diag));
    REQUIRE(!xr_aot_representation_refinement_verify(
        &extra_view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE);

    policy.force_phi_tagged = true;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &full_view, fixture.function, fixture.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_STALE_EVIDENCE);

    xr_aot_refinement_plan_free(extra_plan);
    xr_aot_refinement_builder_free(extra_builder);
    xr_aot_refinement_plan_free(partial);
    xr_aot_refinement_builder_free(builder);
    xr_aot_refinement_plan_free(repeat);
    xr_aot_refinement_plan_free(full);
    representation_fixture_free(&fixture);
}

static XrTargetPlan *build_attached_target_plan(XiFunc *function,
                                                XrTargetProfile **out_profile) {
    char error[512] = {0};
    function->stage = XI_STAGE_OPTIMIZED;
    REQUIRE(xr_semantic_plan_build_and_attach(function, error,
                                              sizeof(error)));
    *out_profile = build_target_profile();
    XrTargetPlan *target_plan = NULL;
    if (!xr_target_plan_build(function->semantic_plan, *out_profile,
                              &target_plan, error, sizeof(error))) {
        fprintf(stderr, "target plan build for %s failed: %s\n",
                function->name, error);
        abort();
    }
    return target_plan;
}

static uint32_t semantic_operation_for_value(const XrSemanticPlan *semantic,
                                             uint32_t value) {
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation && operation->result_value == value)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static XrTargetPlan *build_parameter_target_without_operation(
    XiFunc *function, XiBlock *entry, XiValue *parameter, XiValue *use,
    XrTargetProfile **out_profile) {
    REQUIRE(function != NULL && entry != NULL && parameter != NULL &&
            use != NULL && entry->nvalues == 2 &&
            entry->values[0] == parameter && entry->values[1] == use);
    function->stage = XI_STAGE_OPTIMIZED;
    entry->values[0] = use;
    entry->nvalues = 1;
    char error[512] = {0};
    bool attached = xr_semantic_plan_build_and_attach(
        function, error, sizeof(error));
    entry->values[0] = parameter;
    entry->values[1] = use;
    entry->nvalues = 2;
    REQUIRE(attached);
    REQUIRE(xr_semantic_plan_parameter_count(function->semantic_plan) == 1);
    const XrSemanticParameterRecord *semantic_parameter =
        xr_semantic_plan_parameter(function->semantic_plan, 0);
    REQUIRE(semantic_parameter != NULL);
    REQUIRE(semantic_operation_for_value(function->semantic_plan,
                                         semantic_parameter->value) ==
            XR_SEMANTIC_INDEX_NONE);
    *out_profile = build_target_profile();
    XrTargetPlan *target_plan = NULL;
    REQUIRE(xr_target_plan_build(function->semantic_plan, *out_profile,
                                 &target_plan, error, sizeof(error)));
    return target_plan;
}

static void test_parameter_without_operation_uses_stable_identity(void) {
    XiFunc *function = xi_func_new("rep_parameter_stable", &scalar_int);
    XiBlock *entry = xi_block_new(function);
    XiValue *parameter = xi_param(function, entry, 0, &scalar_int);
    REQUIRE(function != NULL && entry != NULL && parameter != NULL);
    function->nparams = 1;
    function->min_params = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    XiValue *use = xi_value_new(function, entry, XI_UNBOX,
                                &scalar_int, 1);
    REQUIRE(use != NULL);
    use->args[0] = parameter;
    xi_block_set_return(entry, use);

    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = build_parameter_target_without_operation(
        function, entry, parameter, use, &profile);
    const XrSemanticParameterRecord *semantic_parameter =
        xr_semantic_plan_parameter(function->semantic_plan, 0);
    REQUIRE(semantic_parameter != NULL &&
            semantic_parameter->value == parameter->id);

    XiRepPolicy policy = xi_rep_policy_native_boundary();
    policy.force_return_tagged = true;
    XrAotRefinementDiagnostic diag = {0};
    XrAotRefinementPlan *plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build(
        function, target, &policy, &plan, &diag));
    XrAotRefinementPlanView view = xr_aot_refinement_plan_view(plan);
    REQUIRE(view.record_count == 2);
    uint32_t parameter_record = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < view.record_count; i++) {
        const XrAotRepresentationAdapterRecord *adapter =
            &view.records[i].representation_adapter;
        if (adapter->source_kind == XR_AOT_REP_SOURCE_PARAMETER) {
            REQUIRE(parameter_record == XR_SEMANTIC_INDEX_NONE);
            parameter_record = i;
        }
    }
    REQUIRE(parameter_record != XR_SEMANTIC_INDEX_NONE);
    const XrAotRepresentationAdapterRecord *adapter =
        &view.records[parameter_record].representation_adapter;
    REQUIRE(adapter->source_value == semantic_parameter->value);
    REQUIRE(adapter->source_operation == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(xr_stable_id_equal(adapter->source_operation_id,
                               semantic_parameter->id));
    REQUIRE(xr_aot_representation_refinement_verify(
        &view, function, target, &policy, &diag));

    XrAotTransformationRecord records[2];
    memcpy(records, view.records, sizeof(records));
    XrAotRefinementPlanView mutated = view;
    mutated.records = records;
    records[parameter_record].representation_adapter.source_operation =
        adapter->use_operation;
    REQUIRE(!xr_aot_refinement_verify(&mutated, target, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);

    memcpy(records, view.records, sizeof(records));
    records[parameter_record]
        .representation_adapter.source_operation_id.bytes[0] ^= 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, target, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);

    memcpy(records, view.records, sizeof(records));
    XrAotTransformationRecord swap = records[0];
    records[0] = records[1];
    records[1] = swap;
    REQUIRE(!xr_aot_refinement_verify(&mutated, target, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_NONCANONICAL_ORDER);

    mutated = view;
    mutated.record_count = XR_AOT_REFINEMENT_MAX_RECORDS + 1u;
    REQUIRE(!xr_aot_refinement_verify(&mutated, target, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_RESOURCE_BUDGET);

    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(target, &diag);
    REQUIRE(builder != NULL);
    XrAotPassProtocol protocol =
        xr_aot_refinement_representation_protocol(27904);
    XrAotRepresentationAdapterRequest request = {
        .source_value = adapter->source_value,
        .use_operation = adapter->use_operation,
        .use_block = adapter->use_block,
        .use_operand = adapter->use_operand,
        .use_kind = adapter->use_kind,
        .adapter_kind = adapter->adapter_kind,
        .input_rep_kind = adapter->input_rep_kind,
        .output_rep_kind = adapter->output_rep_kind,
        .layout = adapter->layout,
        .policy_fingerprint = adapter->policy_fingerprint,
    };
    uint32_t decision = 0;
    REQUIRE(xr_aot_refinement_try_representation_adapter(
        builder, &protocol, target, &request, &decision, &diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_APPLIED);
    XrAotRefinementPlan *partial = NULL;
    REQUIRE(xr_aot_refinement_builder_freeze(builder, target, &partial,
                                              &diag));
    XrAotRefinementPlanView partial_view =
        xr_aot_refinement_plan_view(partial);
    REQUIRE(!xr_aot_representation_refinement_verify(
        &partial_view, function, target, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE);

    parameter->param_mode = XR_PARAM_REF;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &view, function, target, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    XrAotRefinementPlan *rejected = NULL;
    REQUIRE(!xr_aot_representation_refinement_build(
        function, target, &policy, &rejected, &diag));
    REQUIRE(rejected == NULL &&
            diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    parameter->param_mode = XR_PARAM_READ;
    parameter->op = XI_CONST;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &view, function, target, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    parameter->op = XI_PARAM;
    static XrType scalar_float_mutation = {
        .kind = XR_KIND_FLOAT,
        .id = 2,
        .scalar_rep = XR_NATIVE_F64,
        .frozen = true,
    };
    parameter->type = &scalar_float_mutation;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &view, function, target, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);

    xr_aot_refinement_plan_free(partial);
    xr_aot_refinement_builder_free(builder);
    xr_aot_refinement_plan_free(plan);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    xi_func_free(function);
}

static void test_parameter_phi_and_return_use_domains_are_exact(void) {
    XrAotRefinementDiagnostic diag = {0};

    XiFunc *parameter_function = xi_func_new("rep_parameter", &scalar_int);
    XiBlock *parameter_entry = xi_block_new(parameter_function);
    XiValue *parameter = xi_param(parameter_function, parameter_entry, 0,
                                  &scalar_int);
    parameter_function->nparams = 1;
    parameter_function->params =
        (XiValue **) xr_malloc(sizeof(*parameter_function->params));
    REQUIRE(parameter_function->params != NULL);
    parameter_function->params[0] = parameter;
    XiValue *parameter_use = xi_value_new(parameter_function, parameter_entry,
                                           XI_UNBOX, &scalar_int, 1);
    REQUIRE(parameter_use != NULL);
    parameter_use->args[0] = parameter;
    xi_block_set_return(parameter_entry, parameter_use);
    XrTargetProfile *parameter_profile = NULL;
    XrTargetPlan *parameter_target = build_attached_target_plan(
        parameter_function, &parameter_profile);
    uint32_t parameter_values = parameter_function->next_value_id;
    XiRepPolicy native = xi_rep_policy_native_boundary();
    native.force_return_tagged = true;
    XrAotRefinementPlan *parameter_plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build(
        parameter_function, parameter_target, &native, &parameter_plan,
        &diag));
    XrAotRefinementPlanView parameter_view =
        xr_aot_refinement_plan_view(parameter_plan);
    REQUIRE(parameter_view.record_count == 2);
    uint32_t parameter_sources = 0;
    uint32_t return_controls = 0;
    for (uint32_t i = 0; i < parameter_view.record_count; i++) {
        if (parameter_view.records[i].representation_adapter.source_kind ==
            XR_AOT_REP_SOURCE_PARAMETER)
            parameter_sources++;
        if (parameter_view.records[i].representation_adapter.use_kind ==
            XR_AOT_REP_USE_BLOCK_CONTROL)
            return_controls++;
    }
    REQUIRE(parameter_sources == 1);
    REQUIRE(return_controls == 1);
    REQUIRE(parameter_function->next_value_id == parameter_values);
    REQUIRE(xr_aot_representation_refinement_verify(
        &parameter_view, parameter_function, parameter_target, &native,
        &diag));
    const XrSemanticParameterRecord *parameter_semantic =
        xr_semantic_plan_parameter(parameter_function->semantic_plan, 0);
    REQUIRE(parameter_semantic != NULL);
    uint32_t parameter_operation = semantic_operation_for_value(
        parameter_function->semantic_plan, parameter_semantic->value);
    REQUIRE(parameter_operation != XR_SEMANTIC_INDEX_NONE);
    const XrSemanticOperationRecord *parameter_operation_record =
        xr_semantic_plan_operation(parameter_function->semantic_plan,
                                   parameter_operation);
    REQUIRE(parameter_operation_record != NULL &&
            parameter_operation_record->opcode == XI_PARAM);
    bool exact_parameter_record = false;
    for (uint32_t i = 0; i < parameter_view.record_count; i++) {
        const XrAotRepresentationAdapterRecord *record =
            &parameter_view.records[i].representation_adapter;
        if (record->source_kind == XR_AOT_REP_SOURCE_PARAMETER) {
            REQUIRE(record->source_operation == parameter_operation);
            exact_parameter_record = true;
        }
    }
    REQUIRE(exact_parameter_record);
    parameter->param_mode = XR_PARAM_REF;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &parameter_view, parameter_function, parameter_target, &native,
        &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    XrAotRefinementPlan *inexact_parameter_plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build(
        parameter_function, parameter_target, &native,
        &inexact_parameter_plan, &diag));
    REQUIRE(inexact_parameter_plan == NULL &&
            diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    xr_aot_refinement_plan_free(parameter_plan);
    xr_target_plan_free(parameter_target);
    xr_target_profile_free(parameter_profile);
    xi_func_free(parameter_function);

    XiFunc *return_function = xi_func_new("rep_return", &scalar_int);
    XiBlock *return_entry = xi_block_new(return_function);
    XiValue *return_value = xi_const_int(return_function, return_entry, 9,
                                         &scalar_int);
    xi_block_set_return(return_entry, return_value);
    XrTargetProfile *return_profile = NULL;
    XrTargetPlan *return_target = build_attached_target_plan(
        return_function, &return_profile);
    XiRepPolicy tagged_return = xi_rep_policy_native_boundary();
    tagged_return.force_return_tagged = true;
    uint32_t return_values = return_function->next_value_id;
    XrAotRefinementPlan *return_plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build(
        return_function, return_target, &tagged_return, &return_plan, &diag));
    XrAotRefinementPlanView return_view =
        xr_aot_refinement_plan_view(return_plan);
    REQUIRE(return_view.record_count == 1);
    REQUIRE(return_view.records[0].representation_adapter.use_kind ==
            XR_AOT_REP_USE_BLOCK_CONTROL);
    REQUIRE(return_view.records[0].representation_adapter.use_operation ==
            XR_SEMANTIC_INDEX_NONE);
    REQUIRE(return_function->next_value_id == return_values);
    REQUIRE(xr_aot_representation_refinement_verify(
        &return_view, return_function, return_target, &tagged_return, &diag));
    xr_aot_refinement_plan_free(return_plan);
    xr_target_plan_free(return_target);
    xr_target_profile_free(return_profile);
    xi_func_free(return_function);

    XiFunc *phi_function = xi_func_new("rep_phi", &scalar_int);
    XiBlock *phi_entry = xi_block_new(phi_function);
    XiBlock *then_block = xi_block_new(phi_function);
    XiBlock *else_block = xi_block_new(phi_function);
    XiBlock *merge_block = xi_block_new(phi_function);
    XiValue *condition = xi_const_int(phi_function, phi_entry, 1,
                                      &scalar_int);
    XiValue *then_value = xi_const_int(phi_function, then_block, 1,
                                       &scalar_int);
    XiValue *else_value = xi_const_int(phi_function, else_block, 2,
                                       &scalar_int);
    xi_block_set_if(phi_entry, condition, then_block, else_block);
    xi_block_set_jump(then_block, merge_block);
    xi_block_set_jump(else_block, merge_block);
    XiPhi *phi = xi_phi_new(phi_function, merge_block, &scalar_int, 2);
    REQUIRE(phi != NULL);
    phi->value.args[0] = then_value;
    phi->value.args[1] = else_value;
    xi_block_set_return(merge_block, &phi->value);
    XrTargetProfile *phi_profile = NULL;
    XrTargetPlan *phi_target = build_attached_target_plan(phi_function,
                                                          &phi_profile);
    XiRepPolicy tagged_phi = xi_rep_policy_native_boundary();
    tagged_phi.force_phi_tagged = true;
    tagged_phi.force_return_tagged = true;
    uint32_t phi_values = phi_function->next_value_id;
    XrAotRefinementPlan *phi_plan = NULL;
    REQUIRE(xr_aot_representation_refinement_build(
        phi_function, phi_target, &tagged_phi, &phi_plan, &diag));
    XrAotRefinementPlanView phi_view = xr_aot_refinement_plan_view(phi_plan);
    REQUIRE(phi_view.record_count == 2);
    for (uint32_t i = 0; i < phi_view.record_count; i++) {
        REQUIRE(phi_view.records[i].representation_adapter.use_kind ==
                XR_AOT_REP_USE_OPERATION);
        REQUIRE(phi_view.records[i].representation_adapter.use_operand == i);
    }
    REQUIRE(phi_function->next_value_id == phi_values);
    REQUIRE(xr_aot_representation_refinement_verify(
        &phi_view, phi_function, phi_target, &tagged_phi, &diag));
    phi->value.flags ^= XI_FLAG_SIDE_EFFECT;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &phi_view, phi_function, phi_target, &tagged_phi, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    XrAotRefinementPlan *stale_phi_plan = NULL;
    REQUIRE(!xr_aot_representation_refinement_build(
        phi_function, phi_target, &tagged_phi, &stale_phi_plan, &diag));
    REQUIRE(stale_phi_plan == NULL);
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    xr_aot_refinement_plan_free(phi_plan);
    xr_target_plan_free(phi_target);
    xr_target_profile_free(phi_profile);
    xi_func_free(phi_function);
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
        .use_block = native->use_block,
        .use_operand = native->use_operand,
        .use_kind = native->use_kind,
        .adapter_kind = XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX,
        .input_rep_kind = native->input_rep_kind,
        .output_rep_kind = native->output_rep_kind,
        .layout = XR_SEMANTIC_INDEX_NONE,
        .policy_fingerprint = native->policy_fingerprint,
    };
    uint32_t decision = XR_AOT_REFINEMENT_APPLIED;
    REQUIRE(xr_aot_refinement_try_representation_adapter(
        builder, &protocol, fixture.target_plan, &request, &decision, &diag));
    REQUIRE(decision == XR_AOT_REFINEMENT_REFUSED);
    REQUIRE(diag.issue ==
            XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE);
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
    use.sum->op = XI_CONST;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &use_view, use.function, use.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    xr_aot_refinement_plan_free(use_plan);
    representation_fixture_free(&use);

    RepresentationFixture use_aux = representation_fixture_create();
    XrAotRefinementPlan *use_aux_plan = build_representation_plan(&use_aux,
                                                                  &diag);
    XrAotRefinementPlanView use_aux_view =
        xr_aot_refinement_plan_view(use_aux_plan);
    use_aux.sum->aux_int++;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &use_aux_view, use_aux.function, use_aux.target_plan, &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_USE_SITE);
    xr_aot_refinement_plan_free(use_aux_plan);
    representation_fixture_free(&use_aux);

    RepresentationFixture source_flags = representation_fixture_create();
    XrAotRefinementPlan *source_flags_plan =
        build_representation_plan(&source_flags, &diag);
    XrAotRefinementPlanView source_flags_view =
        xr_aot_refinement_plan_view(source_flags_plan);
    source_flags.native_constant->flags ^= XI_FLAG_SIDE_EFFECT;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &source_flags_view, source_flags.function, source_flags.target_plan,
        &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_IDENTITY);
    xr_aot_refinement_plan_free(source_flags_plan);
    representation_fixture_free(&source_flags);

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

    RepresentationFixture type_flags = representation_fixture_create();
    XrAotRefinementPlan *type_flags_plan =
        build_representation_plan(&type_flags, &diag);
    XrAotRefinementPlanView type_flags_view =
        xr_aot_refinement_plan_view(type_flags_plan);
    type_flags.native_constant->type->is_const = true;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &type_flags_view, type_flags.function, type_flags.target_plan,
        &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    type_flags.native_constant->type->is_const = false;
    uint32_t semantic_type_id =
        type_flags.native_constant->type->semantic_type_id;
    type_flags.native_constant->type->semantic_type_id = semantic_type_id + 1u;
    REQUIRE(!xr_aot_representation_refinement_verify(
        &type_flags_view, type_flags.function, type_flags.target_plan,
        &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    type_flags.native_constant->type->semantic_type_id = semantic_type_id;
    const char *alias_name = type_flags.native_constant->type->alias_name;
    type_flags.native_constant->type->alias_name = "mutated-authority";
    REQUIRE(!xr_aot_representation_refinement_verify(
        &type_flags_view, type_flags.function, type_flags.target_plan,
        &policy, &diag));
    REQUIRE(diag.issue == XR_AOT_REFINEMENT_SOURCE_TYPE);
    type_flags.native_constant->type->alias_name = alias_name;
    xr_aot_refinement_plan_free(type_flags_plan);
    representation_fixture_free(&type_flags);
}

int main(void) {
    test_scalar_direct_call_refuses_without_baseline_change();
    test_stale_state_and_baseline_mutations_fail_closed();
    test_machine_readable_invalidation_is_functional();
    test_null_and_test_backends_cover_refusal();
    test_representation_adapters_are_immutable_and_consumable();
    test_representation_record_mutations_fail_closed();
    test_independent_verifier_requires_exact_coverage();
    test_parameter_without_operation_uses_stable_identity();
    test_parameter_phi_and_return_use_domains_are_exact();
    test_enum_descriptor_adapter_refuses_without_layout_family();
    test_live_source_use_and_type_mutations_are_rederived();
    printf("TargetPlan-native AOT refinement tests passed\n");
    return 0;
}
