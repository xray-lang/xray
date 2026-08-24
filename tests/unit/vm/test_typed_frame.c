/*
 * test_typed_frame.c - Verified typed slot arena contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/xr_typed_frame.h"
#include "../../../src/vm/xr_typed_lifecycle.h"
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

static bool ensure_fixture_module_identity(XiFunc *root) {
    if (!root)
        return false;
    if (!root->module) {
        root->module = xi_module_new("fixture/typed_frame.xr",
                                     root->name ? root->name : "typed_frame_fixture", root);
        if (!root->module)
            return false;
    }
    return root->module->identity ||
           xi_module_set_identity(root->module,
                                  "memory-module-v1:id=22:typed-frame-fixture-v1");
}

typedef struct TypedFrameFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
} TypedFrameFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_owned_string = {
    .kind = XR_KIND_STRING,
    .id = 22,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 23,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};

typedef struct TypedRepCase {
    uint16_t kind;
    uint16_t register_bits;
    uint32_t size;
    uint16_t alignment;
    uint8_t signedness;
    uint8_t root_kind;
    uint8_t ownership;
    uint8_t null_encoding;
    uint16_t lane_count;
    bool supported;
} TypedRepCase;

#define XR_TEST_POINTER_SIZE ((uint32_t) sizeof(void *))
#define XR_TEST_POINTER_ALIGN ((uint16_t) sizeof(void *))
#define XR_TEST_POINTER_BITS ((uint16_t) (sizeof(void *) * 8u))

static const TypedRepCase typed_rep_cases[] = {
    {XR_MACHINE_REP_I1, 1, 1, 1, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_I8, 8, 1, 1, XR_TARGET_SIGN_SIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_U8, 8, 1, 1, XR_TARGET_SIGN_UNSIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_I16, 16, 2, 2, XR_TARGET_SIGN_SIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_U16, 16, 2, 2, XR_TARGET_SIGN_UNSIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_I32, 32, 4, 4, XR_TARGET_SIGN_SIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_U32, 32, 4, 4, XR_TARGET_SIGN_UNSIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_I64, 64, 8, 8, XR_TARGET_SIGN_SIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_U64, 64, 8, 8, XR_TARGET_SIGN_UNSIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_ISIZE, XR_TEST_POINTER_BITS, XR_TEST_POINTER_SIZE,
     XR_TEST_POINTER_ALIGN, XR_TARGET_SIGN_SIGNED, XR_TARGET_ROOT_NONE,
     XR_TARGET_OWNERSHIP_TRIVIAL, XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_USIZE, XR_TEST_POINTER_BITS, XR_TEST_POINTER_SIZE,
     XR_TEST_POINTER_ALIGN, XR_TARGET_SIGN_UNSIGNED, XR_TARGET_ROOT_NONE,
     XR_TARGET_OWNERSHIP_TRIVIAL, XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_F32, 32, 4, 4, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_F64, 64, 8, 8, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_RUNE, 32, 4, 4, XR_TARGET_SIGN_UNSIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_ENUM_ORDINAL, 64, 8, 8, XR_TARGET_SIGN_SIGNED,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_AGGREGATE, 96, 12, 4, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 0, true},
    {XR_MACHINE_REP_VIEW, 128, 16, 8, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_VIEW_OWNER, XR_TARGET_OWNERSHIP_BORROWED,
     XR_TARGET_NULL_NOT_NULLABLE, 0, false},
    {XR_MACHINE_REP_OBJECT_REF, XR_TEST_POINTER_BITS, XR_TEST_POINTER_SIZE,
     XR_TEST_POINTER_ALIGN, XR_TARGET_SIGN_NONE, XR_TARGET_ROOT_OBJECT,
     XR_TARGET_OWNERSHIP_BORROWED, XR_TARGET_NULL_ZERO, 0, false},
    {XR_MACHINE_REP_RAW_PTR, XR_TEST_POINTER_BITS, XR_TEST_POINTER_SIZE,
     XR_TEST_POINTER_ALIGN, XR_TARGET_SIGN_NONE, XR_TARGET_ROOT_NONE,
     XR_TARGET_OWNERSHIP_TRIVIAL, XR_TARGET_NULL_ZERO, 0, true},
    {XR_MACHINE_REP_RAW_PTR, XR_TEST_POINTER_BITS, XR_TEST_POINTER_SIZE,
     XR_TEST_POINTER_ALIGN, XR_TARGET_SIGN_NONE, XR_TARGET_ROOT_NONE,
     XR_TARGET_OWNERSHIP_BORROWED, XR_TARGET_NULL_ZERO, 0, false},
    {XR_MACHINE_REP_CODE_REF, XR_TEST_POINTER_BITS, XR_TEST_POINTER_SIZE,
     XR_TEST_POINTER_ALIGN, XR_TARGET_SIGN_NONE, XR_TARGET_ROOT_NONE,
     XR_TARGET_OWNERSHIP_TRIVIAL, XR_TARGET_NULL_ZERO, 0, false},
    {XR_MACHINE_REP_DYN_VALUE, 128, 16, 8, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_DYNAMIC, XR_TARGET_OWNERSHIP_OWNED,
     XR_TARGET_NULL_TAGGED, 0, false},
    {XR_MACHINE_REP_VECTOR, 128, 16, 16, XR_TARGET_SIGN_NONE,
     XR_TARGET_ROOT_NONE, XR_TARGET_OWNERSHIP_TRIVIAL,
     XR_TARGET_NULL_NOT_NULLABLE, 2, false},
};

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
    REQUIRE(ensure_fixture_module_identity(function));
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
    REQUIRE(ensure_fixture_module_identity(function));
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_lifecycle_semantic_plan(void) {
    XiFunc *function = xi_func_new("typed_frame_lifecycle_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    entry->sealed = true;
    XiValue *prefix = xi_param(function, entry, 0, &stub_int);
    REQUIRE(prefix != NULL);
    function->nparams = 1;
    function->params =
        (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = prefix;
    XiValue *left = xi_const_str(function, entry, "typed", &stub_owned_string);
    XiValue *right = xi_const_str(function, entry, "-frame", &stub_owned_string);
    XiValue *text =
        xi_value_new(function, entry, XI_STR_CONCAT, &stub_owned_string, 2);
    XiValue *yield = xi_value_new(function, entry, XI_YIELD, &stub_unit, 0);
    XiValue *length = xi_value_new(function, entry, XI_LEN, &stub_int, 1);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(left && right && text && yield && length && release);
    text->args[0] = left;
    text->args[1] = right;
    length->args[0] = text;
    release->args[0] = text;
    for (int64_t value = 0; value < 8; value++)
        REQUIRE(xi_const_int(function, entry, value, &stub_int) != NULL);
    xi_block_set_return(entry, length);
    function->stage = XI_STAGE_SEMANTIC_LOWERED;
    function->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(function, NULL));
    REQUIRE(function->coro_plan && function->coro_plan->nstates == 1 &&
            function->coro_plan->points[0].nroots == 1 &&
            function->coro_plan->points[0].ndrops == 1);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(ensure_fixture_module_identity(function));
    REQUIRE(xr_semantic_plan_build(function, &semantic, error,
                                   sizeof(error)));
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

static TypedFrameFixture make_native_fixture_from_semantic(
    XrSemanticPlan *semantic) {
    TypedFrameFixture fixture = {.semantic = semantic};
    XrRuntimeTargetAuthority authority;
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) ==
            XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = authority.machine,
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization =
            &authority.object_header_materialization,
        .string_contract = &authority.string_contract,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    char error[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &fixture.profile, error,
                                    sizeof(error)));
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

static TypedFrameFixture make_lifecycle_fixture(void) {
    return make_fixture_from_semantic(build_lifecycle_semantic_plan());
}

static TypedFrameFixture make_native_fixture(void) {
    return make_native_fixture_from_semantic(build_semantic_plan());
}

static TypedFrameFixture make_native_lifecycle_fixture(void) {
    return make_native_fixture_from_semantic(
        build_lifecycle_semantic_plan());
}

static void append_unrelated_lifecycle_partitions(TypedFrameFixture *fixture,
                                                  uint32_t row_count) {
    REQUIRE(fixture && fixture->plan &&
            fixture->plan->functions_count == 1 && row_count != 0);
    XrTargetPlan *plan = fixture->plan;
    uint32_t original_root_count = plan->root_maps_count;
    uint32_t original_cleanup_count = plan->cleanups_count;
    XrTargetFunctionRecord *next_functions =
        (XrTargetFunctionRecord *) xr_realloc(
            plan->functions, 2u * sizeof(*plan->functions));
    XrTargetCleanupRecord *next_cleanups =
        (XrTargetCleanupRecord *) xr_realloc(
            plan->cleanups,
            (size_t) (original_cleanup_count + row_count) *
                sizeof(*plan->cleanups));
    XrTargetRootMapRecord *next_roots =
        (XrTargetRootMapRecord *) xr_realloc(
            plan->root_maps,
            (size_t) (original_root_count + row_count) *
                sizeof(*plan->root_maps));
    REQUIRE(next_functions && next_cleanups && next_roots);
    plan->functions = next_functions;
    plan->cleanups = next_cleanups;
    plan->root_maps = next_roots;
    plan->functions_count = 2;
    plan->functions[1] = (XrTargetFunctionRecord) {
        .id = 1,
        .semantic_function = 1,
        .slot_begin = plan->slots_count,
        .frame_align = 1,
        .root_begin = original_root_count,
        .root_count = row_count,
        .cleanup_begin = original_cleanup_count,
        .cleanup_count = row_count,
        .coroutine_begin = plan->coroutines_count,
    };
    for (uint32_t i = 0; i < row_count; i++) {
        plan->root_maps[original_root_count + i] =
            (XrTargetRootMapRecord) {
                .id = original_root_count + i,
                .function = 1,
                .semantic_operation = i,
                .slot_begin = plan->root_slots_count,
                .flags = XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL |
                         XR_TARGET_ROOT_EXIT,
            };
        plan->cleanups[original_cleanup_count + i] =
            (XrTargetCleanupRecord) {
                .id = original_cleanup_count + i,
                .function = 1,
                .semantic_operation = i,
                .slot = i,
                .action = XR_TARGET_CLEANUP_RELEASE,
            };
    }
    plan->root_maps_count = original_root_count + row_count;
    plan->cleanups_count = original_cleanup_count + row_count;
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(xr_target_plan_fingerprint_is_intact(plan));
}

static void dispose_fixture(TypedFrameFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

/* The production builder does not yet emit every representation kind the
 * runtime schema accepts. This unit-only rebinding isolates the frame's
 * representation transport classifier from builder reachability: the base
 * plan is verified, every selected slot and referenced rep is reshaped
 * consistently, and the immutable content fingerprint is recomputed before
 * frame creation. It is not evidence that the builder emits these kinds. */
static void rebind_fixture_representation(TypedFrameFixture *fixture,
                                          const TypedRepCase *rep_case) {
    REQUIRE(fixture && fixture->plan && rep_case && rep_case->size != 0 &&
            rep_case->alignment != 0);
    XrTargetPlan *plan = fixture->plan;
    XrTargetFunctionRecord *function = &plan->functions[0];
    REQUIRE(function->slot_begin <= plan->slots_count &&
            function->slot_count <= plan->slots_count - function->slot_begin);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < function->slot_count; i++) {
        XrTargetSlotRecord *slot = &plan->slots[function->slot_begin + i];
        REQUIRE(slot->register_rep < plan->machine_reps_count &&
                slot->memory_rep < plan->machine_reps_count &&
                offset <= UINT32_MAX - rep_case->size);
        uint16_t rep_indexes[2] = {slot->register_rep, slot->memory_rep};
        for (uint32_t r = 0; r < 2; r++) {
            XrTargetMachineRepRecord *rep =
                &plan->machine_reps[rep_indexes[r]];
            rep->kind = rep_case->kind;
            rep->register_bits = rep_case->register_bits;
            rep->memory_size = rep_case->size;
            rep->memory_align = rep_case->alignment;
            rep->signedness = rep_case->signedness;
            rep->root_kind = rep_case->root_kind;
            rep->ownership = rep_case->ownership;
            rep->null_encoding = rep_case->null_encoding;
            rep->detail = 0;
            rep->lane_count = rep_case->lane_count;
            rep->reserved = 0;
            memset(rep->legal_conversion_mask, 0,
                   sizeof(rep->legal_conversion_mask));
        }
        slot->offset = offset;
        slot->size = rep_case->size;
        slot->align = rep_case->alignment;
        slot->root_kind = rep_case->root_kind;
        slot->ownership = rep_case->ownership;
        slot->reserved = 0;
        offset += rep_case->size;
    }
    function->frame_size = offset;
    function->frame_align = rep_case->alignment;
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(xr_target_plan_fingerprint_is_intact(plan));
}

static void make_rep_payload(const TypedRepCase *rep_case, uint8_t *payload) {
    REQUIRE(rep_case && payload);
    memset(payload, 0xa7, rep_case->size);
    if (rep_case->kind == XR_MACHINE_REP_RAW_PTR) {
        static uint8_t object;
        void *pointer = &object;
        REQUIRE(rep_case->size == sizeof(pointer));
        memcpy(payload, &pointer, sizeof(pointer));
    } else if (rep_case->kind == XR_MACHINE_REP_F32) {
        uint32_t nan_payload = UINT32_C(0x7fc12345);
        REQUIRE(rep_case->size == sizeof(nan_payload));
        memcpy(payload, &nan_payload, sizeof(nan_payload));
    } else if (rep_case->kind == XR_MACHINE_REP_F64) {
        uint64_t negative_zero = UINT64_C(0x8000000000000000);
        REQUIRE(rep_case->size == sizeof(negative_zero));
        memcpy(payload, &negative_zero, sizeof(negative_zero));
    } else if (rep_case->kind == XR_MACHINE_REP_ENUM_ORDINAL) {
        int64_t ordinal = -1;
        REQUIRE(rep_case->size == sizeof(ordinal));
        memcpy(payload, &ordinal, sizeof(ordinal));
    }
}

static XrTypedFrame *create_frame(const TypedFrameFixture *fixture,
                                  const XrTypedFrameLimits *limits) {
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture->plan);
    XrTypedFrame *frame = NULL;
    XrTypedFrameStatus status = xr_typed_frame_create(
        fixture->plan, &fingerprint, 0, limits, &frame);
    REQUIRE(status == XR_TYPED_FRAME_OK);
    REQUIRE(frame != NULL);
    return frame;
}

static void free_frame(XrTypedFrame **frame) {
    REQUIRE(xr_typed_frame_free(frame) == XR_TYPED_FRAME_OK);
    REQUIRE(frame && *frame == NULL);
}

static void test_exact_representation_transport_matrix(void) {
    for (size_t i = 0;
         i < sizeof(typed_rep_cases) / sizeof(typed_rep_cases[0]); i++) {
        const TypedRepCase *rep_case = &typed_rep_cases[i];
        TypedFrameFixture fixture = make_fixture();
        rebind_fixture_representation(&fixture, rep_case);
        XrTypedFrameLimits limits;
        xr_typed_frame_limits_default(&limits);
        XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
        XrTypedFrame *frame = NULL;
        XrTypedFrameStatus status = xr_typed_frame_create(
            fixture.plan, &fingerprint, 0, &limits, &frame);
        if (!rep_case->supported) {
            REQUIRE(status == XR_TYPED_FRAME_SLOT_INVALID && frame == NULL);
            dispose_fixture(&fixture);
            continue;
        }
        REQUIRE(status == XR_TYPED_FRAME_OK && frame != NULL);
        uint32_t function_count = 0;
        const XrTargetFunctionRecord *functions =
            xr_target_plan_functions(fixture.plan, &function_count);
        REQUIRE(function_count == 1 && functions[0].slot_count != 0);
        XrTypedSlotAccess access = {0};
        REQUIRE(xr_typed_frame_describe_slot(
                    frame, functions[0].slot_begin, &access) ==
                XR_TYPED_FRAME_OK);
        REQUIRE(access.size == rep_case->size &&
                access.alignment == rep_case->alignment);
        uint8_t source[33] = {0};
        uint8_t destination[33];
        REQUIRE(access.size <= 32);
        make_rep_payload(rep_case, source + 1);
        memset(destination, 0xcc, sizeof(destination));
        REQUIRE(xr_typed_frame_store(frame, &access, source + 1,
                                     access.size) == XR_TYPED_FRAME_OK);
        REQUIRE(xr_typed_frame_load(frame, &access, destination + 1,
                                    access.size) == XR_TYPED_FRAME_OK);
        REQUIRE(destination[0] == 0xcc &&
                destination[access.size + 1u] == 0xcc &&
                memcmp(source + 1, destination + 1, access.size) == 0);
        free_frame(&frame);
        dispose_fixture(&fixture);
    }
}

static void require_frozen_arena_rejects_geometry_mutation(
    TypedFrameFixture *fixture, XrTypedFrame *frame,
    const XrTypedSlotAccess *access) {
    uint32_t original_offset = fixture->plan->slots[access->slot].offset;
    uint32_t original_frame_size = fixture->plan->functions[0].frame_size;
    fixture->plan->slots[access->slot].offset =
        UINT32_MAX & ~((uint32_t) access->alignment - 1u);
    fixture->plan->functions[0].frame_size = UINT32_MAX;
    uint8_t loaded[16];
    uint8_t original[16];
    memset(loaded, 0xcc, sizeof(loaded));
    memcpy(original, loaded, sizeof(loaded));
    REQUIRE(xr_typed_frame_load(frame, access, loaded, access->size) ==
            XR_TYPED_FRAME_SLOT_INVALID);
    REQUIRE(memcmp(loaded, original, sizeof(loaded)) == 0);
    fixture->plan->slots[access->slot].offset = original_offset;
    fixture->plan->functions[0].frame_size = original_frame_size;
}

static void require_access_capability_rejects_mutations(
    XrTypedFrame *frame, const XrTypedSlotAccess *access,
    uint8_t *loaded, const uint8_t *payload) {
    REQUIRE(xr_typed_frame_load(frame, NULL, loaded, access->size) ==
            XR_TYPED_FRAME_INVALID_ARGUMENT);
    REQUIRE(xr_typed_frame_load(frame, access, NULL, access->size) ==
            XR_TYPED_FRAME_INVALID_ARGUMENT);
    REQUIRE(xr_typed_frame_store(frame, access, NULL, access->size) ==
            XR_TYPED_FRAME_INVALID_ARGUMENT);
    REQUIRE(xr_typed_frame_load(frame, access, loaded, access->size - 1u) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    REQUIRE(xr_typed_frame_load(frame, access, loaded, access->size + 1u) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    REQUIRE(xr_typed_frame_store(frame, access, payload, SIZE_MAX) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);

    XrTypedSlotAccess mutation = *access;
    mutation.identity.bytes[0] ^= 1;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = *access;
    mutation.size++;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = *access;
    mutation.alignment++;
    REQUIRE(xr_typed_frame_store(frame, &mutation, payload, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = *access;
    mutation.register_rep ^= 1u;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = *access;
    mutation.memory_rep ^= 1u;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = *access;
    mutation.reserved = 1;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_ACCESS_MISMATCH);
    mutation = *access;
    mutation.slot = UINT32_MAX;
    REQUIRE(xr_typed_frame_load(frame, &mutation, loaded, mutation.size) ==
            XR_TYPED_FRAME_SLOT_INVALID);
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
    REQUIRE(footprint.lifecycle_state_metadata_bytes == 0);
    REQUIRE(footprint.total_bytes ==
            footprint.fixed_frame_bytes + footprint.arena_allocation_bytes +
                footprint.alignment_padding_bytes +
                footprint.slot_state_metadata_bytes +
                footprint.lifecycle_state_metadata_bytes);

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

    require_frozen_arena_rejects_geometry_mutation(&fixture, frame, &access);

    fixture.plan->fingerprint.bytes[0] ^= 1;
    XrTypedSlotAccess cleared = access;
    REQUIRE(xr_typed_frame_describe_slot(frame, access.slot, &cleared) ==
            XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH);
    REQUIRE(memcmp(&cleared, &(XrTypedSlotAccess) {0}, sizeof(cleared)) == 0);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH);
    fixture.plan->fingerprint.bytes[0] ^= 1;

    require_access_capability_rejects_mutations(frame, &access, loaded,
                                                payload);

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
    free_frame(&frame);
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
    free_frame(&frame);

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
    free_frame(&frame);
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
    free_frame(&frame);
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
    free_frame(&frame);
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
    free_frame(&child);

    child = create_frame(&fixture, &limits);
    XrModuleGenerationIdentity first = generation_identity(&fixture, 1, 1);
    XrModuleGenerationIdentity second = generation_identity(&fixture, 2, 2);
    REQUIRE(xr_typed_frame_bind_generation_identity(parent, &first) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_bind_generation_identity(child, &second) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_link_child(parent, child) ==
            XR_TYPED_FRAME_CALL_LINK_INVALID);
    free_frame(&child);

    child = create_frame(&fixture, &limits);
    REQUIRE(xr_typed_frame_bind_generation_identity(child, &first) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_link_child(parent, child) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_cleanup(parent) == XR_TYPED_FRAME_CHILD_ACTIVE);
    REQUIRE(xr_typed_frame_context(child, &child_context) == XR_TYPED_FRAME_OK);
    REQUIRE(child_context.has_parent);
    free_frame(&child);
    REQUIRE(xr_typed_frame_context(parent, &parent_context) == XR_TYPED_FRAME_OK);
    REQUIRE(!parent_context.has_child);
    free_frame(&parent);
    free_frame(&other);
    dispose_fixture(&fixture);
}

typedef struct LifecycleCallbackProbe {
    uint8_t expected[32];
    uint32_t expected_size;
    uint32_t root_visits;
    uint32_t cleanup_calls;
    uint8_t cleanup_action;
    XrTypedFrameStatus cleanup_result;
} LifecycleCallbackProbe;

static void visit_lifecycle_root(void *context,
                                 const XrTypedSlotAccess *access,
                                 const void *bytes) {
    LifecycleCallbackProbe *probe = (LifecycleCallbackProbe *) context;
    REQUIRE(probe && access && bytes && access->size == probe->expected_size &&
            memcmp(bytes, probe->expected, access->size) == 0);
    probe->root_visits++;
}

static XrTypedFrameStatus execute_lifecycle_cleanup(
    void *context, uint8_t action, const XrTypedSlotAccess *access,
    void *bytes) {
    LifecycleCallbackProbe *probe = (LifecycleCallbackProbe *) context;
    REQUIRE(probe && access && bytes && access->size == probe->expected_size &&
            memcmp(bytes, probe->expected, access->size) == 0);
    probe->cleanup_calls++;
    probe->cleanup_action = action;
    return probe->cleanup_result;
}

static void test_owned_string_coroutine_lifecycle(void) {
    TypedFrameFixture fixture = make_lifecycle_fixture();
    REQUIRE(fixture.plan->root_maps_count == 1 &&
            fixture.plan->root_slots_count == 1 &&
            fixture.plan->cleanups_count == 2 &&
            fixture.plan->coroutines_count == 1);
    append_unrelated_lifecycle_partitions(&fixture, 8192);
    REQUIRE(fixture.plan->functions[0].cleanup_count == 2 &&
            fixture.plan->functions[0].root_count == 1 &&
            fixture.plan->functions[1].cleanup_count == 8192 &&
            fixture.plan->functions[1].root_count == 8192);
    uint32_t owned_slot = fixture.plan->root_slots[0];
    uint32_t state_operation =
        fixture.plan->coroutines[0].semantic_operation;
    uint32_t normal_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = fixture.plan->functions[0].cleanup_begin;
         i < fixture.plan->functions[0].cleanup_begin +
                 fixture.plan->functions[0].cleanup_count;
         i++)
        if (fixture.plan->cleanups[i].flags == 0)
            normal_operation = fixture.plan->cleanups[i].semantic_operation;
    REQUIRE(normal_operation != XR_SEMANTIC_INDEX_NONE &&
            normal_operation != state_operation &&
            owned_slot > fixture.plan->functions[0].slot_begin &&
            owned_slot + 1u < fixture.plan->functions[0].slot_begin +
                                  fixture.plan->functions[0].slot_count);
    const XrTargetFunctionRecord *function = &fixture.plan->functions[0];
    uint32_t scalar_before = UINT32_MAX;
    uint32_t scalar_after = UINT32_MAX;
    for (uint32_t slot = function->slot_begin;
         slot < function->slot_begin + function->slot_count; slot++) {
        const XrTargetSlotRecord *record = &fixture.plan->slots[slot];
        const XrTargetMachineRepRecord *rep =
            &fixture.plan->machine_reps[record->memory_rep];
        if (rep->kind < XR_MACHINE_REP_I1 || rep->kind > XR_MACHINE_REP_RUNE ||
            rep->root_kind != XR_TARGET_ROOT_NONE ||
            rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
            continue;
        if (slot < owned_slot)
            scalar_before = slot;
        else if (slot > owned_slot && scalar_after == UINT32_MAX)
            scalar_after = slot;
    }
    REQUIRE(scalar_before != UINT32_MAX && scalar_after != UINT32_MAX);

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrTypedFrame *frame = create_frame(&fixture, &limits);
    XrTypedFrameMemoryFootprint lifecycle_footprint = {0};
    REQUIRE(xr_typed_frame_memory_footprint(frame, &lifecycle_footprint) ==
                XR_TYPED_FRAME_OK &&
            lifecycle_footprint.lifecycle_state_metadata_bytes ==
                sizeof(uint32_t) + sizeof(uint8_t));
    XrTypedSlotAccess access = {0};
    REQUIRE(xr_typed_frame_describe_slot(frame, owned_slot, &access) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(access.size <= 32);
    LifecycleCallbackProbe probe = {.expected_size = access.size};
    for (uint32_t i = 0; i < access.size; i++)
        probe.expected[i] = (uint8_t) (0x40u + i);
    REQUIRE(xr_typed_frame_store(frame, &access, probe.expected, access.size) ==
            XR_TYPED_FRAME_OK);
    XrTypedFrame *active_owner = frame;
    REQUIRE(xr_typed_frame_free(&frame) == XR_TYPED_FRAME_LIFECYCLE_ACTIVE &&
            frame == active_owner);
    uint32_t scalar_slots[] = {scalar_before, scalar_after};
    for (uint32_t i = 0; i < 2; i++) {
        XrTypedSlotAccess scalar_access = {0};
        uint8_t scalar_source[32] = {0};
        uint8_t scalar_loaded[32] = {0};
        REQUIRE(xr_typed_frame_describe_slot(frame, scalar_slots[i],
                                             &scalar_access) ==
                    XR_TYPED_FRAME_OK &&
                scalar_access.size <= sizeof(scalar_source));
        memset(scalar_source, (int) (0x20u + i), scalar_access.size);
        REQUIRE(xr_typed_frame_store(frame, &scalar_access, scalar_source,
                                     scalar_access.size) == XR_TYPED_FRAME_OK);
        REQUIRE(xr_typed_frame_load(frame, &scalar_access, scalar_loaded,
                                    scalar_access.size) == XR_TYPED_FRAME_OK &&
                memcmp(scalar_source, scalar_loaded, scalar_access.size) == 0);
    }
    REQUIRE(xr_typed_frame_store(frame, &access, probe.expected, access.size) ==
            XR_TYPED_FRAME_LIFECYCLE_ACTIVE);
    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_LIFECYCLE_ACTIVE);
    REQUIRE(xr_typed_frame_bind_coroutine_state(frame, 0) == XR_TYPED_FRAME_OK);
    uint32_t visited = 0;
    REQUIRE(xr_typed_frame_visit_coroutine_roots(
                frame, 0, visit_lifecycle_root, &probe, &visited) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(visited == 1 && probe.root_visits == 1);
    REQUIRE(xr_typed_frame_resume_coroutine_state(frame, 0) ==
            XR_TYPED_FRAME_OK);
    uint8_t loaded[32] = {0};
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
                XR_TYPED_FRAME_OK &&
            memcmp(loaded, probe.expected, access.size) == 0);
    uint32_t executed = 0;
    probe.cleanup_result = XR_TYPED_FRAME_ALLOCATION_FAILED;
    REQUIRE(xr_typed_frame_execute_cleanups(
                frame, normal_operation, 0, execute_lifecycle_cleanup,
                &probe, &executed) == XR_TYPED_FRAME_ALLOCATION_FAILED);
    REQUIRE(executed == 0 && probe.cleanup_calls == 1 &&
            probe.cleanup_action == XR_TARGET_CLEANUP_RELEASE);
    memset(loaded, 0, sizeof(loaded));
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
                XR_TYPED_FRAME_OK &&
            memcmp(loaded, probe.expected, access.size) == 0);
    REQUIRE(xr_typed_frame_free(&frame) == XR_TYPED_FRAME_LIFECYCLE_ACTIVE &&
            frame == active_owner);
    probe.cleanup_result = XR_TYPED_FRAME_OK;
    REQUIRE(xr_typed_frame_execute_cleanups(
                frame, normal_operation, 0, execute_lifecycle_cleanup,
                &probe, &executed) == XR_TYPED_FRAME_OK);
    REQUIRE(executed == 1 && probe.cleanup_calls == 2 &&
            probe.cleanup_action == XR_TARGET_CLEANUP_RELEASE);
    REQUIRE(xr_typed_frame_execute_cleanups(
                frame, normal_operation, 0, execute_lifecycle_cleanup,
                &probe, &executed) == XR_TYPED_FRAME_LIFECYCLE_INACTIVE);
    REQUIRE(probe.cleanup_calls == 2);
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
            XR_TYPED_FRAME_LIFECYCLE_INACTIVE);
    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_OK);
    free_frame(&frame);

    frame = create_frame(&fixture, &limits);
    memset(&probe, 0, sizeof(probe));
    probe.expected_size = access.size;
    for (uint32_t i = 0; i < access.size; i++)
        probe.expected[i] = (uint8_t) (0x80u + i);
    REQUIRE(xr_typed_frame_describe_slot(frame, owned_slot, &access) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_store(frame, &access, probe.expected, access.size) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_bind_coroutine_state(frame, 0) == XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_frame_execute_cleanups(
                frame, state_operation, XR_TARGET_CLEANUP_CANCEL,
                execute_lifecycle_cleanup, &probe, &executed) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(executed == 1 && probe.cleanup_calls == 1 &&
            probe.cleanup_action == XR_TARGET_CLEANUP_RELEASE);
    XrTypedFrameContext frame_context = {0};
    REQUIRE(xr_typed_frame_context(frame, &frame_context) == XR_TYPED_FRAME_OK &&
            frame_context.terminal);
    REQUIRE(xr_typed_frame_resume_coroutine_state(frame, 0) ==
            XR_TYPED_FRAME_TERMINAL);
    REQUIRE(xr_typed_frame_visit_coroutine_roots(
                frame, 0, visit_lifecycle_root, &probe, &visited) ==
            XR_TYPED_FRAME_TERMINAL);
    REQUIRE(xr_typed_frame_cleanup(frame) == XR_TYPED_FRAME_OK);
    free_frame(&frame);
    dispose_fixture(&fixture);
}

typedef struct VmLifecycleCoordinates {
    uint32_t slot;
    uint32_t state_operation;
    uint32_t normal_operation;
} VmLifecycleCoordinates;

typedef struct VmLifecycleProbe {
    XrString *object;
    uintptr_t object_address;
    bool resolve_enabled;
    uint32_t resolve_calls;
    uint32_t reclaim_calls;
    uint32_t event_count;
    XrTypedLifecycleEvent event;
} VmLifecycleProbe;

static VmLifecycleCoordinates vm_lifecycle_coordinates(
    const TypedFrameFixture *fixture) {
    REQUIRE(fixture && fixture->plan &&
            fixture->plan->functions_count == 1 &&
            fixture->plan->root_maps_count == 1 &&
            fixture->plan->root_slots_count == 1 &&
            fixture->plan->cleanups_count == 2 &&
            fixture->plan->coroutines_count == 1);
    VmLifecycleCoordinates coordinates = {
        .slot = fixture->plan->root_slots[0],
        .state_operation =
            fixture->plan->coroutines[0].semantic_operation,
        .normal_operation = XR_SEMANTIC_INDEX_NONE,
    };
    for (uint32_t i = 0; i < fixture->plan->cleanups_count; i++)
        if (fixture->plan->cleanups[i].flags == 0)
            coordinates.normal_operation =
                fixture->plan->cleanups[i].semantic_operation;
    REQUIRE(coordinates.normal_operation != XR_SEMANTIC_INDEX_NONE &&
            coordinates.normal_operation != coordinates.state_operation);
    return coordinates;
}

static void vm_lifecycle_probe_allocate(VmLifecycleProbe *probe,
                                        bool retain_once) {
    static const char payload[] = "concat";
    REQUIRE(probe && !probe->object);
    uint64_t allocation_bytes =
        xr_runtime_string_object_allocation_bytes(sizeof(payload) - 1u);
    REQUIRE(allocation_bytes <= SIZE_MAX);
    XrString *object = (XrString *) xr_malloc((size_t) allocation_bytes);
    REQUIRE(object != NULL);
    memset(object, 0, (size_t) allocation_bytes);
    REQUIRE(xr_runtime_string_object_init(
                object, XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL,
                sizeof(payload) - 1u, sizeof(payload) - 1u, 0,
                XR_RUNTIME_STRING_TRAIT_LOCAL) == XR_RUNTIME_ABI_OK);
    memcpy(object->data, payload, sizeof(payload));
    if (retain_once)
        REQUIRE(xr_runtime_object_header_retain(&object->header) ==
                XR_RUNTIME_ABI_OK);
    probe->object = object;
    probe->object_address = (uintptr_t) object;
}

static XrRuntimeObjectHeader *vm_lifecycle_resolve_object(
    void *context, uintptr_t address) {
    VmLifecycleProbe *probe = (VmLifecycleProbe *) context;
    REQUIRE(probe != NULL);
    probe->resolve_calls++;
    return probe->resolve_enabled && probe->object &&
                   address == probe->object_address
               ? &probe->object->header
               : NULL;
}

static void vm_lifecycle_reclaim_object(
    void *context, XrRuntimeObjectHeader *header) {
    VmLifecycleProbe *probe = (VmLifecycleProbe *) context;
    REQUIRE(probe && probe->object && header == &probe->object->header);
    XrString *object = probe->object;
    probe->object = NULL;
    probe->reclaim_calls++;
    xr_free(object);
}

static XrTypedLifecycleStatus vm_lifecycle_observe(
    void *context, const XrTypedLifecycleEvent *event) {
    VmLifecycleProbe *probe = (VmLifecycleProbe *) context;
    REQUIRE(probe && event && probe->event_count == 0);
    if (event->physical_last_release)
        REQUIRE(probe->object == NULL && probe->reclaim_calls == 1);
    else
        REQUIRE(probe->object != NULL && probe->reclaim_calls == 0);
    probe->event = *event;
    probe->event_count++;
    return XR_TYPED_LIFECYCLE_OK;
}

static XrTypedLifecycleBindings vm_lifecycle_bindings(
    VmLifecycleProbe *probe, bool observe) {
    return (XrTypedLifecycleBindings) {
        .resolve_object = vm_lifecycle_resolve_object,
        .reclaim_object = vm_lifecycle_reclaim_object,
        .allocation_context = probe,
        .observer = observe ? vm_lifecycle_observe : NULL,
        .observer_context = probe,
    };
}

static void vm_lifecycle_write_field(
    uint8_t *carrier, size_t carrier_size,
    const XrRuntimePhysicalFieldAbi *field, uint64_t value) {
    REQUIRE(carrier && field && field->width != 0 &&
            field->width <= sizeof(value) && field->offset <= carrier_size &&
            field->width <= carrier_size - field->offset);
    memcpy(carrier + field->offset, &value, field->width);
}

static void vm_lifecycle_make_carrier(
    XrTypedLifecycleContext *context, uint8_t *carrier,
    size_t carrier_capacity) {
    REQUIRE(context && carrier &&
            context->dynamic_value.size <= carrier_capacity);
    const XrRuntimeDynamicTagAbiEntry *tag = NULL;
    for (uint32_t i = 0; i < context->dynamic_value.tag_count; i++)
        if (context->dynamic_value.tags[i].encoding ==
            context->dynamic_value.object_reference_tag)
            tag = &context->dynamic_value.tags[i];
    REQUIRE(tag && tag->payload_kind ==
                       XR_RUNTIME_DYN_PAYLOAD_OBJECT_REFERENCE);
    memset(carrier, 0, carrier_capacity);
    vm_lifecycle_write_field(
        carrier, carrier_capacity, &context->dynamic_value.fields[0],
        context->dynamic_value.object_reference_tag);
    vm_lifecycle_write_field(
        carrier, carrier_capacity, &context->dynamic_value.fields[1],
        tag->required_flags);
    vm_lifecycle_write_field(
        carrier, carrier_capacity, &context->dynamic_value.fields[2],
        (uint64_t) ((VmLifecycleProbe *) context->allocation_context)
            ->object_address);
}

static XrTypedSlotAccess vm_lifecycle_store_owner(
    XrTypedLifecycleContext *context, XrTypedFrame *frame, uint32_t slot,
    uint8_t *carrier, size_t carrier_capacity) {
    REQUIRE(context && frame && carrier);
    XrTypedSlotAccess access = {0};
    REQUIRE(xr_typed_frame_describe_slot(frame, slot, &access) ==
                XR_TYPED_FRAME_OK &&
            access.size == context->dynamic_value.size);
    vm_lifecycle_make_carrier(context, carrier, carrier_capacity);
    REQUIRE(xr_typed_frame_store(frame, &access, carrier, access.size) ==
            XR_TYPED_FRAME_OK);
    return access;
}

static XrTypedFrameStatus vm_lifecycle_discard_owner(
    void *context, uint8_t action, const XrTypedSlotAccess *access,
    void *bytes) {
    (void) context;
    REQUIRE(action == XR_TARGET_CLEANUP_RELEASE && access && bytes);
    return XR_TYPED_FRAME_OK;
}

static void vm_lifecycle_finish_frame(XrTypedFrame **frame) {
    REQUIRE(frame && *frame &&
            xr_typed_frame_cleanup(*frame) == XR_TYPED_FRAME_OK);
    free_frame(frame);
}

static void test_typed_lifecycle_all_exits_and_exact_once(void) {
    TypedFrameFixture fixture = make_native_lifecycle_fixture();
    VmLifecycleCoordinates coordinates =
        vm_lifecycle_coordinates(&fixture);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
    VmLifecycleProbe probe = {.resolve_enabled = true};
    XrTypedLifecycleBindings bindings =
        vm_lifecycle_bindings(&probe, true);
    XrTypedLifecycleContext context = {0};
    XrTypedLifecycleStatus init_status = xr_typed_lifecycle_context_init(
        fixture.plan, &fingerprint, 0, &bindings, &context);
    REQUIRE(init_status == XR_TYPED_LIFECYCLE_OK);

    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    for (uint32_t exit = XR_TYPED_LIFECYCLE_EXIT_NORMAL;
         exit < XR_TYPED_LIFECYCLE_EXIT_COUNT; exit++) {
        memset(&probe, 0, sizeof(probe));
        probe.resolve_enabled = true;
        vm_lifecycle_probe_allocate(&probe, false);
        XrTypedFrame *frame = create_frame(&fixture, &limits);
        uint8_t carrier[32] = {0};
        XrTypedSlotAccess access = vm_lifecycle_store_owner(
            &context, frame, coordinates.slot, carrier,
            sizeof(carrier));
        uint32_t operation = coordinates.normal_operation;
        if (exit != XR_TYPED_LIFECYCLE_EXIT_NORMAL) {
            operation = coordinates.state_operation;
            REQUIRE(xr_typed_frame_bind_coroutine_state(frame, 0) ==
                    XR_TYPED_FRAME_OK);
        }
        uint32_t executed = 0;
        REQUIRE(xr_typed_lifecycle_execute(
                    &context, frame, operation,
                    (XrTypedLifecycleExit) exit, &executed) ==
                    XR_TYPED_LIFECYCLE_OK &&
                executed == 1 && probe.resolve_calls == 1 &&
                probe.reclaim_calls == 1 && probe.event_count == 1 &&
                probe.object == NULL);
        REQUIRE(xr_stable_id_equal(probe.event.slot_identity,
                                   access.identity) &&
                probe.event.function == 0 &&
                probe.event.semantic_operation == operation &&
                probe.event.slot == coordinates.slot &&
                probe.event.physical_rc_before ==
                    XR_RUNTIME_OBJECT_RC_INITIAL &&
                probe.event.physical_rc_after == 0 &&
                probe.event.action == XR_TARGET_CLEANUP_RELEASE &&
                probe.event.exit_kind == exit &&
                probe.event.physical_last_release == 1);
        executed = UINT32_MAX;
        REQUIRE(xr_typed_lifecycle_execute(
                    &context, frame, operation,
                    (XrTypedLifecycleExit) exit, &executed) ==
                    XR_TYPED_LIFECYCLE_ALREADY_EXECUTED &&
                executed == 0 && probe.resolve_calls == 1 &&
                probe.reclaim_calls == 1 && probe.event_count == 1);
        vm_lifecycle_finish_frame(&frame);
    }

    memset(&probe, 0, sizeof(probe));
    probe.resolve_enabled = true;
    vm_lifecycle_probe_allocate(&probe, true);
    XrTypedFrame *frame = create_frame(&fixture, &limits);
    uint8_t carrier[32] = {0};
    vm_lifecycle_store_owner(&context, frame, coordinates.slot,
                             carrier, sizeof(carrier));
    uint32_t executed = 0;
    REQUIRE(xr_typed_lifecycle_execute(
                &context, frame, coordinates.normal_operation,
                XR_TYPED_LIFECYCLE_EXIT_NORMAL, &executed) ==
                XR_TYPED_LIFECYCLE_OK &&
            executed == 1 && probe.object && probe.reclaim_calls == 0 &&
            probe.event_count == 1 &&
            probe.event.physical_rc_before == 2 &&
            probe.event.physical_rc_after == 1 &&
            probe.event.physical_last_release == 0);
    REQUIRE(atomic_load_explicit(&probe.object->header.rc,
                                 memory_order_acquire) == 1);
    REQUIRE(xr_typed_lifecycle_execute(
                &context, frame, coordinates.normal_operation,
                XR_TYPED_LIFECYCLE_EXIT_NORMAL, &executed) ==
                XR_TYPED_LIFECYCLE_ALREADY_EXECUTED &&
            executed == 0 && probe.event_count == 1 &&
            atomic_load_explicit(&probe.object->header.rc,
                                 memory_order_acquire) == 1);
    vm_lifecycle_finish_frame(&frame);
    bool last = false;
    REQUIRE(xr_runtime_object_header_release(&probe.object->header, &last) ==
                XR_RUNTIME_ABI_OK &&
            last);
    vm_lifecycle_reclaim_object(&probe, &probe.object->header);

    xr_typed_lifecycle_context_dispose(&context);
    dispose_fixture(&fixture);
}

static void test_typed_lifecycle_failure_retry_and_defenses(void) {
    TypedFrameFixture fixture = make_native_lifecycle_fixture();
    VmLifecycleCoordinates coordinates =
        vm_lifecycle_coordinates(&fixture);
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
    VmLifecycleProbe probe = {0};
    XrTypedLifecycleBindings bindings =
        vm_lifecycle_bindings(&probe, true);
    XrTypedLifecycleContext context = {0};
    REQUIRE(xr_typed_lifecycle_context_init(
                fixture.plan, &fingerprint, 0, &bindings, &context) ==
            XR_TYPED_LIFECYCLE_OK);
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    vm_lifecycle_probe_allocate(&probe, false);
    XrTypedFrame *frame = create_frame(&fixture, &limits);
    uint8_t carrier[32] = {0};
    XrTypedSlotAccess access = vm_lifecycle_store_owner(
        &context, frame, coordinates.slot, carrier, sizeof(carrier));
    uint32_t executed = UINT32_MAX;
    REQUIRE(xr_typed_lifecycle_execute(
                &context, frame, coordinates.state_operation,
                XR_TYPED_LIFECYCLE_EXIT_NORMAL, &executed) ==
                XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE &&
            executed == 0 && probe.resolve_calls == 0 &&
            probe.reclaim_calls == 0 && probe.event_count == 0);
    REQUIRE(xr_typed_lifecycle_execute(
                &context, frame, coordinates.normal_operation,
                XR_TYPED_LIFECYCLE_EXIT_NORMAL, &executed) ==
                XR_TYPED_LIFECYCLE_CARRIER_INVALID &&
            executed == 0 && probe.resolve_calls == 1 &&
            probe.reclaim_calls == 0 && probe.event_count == 0 &&
            atomic_load_explicit(&probe.object->header.rc,
                                 memory_order_acquire) == 1);
    uint8_t loaded[32] = {0};
    REQUIRE(xr_typed_frame_load(frame, &access, loaded, access.size) ==
                XR_TYPED_FRAME_OK &&
            memcmp(loaded, carrier, access.size) == 0);
    XrTypedFrame *active_frame = frame;
    REQUIRE(xr_typed_frame_free(&frame) ==
                XR_TYPED_FRAME_LIFECYCLE_ACTIVE &&
            frame == active_frame);
    probe.resolve_enabled = true;
    REQUIRE(xr_typed_lifecycle_execute(
                &context, frame, coordinates.normal_operation,
                XR_TYPED_LIFECYCLE_EXIT_NORMAL, &executed) ==
                XR_TYPED_LIFECYCLE_OK &&
            executed == 1 && probe.resolve_calls == 2 &&
            probe.reclaim_calls == 1 && probe.event_count == 1 &&
            probe.object == NULL);
    vm_lifecycle_finish_frame(&frame);

    memset(&probe, 0, sizeof(probe));
    probe.resolve_enabled = true;
    vm_lifecycle_probe_allocate(&probe, false);
    frame = create_frame(&fixture, &limits);
    REQUIRE(xr_typed_frame_describe_slot(frame, coordinates.slot, &access) ==
                XR_TYPED_FRAME_OK &&
            access.size == context.dynamic_value.size);
    vm_lifecycle_make_carrier(&context, carrier, sizeof(carrier));
    bool covered[32] = {false};
    REQUIRE(context.dynamic_value.size <= sizeof(covered));
    for (uint32_t field = 0; field < XR_RUNTIME_DYNAMIC_FIELD_COUNT;
         field++) {
        const XrRuntimePhysicalFieldAbi *record =
            &context.dynamic_value.fields[field];
        for (uint32_t i = 0; i < record->width; i++)
            covered[record->offset + i] = true;
    }
    uint32_t padding = UINT32_MAX;
    for (uint32_t i = 0; i < context.dynamic_value.size; i++)
        if (!covered[i] && padding == UINT32_MAX)
            padding = i;
    REQUIRE(padding != UINT32_MAX);
    carrier[padding] = 1;
    REQUIRE(xr_typed_frame_store(frame, &access, carrier, access.size) ==
            XR_TYPED_FRAME_OK);
    REQUIRE(xr_typed_lifecycle_execute(
                &context, frame, coordinates.normal_operation,
                XR_TYPED_LIFECYCLE_EXIT_NORMAL, &executed) ==
                XR_TYPED_LIFECYCLE_CARRIER_INVALID &&
            executed == 0 && probe.reclaim_calls == 0 &&
            probe.event_count == 0);
    REQUIRE(xr_typed_frame_execute_cleanups(
                frame, coordinates.normal_operation, 0,
                vm_lifecycle_discard_owner, NULL, &executed) ==
                XR_TYPED_FRAME_OK &&
            executed == 1);
    vm_lifecycle_finish_frame(&frame);
    bool last = false;
    REQUIRE(xr_runtime_object_header_release(&probe.object->header, &last) ==
                XR_RUNTIME_ABI_OK &&
            last);
    vm_lifecycle_reclaim_object(&probe, &probe.object->header);

    xr_typed_lifecycle_context_dispose(&context);
    dispose_fixture(&fixture);

    TypedFrameFixture duplicate = make_native_lifecycle_fixture();
    coordinates = vm_lifecycle_coordinates(&duplicate);
    for (uint32_t i = 0; i < duplicate.plan->cleanups_count; i++)
        if (duplicate.plan->cleanups[i].flags == 0) {
            duplicate.plan->cleanups[i].flags =
                XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT;
            duplicate.plan->cleanups[i].semantic_operation =
                coordinates.state_operation;
        }
    xr_target_plan_compute_fingerprint(duplicate.plan,
                                       &duplicate.plan->fingerprint);
    fingerprint = xr_target_plan_fingerprint(duplicate.plan);
    memset(&probe, 0, sizeof(probe));
    bindings.observer = NULL;
    REQUIRE(xr_typed_lifecycle_context_init(
                duplicate.plan, &fingerprint, 0, &bindings, &context) ==
            XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE);
    dispose_fixture(&duplicate);

    TypedFrameFixture scalar = make_native_fixture();
    fingerprint = xr_target_plan_fingerprint(scalar.plan);
    REQUIRE(xr_typed_lifecycle_context_init(
                scalar.plan, &fingerprint, 0, &bindings, &context) ==
            XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE);
    dispose_fixture(&scalar);

    TypedFrameFixture wrong_profile = make_lifecycle_fixture();
    fingerprint = xr_target_plan_fingerprint(wrong_profile.plan);
    REQUIRE(xr_typed_lifecycle_context_init(
                wrong_profile.plan, &fingerprint, 0, &bindings, &context) ==
            XR_TYPED_LIFECYCLE_TARGET_PROFILE_MISMATCH);
    dispose_fixture(&wrong_profile);

    TypedFrameFixture wrong_slot = make_native_lifecycle_fixture();
    coordinates = vm_lifecycle_coordinates(&wrong_slot);
    REQUIRE(wrong_slot.plan->functions[0].slot_count > 1);
    uint32_t alternate_slot = wrong_slot.plan->functions[0].slot_begin;
    if (alternate_slot == coordinates.slot)
        alternate_slot++;
    REQUIRE(alternate_slot != coordinates.slot &&
            alternate_slot < wrong_slot.plan->slots_count);
    wrong_slot.plan->root_slots[0] = alternate_slot;
    xr_target_plan_compute_fingerprint(wrong_slot.plan,
                                       &wrong_slot.plan->fingerprint);
    fingerprint = xr_target_plan_fingerprint(wrong_slot.plan);
    REQUIRE(xr_typed_lifecycle_context_init(
                wrong_slot.plan, &fingerprint, 0, &bindings, &context) ==
            XR_TYPED_LIFECYCLE_CONTRACT_UNAVAILABLE);
    dispose_fixture(&wrong_slot);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "coroutine-lifecycle") == 0) {
        test_owned_string_coroutine_lifecycle();
        puts("typed frame coroutine lifecycle tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "vm-lifecycle") == 0) {
        test_typed_lifecycle_all_exits_and_exact_once();
        test_typed_lifecycle_failure_retry_and_defenses();
        puts("typed VM lifecycle tests passed");
        return 0;
    }
    test_exact_representation_transport_matrix();
    test_exact_slot_access_and_states();
    test_plan_identity_shape_and_budgets();
    test_plan_ownership_and_void_binding();
    test_execution_context_lifecycle();
    test_parent_child_context_links();
    test_owned_string_coroutine_lifecycle();
    test_typed_lifecycle_all_exits_and_exact_once();
    test_typed_lifecycle_failure_retry_and_defenses();
    puts("typed frame tests passed");
    return 0;
}
