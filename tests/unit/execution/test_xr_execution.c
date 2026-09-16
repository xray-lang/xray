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
#include "../program/xr_program_provider_fixture.h"
#include "../program/xr_program_module_fixture.h"
#include "../../../src/program/xr_validated_program_internal.h"

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

static XrValidatedProgram *build_validated_provider_program(
    const XrTargetProfile *requirements_profile, bool nullary) {
    XrCoreIrConstantInput constant = {
        .key = test_key("execution:constant:42"),
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey value = test_key("execution:value:42");
    XrCoreIrKey provider_value = test_key("execution:value:provider-result");
    XrCoreIrKey provider_operand[] = {value};
    XrCoreIrKey return_operand[] = {provider_value};
    XrCoreIrInstructionInput instructions[3] = {0};
    uint32_t instruction_count = 0u;
    if (!nullary) {
        instructions[instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = value,
            .result_type_id = XR_CORE_TYPE_I64,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant.key,
        };
    }
    uint32_t provider_instruction = instruction_count;
    instructions[instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
        .result = provider_value,
        .result_type_id = XR_CORE_TYPE_I64,
        .operands = nullary ? NULL : provider_operand,
        .operand_count = nullary ? 0u : 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
    };
    instructions[instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = return_operand,
            .operand_count = 1,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrKey block_key = test_key("execution:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = instruction_count,
    };
    XrCoreIrFunctionInput function = {
        .key = test_key("execution:function:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = test_key("execution:module"),
        .constants = nullary ? NULL : &constant,
        .constant_count = nullary ? 0u : 1u,
        .functions = &function,
        .function_count = 1,
    };
    XrCoreIrKey semantic_profile = test_key("execution:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    const XrTargetProviderContract *required_contract = NULL;
    for (size_t provider = 0u;
         provider < xr_target_profile_provider_count(requirements_profile); ++provider) {
        const XrTargetProviderContract *candidate =
            xr_target_profile_provider(requirements_profile, provider);
        if (xr_test_target_profile_is_scalar_provider(candidate)) {
            REQUIRE(required_contract == NULL);
            required_contract = candidate;
        }
    }
    REQUIRE(required_contract != NULL && required_contract->operation_count != 0u);
    XrStableId operation_id = required_contract->operations[0].stable_id;
    instructions[provider_instruction].immediate.provider_operation.contract_id =
        required_contract->contract_id;
    instructions[provider_instruction].immediate.provider_operation.operation_id = operation_id;
    XrProgramProviderOperationRequirement operation_requirement = {
        .operation_id = operation_id,
        .logical_contract = xr_program_fixture_scalar_contract(nullary),
    };
    XrCoreIrProviderRequirementInput provider_requirement = {
        .contract_id = required_contract->contract_id,
        .operations = &operation_requirement,
        .operation_count = 1u,
    };
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic_profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1,
        .provider_requirements = &provider_requirement,
        .provider_requirement_count = 1u,
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
    XrProgramProviderRequirementView requirement_view = {0};
    REQUIRE(xr_validated_program_provider_requirement_count(validated) == 1u);
    REQUIRE(xr_validated_program_provider_requirement(validated, 0u, &requirement_view));
    REQUIRE(memcmp(requirement_view.contract_id.bytes, required_contract->contract_id.bytes,
                   XR_STABLE_ID_BYTES) == 0);
    REQUIRE(requirement_view.operation_count == 1u);
    REQUIRE(memcmp(requirement_view.operations[0].operation_id.bytes, operation_id.bytes,
                   XR_STABLE_ID_BYTES) == 0);
    return validated;
}

static XrValidatedProgram *build_validated_program(
    const XrTargetProfile *requirements_profile) {
    return build_validated_provider_program(requirements_profile, false);
}

static XrProviderCallStatus test_provider_entry(void *context, int64_t argument,
                                                int64_t *result_out) {
    if (!context || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = argument + 1;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus test_nullary_provider_entry(void *context, int64_t *result_out) {
    if (!context || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = 73;
    return XR_PROVIDER_CALL_OK;
}

typedef struct ReentrantProviderContext {
    XrInstance *instance;
    XrExecutionLease *lease;
    uint32_t depth;
} ReentrantProviderContext;

static XrProviderCallStatus reentrant_provider_entry(void *opaque, int64_t argument,
                                                     int64_t *result_out) {
    ReentrantProviderContext *context = opaque;
    if (!context || !context->instance || !context->lease || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    if (context->depth == 0u) {
        XrExecutionLease alias = *context->lease;
        XrExecutionDiagnostic diagnostic;
        REQUIRE(!xr_execution_lease_release(&alias));
        REQUIRE(xr_execution_instance_begin_drain(context->instance, &diagnostic) ==
                XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(context->instance, &diagnostic) ==
                XR_EXECUTION_GENERATION_REJECTED);
        REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);
        ++context->depth;
        int64_t nested = 0;
        XrExecutionProviderCallResult call = xr_execution_lease_provider_call_i64_unary(
            context->lease, 0u, 0u, argument, &nested);
        --context->depth;
        if (call != XR_EXECUTION_PROVIDER_CALL_OK)
            return XR_PROVIDER_CALL_FAILED;
        *result_out = nested + 1;
        return XR_PROVIDER_CALL_OK;
    }
    *result_out = argument + 1;
    return XR_PROVIDER_CALL_OK;
}

typedef struct BlockingProviderContext {
    atomic_bool entered;
    atomic_bool return_allowed;
} BlockingProviderContext;

typedef struct BlockingProviderCall {
    const XrExecutionLease *lease;
    XrExecutionProviderCallResult status;
    int64_t result;
} BlockingProviderCall;

static XrProviderCallStatus blocking_provider_entry(void *opaque, int64_t argument,
                                                    int64_t *result_out) {
    BlockingProviderContext *context = opaque;
    if (!context || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    atomic_store_explicit(&context->entered, true, memory_order_release);
    while (!atomic_load_explicit(&context->return_allowed, memory_order_acquire))
        xr_thread_yield();
    *result_out = argument + 1;
    return XR_PROVIDER_CALL_OK;
}

static void *blocking_provider_call_worker(void *opaque) {
    BlockingProviderCall *call = opaque;
    call->status =
        xr_execution_lease_provider_call_i64_unary(call->lease, 0u, 0u, 41, &call->result);
    return NULL;
}

static const XrTargetProviderContract *find_profile_contract(const XrTargetProfile *profile,
                                                             XrStableId contract_id) {
    for (size_t index = 0; index < xr_target_profile_provider_count(profile); ++index) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, index);
        if (candidate && memcmp(candidate->contract_id.bytes, contract_id.bytes,
                                XR_STABLE_ID_BYTES) == 0)
            return candidate;
    }
    return NULL;
}

static const XrTargetProviderOperationContract *find_profile_operation(
    const XrTargetProviderContract *contract, XrStableId operation_id) {
    for (uint16_t index = 0; contract && index < contract->operation_count; ++index) {
        if (memcmp(contract->operations[index].stable_id.bytes, operation_id.bytes,
                   XR_STABLE_ID_BYTES) == 0)
            return &contract->operations[index];
    }
    return NULL;
}

static void build_provider_bindings(const XrValidatedProgram *program,
                                    const XrTargetProfile *profile,
                                    TestProviderBindings *bindings) {
    memset(bindings, 0, sizeof(*bindings));
    bindings->count = xr_validated_program_provider_requirement_count(program);
    REQUIRE(bindings->count > 0);
    for (size_t provider_index = 0; provider_index < bindings->count; ++provider_index) {
        XrProgramProviderRequirementView requirement = {0};
        REQUIRE(xr_validated_program_provider_requirement(program, (uint32_t) provider_index,
                                                          &requirement));
        const XrTargetProviderContract *contract =
            find_profile_contract(profile, requirement.contract_id);
        REQUIRE(contract != NULL);
        XrProviderBinding *provider = &bindings->providers[provider_index];
        provider->contract_id = contract->contract_id;
        REQUIRE(xr_target_provider_contract_fingerprint(
                    contract, &provider->contract_fingerprint) == XR_RUNTIME_ABI_OK);
        provider->behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        provider->operations = bindings->operations[provider_index];
        provider->operation_count = (uint16_t) requirement.operation_count;
        for (uint16_t operation_index = 0; operation_index < provider->operation_count;
             ++operation_index) {
            const XrTargetProviderOperationContract *contract_operation = find_profile_operation(
                contract, requirement.operations[operation_index].operation_id);
            REQUIRE(contract_operation != NULL);
            XrProviderOperationBinding *operation =
                &bindings->operations[provider_index][operation_index];
            operation->operation_id = contract_operation->stable_id;
            if (contract_operation->call_abi.parameter_count == 0u) {
                operation->trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
                operation->entry.i64_nullary = test_nullary_provider_entry;
            } else {
                REQUIRE(contract_operation->call_abi.parameter_count == 1u);
                operation->trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_UNARY;
                operation->entry.i64_unary = test_provider_entry;
            }
            operation->context = operation;
        }
    }
}

static XrInstance *create_instance(XrValidatedProgram *program, XrTargetProfile *profile,
                                   const TestProviderBindings *bindings, uint64_t generation) {
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = bindings->count ? bindings->providers : NULL,
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
                        "378857327ac945ad3254e6ed808095ae2344aa860d038818e3d3d0354cbdf96e");
    require_fingerprint(xr_target_profile_target_semantics_id(native),
                        "dd824775cc3c64949d9d8421f230fc506a1a5f7018479442e56eba9b7d638d72");
    require_fingerprint(xr_target_profile_boundary_abi(native)->id,
                        "1f77211f058a7a6464d002a86dc4121160f3fcedfa12b0dbee47e6bdbd5d7464");
    require_fingerprint(xr_target_profile_runtime_kernel(native)->id,
                        "b4154c1d6fdfc6e54c2ae30b6873e11ec308b1cbf4148ef7428fed0c05f4ec55");
    require_fingerprint(xr_target_profile_provider_contract_set_id(native),
                        "61a7ba4473275dd3e025ac20280f7ede74f2bea1dcf1f5a6ea3aaf05558cac98");
    require_fingerprint(xr_target_profile_fingerprint(foreign),
                        "091656d302eea37d9a09a5e19569841744dd795602948ad47c68f686676702d7");
    require_fingerprint(xr_target_profile_target_semantics_id(foreign),
                        "1787c35ebce26c68f77df851158611abc6f72fb786cb714b58e11b67886f1ff6");
    require_fingerprint(xr_target_profile_boundary_abi(foreign)->id,
                        "afccc033804cac95af572d15ba7735271bab46e47cc53cb14256d32d130ad0ed");
    require_fingerprint(xr_target_profile_runtime_kernel(foreign)->id,
                        "1fe7176670e77c0a0fa15ad3ca430320eff7e15a0854a3a76e6b16f67a69ad11");
    require_fingerprint(xr_target_profile_provider_contract_set_id(foreign),
                        "d9fe3d2bc1bcfb6939cde134820ac22407ad89d96e7034726a9a4fd237ad8293");

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
    REQUIRE(boundary->values[5].type_id == XR_CORE_TYPE_U16);
    REQUIRE(boundary->values[5].representation == XR_BOUNDARY_VALUE_UNSIGNED_INTEGER);
    REQUIRE(boundary->values[5].ownership == XR_BOUNDARY_OWNERSHIP_COPY);
    REQUIRE(boundary->values[5].size == 2);
    REQUIRE(boundary->values[5].alignment == 2);

    xr_target_profile_free(foreign);
    xr_target_profile_free(same);
    xr_target_profile_free(native);
}

static void test_execution_identity_and_lifecycle(void) {
    XrTargetProfile *first_profile =
        xr_test_target_profile_build_with_scalar_clock(
            false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
            XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrTargetProfile *same_profile =
        xr_test_target_profile_build_with_scalar_clock(
            false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
            XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrTargetProfile *foreign_profile =
        xr_test_target_profile_build_with_scalar_clock(
            true, XR_TARGET_RUNTIME_PROFILE_HOSTED,
            XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrValidatedProgram *program = build_validated_program(first_profile);
    TestProviderBindings first_bindings;
    TestProviderBindings same_bindings;
    TestProviderBindings foreign_bindings;
    build_provider_bindings(program, first_profile, &first_bindings);
    build_provider_bindings(program, same_profile, &same_bindings);
    build_provider_bindings(program, foreign_profile, &foreign_bindings);

    XrInstance *first = create_instance(program, first_profile, &first_bindings, 1);
    XrInstance *same = create_instance(program, same_profile, &same_bindings, 1);
    XrInstance *foreign = create_instance(program, foreign_profile, &foreign_bindings, 1);
    require_fingerprint(xr_execution_instance_id(first),
                        "de7d406a812502e67f219de215430c1b845528005deacee80edab634c8626ad5");
    require_fingerprint(xr_execution_instance_id(foreign),
                        "89b7da9aa281ee6ec4a2e76f8af145692bf08770c2199c5f9108bd26b46ae54c");
    REQUIRE(xr_fingerprint_equal(xr_execution_instance_id(first), xr_execution_instance_id(same)));
    REQUIRE(
        !xr_fingerprint_equal(xr_execution_instance_id(first), xr_execution_instance_id(foreign)));
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(first, &lease));
    XrExecutionLease copied_lease = lease;
    XrExecutionLease independent_lease = {0};
    REQUIRE(xr_execution_instance_acquire(first, &independent_lease));
    REQUIRE(xr_execution_lease_is_valid(&lease));
    REQUIRE(xr_execution_lease_is_valid(&copied_lease));
    REQUIRE(xr_execution_lease_is_valid(&independent_lease));
    XrValidatedProgram *retained_program = xr_execution_lease_retain_program(&lease);
    XrTargetProfile *retained_profile = xr_execution_lease_retain_profile(&lease);
    REQUIRE(retained_program == program);
    REQUIRE(retained_profile == first_profile);
    xr_target_profile_free(retained_profile);
    xr_validated_program_free(retained_program);
    REQUIRE(!xr_execution_instance_acquire(first, &lease));
    REQUIRE(xr_execution_instance_lease_count(first) == 2);
    REQUIRE(lease.ticket != 0u && lease.ticket != independent_lease.ticket);
    REQUIRE(xr_execution_instance_generation(first) == 1u);
    int64_t provider_result = 0;
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&lease, 0u, 0u, 41, &provider_result) ==
            XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(provider_result == 42);
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&lease, 0u, 0u, 41, NULL) ==
            XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE);
    REQUIRE(xr_execution_instance_lease_count(first) == 2);
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(first, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&lease, 0u, 0u, -2, &provider_result) ==
            XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(provider_result == -1);
    XrExecutionLease refused = {.instance = first, .ticket = UINT64_MAX};
    REQUIRE(!xr_execution_instance_acquire(first, &refused));
    REQUIRE(!xr_execution_lease_is_valid(&refused));
    REQUIRE(xr_execution_instance_retire(first, &diagnostic) == XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);
    REQUIRE(xr_execution_lease_release(&lease));
    REQUIRE(xr_execution_instance_lease_count(first) == 1);
    REQUIRE(!xr_execution_lease_is_valid(&copied_lease));
    REQUIRE(!xr_execution_lease_release(&copied_lease));
    REQUIRE(xr_execution_instance_lease_count(first) == 1);
    REQUIRE(xr_execution_lease_is_valid(&independent_lease));
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&independent_lease, 0u, 0u, 0,
                                                 &provider_result) ==
            XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(provider_result == 1);
    REQUIRE(!xr_execution_lease_is_valid(&lease));
    REQUIRE(xr_execution_lease_retain_program(&lease) == NULL);
    REQUIRE(xr_execution_lease_retain_profile(&lease) == NULL);
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&lease, 0u, 0u, 0, &provider_result) ==
            XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE);
    REQUIRE(!xr_execution_lease_release(&lease));
    REQUIRE(xr_execution_lease_release(&independent_lease));
    REQUIRE(xr_execution_instance_lease_count(first) == 0);
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

static void test_reentrant_provider_call_pins_lease_without_holding_lock(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
        XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrValidatedProgram *program = build_validated_program(profile);
    TestProviderBindings bindings;
    build_provider_bindings(program, profile, &bindings);
    ReentrantProviderContext context = {0};
    bindings.operations[0][0].entry.i64_unary = reentrant_provider_entry;
    bindings.operations[0][0].context = &context;
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease));
    context.instance = instance;
    context.lease = &lease;

    int64_t result = 0;
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&lease, 0u, 0u, 40, &result) ==
            XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(result == 42);
    REQUIRE(xr_execution_lease_is_valid(&lease));

    XrValidatedProgram *retained_program = xr_execution_lease_retain_program(&lease);
    XrTargetProfile *retained_profile = xr_execution_lease_retain_profile(&lease);
    REQUIRE(retained_program == program);
    REQUIRE(retained_profile == profile);
    REQUIRE(xr_execution_lease_release(&lease));
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_validated_program_provider_requirement_count(retained_program) == 1u);
    REQUIRE(xr_target_profile_provider_count(retained_profile) != 0u);
    xr_target_profile_free(retained_profile);
    xr_validated_program_free(retained_program);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_concurrent_release_drain_and_retire_during_provider_call(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
        XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrValidatedProgram *program = build_validated_program(profile);
    TestProviderBindings bindings;
    build_provider_bindings(program, profile, &bindings);
    BlockingProviderContext provider = {
        .entered = ATOMIC_VAR_INIT(false),
        .return_allowed = ATOMIC_VAR_INIT(false),
    };
    bindings.operations[0][0].entry.i64_unary = blocking_provider_entry;
    bindings.operations[0][0].context = &provider;
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease));
    BlockingProviderCall call = {
        .lease = &lease,
        .status = XR_EXECUTION_PROVIDER_CALL_FAILED,
        .result = 0,
    };
    xr_thread_t worker;
    REQUIRE(xr_thread_create(&worker, blocking_provider_call_worker, &call));
    while (!atomic_load_explicit(&provider.entered, memory_order_acquire))
        xr_thread_yield();

    XrExecutionLease alias = lease;
    REQUIRE(!xr_execution_lease_release(&alias));
    REQUIRE(xr_execution_lease_is_valid(&lease));
    XrExecutionDiagnostic diagnostic;
    REQUIRE(xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) ==
            XR_EXECUTION_GENERATION_REJECTED);
    REQUIRE(diagnostic.kind == XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY);

    atomic_store_explicit(&provider.return_allowed, true, memory_order_release);
    REQUIRE(xr_thread_join(worker, NULL) == 0);
    REQUIRE(call.status == XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(call.result == 42);
    REQUIRE(xr_execution_lease_release(&lease));
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
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
        XrExecutionLease lease = {0};
        if (xr_execution_instance_acquire(race->instance, &lease)) {
            atomic_fetch_add_explicit(&race->acquired, 1u, memory_order_relaxed);
            REQUIRE(xr_execution_lease_release(&lease));
        } else {
            atomic_fetch_add_explicit(&race->refused, 1u, memory_order_relaxed);
        }
    }
    return NULL;
}

static void test_concurrent_pin_and_drain(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_scalar_clock(
            false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
            XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrValidatedProgram *program = build_validated_program(profile);
    TestProviderBindings bindings;
    build_provider_bindings(program, profile, &bindings);
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
    REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
    REQUIRE(xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void require_execution_reject(XrExecutionBindingInput *input, XrExecutionStatus status,
                                     XrExecutionDiagnosticKind expected) {
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = (XrInstance *) (uintptr_t) 1;
    XrExecutionStatus actual = xr_execution_instance_create(input, &instance, &diagnostic);
    if (actual != status || diagnostic.kind != expected)
        fprintf(stderr, "execution rejection mismatch: status=%u expected=%u kind=%u "
                        "expected_kind=%u\n",
                (unsigned) actual, (unsigned) status, (unsigned) diagnostic.kind,
                (unsigned) expected);
    REQUIRE(actual == status);
    REQUIRE(instance == NULL);
    REQUIRE(diagnostic.kind == expected);
}

static void require_provider_reject(XrExecutionBindingInput *input,
                                    XrExecutionDiagnosticKind expected) {
    require_execution_reject(input, XR_EXECUTION_PROVIDER_REJECTED, expected);
}

static void test_provider_admission_matrix(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_scalar_clock(
            false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
            XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    XrValidatedProgram *program = build_validated_program(profile);
    TestProviderBindings bindings;
    build_provider_bindings(program, profile, &bindings);
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
    input.provider_count++;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT);
    input.provider_count--;

    bindings.providers[0].contract_id.bytes[0] ^= 1u;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT);
    bindings.providers[0].contract_id.bytes[0] ^= 1u;

    const XrTargetProviderContract *unrequired_contract = xr_target_profile_provider(profile, 1u);
    REQUIRE(unrequired_contract != NULL && unrequired_contract->operation_count != 0u);
    XrProviderOperationBinding unrequired_operation = {
        .operation_id = unrequired_contract->operations[0].stable_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_UNARY,
        .entry.i64_unary = test_provider_entry,
    };
    unrequired_operation.context = &unrequired_operation;
    XrProviderBinding unrequired_provider = {
        .contract_id = unrequired_contract->contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &unrequired_operation,
        .operation_count = 1u,
    };
    REQUIRE(xr_target_provider_contract_fingerprint(
                unrequired_contract, &unrequired_provider.contract_fingerprint) ==
            XR_RUNTIME_ABI_OK);
    input.providers = &unrequired_provider;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT);
    input.providers = bindings.providers;

    bindings.operations[0][0].operation_id.bytes[0] ^= 1u;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION);
    bindings.operations[0][0].operation_id.bytes[0] ^= 1u;
    bindings.operations[0][0].entry.i64_unary = NULL;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION);
    bindings.operations[0][0].entry.i64_unary = test_provider_entry;
    bindings.providers[0].operation_count++;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION);
    bindings.providers[0].operation_count--;

    bindings.providers[0].behavior_flags &= ~XR_PROVIDER_BEHAVIOR_REENTRANT;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR);
    bindings.providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
    bindings.providers[0].behavior_flags &= ~XR_PROVIDER_BEHAVIOR_THREAD_SAFE;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR);
    bindings.providers[0].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;

    XrTargetProfile *foreign = xr_test_target_profile_build_with_scalar_clock(
        true, XR_TARGET_RUNTIME_PROFILE_HOSTED,
        XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    input.profile = foreign;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT);

    XrTargetProfile *wrong_runtime =
        xr_test_target_profile_build_with_scalar_clock(
            false, XR_TARGET_RUNTIME_PROFILE_FREESTANDING,
            XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(wrong_runtime != NULL);
    input.profile = wrong_runtime;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT);

    XrTargetProfile *wrong_abi = xr_test_target_profile_build_with_scalar_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
        XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER);
    REQUIRE(wrong_abi != NULL);
    TestProviderBindings wrong_abi_bindings;
    build_provider_bindings(program, wrong_abi, &wrong_abi_bindings);
    input.profile = wrong_abi;
    input.providers = wrong_abi_bindings.providers;
    input.provider_count = wrong_abi_bindings.count;
    require_provider_reject(&input, XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI);

    xr_target_profile_free(wrong_abi);
    xr_target_profile_free(wrong_runtime);
    xr_target_profile_free(foreign);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_nullary_provider_call_shape(void) {
    XrTargetProfile *profile = xr_test_target_profile_build_with_nullary_clock(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED,
        XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    REQUIRE(profile != NULL);
    XrValidatedProgram *program = build_validated_provider_program(profile, true);
    TestProviderBindings bindings;
    build_provider_bindings(program, profile, &bindings);
    REQUIRE(bindings.operations[0][0].trampoline_kind ==
            XR_PROVIDER_TRAMPOLINE_I64_NULLARY);
    XrInstance *instance = create_instance(program, profile, &bindings, 1u);
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(instance, &lease));
    XrExecutionInitializationStep step;
    REQUIRE(xr_execution_lease_initialization_next(&lease, &step) ==
            XR_EXECUTION_INITIALIZATION_COMPLETE);
    REQUIRE(step.module_index == UINT32_MAX && step.function_id == UINT32_MAX);
    REQUIRE(!xr_execution_lease_initialization_finish(&lease, 0u, true));
    int64_t result = 0;
    REQUIRE(xr_execution_lease_provider_call_i64_nullary(&lease, 0u, 0u, &result) ==
            XR_EXECUTION_PROVIDER_CALL_OK);
    REQUIRE(result == 73);
    REQUIRE(xr_execution_lease_provider_call_i64_nullary(&lease, 0u, 0u, NULL) ==
            XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE);
    REQUIRE(xr_execution_lease_provider_call_i64_unary(&lease, 0u, 0u, 1, &result) ==
            XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE);
    REQUIRE(xr_execution_lease_release(&lease));
    retire_and_free(&instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static XrValidatedProgram *build_module_program(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    char diagnostic[256] = {0};
    REQUIRE(xr_core_ir_program_build(&fixture.input, &core, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_write(core, &artifact, diagnostic, sizeof(diagnostic)) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    REQUIRE(program->module_count == 4u);
    return program;
}

static void test_module_initialization_authority_and_failure(void) {
    XrValidatedProgram *program = build_module_program();
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings = {0};
    XrInstance *first = create_instance(program, profile, &bindings, 1u);
    XrInstance *second = create_instance(program, profile, &bindings, 1u);
    XrExecutionLease publisher = {0}, waiter = {0}, separate = {0};
    REQUIRE(xr_execution_instance_acquire(first, &publisher));
    REQUIRE(xr_execution_instance_acquire(first, &waiter));
    REQUIRE(xr_execution_instance_acquire(second, &separate));
    XrExecutionInitializationStep step = {0};
    REQUIRE(xr_execution_lease_initialization_next(NULL, &step) ==
            XR_EXECUTION_INITIALIZATION_INVALID);
    REQUIRE(step.module_index == UINT32_MAX && step.function_id == UINT32_MAX);
    REQUIRE(!xr_execution_lease_initialization_finish(&publisher, 0u, true));
    REQUIRE(xr_execution_instance_begin_drain(first, NULL) == XR_EXECUTION_OK);
    XrExecutionLease refused = {0};
    REQUIRE(!xr_execution_instance_acquire(first, &refused));
    for (uint32_t module = 0u; module < 4u; ++module) {
        REQUIRE(xr_execution_lease_initialization_next(&publisher, &step) ==
                XR_EXECUTION_INITIALIZATION_RUN);
        REQUIRE(step.module_index == module);
        REQUIRE(step.function_id == program->modules[module].initializer);
        REQUIRE(xr_execution_lease_initialization_next(&publisher, &step) ==
                XR_EXECUTION_INITIALIZATION_WAIT);
        REQUIRE(xr_execution_lease_initialization_next(&waiter, &step) ==
                XR_EXECUTION_INITIALIZATION_WAIT);
        REQUIRE(step.module_index == UINT32_MAX && step.function_id == UINT32_MAX);
        REQUIRE(!xr_execution_lease_initialization_finish(&waiter, module, true));
        REQUIRE(!xr_execution_lease_initialization_finish(&publisher, module + 1u, true));
        REQUIRE(xr_execution_lease_initialization_finish(&publisher, module, true));
        REQUIRE(!xr_execution_lease_initialization_finish(&publisher, module, true));
        if (module != 3u)
            REQUIRE(xr_execution_lease_initialization_next(&waiter, &step) ==
                    XR_EXECUTION_INITIALIZATION_WAIT);
    }
    REQUIRE(xr_execution_lease_initialization_next(&waiter, &step) ==
            XR_EXECUTION_INITIALIZATION_COMPLETE);
    REQUIRE(xr_execution_lease_initialization_next(&separate, &step) ==
            XR_EXECUTION_INITIALIZATION_RUN);
    REQUIRE(step.module_index == 0u);
    REQUIRE(xr_execution_lease_initialization_finish(&separate, 0u, true));
    REQUIRE(xr_execution_lease_initialization_next(&separate, &step) ==
            XR_EXECUTION_INITIALIZATION_RUN);
    REQUIRE(step.module_index == 1u);
    REQUIRE(xr_execution_lease_initialization_finish(&separate, 1u, false));
    REQUIRE(xr_execution_lease_initialization_next(&separate, &step) ==
            XR_EXECUTION_INITIALIZATION_FAILED);
    REQUIRE(!xr_execution_lease_initialization_finish(&separate, 1u, true));
    REQUIRE(xr_execution_lease_release(&separate));
    REQUIRE(xr_execution_instance_acquire(second, &separate));
    REQUIRE(xr_execution_lease_initialization_next(&separate, &step) ==
            XR_EXECUTION_INITIALIZATION_FAILED);
    REQUIRE(xr_execution_lease_release(&separate));
    XrExecutionLease stale = publisher;
    REQUIRE(xr_execution_lease_release(&publisher));
    REQUIRE(xr_execution_lease_initialization_next(&stale, &step) ==
            XR_EXECUTION_INITIALIZATION_INVALID);
    REQUIRE(xr_execution_lease_initialization_next(&waiter, &step) ==
            XR_EXECUTION_INITIALIZATION_COMPLETE);
    REQUIRE(xr_execution_lease_release(&waiter));
    REQUIRE(xr_execution_instance_retire(first, NULL) == XR_EXECUTION_OK);
    XrInstance *successor = NULL;
    REQUIRE(xr_execution_instance_create_successor(first, NULL, 0u, &successor, NULL) ==
            XR_EXECUTION_OK);
    REQUIRE(xr_execution_instance_generation(successor) == 2u);
    REQUIRE(xr_execution_instance_acquire(successor, &publisher));
    REQUIRE(xr_execution_lease_initialization_next(&publisher, &step) ==
            XR_EXECUTION_INITIALIZATION_RUN);
    REQUIRE(step.module_index == 0u);
    REQUIRE(xr_execution_lease_release(&publisher));
    retire_and_free(&successor);
    REQUIRE(xr_execution_instance_free(&first, NULL) == XR_EXECUTION_OK);
    retire_and_free(&second);

    for (unsigned active = 0u; active < 2u; ++active) {
        first = create_instance(program, profile, &bindings, 1u);
        REQUIRE(xr_execution_instance_acquire(first, &publisher));
        REQUIRE(xr_execution_instance_acquire(first, &waiter));
        REQUIRE(xr_execution_lease_initialization_next(&publisher, &step) ==
                XR_EXECUTION_INITIALIZATION_RUN);
        if (!active)
            REQUIRE(xr_execution_lease_initialization_finish(&publisher, 0u, true));
        REQUIRE(xr_execution_instance_begin_drain(first, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_retire(first, NULL) == XR_EXECUTION_GENERATION_REJECTED);
        REQUIRE(xr_execution_lease_release(&publisher));
        REQUIRE(xr_execution_lease_initialization_next(&waiter, &step) ==
                XR_EXECUTION_INITIALIZATION_FAILED);
        REQUIRE(xr_execution_lease_release(&waiter));
        REQUIRE(xr_execution_instance_retire(first, NULL) == XR_EXECUTION_OK);
        REQUIRE(xr_execution_instance_free(&first, NULL) == XR_EXECUTION_OK);
    }
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

typedef struct InitializationRace {
    XrInstance *instance;
    atomic_bool start;
    uint32_t published[4];
} InitializationRace;

static void *initialize_module_worker(void *opaque) {
    InitializationRace *race = opaque;
    XrExecutionLease lease = {0};
    REQUIRE(xr_execution_instance_acquire(race->instance, &lease));
    while (!atomic_load_explicit(&race->start, memory_order_acquire))
        xr_thread_yield();
    for (;;) {
        XrExecutionInitializationStep step;
        XrExecutionInitializationStatus status =
            xr_execution_lease_initialization_next(&lease, &step);
        if (status == XR_EXECUTION_INITIALIZATION_COMPLETE)
            break;
        if (status == XR_EXECUTION_INITIALIZATION_WAIT) {
            xr_thread_yield();
            continue;
        }
        REQUIRE(status == XR_EXECUTION_INITIALIZATION_RUN && step.module_index < 4u);
        REQUIRE(race->published[step.module_index] == 0u);
        if (step.module_index)
            REQUIRE(race->published[step.module_index - 1u] == 1u);
        ++race->published[step.module_index];
        xr_thread_yield();
        REQUIRE(xr_execution_lease_initialization_finish(&lease, step.module_index, true));
    }
    for (uint32_t module = 0u; module < 4u; ++module)
        REQUIRE(race->published[module] == 1u);
    REQUIRE(xr_execution_lease_release(&lease));
    return NULL;
}

static void test_concurrent_module_initialization(void) {
    XrValidatedProgram *program = build_module_program();
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    TestProviderBindings bindings = {0};
    InitializationRace race = {.instance = create_instance(program, profile, &bindings, 1u)};
    atomic_init(&race.start, false);
    xr_thread_t threads[4];
    for (size_t index = 0u; index < 4u; ++index)
        REQUIRE(xr_thread_create(&threads[index], initialize_module_worker, &race));
    atomic_store_explicit(&race.start, true, memory_order_release);
    for (size_t index = 0u; index < 4u; ++index)
        REQUIRE(xr_thread_join(threads[index], NULL) == 0);
    retire_and_free(&race.instance);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "module-initialization") == 0) {
        test_module_initialization_authority_and_failure();
        test_concurrent_module_initialization();
        puts("module initialization authority tests passed");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "provider-contract") == 0) {
        test_reentrant_provider_call_pins_lease_without_holding_lock();
        test_concurrent_release_drain_and_retire_during_provider_call();
        test_concurrent_pin_and_drain();
        test_provider_admission_matrix();
        test_nullary_provider_call_shape();
        puts("provider admission and lease tests passed");
        return 0;
    }
    if (argc != 1)
        return 2;
    test_module_initialization_authority_and_failure();
    test_concurrent_module_initialization();
    test_profile_partitions_and_foreign_authority();
    test_execution_identity_and_lifecycle();
    test_reentrant_provider_call_pins_lease_without_holding_lock();
    test_concurrent_release_drain_and_retire_during_provider_call();
    test_concurrent_pin_and_drain();
    test_provider_admission_matrix();
    test_nullary_provider_call_shape();
    puts("execution binding tests passed");
    return 0;
}
