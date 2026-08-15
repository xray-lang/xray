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

static XrSemanticPlan *build_executable_semantic_plan(void) {
    XiFunc *function = xi_func_new("typed_frame_context_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_int(function, entry, 40, &stub_int);
    XiValue *right = xi_const_int(function, entry, 2, &stub_int);
    XiValue *sum = xi_value_new(function, entry, XI_ADD, &stub_int, 2);
    REQUIRE(left != NULL && right != NULL && sum != NULL);
    sum->args[0] = left;
    sum->args[1] = right;
    xi_block_set_return(entry, sum);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static TypedFrameFixture make_fixture_from_semantic(XrSemanticPlan *semantic) {
    TypedFrameFixture fixture = {0};
    fixture.semantic = semantic;
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(fixture.profile != NULL);
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile,
                                 &fixture.plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    return fixture;
}

static TypedFrameFixture make_fixture(void) {
    return make_fixture_from_semantic(build_semantic_plan());
}

static TypedFrameFixture make_executable_fixture(void) {
    return make_fixture_from_semantic(build_executable_semantic_plan());
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
    XrTypedFrameMemoryFootprint footprint;
    REQUIRE(xr_typed_frame_memory_footprint(frame, &footprint) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(footprint.arena_allocation_bytes == functions[0].frame_size);
    REQUIRE(footprint.alignment_padding_bytes ==
            (functions[0].frame_size ? functions[0].frame_align - 1u : 0u));
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    REQUIRE(footprint.slot_state_metadata_bytes == functions[0].slot_count);
#else
    REQUIRE(footprint.slot_state_metadata_bytes == 0);
#endif
    REQUIRE(footprint.total_bytes ==
            footprint.fixed_frame_bytes + footprint.arena_allocation_bytes +
                footprint.alignment_padding_bytes +
                footprint.slot_state_metadata_bytes);

    XrTypedSlotAccess access;
    REQUIRE(xr_typed_frame_describe_slot(frame, functions[0].slot_begin,
                                         &access) == XR_TYPED_FRAME_OK);
    REQUIRE(access.size <= 16);
    XrTypedSlotState state = XR_TYPED_SLOT_STATE_INVALID;
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(state == XR_TYPED_SLOT_STATE_UNINITIALIZED);
#else
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_DEBUG_METADATA_UNAVAILABLE);
    REQUIRE(state == XR_TYPED_SLOT_STATE_INVALID);
#endif

    uint8_t loaded[16];
    uint8_t original[16];
    memset(loaded, 0xcc, sizeof(loaded));
    memcpy(original, loaded, sizeof(loaded));
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_UNINITIALIZED);
    REQUIRE(memcmp(loaded, original, sizeof(loaded)) == 0);
#else
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_OK);
    for (uint32_t i = 0; i < access.size; i++)
        REQUIRE(loaded[i] == 0);
    REQUIRE(memcmp(loaded + access.size, original + access.size,
                   sizeof(loaded) - access.size) == 0);
#endif

    uint8_t payload[16];
    memset(payload, 0x5a, sizeof(payload));
    REQUIRE(xr_typed_frame_store(frame, &access, payload, access.size) ==
            XR_TYPED_FRAME_OK);
#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(state == XR_TYPED_SLOT_STATE_INITIALIZED);
#else
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_DEBUG_METADATA_UNAVAILABLE);
#endif
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

#if XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA
    REQUIRE(xr_typed_frame_poison(frame, &access) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_slot_state(frame, access.slot, &state) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(state == XR_TYPED_SLOT_STATE_POISONED);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_POISONED);
    REQUIRE(xr_typed_frame_store(frame, &access, payload, access.size) ==
            XR_TYPED_FRAME_POISONED);
    REQUIRE(xr_typed_frame_poison(frame, &access) == XR_TYPED_FRAME_POISONED);
#else
    REQUIRE(xr_typed_frame_poison(frame, &access) ==
            XR_TYPED_FRAME_DEBUG_METADATA_UNAVAILABLE);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_store(frame, &access, payload, access.size) ==
            XR_TYPED_FRAME_OK);
#endif

    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_CLEANED);
    REQUIRE(xr_typed_frame_arena_size(frame) == 0);
    REQUIRE(xr_typed_frame_slot_count(frame) == 0);
    memset(&footprint, 0xcc, sizeof(footprint));
    REQUIRE(xr_typed_frame_memory_footprint(frame, &footprint) ==
            XR_TYPED_FRAME_CLEANED);
    REQUIRE(memcmp(&footprint, &(XrTypedFrameMemoryFootprint) {0},
                   sizeof(footprint)) == 0);
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

    frame = create_frame(&fixture, &limits);
    XrTypedFrameMemoryFootprint footprint;
    REQUIRE(xr_typed_frame_memory_footprint(frame, &footprint) ==
            XR_TYPED_FRAME_OK);
    xr_typed_frame_free(frame);
    frame = NULL;

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
    small.max_total_bytes = footprint.total_bytes - 1u;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &small,
                                  &frame) == XR_TYPED_FRAME_BUDGET_EXHAUSTED);
    small.max_total_bytes = footprint.total_bytes;
    REQUIRE(xr_typed_frame_create(fixture.plan, &fingerprint, 0, &small,
                                  &frame) == XR_TYPED_FRAME_OK);
    xr_typed_frame_free(frame);
    frame = NULL;
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

static XrModuleGenerationIdentity generation_identity(
    const TypedFrameFixture *fixture, uint64_t generation_number,
    uint8_t generation_marker) {
    XrModuleGenerationIdentity identity = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .target_plan_schema_version = XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION,
        .generation_number = generation_number,
        .completed_family_mask = XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK,
    };
    XrFingerprint semantic = xr_target_plan_semantic_fingerprint(fixture->plan);
    XrFingerprint profile = xr_target_profile_fingerprint(fixture->profile);
    XrFingerprint target = xr_target_plan_fingerprint(fixture->plan);
    memcpy(identity.semantic_fingerprint, semantic.bytes,
           sizeof(identity.semantic_fingerprint));
    memcpy(identity.target_profile_fingerprint, profile.bytes,
           sizeof(identity.target_profile_fingerprint));
    memcpy(identity.target_plan_fingerprint, target.bytes,
           sizeof(identity.target_plan_fingerprint));
    identity.generation_fingerprint[0] = generation_marker;
    return identity;
}

static void test_execution_context_lifecycle(void) {
    TypedFrameFixture fixture = make_executable_fixture();
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = create_frame(&fixture, &limits);
    XrTypedFrameContext context;
    REQUIRE(xr_typed_frame_context(frame, &context) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_fingerprint_equal(context.function_identity.plan_fingerprint,
                                 xr_target_plan_fingerprint(fixture.plan)));
    REQUIRE(context.function_identity.function == 0);
    REQUIRE(context.function_identity.semantic_function == 0);
    REQUIRE(context.block_entry_instruction == XR_TYPED_FRAME_CONTEXT_INDEX_NONE);
    REQUIRE(context.instruction == XR_TYPED_FRAME_CONTEXT_INDEX_NONE);
    REQUIRE(context.coroutine_state == XR_TYPED_FRAME_CONTEXT_INDEX_NONE);
    REQUIRE(!context.generation_bound && !context.has_parent && !context.has_child);

    uint32_t row_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(fixture.plan, 0, &row_count);
    REQUIRE(rows != NULL && row_count >= 3);
    REQUIRE(xr_typed_frame_enter_instruction(frame, rows[1].id) ==
            XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID);
    REQUIRE(xr_typed_frame_enter_instruction(frame, rows[0].id) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_enter_instruction(frame, rows[0].id) ==
            XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID);
    for (uint32_t i = 1; i < row_count; i++)
        REQUIRE(xr_typed_frame_enter_instruction(frame, rows[i].id) ==
                XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_context(frame, &context) == XR_TYPED_FRAME_OK);
    REQUIRE(context.block_entry_instruction == rows[0].id);
    REQUIRE(context.instruction == rows[row_count - 1u].id);
    REQUIRE(xr_typed_frame_enter_instruction(frame, rows[0].id) ==
            XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID);
    REQUIRE(xr_typed_frame_enter_instruction(frame, UINT32_MAX) ==
            XR_TYPED_FRAME_CONTEXT_UNAVAILABLE);
    REQUIRE(xr_typed_frame_bind_coroutine_state(frame, 0) ==
            XR_TYPED_FRAME_CONTEXT_UNAVAILABLE);

    XrModuleGenerationIdentity generation = generation_identity(&fixture, 7, 0xa5);
    REQUIRE(xr_typed_frame_bind_generation_identity(frame, &generation) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_bind_generation_identity(frame, &generation) ==
            XR_TYPED_FRAME_OK);
    XrModuleGenerationIdentity changed = generation;
    changed.generation_number++;
    changed.generation_fingerprint[0]++;
    REQUIRE(xr_typed_frame_bind_generation_identity(frame, &changed) ==
            XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID);
    changed = generation;
    changed.target_plan_fingerprint[0] ^= 1;
    REQUIRE(xr_typed_frame_bind_generation_identity(frame, &changed) ==
            XR_TYPED_FRAME_GENERATION_IDENTITY_MISMATCH);
    REQUIRE(xr_typed_frame_context(frame, &context) == XR_TYPED_FRAME_OK);
    REQUIRE(context.generation_bound && context.generation_number == 7);
    REQUIRE(context.generation_fingerprint.bytes[0] == 0xa5);

    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_OK);
    memset(&context, 0xcc, sizeof(context));
    REQUIRE(xr_typed_frame_context(frame, &context) == XR_TYPED_FRAME_CLEANED);
    REQUIRE(memcmp(&context, &(XrTypedFrameContext) {0}, sizeof(context)) == 0);
    REQUIRE(xr_typed_frame_enter_instruction(frame, rows[0].id) ==
            XR_TYPED_FRAME_CLEANED);
    REQUIRE(xr_typed_frame_bind_generation_identity(frame, &generation) ==
            XR_TYPED_FRAME_CLEANED);
    xr_typed_frame_free(frame);
    dispose_fixture(&fixture);
}

static void test_parent_child_context_links(void) {
    TypedFrameFixture fixture = make_executable_fixture();
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *parent = create_frame(&fixture, &limits);
    XrTypedFrame *child = create_frame(&fixture, &limits);
    XrTypedFrame *other = create_frame(&fixture, &limits);

    REQUIRE(xr_typed_frame_link_child(parent, parent) ==
            XR_TYPED_FRAME_INVALID_ARGUMENT);
    REQUIRE(xr_typed_frame_link_child(parent, child) == XR_TYPED_FRAME_OK);
    XrTypedFrameContext parent_context;
    XrTypedFrameContext child_context;
    REQUIRE(xr_typed_frame_context(parent, &parent_context) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_context(child, &child_context) == XR_TYPED_FRAME_OK);
    REQUIRE(parent_context.has_child && !parent_context.has_parent);
    REQUIRE(child_context.has_parent && !child_context.has_child);
    uint32_t row_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(fixture.plan, 0, &row_count);
    REQUIRE(rows != NULL && row_count != 0);
    REQUIRE(xr_typed_frame_enter_instruction(parent, rows[0].id) ==
            XR_TYPED_FRAME_CHILD_ACTIVE);
    XrModuleGenerationIdentity linked_generation =
        generation_identity(&fixture, 1, 1);
    REQUIRE(xr_typed_frame_bind_generation_identity(parent,
                                                     &linked_generation) ==
            XR_TYPED_FRAME_CALL_LINK_INVALID);
    REQUIRE(xr_typed_frame_link_child(parent, other) ==
            XR_TYPED_FRAME_CALL_LINK_INVALID);
    REQUIRE(xr_typed_frame_link_child(child, parent) ==
            XR_TYPED_FRAME_CALL_LINK_INVALID);
    REQUIRE(xr_typed_frame_unlink_child(parent, other) ==
            XR_TYPED_FRAME_CALL_LINK_INVALID);
    REQUIRE(xr_typed_frame_unlink_child(parent, child) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_link_child(parent, child) == XR_TYPED_FRAME_OK);

    REQUIRE(xr_typed_frame_cleanup(child) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_context(parent, &parent_context) == XR_TYPED_FRAME_OK);
    REQUIRE(!parent_context.has_child);
    REQUIRE(xr_typed_frame_link_child(parent, child) == XR_TYPED_FRAME_CLEANED);
    xr_typed_frame_free(child);

    child = create_frame(&fixture, &limits);
    XrModuleGenerationIdentity first = generation_identity(&fixture, 1, 1);
    XrModuleGenerationIdentity second = generation_identity(&fixture, 2, 2);
    REQUIRE(xr_typed_frame_bind_generation_identity(parent, &first) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_bind_generation_identity(child, &second) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_link_child(parent, child) ==
            XR_TYPED_FRAME_CALL_LINK_INVALID);
    xr_typed_frame_free(child);

    child = create_frame(&fixture, &limits);
    REQUIRE(xr_typed_frame_bind_generation_identity(child, &first) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_link_child(parent, child) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_cleanup(parent) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_context(child, &child_context) == XR_TYPED_FRAME_OK);
    REQUIRE(!child_context.has_parent);
    xr_typed_frame_free(parent);
    xr_typed_frame_free(child);
    xr_typed_frame_free(other);
    dispose_fixture(&fixture);
}

int main(void) {
    test_exact_slot_access_and_states();
    test_plan_identity_shape_and_budgets();
    test_plan_ownership_and_void_binding();
    test_execution_context_lifecycle();
    test_parent_child_context_links();
    puts("typed frame tests passed");
    return 0;
}
