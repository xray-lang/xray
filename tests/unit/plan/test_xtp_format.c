/*
 * test_xtp_format.c - Exact typed TargetPlan artifact contract
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_coro_lower.h"
#include "../../../src/ir/xi_stage.h"
#include "../../../src/plan/format/xr_xtp_internal.h"
#include "../../../src/plan/format/xr_artifact_kind.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/xr_runtime_artifact_authority_internal.h"
#include "../../../include/xray_target_plan_load.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
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

typedef struct XtpFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    uint8_t *bytes;
    size_t size;
} XtpFixture;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_unit = {
    .kind = XR_KIND_UNIT,
    .id = 3,
    .frozen = true,
    .scalar_rep = XR_SCALAR_REP_NONE,
};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 2,
    .frozen = true,
    .function = {.return_type = &stub_int, .throw_effect = XR_FN_EFFECT_NO_THROW},
};

static XrSemanticPlan *build_semantic_plan(void) {
    XiFunc *function = xi_func_new("xtp_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    REQUIRE(xi_const_int(function, entry, 7, &stub_int) != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL);
    xi_func_free(function);
    return semantic;
}

static XrSemanticPlan *build_direct_call_semantic_plan(void) {
    XiFunc *root = xi_func_new("xtp_direct_call_root", &stub_int);
    XiFunc *child = xi_func_new("xtp_direct_call_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    child->nparams = child->min_params = 1;
    child->params = (XiValue **) xr_calloc(1, sizeof(*child->params));
    REQUIRE(child->params != NULL);
    child->params[0] = xi_param(child, child_entry, 0, &stub_int);
    REQUIRE(child->params[0] != NULL);
    xi_block_set_return(child_entry, child->params[0]);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure = xi_value_new(root, root_entry, XI_STACK_ALLOC, &stub_function, 0);
    REQUIRE(closure != NULL);
    closure->aux_int = XI_CLOSURE_NEW;
    closure->aux = child;
    XiValue *alias = xi_value_new(root, root_entry, XI_COPY, &stub_function, 1);
    REQUIRE(alias != NULL);
    alias->args[0] = closure;
    alias->aux_int = XI_COPY_KIND_IDENTITY;
    XiValue *argument = xi_const_int(root, root_entry, 9, &stub_int);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 2);
    REQUIRE(argument != NULL && call != NULL);
    call->args[0] = alias;
    call->args[1] = argument;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL && xr_semantic_plan_call_target_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static XrSemanticPlan *build_coroutine_call_semantic_plan(void) {
    XiFunc *root = xi_func_new("xtp_coroutine_root", &stub_int);
    XiFunc *child = xi_func_new("xtp_coroutine_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    root_entry->sealed = child_entry->sealed = true;
    XiValue *yield = xi_value_new(child, child_entry, XI_YIELD, &stub_unit, 0);
    XiValue *child_result = xi_const_int(child, child_entry, 31, &stub_int);
    REQUIRE(yield != NULL && child_result != NULL);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    XiValue *closure =
        xi_value_new(root, root_entry, XI_CLOSURE_NEW, &stub_function, 0);
    XiValue *call = xi_value_new(root, root_entry, XI_CALL, &stub_int, 1);
    REQUIRE(closure != NULL && call != NULL);
    closure->aux = child;
    call->args[0] = closure;
    xi_block_set_return(root_entry, call);
    root->stage = child->stage = XI_STAGE_SEMANTIC_LOWERED;
    root->invariant_mask = child->invariant_mask =
        xi_stage_invariants(XI_STAGE_SEMANTIC_LOWERED);
    REQUIRE(xi_coro_lower(root, NULL));
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, error, sizeof(error)));
    REQUIRE(semantic != NULL && xr_semantic_plan_call_target_count(semantic) == 1);
    xi_func_free(root);
    return semantic;
}

static XrTargetProfile *build_profile(void) {
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
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_build(&input, &profile, error, sizeof(error)));
    return profile;
}

typedef enum NativeMachineMutation {
    NATIVE_MACHINE_ARCHITECTURE,
    NATIVE_MACHINE_OPERATING_SYSTEM,
    NATIVE_MACHINE_ENVIRONMENT,
    NATIVE_MACHINE_ABI,
    NATIVE_MACHINE_DATA_LAYOUT,
    NATIVE_MACHINE_ATOMIC_WIDTHS,
    NATIVE_MACHINE_ATOMIC_ORDERS,
    NATIVE_MACHINE_FLOAT_FEATURES,
    NATIVE_MACHINE_VECTOR_FEATURES,
    NATIVE_MACHINE_VECTOR_WIDTH,
    NATIVE_MACHINE_RUNTIME_PROFILE,
    NATIVE_MACHINE_MUTATION_COUNT,
} NativeMachineMutation;

typedef enum ForeignProfileMutation {
    FOREIGN_PROFILE_ARCHITECTURE_ABI,
    FOREIGN_PROFILE_PLATFORM_IDENTITY,
    FOREIGN_PROFILE_DATA_LAYOUT,
    FOREIGN_PROFILE_ATOMIC_WIDTHS,
    FOREIGN_PROFILE_ATOMIC_ORDERS,
    FOREIGN_PROFILE_FLOAT_FEATURES,
    FOREIGN_PROFILE_VECTOR_FEATURES,
    FOREIGN_PROFILE_RUNTIME_PROFILE,
    FOREIGN_PROFILE_MUTATION_COUNT,
} ForeignProfileMutation;

static void mutate_exact_machine_field(XrTargetMachineFacts *machine,
                                       NativeMachineMutation mutation) {
    switch (mutation) {
        case NATIVE_MACHINE_ARCHITECTURE:
            machine->architecture = machine->architecture == XR_TARGET_ARCH_X86_64
                                        ? XR_TARGET_ARCH_AARCH64
                                        : XR_TARGET_ARCH_X86_64;
            break;
        case NATIVE_MACHINE_OPERATING_SYSTEM:
            machine->operating_system = machine->operating_system == XR_TARGET_OS_WINDOWS
                                            ? XR_TARGET_OS_LINUX
                                            : XR_TARGET_OS_WINDOWS;
            break;
        case NATIVE_MACHINE_ENVIRONMENT:
            machine->environment = machine->environment == XR_TARGET_ENV_MSVC
                                       ? XR_TARGET_ENV_GNU
                                       : XR_TARGET_ENV_MSVC;
            break;
        case NATIVE_MACHINE_ABI:
            machine->native_abi = machine->native_abi == XR_TARGET_ABI_WIN64_X86_64
                                      ? XR_TARGET_ABI_SYSV_X86_64
                                      : XR_TARGET_ABI_WIN64_X86_64;
            break;
        case NATIVE_MACHINE_DATA_LAYOUT:
            machine->data_layout.stable_hash ^= UINT64_C(1);
            break;
        case NATIVE_MACHINE_ATOMIC_WIDTHS:
            machine->atomic_width_mask ^= XR_TARGET_ATOMIC_WIDTH_128;
            break;
        case NATIVE_MACHINE_ATOMIC_ORDERS:
            machine->atomic_order_mask ^= XR_TARGET_ATOMIC_SEQ_CST;
            break;
        case NATIVE_MACHINE_FLOAT_FEATURES:
            machine->float_feature_mask ^= XR_TARGET_FLOAT_FMA;
            break;
        case NATIVE_MACHINE_VECTOR_FEATURES:
            machine->vector_feature_mask ^= XR_TARGET_VECTOR_SSE2;
            break;
        case NATIVE_MACHINE_VECTOR_WIDTH:
            machine->maximum_vector_bits ^= 128u;
            break;
        case NATIVE_MACHINE_RUNTIME_PROFILE:
            machine->runtime_profile =
                machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_HOSTED
                    ? XR_TARGET_RUNTIME_PROFILE_FREESTANDING
                    : XR_TARGET_RUNTIME_PROFILE_HOSTED;
            break;
        case NATIVE_MACHINE_MUTATION_COUNT:
            abort();
    }
}

static void set_native_abi_for_platform(XrTargetMachineFacts *machine) {
    if (machine->operating_system == XR_TARGET_OS_WINDOWS) {
        machine->native_abi = machine->architecture == XR_TARGET_ARCH_AARCH64
                                  ? XR_TARGET_ABI_WIN64_AARCH64
                                  : XR_TARGET_ABI_WIN64_X86_64;
    } else if (machine->operating_system == XR_TARGET_OS_MACOS) {
        machine->native_abi = machine->architecture == XR_TARGET_ARCH_AARCH64
                                  ? XR_TARGET_ABI_DARWIN_AARCH64
                                  : XR_TARGET_ABI_DARWIN_X86_64;
    } else {
        machine->native_abi = machine->architecture == XR_TARGET_ARCH_AARCH64
                                  ? XR_TARGET_ABI_AAPCS64
                                  : XR_TARGET_ABI_SYSV_X86_64;
    }
}

static void set_supported_vector_profile(XrTargetMachineFacts *machine) {
    switch (machine->architecture) {
        case XR_TARGET_ARCH_X86_64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_SSE2;
            break;
        case XR_TARGET_ARCH_AARCH64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_NEON;
            break;
        case XR_TARGET_ARCH_POWERPC64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_VSX;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            machine->vector_feature_mask = XR_TARGET_VECTOR_LSX;
            break;
        default:
            abort();
    }
    machine->maximum_vector_bits = 128;
}

static void mutate_foreign_profile(XrTargetMachineFacts *machine,
                                   ForeignProfileMutation mutation) {
    switch (mutation) {
        case FOREIGN_PROFILE_ARCHITECTURE_ABI:
            machine->architecture = machine->architecture == XR_TARGET_ARCH_X86_64
                                        ? XR_TARGET_ARCH_AARCH64
                                        : XR_TARGET_ARCH_X86_64;
            set_native_abi_for_platform(machine);
            break;
        case FOREIGN_PROFILE_PLATFORM_IDENTITY:
            if (machine->operating_system == XR_TARGET_OS_WINDOWS) {
                machine->operating_system = XR_TARGET_OS_LINUX;
                machine->environment = XR_TARGET_ENV_GNU;
            } else {
                machine->operating_system = XR_TARGET_OS_WINDOWS;
                machine->environment = XR_TARGET_ENV_MSVC;
            }
            if (machine->architecture != XR_TARGET_ARCH_X86_64 &&
                machine->architecture != XR_TARGET_ARCH_AARCH64)
                machine->architecture = XR_TARGET_ARCH_X86_64;
            set_native_abi_for_platform(machine);
            break;
        case FOREIGN_PROFILE_DATA_LAYOUT:
            machine->data_layout.i16.align =
                machine->data_layout.i16.align == 1 ? 2 : 1;
            machine->data_layout.stable_hash =
                xr_target_data_layout_hash(&machine->data_layout);
            break;
        case FOREIGN_PROFILE_ATOMIC_WIDTHS:
            machine->atomic_width_mask ^= XR_TARGET_ATOMIC_WIDTH_128;
            break;
        case FOREIGN_PROFILE_ATOMIC_ORDERS:
            machine->atomic_order_mask ^= XR_TARGET_ATOMIC_SEQ_CST;
            break;
        case FOREIGN_PROFILE_FLOAT_FEATURES:
            machine->float_feature_mask ^= XR_TARGET_FLOAT_FMA;
            break;
        case FOREIGN_PROFILE_VECTOR_FEATURES:
            set_supported_vector_profile(machine);
            break;
        case FOREIGN_PROFILE_RUNTIME_PROFILE:
            machine->runtime_profile = XR_TARGET_RUNTIME_PROFILE_FREESTANDING;
            break;
        case FOREIGN_PROFILE_MUTATION_COUNT:
            abort();
    }
}

static XrTargetProfile *build_foreign_profile(
    const XrTargetProfile *native_profile, ForeignProfileMutation mutation) {
    XrTargetProfileDraft draft = *xr_target_profile_facts(native_profile);
    mutate_foreign_profile(&draft.machine, mutation);
    XrTargetProfile *foreign = NULL;
    char error[512] = {0};
    REQUIRE(xr_target_profile_freeze(&draft, &foreign, error, sizeof(error)));
    REQUIRE(foreign != NULL);
    REQUIRE(xr_target_profile_verify(foreign, error, sizeof(error)));
    return foreign;
}

static XtpFixture make_fixture_from_semantic(XrSemanticPlan *semantic) {
    XtpFixture fixture = {0};
    fixture.semantic = semantic;
    fixture.profile = build_profile();
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan, error,
                                 sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    REQUIRE(xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size, error,
                               sizeof(error)));
    REQUIRE(fixture.bytes != NULL && fixture.size >= XR_XTP_HEADER_SIZE);
    return fixture;
}


static XtpFixture make_fixture(void) {
    return make_fixture_from_semantic(build_semantic_plan());
}

static XtpFixture make_direct_call_fixture(void) {
    return make_fixture_from_semantic(build_direct_call_semantic_plan());
}

static XtpFixture make_coroutine_call_fixture(void) {
    return make_fixture_from_semantic(build_coroutine_call_semantic_plan());
}

static void dispose_fixture(XtpFixture *fixture) {
    xr_xtp_encoded_free(fixture->bytes);
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static uint8_t *copy_artifact(const XtpFixture *fixture) {
    uint8_t *copy = (uint8_t *) xr_malloc(fixture->size);
    REQUIRE(copy != NULL);
    memcpy(copy, fixture->bytes, fixture->size);
    return copy;
}

static void resign_artifact(uint8_t *bytes, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, bytes, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET);
}

static uint8_t *directory_entry(uint8_t *bytes, XrXtpSectionKind kind) {
    return bytes + XR_XTP_HEADER_SIZE +
           ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
}

static void resign_section(uint8_t *bytes, XrXtpSectionKind kind) {
    uint8_t *entry = directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16);
    xr_sha256(bytes + offset, length, entry + 40);
}

static void expect_decode_failure(const uint8_t *bytes, size_t size) {
    XrXtpCandidate *candidate = (XrXtpCandidate *) (uintptr_t) 1;
    char error[512] = {0};
    REQUIRE(!xr_xtp_decode_candidate(bytes, size, &candidate, error, sizeof(error)));
    REQUIRE(candidate == NULL);
    REQUIRE(strncmp(error, "XR_", 3) == 0);
}

static void expect_materialize_failure(const XtpFixture *fixture, const uint8_t *bytes) {
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(bytes, fixture->size, &candidate, error, sizeof(error)));
    XrTargetPlan *plan = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_xtp_materialize_target_plan(candidate, fixture->semantic, fixture->profile,
                                             &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_", 3) == 0);
    xr_xtp_candidate_release(candidate);
}

static void test_exact_roundtrip_and_owned_candidate(void) {
    XtpFixture fixture = make_fixture();
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate, error,
                                    sizeof(error)));
    REQUIRE(xr_xtp_candidate_retain(candidate) == candidate);
    xr_xtp_candidate_release(candidate);
    XrXtpIdentity identity;
    XrXtpResourceManifest resources;
    REQUIRE(xr_xtp_candidate_identity(candidate, &identity));
    REQUIRE(xr_xtp_candidate_resources(candidate, &resources));
    REQUIRE(identity.plan_schema == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(identity.completed_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(resources.total_rows > 1 && resources.verification_work_units == resources.total_rows);

    uint8_t saved = fixture.bytes[0];
    uint8_t saved_schema = fixture.bytes[4];
    uint8_t saved_digest = fixture.bytes[XR_XTP_FULL_DIGEST_OFFSET];
    uint8_t *function_entry =
        directory_entry(fixture.bytes, XR_XTP_SECTION_FUNCTIONS);
    size_t function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    uint8_t saved_function = fixture.bytes[function_offset];
    fixture.bytes[0] ^= 0xff;
    fixture.bytes[4] ^= 0xff;
    fixture.bytes[XR_XTP_FULL_DIGEST_OFFSET] ^= 0xff;
    fixture.bytes[function_offset] ^= 0xff;
    XrTargetPlan *decoded_plan = NULL;
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic, fixture.profile,
                                            &decoded_plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(decoded_plan));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded_plan),
                                 xr_target_plan_fingerprint(fixture.plan)));
    fixture.bytes[0] = saved;
    fixture.bytes[4] = saved_schema;
    fixture.bytes[XR_XTP_FULL_DIGEST_OFFSET] = saved_digest;
    fixture.bytes[function_offset] = saved_function;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    REQUIRE(xr_xtp_encode_plan(decoded_plan, &encoded, &encoded_size, error, sizeof(error)));
    REQUIRE(encoded_size == fixture.size);
    REQUIRE(memcmp(encoded, fixture.bytes, fixture.size) == 0);
    xr_xtp_encoded_free(encoded);
    xr_target_plan_free(decoded_plan);
    xr_xtp_candidate_release(candidate);

    uint8_t *old_v6 = copy_artifact(&fixture);
    xr_xtp_put_u32(old_v6 + 4, UINT32_C(6));
    resign_artifact(old_v6, fixture.size);
    expect_decode_failure(old_v6, fixture.size);
    xr_free(old_v6);

    uint8_t *mutated_profile = copy_artifact(&fixture);
    uint8_t *profile_entry =
        directory_entry(mutated_profile, XR_XTP_SECTION_TARGET_PROFILE);
    size_t profile_offset =
        (size_t) xr_xtp_take_u64(profile_entry + 8);
    /* Profile v2: dynamic_tag immediately follows the 292-byte v1 prefix
     * and the new literal contract schema. */
    mutated_profile[profile_offset + 296] ^= 1;
    resign_section(mutated_profile, XR_XTP_SECTION_TARGET_PROFILE);
    resign_artifact(mutated_profile, fixture.size);
    expect_materialize_failure(&fixture, mutated_profile);
    xr_free(mutated_profile);
    dispose_fixture(&fixture);
}

static void test_direct_call_rows_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_direct_call_fixture();
    uint32_t count = 0;
    REQUIRE(xr_target_plan_calls(fixture.plan, &count) != NULL && count == 1);
    REQUIRE(xr_target_plan_call_arguments(fixture.plan, &count) != NULL && count == 1);
    REQUIRE(xr_target_plan_adapters(fixture.plan, &count) == NULL && count == 0);
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                            fixture.profile, &decoded,
                                            error, sizeof(error)));
    REQUIRE(xr_target_plan_calls(decoded, &count) != NULL && count == 1);
    REQUIRE(xr_target_plan_call_arguments(decoded, &count) != NULL && count == 1);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *call_entry = directory_entry(copy, XR_XTP_SECTION_CALLS);
    size_t call_offset = (size_t) xr_xtp_take_u64(call_entry + 8);
    copy[call_offset + 20] ^= 1; /* semantic_call_target */
    resign_section(copy, XR_XTP_SECTION_CALLS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    uint8_t *argument_entry = directory_entry(copy, XR_XTP_SECTION_CALL_ARGUMENTS);
    size_t argument_offset = (size_t) xr_xtp_take_u64(argument_entry + 8);
    copy[argument_offset + 20] ^= 1; /* semantic_operand */
    resign_section(copy, XR_XTP_SECTION_CALL_ARGUMENTS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);
    dispose_fixture(&fixture);
}

static void test_coroutine_rows_roundtrip_and_mutate(void) {
    XtpFixture fixture = make_coroutine_call_fixture();
    uint32_t count = 0;
    REQUIRE(xr_target_plan_coroutines(fixture.plan, &count) != NULL &&
            count == 2);
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(xr_xtp_decode_candidate(fixture.bytes, fixture.size, &candidate,
                                    error, sizeof(error)));
    REQUIRE(xr_xtp_materialize_target_plan(candidate, fixture.semantic,
                                           fixture.profile, &decoded,
                                           error, sizeof(error)));
    REQUIRE(xr_target_plan_coroutines(decoded, &count) != NULL && count == 2);
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);

    static const size_t mutations[] = {
        8,  /* semantic_entity */
        12, /* semantic_operation */
        16, /* logical_state */
        20, /* suspend_block */
        24, /* resume_block */
        28, /* resume_predecessor */
        32, /* direct_call */
        36, /* result_slot */
        40, /* resume_predecessor_ordinal */
        42, /* flags */
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_COROUTINES);
        size_t offset = (size_t) xr_xtp_take_u64(entry + 8);
        copy[offset + mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_COROUTINES);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static void test_artifact_classifier(void) {
    static const uint8_t source[] = "print(1)";
    static const uint8_t removed_xtp_v1[] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 0};
    static const uint8_t legacy_xrc[] = {
        'X', 'R', 'A', 'Y', (uint8_t) XR_LEGACY_XRC_VERSION,
        (uint8_t) (XR_LEGACY_XRC_VERSION >> 8)};
    static const uint8_t corrupt_removed[] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 1};
    static const uint8_t unknown_reserved[] = {
        'X', 'R', 'A', 'Y', 'Q', 'Q', 'Q', 0};
    static const uint8_t wrong_xrc_version[] = {
        'X', 'R', 'A', 'Y', (uint8_t) (XR_LEGACY_XRC_VERSION - 1u), 0};
    XrArtifactProbeResult probe =
        xr_artifact_probe("renamed.bin", xr_xsm_artifact_magic,
                          XR_XSM_ARTIFACT_MAGIC_SIZE);
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_XSM);
    probe = xr_artifact_probe("renamed.bin", xr_xtp_artifact_magic,
                              XR_XTP_ARTIFACT_MAGIC_SIZE);
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_XTP);
    probe = xr_artifact_probe("renamed.bin", legacy_xrc,
                              sizeof(legacy_xrc));
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_LEGACY_XRC);
    probe = xr_artifact_probe("program.xr", source, sizeof(source) - 1);
    REQUIRE(probe.status == XR_ARTIFACT_PROBE_MATCH &&
            probe.kind == XR_ARTIFACT_KIND_SOURCE);
    REQUIRE(xr_artifact_probe("program.xtp", source, sizeof(source) - 1).status ==
            XR_ARTIFACT_PROBE_CONFLICT);
    REQUIRE(xr_artifact_probe("program.xsm", xr_xtp_artifact_magic,
                              XR_XTP_ARTIFACT_MAGIC_SIZE).status ==
            XR_ARTIFACT_PROBE_CONFLICT);
    REQUIRE(xr_artifact_probe("renamed.bin", removed_xtp_v1,
                              sizeof(removed_xtp_v1)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    REQUIRE(xr_artifact_probe("renamed.bin", corrupt_removed,
                              sizeof(corrupt_removed)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    REQUIRE(xr_artifact_probe("renamed.bin", unknown_reserved,
                              sizeof(unknown_reserved)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    REQUIRE(xr_artifact_probe("renamed.bin", wrong_xrc_version,
                              sizeof(wrong_xrc_version)).status ==
            XR_ARTIFACT_PROBE_UNKNOWN_RESERVED);
    for (size_t size = 5; size < XR_XSM_ARTIFACT_MAGIC_SIZE; size++)
        REQUIRE(xr_artifact_probe("renamed.bin", xr_xsm_artifact_magic,
                                  size).status ==
                XR_ARTIFACT_PROBE_NEED_MORE);
}

static void test_runtime_machine_authority_is_exact_and_scalar_only(void) {
    XrRuntimeTargetAuthority authority;
    REQUIRE(xr_runtime_target_authority_native_hosted(&authority) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(authority.machine.runtime_profile ==
            XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(authority.machine.vector_feature_mask == 0);
    REQUIRE(authority.machine.maximum_vector_bits == 0);
    REQUIRE(xr_runtime_target_authority_machine_matches(
        &authority, &authority.machine));
    for (NativeMachineMutation mutation = 0;
         mutation < NATIVE_MACHINE_MUTATION_COUNT; mutation++) {
        XrTargetMachineFacts candidate = authority.machine;
        mutate_exact_machine_field(&candidate, mutation);
        REQUIRE(!xr_runtime_target_authority_machine_matches(&authority,
                                                              &candidate));
    }
}

static void test_runtime_factory_owns_native_profile(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority != NULL);
    REQUIRE(authority->target_profile != fixture.profile);
    REQUIRE(xr_target_profile_require_exact(
        fixture.profile, authority->target_profile, diagnostic,
        sizeof(diagnostic)));
    XrRuntimeTargetAuthority runtime;
    REQUIRE(xr_runtime_target_authority_native_hosted(&runtime) ==
            XR_RUNTIME_ABI_OK);
    REQUIRE(xr_runtime_target_authority_machine_matches(
        &runtime, xr_target_profile_machine_facts(authority->target_profile)));
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_independent_verifier_rejects_forged_machine_profiles(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    XrTargetProfile *native_profile = authority->target_profile;
    XrRuntimeArtifactAuthorityIdentity native_identity = authority->identity;

    for (ForeignProfileMutation mutation = 0;
         mutation < FOREIGN_PROFILE_MUTATION_COUNT; mutation++) {
        XrTargetProfile *foreign =
            build_foreign_profile(fixture.profile, mutation);
        authority->target_profile = foreign;
        XrFingerprint foreign_fingerprint =
            xr_target_profile_fingerprint(foreign);
        memcpy(authority->identity.target_profile_fingerprint,
               foreign_fingerprint.bytes,
               XR_RUNTIME_ARTIFACT_FINGERPRINT_SIZE);
        xr_runtime_artifact_authority_compute_fingerprint(
            &authority->identity, authority->identity.authority_fingerprint);
        memset(diagnostic, 0, sizeof(diagnostic));
        REQUIRE(!xr_runtime_artifact_authority_verify(
            authority, diagnostic, sizeof(diagnostic)));
        REQUIRE(strstr(diagnostic, "canonical native machine") != NULL);
        authority->target_profile = native_profile;
        authority->identity = native_identity;
        xr_target_profile_free(foreign);
    }

    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_runtime_load_rejects_foreign_profile_artifacts(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    for (ForeignProfileMutation mutation = 0;
         mutation < FOREIGN_PROFILE_MUTATION_COUNT; mutation++) {
        XrTargetProfile *foreign =
            build_foreign_profile(fixture.profile, mutation);
        XrTargetPlan *foreign_plan = NULL;
        uint8_t *foreign_bytes = NULL;
        size_t foreign_size = 0;
        REQUIRE(xr_target_plan_build(fixture.semantic, foreign, &foreign_plan,
                                     diagnostic, sizeof(diagnostic)));
        REQUIRE(xr_xtp_encode_plan(foreign_plan, &foreign_bytes,
                                   &foreign_size, diagnostic,
                                   sizeof(diagnostic)));
        XrTargetPlan *loaded = (XrTargetPlan *) (uintptr_t) 1;
        memset(diagnostic, 0, sizeof(diagnostic));
        REQUIRE(!xr_runtime_target_plan_load(
            foreign_bytes, foreign_size, authority, &loaded, diagnostic,
            sizeof(diagnostic)));
        REQUIRE(loaded == NULL);
        REQUIRE(strstr(diagnostic, "XR_TARGET_1000") != NULL);
        xr_xtp_encoded_free(foreign_bytes);
        xr_target_plan_free(foreign_plan);
        xr_target_profile_free(foreign);
    }
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_runtime_load_materializes_only_verified_plan(void) {
    XtpFixture fixture = make_fixture();
    char diagnostic[512] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    REQUIRE(xr_runtime_artifact_authority_create_internal(
        fixture.semantic, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority != NULL);
    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    XrRuntimeArtifactAuthorityIdentity identity;
    REQUIRE(xr_runtime_artifact_authority_identity(authority, &identity));
    REQUIRE(identity.schema_version ==
            XR_RUNTIME_ARTIFACT_AUTHORITY_SCHEMA_VERSION);
    REQUIRE(identity.required_family_mask == XR_TARGET_REQUIRED_FAMILIES);
    REQUIRE(identity.required_capability_mask ==
            XR_TARGET_FOUNDATION_CAPABILITY_MASK);
    REQUIRE((identity.provider_mask & identity.required_capability_mask) ==
            identity.required_capability_mask);

    XrTargetPlan *loaded = NULL;
    REQUIRE(xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded != NULL && xr_target_plan_is_verified(loaded));
    REQUIRE(xr_fingerprint_equal(xr_target_plan_fingerprint(loaded),
                                 xr_target_plan_fingerprint(fixture.plan)));
    xr_target_plan_free(loaded);

    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, NULL, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2004") != NULL);

    static const uint8_t xsm[] = {'X', 'R', 'A', 'Y', 'X', 'S', 'M', 0};
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        xsm, sizeof(xsm), authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2000") != NULL);

    uint8_t *corrupt = copy_artifact(&fixture);
    corrupt[XR_XTP_FULL_DIGEST_OFFSET] ^= 1;
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        corrupt, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    REQUIRE(strncmp(diagnostic, "XR_ARTIFACT_2002", 16) == 0);
    xr_free(corrupt);

    XrRuntimeArtifactAuthorityIdentity saved = authority->identity;
    authority->identity.semantic_fingerprint[0] ^= 1;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.target_profile_fingerprint[0] ^= 1;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.required_family_mask = 0;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.required_capability_mask &=
        ~XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC);
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.provider_set_fingerprint[0] ^= 1;
    REQUIRE(!xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                  sizeof(diagnostic)));
    authority->identity = saved;
    authority->identity.authority_fingerprint[0] ^= 1;
    loaded = (XrTargetPlan *) (uintptr_t) 1;
    REQUIRE(!xr_runtime_target_plan_load(
        fixture.bytes, fixture.size, authority, &loaded, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(loaded == NULL);
    authority->identity = saved;
    REQUIRE(xr_runtime_artifact_authority_verify(authority, diagnostic,
                                                 sizeof(diagnostic)));
    xr_runtime_artifact_authority_free(authority);
    dispose_fixture(&fixture);
}

static void test_wire_row_inventory(void) {
    static const uint32_t expected[] = {
        0, 448, 58, 12, 24, 108, 24, 40, 24, 12,
        48, 58, 32, 114, 50, 20, 4, 20, 44, 12, 44,
    };
    REQUIRE(sizeof(expected) / sizeof(expected[0]) == XR_XTP_SECTION_COUNT);
    for (uint32_t kind = 1; kind < XR_XTP_SECTION_COUNT; kind++) {
        REQUIRE(xr_xtp_wire_row_size((XrXtpSectionKind) kind) == expected[kind]);
        REQUIRE(xr_xtp_table_count_limit((XrXtpSectionKind) kind) > 0);
    }
    REQUIRE(xr_xtp_runtime_peak_within_budget(
        XR_XTP_MAX_ARTIFACT_SIZE, XR_XTP_MAX_DECODED_TABLE_BYTES));
    REQUIRE(!xr_xtp_runtime_peak_within_budget(
        XR_XTP_MAX_ARTIFACT_SIZE,
        XR_XTP_MAX_DECODED_TABLE_BYTES + 1u));
    REQUIRE(!xr_xtp_runtime_peak_within_budget(SIZE_MAX, 0));
}

static void require_row_codec_roundtrip(XrXtpSectionKind kind, size_t native_size) {
    uint32_t wire_size = xr_xtp_wire_row_size(kind);
    REQUIRE(wire_size > 0 && wire_size <= 512);
    void *source = xr_malloc(native_size);
    void *decoded = xr_calloc(1, native_size);
    REQUIRE(source != NULL && decoded != NULL);
    memset(source, 0xa5, native_size);
    uint8_t first[512] = {0};
    uint8_t second[512] = {0};
    REQUIRE(xr_xtp_encode_rows(kind, source, 1, first));
    REQUIRE(xr_xtp_decode_rows(kind, first, 1, decoded));
    REQUIRE(xr_xtp_encode_rows(kind, decoded, 1, second));
    REQUIRE(memcmp(first, second, wire_size) == 0);
    xr_free(decoded);
    xr_free(source);
}

static void test_every_typed_row_codec(void) {
#define XR_XTP_ROW_ROUNDTRIP(kind, type)                                                           \
    require_row_codec_roundtrip(XR_XTP_SECTION_##kind, sizeof(type))
    XR_XTP_ROW_ROUNDTRIP(TARGET_PROFILE, XrTargetProfileDraft);
    XR_XTP_ROW_ROUNDTRIP(MACHINE_REPS, XrTargetMachineRepRecord);
    XR_XTP_ROW_ROUNDTRIP(VALUE_REPS, XrTargetValueRepRecord);
    XR_XTP_ROW_ROUNDTRIP(EXTENTS, XrTargetExtentRecord);
    XR_XTP_ROW_ROUNDTRIP(LAYOUTS, XrTargetLayoutRecord);
    XR_XTP_ROW_ROUNDTRIP(FIELDS, XrTargetFieldRecord);
    XR_XTP_ROW_ROUNDTRIP(STORAGE, XrTargetStorageRecord);
    XR_XTP_ROW_ROUNDTRIP(ALLOCATIONS, XrTargetAllocationRecord);
    XR_XTP_ROW_ROUNDTRIP(EXTENT_OPERANDS, XrTargetExtentOperandRecord);
    XR_XTP_ROW_ROUNDTRIP(FUNCTIONS, XrTargetFunctionRecord);
    XR_XTP_ROW_ROUNDTRIP(SLOTS, XrTargetSlotRecord);
    XR_XTP_ROW_ROUNDTRIP(INSTRUCTIONS, XrTargetInstructionRecord);
    XR_XTP_ROW_ROUNDTRIP(CALLS, XrTargetCallRecord);
    XR_XTP_ROW_ROUNDTRIP(CALL_ARGUMENTS, XrTargetCallArgumentRecord);
    XR_XTP_ROW_ROUNDTRIP(ROOT_MAPS, XrTargetRootMapRecord);
    XR_XTP_ROW_ROUNDTRIP(ROOT_SLOTS, uint32_t);
    XR_XTP_ROW_ROUNDTRIP(CLEANUPS, XrTargetCleanupRecord);
    XR_XTP_ROW_ROUNDTRIP(ADAPTERS, XrTargetAdapterRecord);
    XR_XTP_ROW_ROUNDTRIP(CAPABILITIES, XrTargetCapabilityRecord);
    XR_XTP_ROW_ROUNDTRIP(COROUTINES, XrTargetCoroutineStateRecord);
#undef XR_XTP_ROW_ROUNDTRIP
}

static void test_header_and_directory_mutations(void) {
    XtpFixture fixture = make_fixture();
    uint8_t *copy = copy_artifact(&fixture);
    copy[XR_XTP_FULL_DIGEST_OFFSET] ^= 1;
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u32(copy + 4, UINT32_C(6)); /* v6 is a hard-cutover negative. */
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u64(copy + 48, 0);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u64(copy + 296, XR_XTP_MAX_TOTAL_ROWS + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);
    expect_decode_failure(fixture.bytes, XR_XTP_MAX_ARTIFACT_SIZE + 1u);

    uint8_t *entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 8, UINT64_MAX);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 16, xr_xtp_take_u64(entry + 16) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u64(entry + 24, xr_xtp_take_u64(entry + 24) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u32(entry + 32, xr_xtp_take_u32(entry + 32) + 1u);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    xr_xtp_put_u32(entry + 36, 8);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    xr_xtp_put_u32(directory_entry(copy, XR_XTP_SECTION_TARGET_PROFILE),
                   XR_XTP_SECTION_MACHINE_REPS);
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);

    memcpy(copy, fixture.bytes, fixture.size);
    entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    entry[40] ^= 1;
    resign_artifact(copy, fixture.size);
    expect_decode_failure(copy, fixture.size);
    xr_free(copy);
    dispose_fixture(&fixture);
}

static void test_identity_and_typed_mutations(void) {
    XtpFixture fixture = make_fixture();
    static const size_t identity_offsets[] = {72, 104, 136, 168, 200, 232, 264};
    for (size_t i = 0; i < sizeof(identity_offsets) / sizeof(identity_offsets[0]); i++) {
        uint8_t *copy = copy_artifact(&fixture);
        copy[identity_offsets[i]] ^= 1;
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    uint8_t *copy = copy_artifact(&fixture);
    uint8_t *function_entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    size_t function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    copy[function_offset + 16] ^= 1;
    resign_section(copy, XR_XTP_SECTION_FUNCTIONS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    function_entry = directory_entry(copy, XR_XTP_SECTION_FUNCTIONS);
    function_offset = (size_t) xr_xtp_take_u64(function_entry + 8);
    copy[function_offset + 20] ^= 1;
    resign_section(copy, XR_XTP_SECTION_FUNCTIONS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    static const size_t slot_mutations[] = {0, 24, 28, 32, 50};
    for (size_t i = 0; i < sizeof(slot_mutations) / sizeof(slot_mutations[0]); i++) {
        copy = copy_artifact(&fixture);
        uint8_t *slot_entry = directory_entry(copy, XR_XTP_SECTION_SLOTS);
        size_t slot_offset = (size_t) xr_xtp_take_u64(slot_entry + 8);
        copy[slot_offset + slot_mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_SLOTS);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    copy = copy_artifact(&fixture);
    uint8_t *slot_entry = directory_entry(copy, XR_XTP_SECTION_SLOTS);
    size_t slot_offset = (size_t) xr_xtp_take_u64(slot_entry + 8);
    uint32_t slot_count = (uint32_t) xr_xtp_take_u64(slot_entry + 24);
    REQUIRE(slot_count >= 2);
    uint32_t slot_size = xr_xtp_take_u32(slot_entry + 32);
    memcpy(copy + slot_offset + slot_size, copy + slot_offset, XR_STABLE_ID_BYTES);
    resign_section(copy, XR_XTP_SECTION_SLOTS);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    copy = copy_artifact(&fixture);
    xr_xtp_put_u64(copy + 312, xr_xtp_take_u64(copy + 312) + 1u);
    resign_artifact(copy, fixture.size);
    expect_materialize_failure(&fixture, copy);
    xr_free(copy);

    static const size_t capability_mutations[] = {4, 8, 10};
    for (size_t i = 0;
         i < sizeof(capability_mutations) / sizeof(capability_mutations[0]);
         i++) {
        copy = copy_artifact(&fixture);
        uint8_t *capability_entry =
            directory_entry(copy, XR_XTP_SECTION_CAPABILITIES);
        size_t capability_offset =
            (size_t) xr_xtp_take_u64(capability_entry + 8);
        REQUIRE(xr_xtp_take_u64(capability_entry + 24) == 2);
        copy[capability_offset + capability_mutations[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_CAPABILITIES);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }

    static const size_t machine_identity_offsets[] = {4, 6, 8, 10};
    for (size_t i = 0;
         i < sizeof(machine_identity_offsets) /
                 sizeof(machine_identity_offsets[0]);
         i++) {
        uint8_t *copy = copy_artifact(&fixture);
        uint8_t *profile_entry =
            directory_entry(copy, XR_XTP_SECTION_TARGET_PROFILE);
        size_t profile_offset =
            (size_t) xr_xtp_take_u64(profile_entry + 8);
        copy[profile_offset + machine_identity_offsets[i]] ^= 1;
        resign_section(copy, XR_XTP_SECTION_TARGET_PROFILE);
        resign_artifact(copy, fixture.size);
        expect_materialize_failure(&fixture, copy);
        xr_free(copy);
    }
    dispose_fixture(&fixture);
}

static int write_fixture(const char *path) {
    XtpFixture fixture = make_fixture();
    FILE *file = fopen(path, "wb");
    if (!file) {
        dispose_fixture(&fixture);
        return 1;
    }
    bool written = fwrite(fixture.bytes, 1, fixture.size, file) == fixture.size;
    bool closed = fclose(file) == 0;
    dispose_fixture(&fixture);
    return written && closed ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_fixture(argv[2]);
    test_artifact_classifier();
    test_wire_row_inventory();
    test_every_typed_row_codec();
    test_exact_roundtrip_and_owned_candidate();
    test_direct_call_rows_roundtrip_and_mutate();
    test_coroutine_rows_roundtrip_and_mutate();
    test_header_and_directory_mutations();
    test_identity_and_typed_mutations();
    test_runtime_machine_authority_is_exact_and_scalar_only();
    test_runtime_factory_owns_native_profile();
    test_independent_verifier_rejects_forged_machine_profiles();
    test_runtime_load_rejects_foreign_profile_artifacts();
    test_runtime_load_materializes_only_verified_plan();
    puts("typed XTP format tests passed");
    return 0;
}
