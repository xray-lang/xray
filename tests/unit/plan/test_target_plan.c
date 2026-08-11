/*
 * test_target_plan.c - Immutable backend-neutral TargetPlan contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/plan/target/xr_target_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "target_profile_test_fixture.h"
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

typedef struct TargetFixture {
    XrTargetMachineRepRecord reps[7];
    XrTargetValueRepRecord value_reps[3];
    XrTargetExtentRecord extents[1];
    XrTargetLayoutRecord layouts[2];
    XrTargetFieldRecord fields[1];
    XrTargetStorageRecord storage[1];
    XrTargetAllocationRecord allocations[1];
    XrTargetFunctionRecord functions[1];
    XrTargetSlotRecord slots[3];
    XrTargetCallRecord calls[1];
    XrTargetCallArgumentRecord call_arguments[1];
    XrTargetRootMapRecord roots[1];
    uint32_t root_slots[1];
    XrTargetCleanupRecord cleanups[1];
    XrTargetAdapterRecord adapters[1];
    XrTargetCapabilityRecord capabilities[1];
    XrTargetCoroutineStateRecord coroutines[1];
    XrTargetPlanDraft draft;
} TargetFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_string = {.kind = XR_KIND_STRING, .id = 2, .frozen = true};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 3,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_bool = {
    .kind = XR_KIND_BOOL,
    .id = 4,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_nullable_int = {
    .kind = XR_KIND_INT,
    .id = 5,
    .frozen = true,
    .is_nullable = true,
};

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("target_plan_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    XiValue *string = xi_const_str(function, entry, "target", &stub_string);
    REQUIRE(string != NULL);
    XiValue *release = xi_value_new(function, entry, XI_RELEASE, &stub_unit, 1);
    REQUIRE(release != NULL);
    release->args[0] = string;
    REQUIRE(xi_const_int(function, entry, 7, &stub_int) != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "semantic fixture failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(xr_semantic_plan_function_count(plan) == 1);
    REQUIRE(xr_semantic_plan_type_count(plan) >= 2);
    REQUIRE(xr_semantic_plan_operation_count(plan) >= 1);
    xi_func_free(function);
    return plan;
}

static XrTargetProfile *build_profile(uint64_t extra_atomic_width) {
    XrTestTargetProfileFixture fixture;
    REQUIRE(xr_test_target_profile_fixture_init(
        &fixture, false, XR_TARGET_RUNTIME_PROFILE_HOSTED));
    fixture.input.machine.atomic_width_mask |= extra_atomic_width;
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    bool built = xr_target_profile_build(&fixture.input, &profile, error,
                                         sizeof(error));
    if (!built)
        fprintf(stderr, "target profile failed: %s\n", error);
    REQUIRE(built && profile != NULL);
    return profile;
}

static uint32_t operation_for_value(const XrSemanticPlan *semantic, uint32_t value) {
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation && operation->result_value == value)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static const XrSemanticParameterRecord *parameter_for_value(const XrSemanticPlan *semantic,
                                                            uint32_t function,
                                                            uint32_t value) {
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(semantic);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(semantic, i);
        if (parameter && parameter->function == function && parameter->value == value)
            return parameter;
    }
    return NULL;
}

static void fill_slot_source(const XrSemanticPlan *semantic, XrTargetSlotRecord *slot,
                             uint32_t function, uint32_t value) {
    uint32_t operation_index = operation_for_value(semantic, value);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticParameterRecord *parameter =
        parameter_for_value(semantic, function, value);
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(semantic, function);
    REQUIRE(operation && semantic_function && operation->function == function);
    XrStableId source = operation->id;
    slot->role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                             : XR_TARGET_SLOT_TEMPORARY;
    if (parameter) {
        REQUIRE(operation->opcode == XI_PARAM);
        source = parameter->id;
        slot->role = XR_TARGET_SLOT_PARAMETER;
    }
    slot->semantic_value = value;
    slot->semantic_operation = operation_index;
    slot->logical_slot = XR_SEMANTIC_INDEX_NONE;
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char source_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(semantic_function->id, function_id);
    xr_stable_id_hex(source, source_id);
    int written = snprintf(key, sizeof(key),
                           "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                           function_id, (unsigned) slot->role, source_id,
                           XR_SEMANTIC_INDEX_NONE);
    XrFingerprint digest;
    REQUIRE(written > 0 && (size_t) written < sizeof(key));
    REQUIRE(xr_stable_id_from_key(key, &slot->identity, &digest));
}

static XrSemanticPlan *build_single_scalar_semantic(XrType *type) {
    XiFunc *function = xi_func_new("target_scalar_probe", type);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = type->kind == XR_KIND_BOOL ? xi_const_bool(function, entry, true, type)
                                                  : xi_const_int(function, entry, 1, type);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    xi_func_free(function);
    return plan;
}

static bool freeze_single_scalar(XrSemanticPlan *semantic, XrTargetProfile *profile,
                                 bool bind_value, bool boolean_rep, XrTargetPlan **out,
                                 char *error, size_t error_size) {
    const XrSemanticFunctionRecord *semantic_function = xr_semantic_plan_function(semantic, 0);
    REQUIRE(semantic_function != NULL && semantic_function->value_count == 1);
    uint32_t semantic_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        if ((boolean_rep && type->kind == XR_KIND_BOOL) ||
            (!boolean_rep && type->kind == XR_KIND_INT))
            semantic_type = i;
    }
    REQUIRE(semantic_type != XR_SEMANTIC_INDEX_NONE);
    XrTargetMachineRepRecord reps[2] = {
        {.id = 0, .kind = XR_MACHINE_REP_VOID},
        {
            .id = 1,
            .kind = boolean_rep ? XR_MACHINE_REP_I1 : XR_MACHINE_REP_I64,
            .register_bits = boolean_rep ? 1u : 64u,
            .memory_size = boolean_rep ? 1u : 8u,
            .memory_align = boolean_rep ? 1u : 8u,
            .signedness = boolean_rep ? XR_TARGET_SIGN_NONE : XR_TARGET_SIGN_SIGNED,
            .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        },
    };
    XrTargetValueRepRecord value = {
        .semantic_value = semantic_function->value_begin,
        .register_rep = 1,
        .memory_rep = 1,
        .slot = 0,
    };
    XrTargetExtentRecord extent = {
        .id = 0,
        .kind = XR_TARGET_EXTENT_FIXED,
        .element_layout = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetLayoutRecord layout = {
        .id = 0,
        .semantic_type = semantic_type,
        .kind = XR_TARGET_LAYOUT_SCALAR,
        .align = boolean_rep ? 1u : 8u,
        .fixed_prefix_size = boolean_rep ? 1u : 8u,
        .extent = 0,
    };
    XrTargetFunctionRecord function = {
        .id = 0,
        .semantic_function = 0,
        .slot_count = bind_value ? 1u : 0u,
        .frame_size = bind_value ? (boolean_rep ? 1u : 8u) : 0u,
        .frame_align = bind_value ? (boolean_rep ? 1u : 8u) : 1u,
    };
    XrTargetSlotRecord slot = {
        .id = 0,
        .function = 0,
        .size = boolean_rep ? 1u : 8u,
        .align = boolean_rep ? 1u : 8u,
        .register_rep = 1,
        .memory_rep = 1,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    if (bind_value)
        fill_slot_source(semantic, &slot, 0, semantic_function->value_begin);
    XrTargetPlanDraft draft = {
        .semantic_plan = semantic,
        .profile = profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
        .machine_reps = reps,
        .machine_reps_count = 2,
        .value_reps = bind_value ? &value : NULL,
        .value_reps_count = bind_value ? 1u : 0u,
        .extents = bind_value ? &extent : NULL,
        .extents_count = bind_value ? 1u : 0u,
        .layouts = bind_value ? &layout : NULL,
        .layouts_count = bind_value ? 1u : 0u,
        .functions = &function,
        .functions_count = 1,
        .slots = bind_value ? &slot : NULL,
        .slots_count = bind_value ? 1u : 0u,
    };
    return xr_target_plan_freeze(&draft, out, error, error_size);
}

static void fill_representation_fixture(TargetFixture *fixture, XrSemanticPlan *semantic,
                                        uint32_t *out_int_layout,
                                        uint32_t *out_string_layout) {
    uint32_t int_type = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(semantic); i++) {
        const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, i);
        if (type->kind == XR_KIND_INT)
            int_type = i;
    }
    REQUIRE(int_type != XR_SEMANTIC_INDEX_NONE);
    fixture->reps[0] = (XrTargetMachineRepRecord) {
        .id = 0,
        .kind = XR_MACHINE_REP_VOID,
    };
    fixture->reps[1] = (XrTargetMachineRepRecord) {
        .id = 1,
        .kind = XR_MACHINE_REP_I64,
        .register_bits = 64,
        .memory_size = 8,
        .memory_align = 8,
        .signedness = XR_TARGET_SIGN_SIGNED,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .legal_conversion_mask = {UINT64_C(1) << 2},
    };
    fixture->reps[2] = (XrTargetMachineRepRecord) {
        .id = 2,
        .kind = XR_MACHINE_REP_I64,
        .register_bits = 64,
        .memory_size = 8,
        .memory_align = 8,
        .signedness = XR_TARGET_SIGN_SIGNED,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .legal_conversion_mask = {UINT64_C(1) << 1},
    };
    fixture->value_reps[0] = (XrTargetValueRepRecord) {
        .semantic_value = 0,
        .register_rep = 1,
        .memory_rep = 1,
        .slot = 0,
    };
    fixture->value_reps[1] = (XrTargetValueRepRecord) {
        .semantic_value = 2,
        .register_rep = 0,
        .memory_rep = 0,
        .slot = XR_SEMANTIC_INDEX_NONE,
    };
    fixture->value_reps[2] = (XrTargetValueRepRecord) {
        .semantic_value = 3,
        .register_rep = 1,
        .memory_rep = 1,
        .slot = 1,
    };
    fixture->extents[0] = (XrTargetExtentRecord) {
        .id = 0,
        .kind = XR_TARGET_EXTENT_FIXED,
        .element_layout = XR_SEMANTIC_INDEX_NONE,
    };
    fixture->layouts[0] = (XrTargetLayoutRecord) {
        .id = 0,
        .semantic_type = int_type,
        .kind = XR_TARGET_LAYOUT_SCALAR,
        .align = 8,
        .fixed_prefix_size = 8,
        .extent = 0,
        .field_begin = 0,
    };
    *out_int_layout = 0;
    *out_string_layout = XR_SEMANTIC_INDEX_NONE;
}

static void fill_execution_fixture(TargetFixture *fixture, XrSemanticPlan *semantic,
                                   uint32_t int_layout, uint32_t string_layout) {
    (void) int_layout;
    (void) string_layout;
    fixture->functions[0] = (XrTargetFunctionRecord) {
        .id = 0,
        .semantic_function = 0,
        .slot_begin = 0,
        .slot_count = 2,
        .frame_size = 16,
        .frame_align = 8,
        .root_begin = 0,
        .root_count = 0,
        .cleanup_begin = 0,
        .cleanup_count = 0,
    };
    fixture->slots[0] = (XrTargetSlotRecord) {
        .id = 0,
        .function = 0,
        .offset = 0,
        .size = 8,
        .align = 8,
        .register_rep = 1,
        .memory_rep = 1,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    fill_slot_source(semantic, &fixture->slots[0], 0, fixture->value_reps[0].semantic_value);
    fixture->slots[1] = (XrTargetSlotRecord) {
        .id = 1,
        .function = 0,
        .offset = 8,
        .size = 8,
        .align = 8,
        .register_rep = 1,
        .memory_rep = 1,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    fill_slot_source(semantic, &fixture->slots[1], 0, fixture->value_reps[2].semantic_value);
    if (xr_stable_id_compare(fixture->slots[0].identity, fixture->slots[1].identity) > 0) {
        XrTargetSlotRecord temporary = fixture->slots[0];
        fixture->slots[0] = fixture->slots[1];
        fixture->slots[1] = temporary;
    }
    for (uint32_t i = 0; i < 2; i++) {
        fixture->slots[i].id = i;
        fixture->slots[i].offset = i * 8u;
        for (uint32_t value = 0; value < 3; value++)
            if (fixture->value_reps[value].semantic_value == fixture->slots[i].semantic_value)
                fixture->value_reps[value].slot = i;
    }
}

static void fill_draft(TargetFixture *fixture, XrSemanticPlan *semantic,
                       XrTargetProfile *profile) {
    fixture->draft = (XrTargetPlanDraft) {
        .semantic_plan = semantic,
        .profile = profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
        .machine_reps = fixture->reps,
        .machine_reps_count = 3,
        .value_reps = fixture->value_reps,
        .value_reps_count = 3,
        .extents = fixture->extents,
        .extents_count = 1,
        .layouts = fixture->layouts,
        .layouts_count = 1,
        .functions = fixture->functions,
        .functions_count = 1,
        .slots = fixture->slots,
        .slots_count = 2,
    };
}

static void fill_fixture(TargetFixture *fixture, XrSemanticPlan *semantic,
                         XrTargetProfile *profile) {
    memset(fixture, 0, sizeof(*fixture));
    uint32_t int_layout;
    uint32_t string_layout;
    fill_representation_fixture(fixture, semantic, &int_layout, &string_layout);
    fill_execution_fixture(fixture, semantic, int_layout, string_layout);
    fill_draft(fixture, semantic, profile);
}

static XrTargetPlan *build_target_plan(XrSemanticPlan *semantic, XrTargetProfile *profile) {
    TargetFixture fixture;
    fill_fixture(&fixture, semantic, profile);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    bool frozen = xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error));
    if (!frozen)
        fprintf(stderr, "target fixture failed: %s\n", error);
    REQUIRE(frozen && plan != NULL);
    return plan;
}

static void expect_verify_failure_raw_at(XrTargetPlan *plan, const char *code,
                                         uint32_t mutation_line) {
    char error[512] = {0};
    bool verified = xr_target_plan_verify(plan, error, sizeof(error));
    if (verified)
        fprintf(stderr, "mutation at test_target_plan.c:%u unexpectedly verified\n",
                mutation_line);
    REQUIRE(!verified);
    if (strncmp(error, code, strlen(code)) != 0)
        fprintf(stderr, "expected verifier code %s, got: %s\n", code, error);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_verify_failure_at(XrTargetPlan *plan, const char *code,
                                     uint32_t mutation_line) {
    XrFingerprint frozen_fingerprint = plan->fingerprint;
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure_raw_at(plan, code, mutation_line);
    plan->fingerprint = frozen_fingerprint;
}

#define expect_verify_failure(plan, code) expect_verify_failure_at((plan), (code), __LINE__)
#define expect_verify_failure_raw(plan, code)                                                      \
    expect_verify_failure_raw_at((plan), (code), __LINE__)

static void test_profile_freeze_and_determinism(void) {
    XrTargetProfile *first = build_profile(0);
    XrTargetProfile *same = build_profile(0);
    XrTargetProfile *different = build_profile(XR_TARGET_ATOMIC_WIDTH_128);
    REQUIRE(xr_target_profile_is_frozen(first));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_fingerprint(first),
                                 xr_target_profile_fingerprint(same)));
    REQUIRE(!xr_fingerprint_equal(xr_target_profile_fingerprint(first),
                                  xr_target_profile_fingerprint(different)));

    first->facts.machine.data_layout.pointer.size = 4;
    char error[512] = {0};
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    REQUIRE(strncmp(error, "XR_TARGET_1000", strlen("XR_TARGET_1000")) == 0);
    first->facts.machine.data_layout.pointer.size = 8;
    REQUIRE(xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.native_abi = XR_TARGET_ABI_COUNT;
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.native_abi = XR_TARGET_ABI_WIN64_X86_64;
    XrFingerprint saved_profile_fingerprint = first->fingerprint;
    first->facts.machine.maximum_vector_bits = 256;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.maximum_vector_bits = 128;
    first->facts.machine.vector_feature_mask = XR_TARGET_VECTOR_AVX2;
    first->facts.machine.maximum_vector_bits = 256;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.vector_feature_mask = XR_TARGET_VECTOR_SSE2;
    first->facts.machine.maximum_vector_bits = 128;
    first->facts.machine.operating_system = XR_TARGET_OS_FREESTANDING;
    first->facts.machine.environment = XR_TARGET_ENV_FREESTANDING;
    first->facts.machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.machine.operating_system = XR_TARGET_OS_WINDOWS;
    first->facts.machine.environment = XR_TARGET_ENV_MSVC;
    first->facts.machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED;
    first->fingerprint = saved_profile_fingerprint;
    first->facts.provider_mask |= UINT64_C(1) << 63;
    xr_target_profile_compute_fingerprint(&first->facts, &first->fingerprint);
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->facts.provider_mask &= ~(UINT64_C(1) << 63);
    first->fingerprint = saved_profile_fingerprint;
    first->fingerprint.bytes[0] ^= 1;
    REQUIRE(!xr_target_profile_verify(first, error, sizeof(error)));
    first->fingerprint.bytes[0] ^= 1;

    xr_target_profile_free(first);
    xr_target_profile_free(same);
    xr_target_profile_free(different);
}

static void test_plan_snapshot_and_determinism(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    TargetFixture fixture;
    fill_fixture(&fixture, semantic, profile);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_freeze(&fixture.draft, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_freeze(&fixture.draft, &second, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_frozen(first));
    REQUIRE(xr_target_plan_is_verified(first));
    REQUIRE(xr_target_plan_schema_version(first) == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(second)));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_semantic_fingerprint(first),
                                 xr_semantic_plan_fingerprint(semantic)));
    char target_hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(xr_target_plan_fingerprint(first), target_hex);
    REQUIRE(strcmp(target_hex,
                   "7d0b0b9d5a1c5f5493404d571715f5972e8e7530bcb912e29b4311ecb8ef85fe") == 0);

    fixture.slots[0].offset = 64;
    uint32_t count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(first, &count);
    REQUIRE(count == 2 && slots[0].offset == 0);
    REQUIRE(xr_target_plan_machine_reps(first, &count) != NULL && count == 3);
    const XrTargetValueRepRecord *value_reps = xr_target_plan_value_reps(first, &count);
    REQUIRE(value_reps != NULL && count == 3 && value_reps[0].register_rep == 1);
    REQUIRE(xr_target_plan_value_rep(first, 0) == &value_reps[0]);
    REQUIRE(xr_target_plan_value_rep(first, 1) == NULL);
    REQUIRE(xr_target_plan_value_rep(first, 2) == &value_reps[1]);
    REQUIRE(xr_target_plan_value_rep(first, 3) == &value_reps[2]);
    REQUIRE(xr_target_plan_machine_rep(first, 2) != NULL);
    REQUIRE(xr_target_plan_machine_rep(first, 3) == NULL);
    REQUIRE(xr_target_plan_profile(first) == profile);
    REQUIRE(xr_target_plan_semantic_plan(first) == semantic);

    XrTargetCallArgumentRecord fingerprint_argument = {
        .register_rep = 1,
        .memory_rep = 1,
    };
    XrTargetCallRecord fingerprint_call = {
        .semantic_operation = 0,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .result_register_rep = 1,
        .result_memory_rep = 1,
        .argument_count = 1,
        .adapter = XR_SEMANTIC_INDEX_NONE,
    };
    first->calls = &fingerprint_call;
    first->calls_count = 1;
    first->call_arguments = &fingerprint_argument;
    first->call_arguments_count = 1;
    XrFingerprint call_fingerprint;
    XrFingerprint changed_call_fingerprint;
    xr_target_call_compute_fingerprint(first, 0, &call_fingerprint);
    fingerprint_argument.memory_rep = 2;
    xr_target_call_compute_fingerprint(first, 0, &changed_call_fingerprint);
    REQUIRE(!xr_fingerprint_equal(call_fingerprint, changed_call_fingerprint));
    fingerprint_argument.memory_rep = 1;
    REQUIRE(xr_semantic_plan_operation_count(semantic) > 1);
    fingerprint_call.semantic_operation = 1;
    xr_target_call_compute_fingerprint(first, 0, &changed_call_fingerprint);
    REQUIRE(!xr_fingerprint_equal(call_fingerprint, changed_call_fingerprint));
    first->calls = NULL;
    first->calls_count = 0;
    first->call_arguments = NULL;
    first->call_arguments_count = 0;

    xr_target_plan_free(first);
    xr_target_plan_free(second);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_builder_materializes_canonical_scalar_intents(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *first = NULL;
    XrTargetPlan *second = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(semantic, profile, &first, error, sizeof(error)));
    REQUIRE(xr_target_plan_build(semantic, profile, &second, error, sizeof(error)));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(first),
                                 xr_target_plan_fingerprint(second)));
    REQUIRE(first->completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(first->functions_count == 1 && first->slots_count == 2);
    REQUIRE(first->functions[0].slot_begin == 0 && first->functions[0].slot_count == 2);
    REQUIRE(first->functions[0].frame_size == 16 && first->functions[0].frame_align == 8);
    REQUIRE(xr_stable_id_compare(first->slots[0].identity, first->slots[1].identity) < 0);
    for (uint32_t i = 0; i < first->slots_count; i++) {
        const XrTargetSlotRecord *slot = &first->slots[i];
        REQUIRE(slot->id == i && slot->function == 0);
        REQUIRE(slot->logical_slot == XR_SEMANTIC_INDEX_NONE);
        REQUIRE(slot->role == XR_TARGET_SLOT_TEMPORARY);
        REQUIRE(slot->semantic_operation == operation_for_value(semantic,
                                                                 slot->semantic_value));
    }
    REQUIRE(first->value_reps_count == 3);
    REQUIRE(first->value_reps[0].semantic_value < first->value_reps[1].semantic_value);
    REQUIRE(first->value_reps[1].semantic_value < first->value_reps[2].semantic_value);

    first->slots[0].identity.bytes[0] ^= 1;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[0].identity.bytes[0] ^= 1;
    first->slots[0].role = XR_TARGET_SLOT_ROLE_INVALID;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[0].role = XR_TARGET_SLOT_TEMPORARY;
    first->slots[0].logical_slot = 0;
    expect_verify_failure(first, "XR_TARGET_1001");
    first->slots[0].logical_slot = XR_SEMANTIC_INDEX_NONE;
    first->functions[0].frame_size += first->functions[0].frame_align;
    expect_verify_failure(first, "XR_TARGET_1002");
    first->functions[0].frame_size -= first->functions[0].frame_align;
    REQUIRE(xr_target_plan_verify(first, NULL, 0));

    xr_target_plan_free(first);
    xr_target_plan_free(second);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_structural_mutations_fail_closed(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = build_target_plan(semantic, profile);
    plan->schema_version++;
    expect_verify_failure(plan, "XR_ARTIFACT_2000");
    plan->schema_version--;

    plan->machine_reps[1].memory_size = 7;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[1].memory_size = 8;
    plan->machine_reps[1].kind = UINT16_MAX;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[1].kind = XR_MACHINE_REP_I64;

    XrTargetMachineRepRecord saved_rep2 = plan->machine_reps[2];
    plan->machine_reps[1].legal_conversion_mask[0] = 0;
    plan->machine_reps[2] = (XrTargetMachineRepRecord) {
        .id = 2,
        .kind = XR_MACHINE_REP_VECTOR,
        .register_bits = 128,
        .memory_size = 16,
        .memory_align = 16,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .detail = 1,
        .lane_count = 2,
    };
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    plan->machine_reps[2].root_kind = XR_TARGET_ROOT_OBJECT;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].root_kind = XR_TARGET_ROOT_NONE;
    plan->machine_reps[2].ownership = XR_TARGET_OWNERSHIP_SHARED;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    plan->machine_reps[2].null_encoding = XR_TARGET_NULL_TAGGED;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].null_encoding = XR_TARGET_NULL_NOT_NULLABLE;
    plan->machine_reps[2].lane_count = 3;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].lane_count = 4;
    plan->machine_reps[2].register_bits = 256;
    plan->machine_reps[2].memory_size = 32;
    plan->machine_reps[2].memory_align = 32;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2] = saved_rep2;
    plan->machine_reps[1].legal_conversion_mask[0] = UINT64_C(1) << 2;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);

    plan->extents[0].operand_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents[0].operand_count = 0;
    plan->extents[0].kind = UINT8_MAX;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents[0].kind = XR_TARGET_EXTENT_FIXED;

    plan->layouts[0].kind = UINT8_MAX;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->layouts[0].kind = XR_TARGET_LAYOUT_SCALAR;

    XrTargetFieldRecord fabricated_field = {
        .layout = 0,
        .size = 8,
        .align = 4,
        .memory_rep = 1,
    };
    plan->fields = &fabricated_field;
    plan->fields_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->fields_count = 0;
    plan->fields = NULL;

    plan->slots[1].offset = 0;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->slots[1].offset = 8;
    plan->slots[0].offset = UINT32_MAX - 7u;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->slots[0].offset = 0;

    XrTargetStorageRecord fabricated_storage = {0};
    XrTargetAllocationRecord fabricated_allocation = {0};
    XrTargetExtentOperandRecord fabricated_extent_operand = {0};
    fabricated_storage.domain.bytes[0] = 1;
    plan->storage = &fabricated_storage;
    plan->allocations = &fabricated_allocation;
    plan->extent_operands = &fabricated_extent_operand;
    plan->storage_count = 1;
    plan->allocations_count = 1;
    plan->extent_operands_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->storage_count = 0;
    plan->allocations_count = 0;
    plan->extent_operands_count = 0;
    plan->storage = NULL;
    plan->allocations = NULL;
    plan->extent_operands = NULL;

    XrTargetCallRecord fabricated_call = {.semantic_operation = 0, .callee_function = 1};
    XrTargetCallArgumentRecord fabricated_argument = {0};
    plan->calls = &fabricated_call;
    plan->call_arguments = &fabricated_argument;
    plan->calls_count = 1;
    plan->call_arguments_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1003");
    plan->calls_count = 0;
    plan->call_arguments_count = 0;
    plan->calls = NULL;
    plan->call_arguments = NULL;

    XrTargetRootMapRecord fabricated_root = {.function = 1};
    uint32_t fabricated_root_slot = 0;
    XrTargetCleanupRecord fabricated_cleanup = {.function = 1};
    plan->root_maps = &fabricated_root;
    plan->root_slots = &fabricated_root_slot;
    plan->cleanups = &fabricated_cleanup;
    plan->root_maps_count = 1;
    plan->root_slots_count = 1;
    plan->cleanups_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->root_maps_count = 0;
    plan->root_slots_count = 0;
    plan->cleanups_count = 0;
    plan->root_maps = NULL;
    plan->root_slots = NULL;
    plan->cleanups = NULL;

    XrTargetAdapterRecord fabricated_adapter = {0};
    plan->adapters = &fabricated_adapter;
    plan->adapters_count = 1;
    expect_verify_failure(plan, "XR_EXEC_5002");
    plan->adapters_count = 0;
    plan->adapters = NULL;

    XrTargetCapabilityRecord fabricated_capability = {0};
    plan->capabilities = &fabricated_capability;
    plan->capabilities_count = 1;
    expect_verify_failure(plan, "XR_TARGET_1004");
    plan->capabilities_count = 0;
    plan->capabilities = NULL;

    XrTargetCoroutineStateRecord fabricated_coroutine = {.function = 1};
    plan->coroutines = &fabricated_coroutine;
    plan->coroutines_count = 1;
    expect_verify_failure(plan, "XR_CORO_4000");
    plan->coroutines_count = 0;
    plan->coroutines = NULL;

    plan->extents[0].flags = XR_TARGET_EXTENT_ZERO;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents[0].flags = 0;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);

    XrTargetExtentRecord *saved_extents = plan->extents;
    XrTargetExtentRecord extra_extents[2] = {
        saved_extents[0],
        {.id = 1, .kind = XR_TARGET_EXTENT_FIXED,
         .element_layout = XR_SEMANTIC_INDEX_NONE},
    };
    plan->extents = extra_extents;
    plan->extents_count = 2;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    expect_verify_failure(plan, "XR_TARGET_1002");
    plan->extents = saved_extents;
    plan->extents_count = 1;
    xr_target_layout_compute_fingerprint(plan, 0, &plan->layouts[0].fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);

    plan->semantic_fingerprint.bytes[0] ^= 1;
    expect_verify_failure(plan, "XR_TARGET_1000");
    plan->semantic_fingerprint.bytes[0] ^= 1;
    plan->fingerprint.bytes[0] ^= 1;
    expect_verify_failure_raw(plan, "XR_TARGET_1000");
    plan->fingerprint.bytes[0] ^= 1;

    plan->calls_count = 10000001u;
    expect_verify_failure_raw(plan, "XR_EXEC_5003");
    plan->calls_count = 0;
    plan->frozen = false;
    expect_verify_failure(plan, "XR_EXEC_5000");
    plan->frozen = true;

    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_value_rep_mutations_fail_closed(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    XrTargetPlan *plan = build_target_plan(semantic, profile);
    uint32_t first_slot = plan->value_reps[0].slot;
    uint32_t last_slot = plan->value_reps[2].slot;
    REQUIRE(first_slot < plan->slots_count && last_slot < plan->slots_count &&
            first_slot != last_slot);

    plan->value_reps[2].semantic_value = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[2].semantic_value = 3;
    plan->value_reps[2].semantic_value = 2;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[2].semantic_value = 3;
    plan->value_reps[0].semantic_value = 2;
    plan->value_reps[1].semantic_value = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].semantic_value = 0;
    plan->value_reps[1].semantic_value = 2;
    plan->value_reps[1].semantic_value = 1;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[1].semantic_value = 2;
    plan->value_reps[0].register_rep = plan->machine_reps_count;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].register_rep = 1;
    plan->machine_reps[1].legal_conversion_mask[0] = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[1].legal_conversion_mask[0] = UINT64_C(1) << 2;
    plan->machine_reps[2].legal_conversion_mask[0] = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->machine_reps[2].legal_conversion_mask[0] = UINT64_C(1) << 1;
    plan->value_reps[0].register_rep = 0;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].register_rep = 1;
    plan->value_reps_count = 2;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps_count = 3;
    plan->value_reps[0].slot = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].slot = last_slot;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[0].slot = first_slot;
    plan->slots[first_slot].function = 1;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].function = 0;
    plan->slots[first_slot].register_rep = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].register_rep = 1;
    plan->slots[first_slot].size = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].size = 8;
    plan->slots[first_slot].align = 4;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->slots[first_slot].align = 8;
    plan->value_reps[2].slot = first_slot;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[2].slot = last_slot;
    plan->value_reps[1].slot = first_slot;
    expect_verify_failure(plan, "XR_TARGET_1001");
    plan->value_reps[1].slot = XR_SEMANTIC_INDEX_NONE;
    REQUIRE(xr_target_plan_verify(plan, NULL, 0));
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_freeze_rejects_invalid_draft(void) {
    XrSemanticPlan *semantic = build_semantic_plan();
    XrTargetProfile *profile = build_profile(0);
    TargetFixture fixture;
    fill_fixture(&fixture, semantic, profile);
    XrTargetPlan *plan = NULL;
    char error[512] = {0};
    fixture.draft.completed_family_mask = 0;
    REQUIRE(!xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    fixture.draft.completed_family_mask = XR_TARGET_REQUIRED_FAMILIES;
    fixture.slots[1].offset = 0;
    REQUIRE(!xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1002", strlen("XR_TARGET_1002")) == 0);
    fixture.slots[1].offset = 8;
    fixture.draft.value_reps_count = 40000000u;
    fixture.draft.extents_count = 1000000u;
    fixture.draft.layouts_count = 1000000u;
    fixture.draft.fields_count = 16000000u;
    fixture.draft.storage_count = 4000000u;
    fixture.draft.allocations_count = 10000000u;
    fixture.draft.extent_operands_count = 40000000u;
    fixture.draft.functions_count = 100000u;
    fixture.draft.slots_count = 16000000u;
    fixture.draft.calls_count = 10000000u;
    fixture.draft.call_arguments_count = 40000000u;
    fixture.draft.root_maps_count = 10000000u;
    fixture.draft.root_slots_count = 40000000u;
    fixture.draft.cleanups_count = 40000000u;
    fixture.draft.adapters_count = 1000000u;
    fixture.draft.capabilities_count = 65536u;
    fixture.draft.coroutines_count = 10000000u;
    REQUIRE(!xr_target_plan_freeze(&fixture.draft, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_EXEC_5003", strlen("XR_EXEC_5003")) == 0);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
}

static void test_bool_and_nullable_scalar_boundary(void) {
    XrTargetProfile *profile = build_profile(0);
    char error[512] = {0};

    XrSemanticPlan *boolean_semantic = build_single_scalar_semantic(&stub_bool);
    const XrSemanticTypeRecord *boolean_type = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(boolean_semantic); i++)
        if (xr_semantic_plan_type(boolean_semantic, i)->kind == XR_KIND_BOOL)
            boolean_type = xr_semantic_plan_type(boolean_semantic, i);
    REQUIRE(boolean_type != NULL && boolean_type->scalar_rep == XR_SCALAR_REP_NONE);
    XrTargetPlan *boolean_plan = NULL;
    REQUIRE(freeze_single_scalar(boolean_semantic, profile, true, true, &boolean_plan, error,
                                 sizeof(error)));
    boolean_plan->value_reps[0].memory_rep = 0;
    expect_verify_failure(boolean_plan, "XR_TARGET_1001");
    boolean_plan->value_reps[0].memory_rep = 1;
    REQUIRE(xr_target_plan_verify(boolean_plan, NULL, 0));
    xr_target_plan_free(boolean_plan);
    xr_semantic_plan_free(boolean_semantic);

    XrSemanticPlan *nullable_semantic = build_single_scalar_semantic(&stub_nullable_int);
    const XrSemanticTypeRecord *nullable_type = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_type_count(nullable_semantic); i++)
        if (xr_semantic_plan_type(nullable_semantic, i)->kind == XR_KIND_INT)
            nullable_type = xr_semantic_plan_type(nullable_semantic, i);
    REQUIRE(nullable_type != NULL && (nullable_type->flags & XR_SEM_TYPE_NULLABLE) != 0);
    XrTargetPlan *nullable_plan = NULL;
    REQUIRE(freeze_single_scalar(nullable_semantic, profile, false, false, &nullable_plan, error,
                                 sizeof(error)));
    xr_target_plan_free(nullable_plan);
    nullable_plan = NULL;
    REQUIRE(!freeze_single_scalar(nullable_semantic, profile, true, false, &nullable_plan, error,
                                  sizeof(error)));
    REQUIRE(nullable_plan == NULL);
    REQUIRE(strncmp(error, "XR_TARGET_1001", strlen("XR_TARGET_1001")) == 0);
    xr_semantic_plan_free(nullable_semantic);
    xr_target_profile_free(profile);
}

int main(void) {
    test_profile_freeze_and_determinism();
    test_plan_snapshot_and_determinism();
    test_builder_materializes_canonical_scalar_intents();
    test_structural_mutations_fail_closed();
    test_value_rep_mutations_fail_closed();
    test_freeze_rejects_invalid_draft();
    test_bool_and_nullable_scalar_boundary();
    printf("TargetProfile/TargetPlan tests passed\n");
    return 0;
}
