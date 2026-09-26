/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_vm_allocations.c - Failed initializer ownership and physical release
 */

#include "vm/xr_program_vm.h"
#include "../plan/target_profile_test_fixture.h"
#include "../program/xr_program_module_fixture.h"
#include "../program/xr_program_output_trap_fixture.h"
#include "../program/xr_program_array_fixture.h"
#include "../program/xr_program_string_builder_fixture.h"
#include "../program/xr_program_array_default_fixture.h"
#include "../program/xr_program_array_append_fixture.h"
#include "../program/xr_program_string_slice_fixture.h"
#include "../program/xr_program_atomic_fixture.h"
#include "../program/xr_program_channel_fixture.h"
#include "../program/xr_program_callable_fixture.h"
#include "../program/xr_program_reborrow_fixture.h"
#include "../program/xr_program_assert_fixture.h"
#include "../program/xr_program_allocation_probe.h"

#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static size_t attempt, fail_at, live_count;
static bool armed, failed;
static void *live[1024];

static bool reject_allocation(void) {
    if (!armed || ++attempt != fail_at)
        return false;
    failed = true;
    return true;
}

static void *record_allocation(void *pointer) {
    REQUIRE(pointer);
    for (size_t i = 0u; i < XR_COUNTOF(live); ++i) {
        if (live[i])
            continue;
        live[i] = pointer;
        ++live_count;
        return pointer;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return reject_allocation() ? NULL : record_allocation(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return reject_allocation() ? NULL
                               : record_allocation(xr_program_test_system_calloc(count, size));
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (reject_allocation())
        return NULL;
    if (!pointer)
        return record_allocation(xr_program_test_system_realloc(NULL, size));
    for (size_t i = 0u; i < XR_COUNTOF(live); ++i) {
        if (live[i] != pointer)
            continue;
        live[i] = xr_program_test_system_realloc(pointer, size);
        REQUIRE(live[i]);
        return live[i];
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0u; i < XR_COUNTOF(live); ++i) {
        if (live[i] != pointer)
            continue;
        live[i] = NULL;
        --live_count;
        xr_program_test_system_free(pointer);
        return;
    }
    abort();
}

typedef struct Lifecycle {
    uint64_t identities[4];
    uint32_t constructed, finalized[4], reclaimed[4];
} Lifecycle;

static void lifecycle(void *context, const XrVmLifecycleEvent *event) {
    Lifecycle *log = context;
    if (event->kind == XR_VM_EVENT_CLASS_CONSTRUCT) {
        REQUIRE(log->constructed < 4u);
        log->identities[log->constructed++] = event->identity;
    }
    for (uint32_t i = 0u; i < log->constructed; ++i) {
        if (log->identities[i] != event->identity)
            continue;
        log->finalized[i] += event->kind == XR_VM_EVENT_CLASS_FINALIZE;
        log->reclaimed[i] += event->kind == XR_VM_EVENT_CLASS_RECLAIM;
    }
}

static void require_reclaimed(const Lifecycle *log) {
    for (uint32_t i = 0u; i < log->constructed; ++i)
        REQUIRE(log->finalized[i] == 1u && log->reclaimed[i] == 1u);
}

static XrProviderCallStatus allocation_output(void *context, const uint8_t *bytes, size_t size) {
    const bool *refuse = context;
    REQUIRE(size == 6u && memcmp(bytes, "ready\n", 6u) == 0);
    return *refuse ? XR_PROVIDER_CALL_FAILED : XR_PROVIDER_CALL_OK;
}

static void test_output_allocation_failures(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_output_trap_fixture_write(XR_OUTPUT_TRAP_VALID, &artifact, NULL, 0u) ==
            XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrProgramProviderRequirementView requirement = {0};
    REQUIRE(xr_validated_program_provider_requirement(program, 0u, &requirement));
    bool refuse = false;
    XrProviderOperationBinding operation = {.operation_id = requirement.operations[0].operation_id,
                                            .trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE,
                                            .context = &refuse};
    operation.entry.output_write = allocation_output;
    XrProviderBinding provider = {.contract_id = requirement.contract_id,
                                  .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
                                  .operations = &operation,
                                  .operation_count = 1u};
    const XrTargetProviderContract *contract = NULL;
    for (size_t i = 0u; i < xr_target_profile_provider_count(profile); ++i) {
        const XrTargetProviderContract *candidate = xr_target_profile_provider(profile, i);
        if (xr_test_target_profile_is_provider(candidate, XR_PROVIDER_IO_CONTRACT_KEY))
            contract = candidate;
    }
    REQUIRE(contract);
    REQUIRE(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint) ==
            XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                                       .program = program,
                                       .profile = profile,
                                       .providers = &provider,
                                       .provider_count = 1u,
                                       .generation = 1u};
    {
        Lifecycle log = {0};
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = &log;
        options.lifecycle_event = lifecycle;
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        size_t baseline = live_count;
        for (uint32_t rejected = 0u; rejected < 2u; ++rejected) {
            refuse = rejected != 0u;
            bool completed = false;
            for (size_t failure = 1u; failure < 128u; ++failure) {
                log = (Lifecycle) {0};
                XrInstance *instance = NULL;
                REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
                XrVmExecution *execution = NULL;
                REQUIRE(xr_vm_execution_create(code, instance,
                                               xr_validated_program_entry_function(program), NULL,
                                               0u, &execution));
                attempt = 0u;
                fail_at = failure;
                failed = false;
                armed = true;
                XrVmOutcome outcome = xr_vm_execution_step(execution);
                armed = false;
                REQUIRE(outcome.kind == (failed   ? XR_VM_OUTCOME_RESOURCE_LIMIT
                                         : refuse ? XR_VM_OUTCOME_TRAP
                                                  : XR_VM_OUTCOME_RETURN));
                if (!failed) {
                    REQUIRE(log.constructed == 1u);
                    if (refuse)
                        REQUIRE(outcome.trap == XR_VM_TRAP_PROVIDER_CALL_FAILED);
                    completed = true;
                }
                require_reclaimed(&log);
                xr_vm_execution_free(execution);
                REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(live_count == baseline);
                if (completed) {
                    printf("VM output allocation failures: refuse=%u 0u=%u points=%zu\n",
                           rejected, 0u, failure - 1u);
                    break;
                }
            }
            REQUIRE(completed);
        }
        xr_vm_code_free(code);
        REQUIRE(live_count == 0u);
    }
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void test_initializer_error_allocation_failures(void) {
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_module_cleanup_fixture_write(4u, &artifact, NULL, 0u) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
            XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program, .profile = profile, .generation = 1u,
    };
    {
        Lifecycle log = {0};
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = &log;
        options.lifecycle_event = lifecycle;
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
        size_t baseline = live_count;
        bool completed = false;
        for (size_t failure = 1u; failure < 256u; ++failure) {
            log = (Lifecycle) {0};
            XrInstance *instance = NULL;
            REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
            XrVmExecution *execution = NULL;
            uint32_t entry = xr_validated_program_entry_function(program);
            REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
            attempt = 0u;
            fail_at = failure;
            failed = false;
            armed = true;
            XrVmOutcome outcome = xr_vm_execution_step(execution);
            armed = false;
            bool initialization_failed = failed;
            REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_ERROR));
            if (failed) {
                require_reclaimed(&log);
                XrVmOutcome repeated = xr_vm_code_execute(code, instance, entry, NULL, 0u);
                REQUIRE(repeated.kind == XR_VM_OUTCOME_RESOURCE_LIMIT && repeated.steps == 0u);
            } else {
                REQUIRE(log.constructed == 3u);
                REQUIRE(log.finalized[0] == 1u && log.finalized[1] == 1u && log.finalized[2] == 0u);
                REQUIRE(outcome.error_value.kind == XR_VM_VALUE_CLASS_REFERENCE);
                REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
                /* Admission failure must not overwrite the already published typed failure. */
                bool detached = false;
                for (size_t observation_failure = 1u; observation_failure < 16u; ++observation_failure) {
                    attempt = 0u;
                    fail_at = observation_failure;
                    failed = false;
                    armed = true;
                    XrVmOutcome repeated = xr_vm_code_execute(code, instance, entry, NULL, 0u);
                    armed = false;
                    REQUIRE(repeated.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_ERROR));
                    if (!failed) {
                        REQUIRE(repeated.private_owner && repeated.owns_dynamic_values);
                        REQUIRE(repeated.error_value.as.class_reference == outcome.error_value.as.class_reference);
                        detached = true;
                    }
                    xr_vm_outcome_dispose(&repeated);
                    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
                    REQUIRE(log.finalized[2] == 0u);
                    if (detached)
                        break;
                }
                REQUIRE(detached);
                completed = true;
            }
            xr_vm_execution_free(execution);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            require_reclaimed(&log);
            REQUIRE(live_count == baseline);
            if (!initialization_failed) {
                printf("VM sticky error allocation failures: 0u=%u points=%zu\n", 0u, failure - 1u);
                break;
            }
        }
        REQUIRE(completed);
        xr_vm_code_free(code);
        REQUIRE(live_count == 0u);
    }
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void test_array_allocation_failures(void) {
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    const int64_t expected[] = {2, 0, 2, 1};
    for (unsigned scenario = 0u; scenario <= XR_COUNTOF(expected); ++scenario) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_array_fixture_write(scenario, 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = program, .profile = profile, .generation = 1u};
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            size_t baseline = live_count;
            bool completed = false;
            for (size_t failure = 1u; failure < 128u; ++failure) {
                XrInstance *instance = NULL;
                REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
                XrVmExecution *execution = NULL;
                if (scenario != 4u)
                    REQUIRE(xr_vm_execution_create(code, instance, xr_validated_program_entry_function(program),
                                                   NULL, 0u, &execution));
                attempt = 0u;
                fail_at = failure;
                failed = false;
                armed = true;
                XrVmOutcome outcome = scenario == 4u
                    ? xr_vm_code_execute(code, instance, xr_validated_program_entry_function(program), NULL, 0u)
                    : xr_vm_execution_step(execution);
                armed = false;
                REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_RETURN));
                if (!failed) {
                    if (scenario == 4u) {
                        XrVmAggregateView outer;
                        REQUIRE(outcome.private_owner && xr_vm_value_aggregate_view(&outcome.value, &outer));
                        REQUIRE(outer.field_count == 3u && outer.fields[0].as.aggregate == outer.fields[1].as.aggregate);
                        REQUIRE(outer.fields[0].as.aggregate != outer.fields[2].as.aggregate);
                    } else {
                        REQUIRE(outcome.value.kind == XR_VM_VALUE_I64 && outcome.value.as.i64 == expected[scenario]);
                    }
                    completed = true;
                }
                xr_vm_outcome_dispose(&outcome);
                xr_vm_execution_free(execution);
                REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(live_count == baseline);
                if (completed) {
                    printf("VM array allocation failures: scenario=%u 0u=%u points=%zu\n",
                           scenario, 0u, failure - 1u);
                    break;
                }
            }
            REQUIRE(completed);
            xr_vm_code_free(code);
            REQUIRE(live_count == 0u);
        }
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static XrProgramBuildStatus write_owned_result_fixture(bool class_value, bool error,
                                                       XrProgramArtifact *artifact) {
    XrCoreIrKey keys[4];
    for (unsigned i = 0u; i < XR_COUNTOF(keys); ++i) {
        uint8_t material[] = {0xd3, (uint8_t) i};
        keys[i] = xr_core_ir_key(material, sizeof(material));
    }
    uint16_t field = XR_CORE_TYPE_I64;
    XrCoreIrTypeInput type = {.key = keys[3], .local_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
        .kind = XR_CORE_IR_TYPE_CLASS_REFERENCE, .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = &field, .field_count = 1u};
    XrCoreIrConstantInput constant = {.key = keys[0],
        .type_id = class_value ? XR_CORE_TYPE_I64 : XR_CORE_TYPE_STRING,
        .kind = class_value ? XR_CORE_IR_CONSTANT_I64 : XR_CORE_IR_CONSTANT_STRING};
    if (class_value) constant.value.i64 = 42;
    else {
        constant.value.string.bytes = (const uint8_t *) "result";
        constant.value.string.size = 6u;
    }
    uint16_t result_type = class_value ? type.local_id : XR_CORE_TYPE_STRING;
    XrCoreIrInstructionInput instructions[3] = {{
        .operation_id = class_value ? XR_CORE_OP_CORE_CONSTANT_I64 : XR_CORE_OP_CORE_CONSTANT_STRING,
        .result = keys[0], .result_type_id = constant.type_id,
        .result_ownership = class_value ? XR_CORE_IR_NON_OWNER : XR_CORE_IR_OWNER,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = keys[0]}};
    unsigned count = 1u;
    if (class_value) instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CLASS_CONSTRUCT, .result = keys[1],
        .result_type_id = result_type, .result_ownership = XR_CORE_IR_OWNER,
        .operands = &keys[0], .operand_count = 1u};
    instructions[count++] = (XrCoreIrInstructionInput) {
        .operation_id = error ? XR_CORE_OP_CORE_ERROR_PUBLISH : XR_CORE_OP_CORE_RETURN,
        .operands = &keys[class_value ? 1u : 0u], .operand_count = 1u};
    XrCoreIrBlockInput block = {.key = keys[2], .instructions = instructions, .instruction_count = count};
    XrCoreIrFunctionInput function = {.key = keys[2], .entry_block = keys[2],
        .blocks = &block, .block_count = 1u, .flags = XR_PROGRAM_FUNCTION_ENTRY,
        .result_type_id = error ? XR_CORE_TYPE_VOID : result_type,
        .result_ownership = error ? XR_CORE_IR_NON_OWNER : XR_CORE_IR_OWNER,
        .error_type_id = error ? result_type : XR_CORE_TYPE_VOID,
        .effect_mask = error ? XR_CORE_EFFECT_ERROR : 0u};
    XrCoreIrModuleInput module = {.key = keys[2], .constants = &constant, .constant_count = 1u,
        .functions = &function, .function_count = 1u};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {.semantic_profile_fingerprint = keys[2].bytes,
        .required_features = &feature, .required_feature_count = 1u,
        .modules = &module, .module_count = 1u,
        .types = class_value ? &type : NULL, .type_count = class_value ? 1u : 0u};
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, NULL, 0u);
    if (status == XR_PROGRAM_BUILD_OK) status = xr_program_write(program, artifact, NULL, 0u);
    xr_core_ir_program_free(program);
    return status;
}

static void test_owned_result_allocation_failures(void) {
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    for (unsigned shape = 0u; shape < 6u; ++shape) {
        bool class_value = (shape & 1u) != 0u, error = (shape & 2u) != 0u;
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        bool callable = shape == 4u, existential = shape == 5u;
        REQUIRE((existential ? xr_program_reborrow_fixture_write_mutated(
                    XR_REBORROW_FIXTURE_RETURN_OWNER, &artifact, NULL, 0u)
                : callable ? xr_program_callable_fixture_write_mutated(
                    XR_CALLABLE_FIXTURE_RETURN_CAPTURE, &artifact, NULL, 0u)
                          : write_owned_result_fixture(class_value, error, &artifact)) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        {
            XrVmCodeOptions options = xr_vm_code_default_options();
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            size_t baseline = live_count;
            bool completed = false;
            for (size_t failure = 1u; failure < 128u; ++failure) {
                XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                    .program = program, .profile = profile, .generation = 1u};
                XrInstance *instance = NULL;
                REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
                attempt = 0u; fail_at = failure; failed = false; armed = true;
                XrVmOutcome outcome = xr_vm_code_execute(code, instance,
                    xr_validated_program_entry_function(program), NULL, 0u);
                armed = false;
                REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT :
                    error ? XR_VM_OUTCOME_ERROR : XR_VM_OUTCOME_RETURN));
                if (!failed) {
                    XrVmValue value = error ? outcome.error_value : outcome.value;
                    REQUIRE(outcome.private_owner && outcome.owns_dynamic_values);
                    REQUIRE(value.kind == (existential ? XR_VM_VALUE_EXISTENTIAL : callable ? XR_VM_VALUE_CALLABLE :
                        class_value ? XR_VM_VALUE_CLASS_REFERENCE : XR_VM_VALUE_STRING));
                    REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
                    completed = true;
                }
                xr_vm_outcome_dispose(&outcome);
                REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(live_count == baseline);
                if (completed) break;
            }
            REQUIRE(completed);
            xr_vm_code_free(code);
            REQUIRE(live_count == 0u);
        }
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_sequence_allocation_failures(bool array) {
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE((array ? xr_program_array_default_fixture_write(0u, &artifact) : xr_program_string_slice_fixture_write(0u, &artifact)) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
    size_t baseline = live_count;
    for (unsigned scenario = 0u; scenario < 3u; ++scenario) {
        bool completed = false;
        for (size_t failure = 1u; failure < 128u; ++failure) {
            XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                .program = program, .profile = profile, .generation = 1u};
            XrInstance *instance = NULL;
            REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
            XrVmValue args[2] = {{.kind = XR_VM_VALUE_I64, .as.i64 = scenario == 0u ? 1 : scenario == 1u ? 4 : -1},
                                {.kind = XR_VM_VALUE_I64, .as.i64 = 4}};
            if (array) args[0].as.i64 = scenario == 0u ? 3 : scenario == 1u ? 0 : -1;
            attempt = 0u; fail_at = failure; failed = false; armed = true;
            XrVmOutcome outcome = xr_vm_code_execute(code, instance,
                xr_validated_program_entry_function(program), args, array ? 1u : 2u);
            armed = false;
            REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT :
                scenario == 2u ? XR_VM_OUTCOME_PANIC : XR_VM_OUTCOME_RETURN));
            if (failed) {
                REQUIRE(!outcome.private_owner && !outcome.owns_dynamic_values);
            } else if (scenario == 2u) {
                REQUIRE(outcome.panic_value.as.panic_info.code == (array ? 452u : 430u));
            } else if (array) {
                REQUIRE(outcome.value.kind == XR_VM_VALUE_I64 && outcome.value.as.i64 == args[0].as.i64);
            } else {
                XrVmStringView view = {0};
                static const uint8_t expected[] = {0xc3,0xa9,0xe4,0xb8,0xad,0xf0,0x9f,0x99,0x82};
                REQUIRE(xr_vm_value_string_view(&outcome.value, &view));
                REQUIRE(view.size == (scenario == 0u ? sizeof(expected) : 0u));
                REQUIRE(!view.size || memcmp(view.bytes, expected, sizeof(expected)) == 0);
            }
            xr_vm_outcome_dispose(&outcome);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(live_count == baseline);
            if (!failed) {
                printf("VM sequence allocation failures: array=%u scenario=%u points=%zu\n", array, scenario, failure - 1u);
                completed = true;
                break;
            }
        }
        REQUIRE(completed);
    }
    xr_vm_code_free(code);
    REQUIRE(live_count == 0u);
    xr_target_profile_free(profile);
    xr_validated_program_free(program);
}

static void test_shared_handle_allocation_failures(void) {
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    for (unsigned kind = 0u; kind < 4u; ++kind) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE((kind >= 2u ? xr_program_channel_fixture_write(kind == 2u ? 0 : 2, 0u, &artifact)
                              : xr_program_atomic_fixture_write(kind != 0u, 0u, &artifact)) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = program, .profile = profile, .generation = 1u};
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
        size_t baseline = live_count;
        bool completed = false;
        for (size_t failure = 1u; failure < 128u; ++failure) {
            XrInstance *instance = NULL;
            REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
            attempt = 0u;
            fail_at = failure;
            failed = false;
            armed = true;
            XrVmOutcome outcome = xr_vm_code_execute(code, instance,
                xr_validated_program_entry_function(program), NULL, 0u);
            armed = false;
            REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_RETURN));
            if (!failed) {
                REQUIRE(outcome.value.kind == (kind ? XR_VM_VALUE_BOOL : XR_VM_VALUE_I64));
                REQUIRE(kind >= 2u ? !outcome.value.as.boolean :
                        kind ? outcome.value.as.boolean : outcome.value.as.i64 == 42);
                completed = true;
            }
            xr_vm_outcome_dispose(&outcome);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(live_count == baseline);
            if (completed) {
                printf("VM shared handle allocation failures: kind=%u points=%zu\n", kind, failure - 1u);
                break;
            }
        }
        REQUIRE(completed);
        xr_vm_code_free(code);
        REQUIRE(live_count == 0u);
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
}

static void test_assert_message_allocation_failures(void) {
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    REQUIRE(xr_program_assert_message_fixture_write(0u, &artifact) == XR_PROGRAM_BUILD_OK);
    REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program, .profile = profile, .generation = 1u};
    XrVmCode *code = NULL;
    REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
    size_t baseline = live_count;
    for (unsigned stepped = 0u; stepped < 2u; ++stepped) {
        bool completed = false;
        for (size_t failure = 1u; failure < 128u; ++failure) {
            XrInstance *instance = NULL;
            REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
            XrVmValue argument = {.kind = XR_VM_VALUE_BOOL, .as.boolean = false};
            XrVmExecution *execution = NULL;
            attempt = 0u; fail_at = failure; failed = false; armed = true;
            XrVmOutcome outcome = {.kind = XR_VM_OUTCOME_RESOURCE_LIMIT};
            if (!stepped)
                outcome = xr_vm_code_execute(code, instance,
                    xr_validated_program_entry_function(program), &argument, 1u);
            else if (xr_vm_execution_create(code, instance,
                    xr_validated_program_entry_function(program), &argument, 1u, &execution))
                outcome = xr_vm_execution_step(execution);
            armed = false;
            REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_PANIC));
            if (!failed) {
                static const uint8_t expected[] = {'a', 0, 0xe4, 0xb8, 0xad};
                XrVmStringView message = {0};
                REQUIRE(xr_vm_panic_message_view(&outcome.panic_value.as.panic_info, &message));
                REQUIRE(message.size == sizeof(expected));
                REQUIRE(memcmp(message.bytes, expected, sizeof(expected)) == 0);
                REQUIRE(xr_execution_instance_lease_count(instance) == 1u);
                completed = true;
            }
            xr_vm_outcome_dispose(&outcome);
            xr_vm_execution_free(execution);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(live_count == baseline);
            if (completed) {
                printf("VM assertion message allocation failures: stepped=%u points=%zu\n", stepped, failure - 1u);
                break;
            }
        }
        REQUIRE(completed);
    }
    xr_vm_code_free(code);
    REQUIRE(live_count == 0u);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
}

static void test_string_builder_allocation_failures(void) {
    for (unsigned scenario = 0u; scenario < 3u; ++scenario) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_string_builder_fixture_write(scenario ? 99u + scenario : 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile);
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
        size_t baseline = live_count;
        bool completed = false;
        for (size_t failure = 1u; failure < 128u; ++failure) {
            XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                .program = program, .profile = profile, .generation = 1u};
            XrInstance *instance = NULL;
            REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
            attempt = 0u; fail_at = failure; failed = false; armed = true;
            XrVmOutcome outcome = xr_vm_code_execute(code, instance,
                xr_validated_program_entry_function(program), NULL, 0u);
            armed = false;
            REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_RETURN));
            if (failed) REQUIRE(!outcome.private_owner && !outcome.owns_dynamic_values);
            else {
                const uint8_t expected[] = {'A', 0, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80};
                XrVmStringView view;
                REQUIRE(xr_vm_value_string_view(&outcome.value, &view));
                REQUIRE(view.size == (scenario == 2u ? 0u : scenario == 1u ? 72u : 8u));
                for (size_t offset = 0u; offset < view.size; offset += sizeof(expected))
                    REQUIRE(memcmp(view.bytes + offset, expected, sizeof(expected)) == 0);
            }
            xr_vm_outcome_dispose(&outcome);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(live_count == baseline);
            if (!failed) {
                printf("VM StringBuilder allocation failures: scenario=%u points=%zu\n", scenario, failure - 1u);
                completed = true;
                break;
            }
        }
        REQUIRE(completed);
        xr_vm_code_free(code);
        REQUIRE(live_count == 0u);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

static void test_array_append_allocation_failures(void) {
    for (unsigned scenario = 0u; scenario < 2u; ++scenario) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_array_append_fixture_write(scenario ? 100u : 0u, &artifact) == XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) == XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
        REQUIRE(profile);
        XrVmCode *code = NULL;
        REQUIRE(xr_vm_code_build(program, profile, NULL, &code, NULL) == XR_VM_CODE_OK);
        size_t baseline = live_count;
        bool completed = false;
        for (size_t failure = 1u; failure < 128u; ++failure) {
            XrExecutionBindingInput binding = {.schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                .program = program, .profile = profile, .generation = 1u};
            XrInstance *instance = NULL;
            REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
            attempt = 0u; fail_at = failure; failed = false; armed = true;
            XrVmValue argument = {.kind = XR_VM_VALUE_I64, .as.i64 = 42};
            XrVmOutcome outcome = xr_vm_code_execute(code, instance,
                xr_validated_program_entry_function(program), &argument, 1u);
            armed = false;
            REQUIRE(outcome.kind == (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_RETURN));
            if (failed) REQUIRE(!outcome.private_owner && !outcome.owns_dynamic_values);
            else REQUIRE(outcome.value.kind == XR_VM_VALUE_I64 && outcome.value.as.i64 == 9);
            xr_vm_outcome_dispose(&outcome);
            REQUIRE(xr_execution_instance_lease_count(instance) == 0u);
            REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
            REQUIRE(live_count == baseline);
            if (!failed) {
                printf("VM array append allocation failures: scenario=%u points=%zu\n", scenario, failure - 1u);
                completed = true;
                break;
            }
        }
        REQUIRE(completed);
        xr_vm_code_free(code);
        REQUIRE(live_count == 0u);
        xr_target_profile_free(profile);
        xr_validated_program_free(program);
    }
}

int main(void) {
    test_array_append_allocation_failures();
    test_string_builder_allocation_failures();
    test_assert_message_allocation_failures();
    test_shared_handle_allocation_failures();
    test_owned_result_allocation_failures();
    test_sequence_allocation_failures(false);
    test_sequence_allocation_failures(true);
    test_array_allocation_failures();
    test_initializer_error_allocation_failures();
    test_output_allocation_failures();
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile);
    for (uint32_t delegated = 0u; delegated < 2u; ++delegated) {
        XrProgramArtifact artifact = {0};
        XrValidatedProgram *program = NULL;
        REQUIRE(xr_program_module_cleanup_fixture_write(delegated ? 3u : 0u, &artifact, NULL, 0u) ==
                XR_PROGRAM_BUILD_OK);
        REQUIRE(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
                XR_PROGRAM_VERIFY_OK);
        xr_program_artifact_free(&artifact);
        {
            Lifecycle log = {0};
            XrVmCodeOptions options = xr_vm_code_default_options();
            options.lifecycle_context = &log;
            options.lifecycle_event = lifecycle;
            XrVmCode *code = NULL;
            REQUIRE(xr_vm_code_build(program, profile, &options, &code, NULL) == XR_VM_CODE_OK);
            size_t baseline = live_count;
            bool completed = false;
            for (size_t failure = 1u; failure < 256u; ++failure) {
                log = (Lifecycle) {0};
                XrExecutionBindingInput binding = {
                    .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                    .program = program,
                    .profile = profile,
                    .generation = 1u,
                };
                XrInstance *instance = NULL;
                REQUIRE(xr_execution_instance_create(&binding, &instance, NULL) == XR_EXECUTION_OK);
                XrExecutionLease held = {0};
                REQUIRE(xr_execution_instance_acquire(instance, &held) == XR_EXECUTION_OK);
                XrVmExecution *execution = NULL;
                uint32_t entry = xr_validated_program_entry_function(program);
                REQUIRE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
                attempt = 0u;
                fail_at = failure;
                failed = false;
                armed = true;
                XrVmOutcome outcome = xr_vm_execution_step(execution);
                armed = false;
                REQUIRE(outcome.kind ==
                        (failed ? XR_VM_OUTCOME_RESOURCE_LIMIT : XR_VM_OUTCOME_RETURN));
                if (failed) {
                    /* A live entry frame and another lease cannot postpone semantic cleanup. */
                    require_reclaimed(&log);
                    XrVmOutcome repeated = xr_vm_code_execute(code, instance, entry, NULL, 0u);
                    REQUIRE(repeated.kind == XR_VM_OUTCOME_RESOURCE_LIMIT && repeated.steps == 0u);
                    require_reclaimed(&log);
                } else {
                    REQUIRE(log.constructed == 4u);
                    completed = true;
                }
                xr_vm_execution_free(execution);
                REQUIRE(xr_execution_instance_begin_drain(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_lease_release(&held));
                REQUIRE(xr_execution_instance_retire(instance, NULL) == XR_EXECUTION_OK);
                REQUIRE(xr_execution_instance_free(&instance, NULL) == XR_EXECUTION_OK);
                require_reclaimed(&log);
                REQUIRE(live_count == baseline);
                if (completed) {
                    printf("VM module allocation failures: delegated=%u 0u=%u points=%zu\n",
                           delegated, 0u, failure - 1u);
                    break;
                }
            }
            REQUIRE(completed);
            xr_vm_code_free(code);
            REQUIRE(live_count == 0u);
        }
        xr_validated_program_free(program);
    }
    xr_target_profile_free(profile);
    return 0;
}
