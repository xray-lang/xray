/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_execution.c - Exact target and provider execution binding tests
 */

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/execution/xr_boundary_materialization.h"
#include "../../../src/execution/xr_execution.h"
#include "../../../src/os/os_thread.h"
#include "../../../src/plan/semantic/xr_semantic_ids.h"
#include "../../../src/program/xr_program.h"
#include "../plan/target_profile_test_fixture.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct TestProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} TestProviderBindings;

static void require_fingerprint(XrFingerprint fingerprint, const char *expected) {
    char hex[XR_FINGERPRINT_BYTES * 2u + 1u];
    xr_fingerprint_hex(fingerprint, hex);
    if (strcmp(hex, expected) != 0)
        fprintf(stderr, "fingerprint mismatch: expected %s, got %s\n", expected, hex);
    REQUIRE(strcmp(hex, expected) == 0);
}

static XrCoreIrKey test_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrValidatedProgram *build_validated_program(void) {
    XrCoreIrConstantInput constant = {
        .key = test_key("execution:constant:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey value = test_key("execution:value:42");
    XrCoreIrKey return_operand[] = {value};
    XrCoreIrInstructionInput instructions[] = {
        {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = value,
            .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant.key,
        },
        {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = return_operand,
            .operand_count = 1,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    XrCoreIrKey block_key = test_key("execution:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = 2,
    };
    XrCoreIrFunctionInput function = {
        .key = test_key("execution:function:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = 1u,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = test_key("execution:module"),
        .constants = &constant,
        .constant_count = 1,
        .functions = &function,
        .function_count = 1,
    };
    XrCoreIrKey semantic_profile = test_key("execution:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic_profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1,
        .modules = &module,
        .module_count = 1,
    };
    XrCoreIrProgram *core_program = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    char build_diagnostic[256] = {0};
    REQUIRE(xr_core_ir_program_build(&input, &core_program, build_diagnostic,
                                     sizeof(build_diagnostic)) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_write(core_program, &artifact, build_diagnostic, sizeof(build_diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated,
                                &verify_diagnostic) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core_program);
    REQUIRE(validated != NULL);
    return validated;
}

static void test_provider_entry(void) {
}

static void build_provider_bindings(const XrTargetProfile *profile,
                                    TestProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_target_profile_provider_count(profile);
    REQUIRE(bindings->count > 0);
    for (size_t provider_index = 0; provider_index < bindings->count; ++provider_index) {
        const XrTargetProviderContract *contract =
            xr_target_profile_provider(profile, provider_index);
        REQUIRE(contract != NULL);
        XrProviderBinding *provider = &bindings->providers[provider_index];
        provider->contract_id = contract->contract_id;
        REQUIRE(xr_target_provider_contract_fingerprint(
                    contract, &provider->contract_fingerprint) == XR_RUNTIME_ABI_OK);
        provider->behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider->operations = bindings->operations[provider_index];
        provider->operation_count = contract->operation_count;
        for (uint16_t operation_index = 0; operation_index < contract->operation_count;
             ++operation_index) {
            XrProviderOperationBinding *operation =
                &bindings->operations[provider_index][operation_index];
            operation->operation_id = contract->operations[operation_index].stable_id;
            operation->entry = test_provider_entry;
        }
    }
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile,
                                   const TestProviderBindings *bindings, uint64_t generation) {
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings->providers,
        .provider_count = bindings->count,
        .generation = generation,
    };
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = NULL;
    REQUIRE(xr_execution_instance_create(&input, &instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_NONE);
    REQUIRE(instance != NULL);
    return instance;
}

static void retire_and_free(XrInstance **instance) {
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(*instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(*instance == NULL);
}

static void test_profile_partitions_and_foreign_authority(void) {
    XrTargetProfile *native = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *same = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *foreign = xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(native && same && foreign);

    require_fingerprint(xr_target_profile_fingerprint(native),
                        "996af3b1e0c6a0416f0ad66a7dfd1fc73ee77fdce7c2c7e864af7bf110a942cf");
    require_fingerprint(xr_target_profile_target_semantics_id(native),
                        "dd824775cc3c64949d9d8421f230fc506a1a5f7018479442e56eba9b7d638d72");
    require_fingerprint(xr_target_profile_boundary_abi(native)->id,
                        "e32eb4398c14d6e4998cdc857710cfe4b858166f825063b7bc672198b53d0cf4");
    require_fingerprint(xr_target_profile_runtime_kernel(native)->id,
                        "fbebecd9114ba1ad75b8b121c9f6e09aac9d0725ac2c72b503f7ce60dec8cf4a");
    require_fingerprint(xr_target_profile_provider_contract_set_id(native),
                        "3142fbdb7105d9da1029f25f02cbd4c71c79a3ce2f0c9e870ebaff90f755812f");
    require_fingerprint(xr_target_profile_fingerprint(foreign),
                        "d8ce347d62b3d035a442528ad5d8f8d97acf2fbe2d563b5a8ea551540298f8df");
    require_fingerprint(xr_target_profile_target_semantics_id(foreign),
                        "1787c35ebce26c68f77df851158611abc6f72fb786cb714b58e11b67886f1ff6");
    require_fingerprint(xr_target_profile_boundary_abi(foreign)->id,
                        "9396b084e428eb6515060056163900befef766e0cfdc5b5b0c155008188efd96");
    require_fingerprint(xr_target_profile_runtime_kernel(foreign)->id,
                        "65511a917abd83be9c24a1521e77eb9beca6fa3b3be6417ff176d7317e22471d");
    require_fingerprint(xr_target_profile_provider_contract_set_id(foreign),
                        "136a4ef4f9f56df1e4b8f3ba4323124860f852cedca14dd9a9842a1579a53359");

    REQUIRE(xr_fingerprint_equal(xr_target_profile_fingerprint(native),
                                 xr_target_profile_fingerprint(same)));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_target_semantics_id(native),
                                 xr_target_profile_target_semantics_id(same)));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_boundary_abi(native)->id,
                                 xr_target_profile_boundary_abi(same)->id));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_runtime_kernel(native)->id,
                                 xr_target_profile_runtime_kernel(same)->id));
    REQUIRE(xr_fingerprint_equal(xr_target_profile_provider_contract_set_id(native),
                                 xr_target_profile_provider_contract_set_id(same)));

    REQUIRE(!xr_fingerprint_equal(xr_target_profile_fingerprint(native),
                                  xr_target_profile_fingerprint(foreign)));
    REQUIRE(!xr_fingerprint_equal(xr_target_profile_target_semantics_id(native),
                                  xr_target_profile_target_semantics_id(foreign)));
    REQUIRE(!xr_fingerprint_equal(xr_target_profile_boundary_abi(native)->id,
                                  xr_target_profile_boundary_abi(foreign)->id));
    REQUIRE(xr_target_profile_machine_facts(foreign)->architecture == XR_TARGET_ARCH_WASM32);
    REQUIRE(xr_target_profile_machine_facts(foreign)->operating_system == XR_TARGET_OS_WASI);
    REQUIRE(xr_target_profile_boundary_abi(foreign)->pointer_size == 4);
    REQUIRE(xr_target_profile_boundary_abi(native)->pointer_size == 8);

    const XrBoundaryAbi *boundary = xr_target_profile_boundary_abi(foreign);
    REQUIRE(boundary->schema_version == XR_BOUNDARY_ABI_SCHEMA_VERSION);
    REQUIRE(boundary->value_count == XR_BOUNDARY_ABI_VALUE_COUNT);
    REQUIRE(boundary->call_convention == XR_BOUNDARY_CALL_FRAME_V1);
    REQUIRE(boundary->error_model == XR_BOUNDARY_ERROR_TYPED_CODE);
    REQUIRE(boundary->aggregate_layout_model ==
            XR_BOUNDARY_AGGREGATE_LAYOUT_DECLARATION_ORDER_NATURAL);
    REQUIRE(boundary->variant_layout_model == XR_BOUNDARY_VARIANT_LAYOUT_U32_TAG_NATURAL_PAYLOAD);
    REQUIRE(boundary->root_model == XR_BOUNDARY_ROOT_MODEL_EXPLICIT_OFFSETS);
    REQUIRE(boundary->cleanup_model == XR_BOUNDARY_CLEANUP_MODEL_EXPLICIT_ACTIONS);
    REQUIRE(boundary->variant_tag_type == XR_CORE_TYPE_U32);
    REQUIRE(boundary->values[0].type_id == XR_CORE_TYPE_VOID);
    REQUIRE(boundary->values[0].size == 0);
    REQUIRE(boundary->values[1].type_id == XR_CORE_TYPE_BOOL);
    REQUIRE(boundary->values[1].size == 1);
    REQUIRE(boundary->values[2].type_id == XR_CORE_TYPE_I64);
    REQUIRE(boundary->values[2].size == 8);
    REQUIRE(boundary->values[3].type_id == XR_CORE_TYPE_U32);
    REQUIRE(boundary->values[3].size == 4);
    REQUIRE(boundary->values[4].type_id == XR_CORE_TYPE_ERROR);
    REQUIRE(boundary->values[4].representation == XR_BOUNDARY_VALUE_TYPED_ERROR_CODE);

    xr_target_profile_free(foreign);
    xr_target_profile_free(same);
    xr_target_profile_free(native);
}

static void test_execution_identity_and_lifecycle(void) {
    XrValidatedProgram *program = build_validated_program();
    XrTargetProfile *first_profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *same_profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetProfile *foreign_profile =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    TestProviderBindings first_bindings;
    TestProviderBindings same_bindings;
    TestProviderBindings foreign_bindings;
    build_provider_bindings(first_profile, &first_bindings);
    build_provider_bindings(same_profile, &same_bindings);
    build_provider_bindings(foreign_profile, &foreign_bindings);

    XrInstance *first = create_instance(program, first_profile, &first_bindings, 1);
    XrInstance *same = create_instance(program, same_profile, &same_bindings, 1);
    XrInstance *foreign = create_instance(program, foreign_profile, &foreign_bindings, 1);
    require_fingerprint(xr_execution_instance_id(first),
                        "3d1417f4379c282bfe012eadd1b13ea7626691e04cd476a473c07d0be74b8e9c");
    require_fingerprint(xr_execution_instance_id(foreign),
                        "c061ddc44857ad05e9a4643238c78a028cfd738050851afc2b3abbc88222d968");
    REQUIRE(xr_fingerprint_equal(xr_execution_instance_id(first), xr_execution_instance_id(same)));
    REQUIRE(
        !xr_fingerprint_equal(xr_execution_instance_id(first), xr_execution_instance_id(foreign)));
    REQUIRE(xr_execution_instance_program(first) == program);
    REQUIRE(xr_execution_instance_profile(first) == first_profile);

    REQUIRE(xr_execution_instance_pin(first));
    REQUIRE(xr_execution_instance_pin_count(first) == 1);
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(first, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(!xr_execution_instance_pin(first));
    REQUIRE(xr_execution_instance_retire(first, &diagnostic) == XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);
    xr_execution_instance_unpin(first);
    xr_execution_instance_unpin(first);
    REQUIRE(xr_execution_instance_pin_count(first) == 0);
    REQUIRE(xr_execution_instance_retire(first, &diagnostic) == XR_EXECUTION_OK);

    XrExecutionCacheKey retired_key = xr_execution_instance_cache_key(first);
    XrInstance *successor = NULL;
    REQUIRE(xr_execution_instance_create_successor(first, first_bindings.providers,
                                                   first_bindings.count, &successor,
                                                   &diagnostic) == XR_EXECUTION_OK);
    XrExecutionCacheKey successor_key = xr_execution_instance_cache_key(successor);
    REQUIRE(xr_fingerprint_equal(retired_key.execution_id, successor_key.execution_id));
    REQUIRE(retired_key.generation == 1);
    REQUIRE(successor_key.generation == 2);
    REQUIRE(xr_execution_instance_free(&first, &diagnostic) == XR_EXECUTION_OK);
    retire_and_free(&successor);
    retire_and_free(&foreign);
    retire_and_free(&same);

    xr_target_profile_free(foreign_profile);
    xr_target_profile_free(same_profile);
    xr_target_profile_free(first_profile);
    xr_validated_program_free(program);
}

enum {
    PIN_RACE_THREADS = 4,
    PIN_RACE_ROUNDS = 20000,
};

typedef struct PinRace {
    XrInstance *instance;
    atomic_bool start;
    atomic_uint_least64_t acquired;
    atomic_uint_least64_t refused;
} PinRace;

static void *pin_race_worker(void *opaque) {
    PinRace *race = opaque;
    while (!atomic_load_explicit(&race->start, memory_order_acquire)) {
    }
    for (uint32_t round = 0; round < PIN_RACE_ROUNDS; ++round) {
        if (xr_execution_instance_pin(race->instance)) {
            atomic_fetch_add_explicit(&race->acquired, 1u, memory_order_relaxed);
            xr_execution_instance_unpin(race->instance);
        } else {
            atomic_fetch_add_explicit(&race->refused, 1u, memory_order_relaxed);
        }
    }
    return NULL;
}

static void test_concurrent_pin_and_drain(void) {
    XrValidatedProgram *program = build_validated_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrInstance *instance = create_instance(program, profile, &bindings, 1);
    PinRace race = {
        .instance = instance,
        .start = ATOMIC_VAR_INIT(false),
        .acquired = ATOMIC_VAR_INIT(0),
        .refused = ATOMIC_VAR_INIT(0),
    };
    xr_thread_t threads[PIN_RACE_THREADS];
    for (size_t index = 0; index < PIN_RACE_THREADS; ++index)
        REQUIRE(xr_thread_create(&threads[index], pin_race_worker, &race));
    atomic_store_explicit(&race.start, true, memory_order_release);
    while (atomic_load_explicit(&race.acquired, memory_order_acquire) < 100u) {
    }
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK);
    for (size_t index = 0; index < PIN_RACE_THREADS; ++index)
        REQUIRE(xr_thread_join(threads[index], NULL) == 0);
    REQUIRE(atomic_load_explicit(&race.acquired, memory_order_acquire) >= 100u);
    REQUIRE(atomic_load_explicit(&race.refused, memory_order_acquire) > 0u);
    REQUIRE(xr_execution_instance_pin_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void require_provider_reject(XrExecutionBindingInput *input,
                                    XrExecutionDiagnosticKind expected) {
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = (XrInstance *) (uintptr_t) 1;
    REQUIRE(xr_execution_instance_create(input, &instance, &diagnostic) ==
            XR_EXECUTION_PROVIDER_REJECTED);
    REQUIRE(instance == NULL);
    REQUIRE(diagnostic.kind == expected);
}

static void test_provider_admission_matrix(void) {
    XrValidatedProgram *program = build_validated_program();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    TestProviderBindings bindings;
    build_provider_bindings(profile, &bindings);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings.providers,
        .provider_count = bindings.count,
        .generation = 1,
    };

    input.provider_count--;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT);
    input.provider_count++;

    bindings.providers[0].contract_id.bytes[0] ^= 1u;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT);
    bindings.providers[0].contract_id.bytes[0] ^= 1u;

    bindings.operations[0][0].operation_id.bytes[0] ^= 1u;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION);
    bindings.operations[0][0].operation_id.bytes[0] ^= 1u;
    bindings.operations[0][0].entry = NULL;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION);
    bindings.operations[0][0].entry = test_provider_entry;

    bindings.providers[0].behavior_flags &= ~XR_PROVIDER_BEHAVIOR_REENTRANT;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR);
    bindings.providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
    bindings.providers[0].behavior_flags &= ~XR_PROVIDER_BEHAVIOR_THREAD_SAFE;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR);
    bindings.providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;

    XrTargetProfile *foreign = xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    input.profile = foreign;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT);

    xr_target_profile_free(foreign);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

int main(void) {
    test_profile_partitions_and_foreign_authority();
    test_execution_identity_and_lifecycle();
    test_concurrent_pin_and_drain();
    test_provider_admission_matrix();
    puts("execution binding tests passed");
    return 0;
}
