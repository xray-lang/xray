/*
 * test_typed_frame.c - Verified typed slot arena contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/xr_typed_frame.h"
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

typedef struct TypedFrameFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
} TypedFrameFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("typed_frame_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    XiValue *released = xi_const_int(function, entry, 7, &stub_int);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_int, 1);
    REQUIRE(result != NULL && released != NULL && release != NULL);
    release->args[0] = released;
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static TypedFrameFixture make_fixture(void) {
    TypedFrameFixture fixture = {0};
    fixture.semantic = build_semantic_plan();
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(fixture.profile != NULL);
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile,
                                 &fixture.plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    return fixture;
}

static void dispose_fixture(TypedFrameFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static XrTypedFrame *create_frame(const TypedFrameFixture *fixture,
                                  const XrTypedFrameLimits *limits) {
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture->plan);
    XrTypedFrame *frame = NULL;
    REQUIRE(xr_typed_frame_create(fixture->plan, &fingerprint, 0, limits,
                                  &frame) == XR_TYPED_FRAME_OK);
    REQUIRE(frame != NULL);
    return frame;
}

static void test_exact_slot_access_and_states(void) {
    TypedFrameFixture fixture = make_fixture();
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = create_frame(&fixture, &limits);
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(fixture.plan, &function_count);
    REQUIRE(function_count == 1);
    REQUIRE(xr_typed_frame_arena_size(frame) == functions[0].frame_size);
    REQUIRE(xr_typed_frame_slot_count(frame) == functions[0].slot_count);

    XrTypedSlotAccess access;
    REQUIRE(xr_typed_frame_describe_slot(frame, functions[0].slot_begin,
                                         &access) == XR_TYPED_FRAME_OK);
    REQUIRE(access.size <= 16);
    XrTypedSlotState state = XR_TYPED_SLOT_STATE_INVALID;
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(state == XR_TYPED_SLOT_STATE_UNINITIALIZED);

    uint8_t loaded[16];
    uint8_t original[16];
    memset(loaded, 0xcc, sizeof(loaded));
    memcpy(original, loaded, sizeof(loaded));
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_UNINITIALIZED);
    REQUIRE(memcmp(loaded, original, sizeof(loaded)) == 0);

    uint8_t payload[16];
    memset(payload, 0x5a, sizeof(payload));
    REQUIRE(xr_typed_frame_store(frame, &access, payload, access.size) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(state == XR_TYPED_SLOT_STATE_INITIALIZED);
    memset(loaded, 0, sizeof(loaded));
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(memcmp(loaded, payload, access.size) == 0);

    fixture.plan->fingerprint.bytes[0] ^= 1;
    XrTypedSlotAccess cleared = access;
    REQUIRE(xr_typed_frame_describe_slot(frame, access.slot, &cleared) ==
            XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH);
    REQUIRE(memcmp(&cleared, &(XrTypedSlotAccess) {0}, sizeof(cleared)) == 0);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH);
    fixture.plan->fingerprint.bytes[0] ^= 1;

    XrTypedSlotAccess mutation = access;
    mutation.identity.bytes[0] ^= 1;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = access;
    mutation.alignment = (uint16_t) (mutation.alignment + 1u);
    REQUIRE(xr_typed_frame_store(frame, &mutation, payload, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = access;
    mutation.register_rep ^= 1u;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = access;
    mutation.slot = UINT32_MAX;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size - 1u) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);

    REQUIRE(xr_typed_frame_poison(frame, &access) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(state == XR_TYPED_SLOT_STATE_POISONED);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_POISONED);
    REQUIRE(xr_typed_frame_store(frame, &access, payload, access.size) ==
            XR_TYPED_FRAME_POISONED);
    REQUIRE(xr_typed_frame_poison(frame, &access) == XR_TYPED_FRAME_POISONED);

    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_CLEANED);
    REQUIRE(xr_typed_frame_arena_size(frame) == 0);
    REQUIRE(xr_typed_frame_slot_count(frame) == 0);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_CLEANED);
    xr_typed_frame_free(frame);
    dispose_fixture(&fixture);
}

static void test_plan_identity_shape_and_budgets(void) {
    TypedFrameFixture fixture = make_fixture();
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
    XrTypedFrame *frame = (XrTypedFrame *) (uintptr_t) 1;
    REQUIRE(xr_typed_frame_create(NULL, &fingerprint, 0, &limits, &frame) ==
            XR_TYPED_FRAME_INVALID_ARGUMENT);
    REQUIRE(frame == NULL);

    fixture.plan->verified = false;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &limits,
                                  &frame) == XR_TYPED_FRAME_PLAN_NOT_VERIFIED);
    REQUIRE(frame == NULL);
    fixture.plan->verified = true;

    uint32_t schema = fixture.plan->schema_version;
    fixture.plan->schema_version = schema - 1u;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &limits,
                                  &frame) == XR_TYPED_FRAME_PLAN_NOT_VERIFIED);
    REQUIRE(frame == NULL);
    fixture.plan->schema_version = schema;

    XrFingerprint wrong = fingerprint;
    wrong.bytes[0] ^= 1;
    REQUIRE(xr_typed_frame_create(fixture.plan, &wrong, 0, &limits, &frame) ==
            XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH);
    REQUIRE(frame == NULL);

    fixture.plan->completed_family_mask |= UINT64_C(1) << 63;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &limits,
                                  &frame) == XR_TYPED_FRAME_UNSUPPORTED_FAMILY);
    REQUIRE(frame == NULL);
    fixture.plan->completed_family_mask = XR_TARGET_REQUIRED_FAMILIES;

    uint16_t frame_align = fixture.plan->functions[0].frame_align;
    fixture.plan->functions[0].frame_align = 3;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &limits,
                                  &frame) == XR_TYPED_FRAME_PLAN_NOT_VERIFIED);
    REQUIRE(frame == NULL);
    fixture.plan->functions[0].frame_align = frame_align;

    uint32_t slot_offset = fixture.plan->slots[0].offset;
    fixture.plan->slots[0].offset = fixture.plan->functions[0].frame_size;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &limits,
                                  &frame) == XR_TYPED_FRAME_PLAN_NOT_VERIFIED);
    REQUIRE(frame == NULL);
    fixture.plan->slots[0].offset = slot_offset;

    XrTypedFrameLimits small = limits;
    REQUIRE(fixture.plan->functions[0].frame_size > 0);
    small.max_arena_bytes = fixture.plan->functions[0].frame_size - 1u;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &small,
                                  &frame) == XR_TYPED_FRAME_BUDGET_EXHAUSTED);
    small = limits;
    REQUIRE(fixture.plan->functions[0].slot_count > 0);
    small.max_slot_count = fixture.plan->functions[0].slot_count - 1u;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &small,
                                  &frame) == XR_TYPED_FRAME_BUDGET_EXHAUSTED);
    small = limits;
    small.max_total_bytes = 0;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &small,
                                  &frame) == XR_TYPED_FRAME_BUDGET_EXHAUSTED);
    small = limits;
    small.max_arena_bytes = XR_TYPED_FRAME_MAX_ARENA_BYTES + 1u;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &small,
                                  &frame) == XR_TYPED_FRAME_BUDGET_EXHAUSTED);
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, UINT32_MAX,
                                  &limits, &frame) ==
            XR_TYPED_FRAME_FUNCTION_INVALID);
    REQUIRE(frame == NULL);
    dispose_fixture(&fixture);
}

static void test_plan_ownership_and_void_binding(void) {
    TypedFrameFixture fixture = make_fixture();
    uint32_t value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(fixture.plan, &value_count);
    bool found_void = false;
    for (uint32_t i = 0; i < value_count; i++) {
        const XrTargetMachineRepRecord *rep =
            xr_target_plan_machine_rep(fixture.plan, values[i].memory_rep);
        if (rep && rep->kind == XR_MACHINE_REP_VOID) {
            REQUIRE(values[i].slot == XR_SEMANTIC_INDEX_NONE);
            found_void = true;
        }
    }
    REQUIRE(found_void);

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = create_frame(&fixture, &limits);
    xr_target_plan_free(fixture.plan);
    fixture.plan = NULL;

    XrTypedSlotAccess access;
    REQUIRE(xr_typed_frame_describe_slot(frame, 0, &access) ==
            XR_TYPED_FRAME_OK);
    uint8_t payload[16];
    uint8_t loaded[16];
    REQUIRE(access.size <= sizeof(payload));
    memset(payload, 0xa5, sizeof(payload));
    REQUIRE(xr_typed_frame_store(frame, &access, payload, access.size) ==
            XR_TYPED_FRAME_OK);
    memset(loaded, 0, sizeof(loaded));
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(memcmp(payload, loaded, access.size) == 0);
    xr_typed_frame_free(frame);
    dispose_fixture(&fixture);
}

int main(void) {
    test_exact_slot_access_and_states();
    test_plan_identity_shape_and_budgets();
    test_plan_ownership_and_void_binding();
    puts("typed frame tests passed");
    return 0;
}
