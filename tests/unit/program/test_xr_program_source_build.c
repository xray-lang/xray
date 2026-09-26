/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_source_build.c - Shared source build owner tests
 */

#include "../test_framework.h"

#include "base/xmalloc.h"
#include "frontend/analyzer/xanalyzer.h"
#include "runtime/value/xtype_internal.h"
#include "frontend/analyzer/xanalyzer_builtins.h"
#include "base/xfileio.h"
#include "aot/program/xr_backend_ir.h"
#include "aot/program/xr_backend_ir_internal.h"
#include "core/xr_core_spec_gen.h"
#include "shared/xr_assertion_plan.h"
#include "execution/xr_execution.h"
#include "execution/xr_stdlib_provider_binding.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "os/os_temp.h"
#include "program/xr_program_source_build.h"
#include "program/xr_validated_program_internal.h"
#include "runtime/abi/xr_builtin_provider_contract.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "runtime/xerror_codes.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "toolchain/xcompiler_session.h"
#include "vm/xr_program_vm.h"
#include "xray_vm.h"
#include "plan/target_profile_test_fixture.h"
#include "xr_program_source_cases.gen.h"

#include <stdio.h>
#include <string.h>

typedef struct SourceBuildFixture {
    char directory[XR_TEST_PATH_MAX];
    char entry_path[XR_TEST_PATH_MAX];
    char dependency_path[XR_TEST_PATH_MAX];
    char facade_path[XR_TEST_PATH_MAX];
    XrVMRuntime *isolate;
    XrCompilerSession *session;
    XrCompilerSession *original_session;
    XrModuleResolver *resolver;
    XrModuleIdentityAuthority authority;
    char *entry_identity;
    char *entry_logical_path;
    XrProgramSourceBuildInput input;
} SourceBuildFixture;

static XrSourceFixtureId selected_source_fixture;
static const char *selected_source_output;
static const char *selected_source_case;

static const char *source_fixture_output_path(XrSourceFixtureId fixture) {
    return selected_source_fixture == fixture ? selected_source_output : NULL;
}

typedef struct ClockProviderProbe {
    uint32_t calls;
    int64_t nanos;
    int64_t expected_argument;
    bool reject;
} ClockProviderProbe;

typedef struct PipeProviderProbe {
    uint32_t open_calls;
    uint32_t close_calls;
    bool present;
    int64_t read_handle;
    int64_t write_handle;
    bool close_results[2];
} PipeProviderProbe;

typedef struct PipeCloseSequenceProbe {
    const int64_t *expected_handles;
    uint32_t expected_count;
    uint32_t calls;
} PipeCloseSequenceProbe;

typedef struct ProviderTrapCleanupProbe {
    uint32_t events;
    uint32_t clock_calls;
    uint32_t close_calls;
    int64_t close_handles[4];
} ProviderTrapCleanupProbe;

typedef struct ChildCleanupMode {
    bool cancel;
    bool refuse_clock;
    int64_t refused_handle;
    int64_t second_refused_handle;
    bool false_close;
    XrBackendExecutionOutcomeKind expected;
} ChildCleanupMode;

static const ChildCleanupMode child_cleanup_modes[] = {
    {false, false, 0, 0, false, XR_BACKEND_EXECUTION_RETURN},
    {false, true, 0, 0, false, XR_BACKEND_EXECUTION_TRAP},
    {true, false, 0, 0, false, XR_BACKEND_EXECUTION_CANCELLED},
    {true, false, INT64_C(2147483642), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {true, false, INT64_C(2147483643), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {false, false, INT64_C(2147483642), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {false, false, INT64_C(2147483643), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {false, false, 0, 0, true, XR_BACKEND_EXECUTION_RETURN},
    {true, false, 0, 0, true, XR_BACKEND_EXECUTION_CANCELLED},
    {false, false, INT64_C(2147483638), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {true, false, INT64_C(2147483638), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {false, false, INT64_C(2147483642), INT64_C(2147483643), false, XR_BACKEND_EXECUTION_TRAP},
    {true, false, INT64_C(2147483642), INT64_C(2147483643), false, XR_BACKEND_EXECUTION_TRAP},
    {false, false, INT64_C(2147483639), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {true, false, INT64_C(2147483639), 0, false, XR_BACKEND_EXECUTION_TRAP},
    {false, false, INT64_C(2147483638), INT64_C(2147483639), false, XR_BACKEND_EXECUTION_TRAP},
    {true, false, INT64_C(2147483638), INT64_C(2147483639), false, XR_BACKEND_EXECUTION_TRAP},
};

typedef struct ChildCleanupProbe {
    const ChildCleanupMode *mode;
    uint32_t count;
    int64_t events[8];
} ChildCleanupProbe;

typedef struct FieldRefCleanupMode {
    bool cancel;
    bool refuse_clock;
    XrBackendExecutionOutcomeKind expected;
} FieldRefCleanupMode;

static const FieldRefCleanupMode field_ref_cleanup_modes[] = {
    {false, false, XR_BACKEND_EXECUTION_RETURN},
    {false, true, XR_BACKEND_EXECUTION_TRAP},
    {true, false, XR_BACKEND_EXECUTION_CANCELLED},
};

typedef struct FieldRefCleanupProbe {
    const FieldRefCleanupMode *mode;
    uint32_t count;
    int64_t events[3];
} FieldRefCleanupProbe;

typedef struct BranchingCleanupProbe {
    uint32_t calls;
    uint32_t refuse_call;
    int64_t events[16];
} BranchingCleanupProbe;

typedef struct BranchingCleanupScenario {
    uint32_t cancel_suspension;
    uint32_t refuse_call;
    XrBackendExecutionOutcomeKind expected;
    uint32_t expected_calls;
} BranchingCleanupScenario;

static const BranchingCleanupScenario branching_cleanup_scenarios[] = {
    {UINT32_MAX, 0u, XR_BACKEND_EXECUTION_RETURN, 4u},
    {0u, 0u, XR_BACKEND_EXECUTION_CANCELLED, 2u},
    {1u, 0u, XR_BACKEND_EXECUTION_CANCELLED, 4u},
    {UINT32_MAX, 1u, XR_BACKEND_EXECUTION_TRAP, 3u},
    {UINT32_MAX, 4u, XR_BACKEND_EXECUTION_TRAP, 5u},
};

typedef enum NestedCleanupTrace {
    NESTED_CLEANUP_TRACE_TRUE = 0,
    NESTED_CLEANUP_TRACE_BOTH,
    NESTED_CLEANUP_TRACE_TRUE_PRELUDE_RETRY,
    NESTED_CLEANUP_TRACE_BOTH_FALSE_WRITE_RETRY,
} NestedCleanupTrace;

typedef struct NestedCleanupScenario {
    uint32_t cancel_suspension;
    uint32_t refuse_call;
    XrBackendExecutionOutcomeKind expected;
    NestedCleanupTrace trace;
} NestedCleanupScenario;

static const NestedCleanupScenario nested_cleanup_scenarios[] = {
    {UINT32_MAX, 0u, XR_BACKEND_EXECUTION_RETURN, NESTED_CLEANUP_TRACE_BOTH},
    {0u, 0u, XR_BACKEND_EXECUTION_CANCELLED, NESTED_CLEANUP_TRACE_TRUE},
    {1u, 0u, XR_BACKEND_EXECUTION_CANCELLED, NESTED_CLEANUP_TRACE_BOTH},
    {UINT32_MAX, 1u, XR_BACKEND_EXECUTION_TRAP, NESTED_CLEANUP_TRACE_TRUE_PRELUDE_RETRY},
    {UINT32_MAX, 2u, XR_BACKEND_EXECUTION_TRAP, NESTED_CLEANUP_TRACE_TRUE},
    {UINT32_MAX, 4u, XR_BACKEND_EXECUTION_TRAP, NESTED_CLEANUP_TRACE_TRUE},
    {UINT32_MAX, 7u, XR_BACKEND_EXECUTION_TRAP, NESTED_CLEANUP_TRACE_BOTH_FALSE_WRITE_RETRY},
    {UINT32_MAX, 8u, XR_BACKEND_EXECUTION_TRAP, NESTED_CLEANUP_TRACE_BOTH},
};

static XrProviderCallStatus child_cleanup_clock_probe(void *context, int64_t *result_out) {
    ChildCleanupProbe *probe = context;
    if (!probe || !probe->mode || !result_out || probe->count >= 8u)
        return XR_PROVIDER_CALL_FAILED;
    probe->events[probe->count++] = 0;
    if (probe->mode->refuse_clock)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = INT64_C(42000000);
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus child_cleanup_close_probe(void *context, int64_t handle,
                                                      bool *result_out) {
    ChildCleanupProbe *probe = context;
    if (!probe || !probe->mode || !result_out || probe->count >= 8u)
        return XR_PROVIDER_CALL_FAILED;
    probe->events[probe->count++] = handle;
    if (handle == probe->mode->refused_handle || handle == probe->mode->second_refused_handle)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = !probe->mode->false_close;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus field_ref_cleanup_clock_probe(void *context, int64_t *result_out) {
    FieldRefCleanupProbe *probe = context;
    if (!probe || !probe->mode || !result_out || probe->count != 0u)
        return XR_PROVIDER_CALL_FAILED;
    probe->events[probe->count++] = 0;
    if (probe->mode->refuse_clock)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = 42;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus field_ref_cleanup_close_probe(void *context, int64_t handle,
                                                          bool *result_out) {
    FieldRefCleanupProbe *probe = context;
    if (!probe || !probe->mode || !result_out || probe->count >= 3u)
        return XR_PROVIDER_CALL_FAILED;
    probe->events[probe->count++] = handle;
    *result_out = true;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus branching_cleanup_close_probe(void *context, int64_t handle,
                                                          bool *result_out) {
    BranchingCleanupProbe *probe = context;
    if (!probe || !result_out || probe->calls >= 16u)
        return XR_PROVIDER_CALL_FAILED;
    probe->events[probe->calls++] = handle;
    if (probe->refuse_call != 0u && probe->calls == probe->refuse_call)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = true;
    return XR_PROVIDER_CALL_OK;
}

static void branching_cleanup_assert_trace(const BranchingCleanupProbe *probe,
                                           uint32_t expected_count) {
    static const int64_t ordinary[] = {
        INT64_C(2147483642), INT64_C(2147483643), INT64_C(2147483642),
        INT64_C(2147483643), INT64_C(2147483643),
    };
    static const int64_t first_refusal[] = {
        INT64_C(2147483642),
        INT64_C(2147483642),
        INT64_C(2147483643),
    };
    const int64_t *expected = probe->refuse_call == 1u ? first_refusal : ordinary;
    ASSERT_EQ_UINT(probe->calls, expected_count);
    for (uint32_t event = 0u; event < expected_count; ++event)
        ASSERT_EQ_INT(probe->events[event], expected[event]);
}

static void nested_cleanup_assert_trace(const BranchingCleanupProbe *probe,
                                        NestedCleanupTrace trace) {
    static const int64_t expected[][9] = {
        [NESTED_CLEANUP_TRACE_TRUE] = {INT64_C(2147483642), INT64_C(2147483643),
                                       INT64_C(2147483638), INT64_C(2147483639)},
        [NESTED_CLEANUP_TRACE_BOTH] =
            {
                INT64_C(2147483642),
                INT64_C(2147483643),
                INT64_C(2147483638),
                INT64_C(2147483639),
                INT64_C(2147483642),
                INT64_C(2147483643),
                INT64_C(2147483639),
                INT64_C(2147483638),
            },
        [NESTED_CLEANUP_TRACE_TRUE_PRELUDE_RETRY] =
            {
                INT64_C(2147483642),
                INT64_C(2147483642),
                INT64_C(2147483643),
                INT64_C(2147483638),
                INT64_C(2147483639),
            },
        [NESTED_CLEANUP_TRACE_BOTH_FALSE_WRITE_RETRY] =
            {
                INT64_C(2147483642),
                INT64_C(2147483643),
                INT64_C(2147483638),
                INT64_C(2147483639),
                INT64_C(2147483642),
                INT64_C(2147483643),
                INT64_C(2147483639),
                INT64_C(2147483638),
                INT64_C(2147483639),
            },
    };
    static const uint32_t expected_counts[] = {4u, 8u, 5u, 9u};
    ASSERT_LT(trace, sizeof(expected_counts) / sizeof(expected_counts[0]));
    ASSERT_EQ_UINT(probe->calls, expected_counts[trace]);
    for (uint32_t event = 0u; event < probe->calls; ++event)
        ASSERT_EQ_INT(probe->events[event], expected[trace][event]);
}

static void child_cleanup_assert_trace(const ChildCleanupProbe *probe) {
    static const int64_t handles[] = {
        INT64_C(2147483642), INT64_C(2147483643), INT64_C(2147483638),
        INT64_C(2147483639), INT64_C(2147483646), INT64_C(2147483647),
    };
    uint32_t start = probe->mode->cancel ? 0u : 1u;
    bool exact = probe->count == start + 6u;
    exact = exact && (start == 0u || probe->events[0] == 0);
    for (uint32_t index = 0u; exact && index < 6u; ++index)
        exact = probe->events[start + index] == handles[index];
    if (!exact) {
        fprintf(
            stderr,
            "child cleanup trace mismatch: cancel=%u refuse_clock=%u refused_handles=%lld/%lld "
            "false_close=%u count=%u events=",
            probe->mode->cancel, probe->mode->refuse_clock, (long long) probe->mode->refused_handle,
            (long long) probe->mode->second_refused_handle, probe->mode->false_close, probe->count);
        for (uint32_t index = 0u; index < probe->count; ++index)
            fprintf(stderr, "%s%lld", index ? "," : "", (long long) probe->events[index]);
        fputc('\n', stderr);
    }
    ASSERT_EQ_UINT(probe->count, start + 6u);
    if (start != 0u)
        ASSERT_EQ_INT(probe->events[0], 0);
    for (uint32_t index = 0u; index < 6u; ++index)
        ASSERT_EQ_INT(probe->events[start + index], handles[index]);
}

static void field_ref_cleanup_assert_trace(const FieldRefCleanupProbe *probe) {
    static const int64_t resumed[] = {
        0,
        INT64_C(2147483649),
        INT64_C(2147483652),
    };
    static const int64_t cancelled[] = {
        INT64_C(2147483647),
        INT64_C(2147483649),
    };
    const int64_t *expected = probe->mode->cancel ? cancelled : resumed;
    uint32_t expected_count = probe->mode->cancel ? 2u : 3u;
    ASSERT_EQ_UINT(probe->count, expected_count);
    for (uint32_t index = 0u; index < expected_count; ++index)
        ASSERT_EQ_INT(probe->events[index], expected[index]);
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static uint32_t program_operation_count(const XrValidatedProgram *program, uint16_t operation_id) {
    uint32_t count = 0u;
    for (uint32_t function_index = 0u; program && function_index < program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                count += block->instructions[instruction_index].operation_id == operation_id;
            }
        }
    }
    return count;
}

typedef struct SourceClassLifecycleLog {
    XrVmLifecycleEvent events[128];
    uint32_t count;
    bool overflow;
} SourceClassLifecycleLog;

static void record_source_class_lifecycle(void *context, const XrVmLifecycleEvent *event) {
    SourceClassLifecycleLog *log = context;
    if (!log || !event)
        return;
    if (log->count >= sizeof(log->events) / sizeof(log->events[0])) {
        log->overflow = true;
        return;
    }
    log->events[log->count++] = *event;
}

static uint32_t source_class_lifecycle_count(const SourceClassLifecycleLog *log,
                                             XrVmLifecycleEventKind kind, uint64_t identity) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; log && index < log->count; ++index)
        count += log->events[index].kind == kind && log->events[index].identity == identity;
    return count;
}

static bool program_instruction_matches(const XrValidatedProgram *program, uint32_t function_id,
                                        uint32_t block_id, uint32_t instruction_id,
                                        uint16_t operation_id) {
    if (!program || function_id >= program->function_count)
        return false;
    const XrValidatedFunction *function = &program->functions[function_id];
    if (block_id >= function->block_count)
        return false;
    const XrValidatedBlock *block = &function->blocks[block_id];
    return instruction_id < block->instruction_count &&
           block->instructions[instruction_id].operation_id == operation_id;
}

static void assert_vm_rejects_inactive_operation(const XrTargetProfile *profile,
                                                 const XrValidatedProgram *program,
                                                 uint16_t expected_operation) {

    XrVmCodeDiagnostic first = {0};
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic),
                      XR_VM_CODE_UNSUPPORTED_OPERATION);
        ASSERT_NULL(code);
        ASSERT_EQ_INT(diagnostic.status, XR_VM_CODE_UNSUPPORTED_OPERATION);
        ASSERT_EQ_UINT(diagnostic.operation_id, expected_operation);
        ASSERT_TRUE(program_instruction_matches(program, diagnostic.function_id,
                                                diagnostic.block_id, diagnostic.instruction_id,
                                                diagnostic.operation_id));
        const XrCoreOperationSpec *spec = xr_core_spec_operation_by_id(diagnostic.operation_id);
        ASSERT_NOT_NULL(spec);
        if (spec)
            ASSERT_EQ_INT(spec->vm_status, XR_CORE_COVERAGE_NOT_YET_ACTIVE);
        if (true)
            first = diagnostic;
        else {
            ASSERT_EQ_UINT(diagnostic.operation_id, first.operation_id);
            ASSERT_EQ_UINT(diagnostic.function_id, first.function_id);
            ASSERT_EQ_UINT(diagnostic.block_id, first.block_id);
            ASSERT_EQ_UINT(diagnostic.instruction_id, first.instruction_id);
        }
    }
}

static void assert_vm_i64_result(XrInstance *instance, const XrValidatedProgram *program,
                                 const XrTargetProfile *profile, int64_t expected) {

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCodeDiagnostic diagnostic;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic),
                      XR_VM_CODE_OK);
        ASSERT_NOT_NULL(code);
        ASSERT_EQ_INT(diagnostic.status, XR_VM_CODE_OK);
        XrVmOutcome outcome = xr_vm_code_execute(
            code, instance, xr_validated_program_entry_function(program), NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, expected);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
    }
}

static void assert_vm_fixture_backend_contract(XrInstance *instance,
                                               const XrValidatedProgram *program,
                                               const XrTargetProfile *profile,
                                               XrSourceFixtureId fixture) {
    uint16_t expected_operation =
        xr_source_fixture_unsupported_operation(fixture, XR_SOURCE_BACKEND_VM);
    if (expected_operation != 0u) {
        assert_vm_rejects_inactive_operation(profile, program, expected_operation);
        return;
    }
    assert_vm_i64_result(instance, program, profile, 42);
}

static void assert_detached_program_i64_result(XrValidatedProgram *program,
                                               XrTargetProfile *profile, int64_t expected) {
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .generation = 1u,
    };
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
    assert_vm_i64_result(instance, program, profile, expected);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
}

static void assert_source_class_lifecycle_result(XrValidatedProgram *program,
                                                 XrTargetProfile *profile,
                                                 SourceClassLifecycleLog *log) {
    SourceClassLifecycleLog first = {0};
    {
        *log = (SourceClassLifecycleLog) {0};
        XrExecutionBindingInput binding = {
            .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = program,
            .profile = profile,
            .generation = 1u,
        };
        XrInstance *instance = NULL;
        ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = log;
        options.lifecycle_event = record_source_class_lifecycle;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, NULL), XR_VM_CODE_OK);
        XrVmOutcome outcome = xr_vm_code_execute(
            code, instance, xr_validated_program_entry_function(program), NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, 42);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
        ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
        if (true)
            first = *log;
        else {
            ASSERT_EQ_UINT(log->count, first.count);
            for (uint32_t i = 0u; i < log->count; ++i) {
                const XrVmLifecycleEvent *left = &log->events[i];
                const XrVmLifecycleEvent *right = &first.events[i];
                ASSERT_TRUE(left->kind == right->kind && left->origin == right->origin &&
                            left->type_id == right->type_id &&
                            left->field_ordinal == right->field_ordinal &&
                            left->identity == right->identity &&
                            left->related_identity == right->related_identity &&
                            left->previous_value_kind == right->previous_value_kind &&
                            left->replacement_value_kind == right->replacement_value_kind &&
                            left->previous_i64 == right->previous_i64 &&
                            left->replacement_i64 == right->replacement_i64);
            }
        }
    }
}

static void assert_aot_rejects_inactive_operation(const XrValidatedProgram *program,
                                                  const XrTargetProfile *profile,
                                                  uint16_t expected_operation) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic),
                  XR_BACKEND_UNSUPPORTED_OPERATION);
    ASSERT_NULL(ir);
    ASSERT_EQ_INT(diagnostic.status, XR_BACKEND_UNSUPPORTED_OPERATION);
    ASSERT_EQ_UINT(diagnostic.operation_id, expected_operation);
    ASSERT_TRUE(program_instruction_matches(program, diagnostic.function_id, diagnostic.block_id,
                                            diagnostic.instruction_id, diagnostic.operation_id));
    const XrCoreOperationSpec *spec = xr_core_spec_operation_by_id(diagnostic.operation_id);
    ASSERT_NOT_NULL(spec);
    if (spec)
        ASSERT_EQ_INT(spec->aot_status, XR_CORE_COVERAGE_NOT_YET_ACTIVE);
}

static void assert_aot_fixture_backend_contract(const XrValidatedProgram *program,
                                                const XrTargetProfile *profile,
                                                XrSourceFixtureId fixture) {
    uint16_t expected_operation =
        xr_source_fixture_unsupported_operation(fixture, XR_SOURCE_BACKEND_AOT);
    if (expected_operation != 0u) {
        assert_aot_rejects_inactive_operation(program, profile, expected_operation);
        return;
    }

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic), XR_BACKEND_OK);
    ASSERT_NOT_NULL(ir);
    ASSERT_EQ_INT(diagnostic.status, XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(ir, &diagnostic));

    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &repeated, &diagnostic), XR_BACKEND_OK);
    ASSERT_NOT_NULL(generated.bytes);
    ASSERT_NOT_NULL(repeated.bytes);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "int main(void)"));

    const char *output_path = source_fixture_output_path(fixture);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static void assert_vm_panic_cleanup_policy(const XrValidatedProgram *program,
                                           const XrTargetProfile *profile, XrInstance *instance,
                                           uint32_t entry, PipeCloseSequenceProbe *probe) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCodeDiagnostic diagnostic;
    XrVmCode *code = NULL;
    ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic), XR_VM_CODE_OK);
    ASSERT_NOT_NULL(code);
    ASSERT_EQ_INT(diagnostic.status, XR_VM_CODE_OK);

    probe->calls = 0u;
    XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    const int outcome_kind = (int) outcome.kind;
    const int panic_kind = (int) outcome.panic_value.kind;
    const uint32_t panic_info = outcome.panic_value.as.panic_info.code;
    const uint32_t close_calls = probe->calls;
    xr_vm_outcome_dispose(&outcome);
    xr_vm_code_free(code);

    if (outcome_kind != XR_VM_OUTCOME_PANIC || panic_kind != XR_VM_VALUE_PANIC_INFO ||
        panic_info != XR_ASSERTION_FAILURE_CONDITION_FALSE || close_calls != 2u) {
        fprintf(stderr,
                "VM panic cleanup mismatch: outcome=%d panic_kind=%d panic_info=%u "
                "close_calls=%u\n",
                outcome_kind, panic_kind, (unsigned) panic_info,
                (unsigned) close_calls);
    }
    ASSERT_EQ_INT(outcome_kind, XR_VM_OUTCOME_PANIC);
    ASSERT_EQ_INT(panic_kind, XR_VM_VALUE_PANIC_INFO);
    ASSERT_EQ_UINT(panic_info, XR_ASSERTION_FAILURE_CONDITION_FALSE);
    ASSERT_EQ_UINT(close_calls, 2u);
}

static void assert_vm_panic_cleanup(const XrValidatedProgram *program,
                                    const XrTargetProfile *profile, XrInstance *instance,
                                    uint32_t entry, PipeCloseSequenceProbe *probe) {

    {
        assert_vm_panic_cleanup_policy(program, profile, instance, entry, probe);
    }
}

static void write_probe_typed_host(FILE *output);

static void assert_aot_panic_cleanup(const XrValidatedProgram *program,
                                     const XrTargetProfile *profile, uint32_t entry,
                                     XrSourceFixtureId fixture) {
    ASSERT_LT(entry, program->function_count);
    ASSERT_EQ_UINT(program->functions[entry].panic_type_id, XR_CORE_TYPE_PANIC_INFO);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic), XR_BACKEND_OK);
    ASSERT_NOT_NULL(ir);
    ASSERT_EQ_INT(diagnostic.status, XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(ir, &diagnostic));

    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, false, &repeated, &diagnostic), XR_BACKEND_OK);
    ASSERT_NOT_NULL(generated.bytes);
    ASSERT_NOT_NULL(repeated.bytes);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NULL(strstr(generated.bytes, "int main(void)"));

    const char *output_path = source_fixture_output_path(fixture);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
        ASSERT_TRUE(
            fprintf(output,
                    "\ntypedef struct XrPanicCleanupProbe {\n"
                    "    uint32_t close_calls;\n"
                    "} XrPanicCleanupProbe;\n\n"
                    "static int xr_test_pipe_close(void *opaque, uint32_t requirement, \n"
                    "                              uint32_t operation, int64_t handle, \n"
                    "                              uint8_t *result) {\n"
                    "    static const int64_t expected[] = {INT64_C(2147483646), "
                    "INT64_C(2147483647)};\n"
                    "    XrPanicCleanupProbe *probe = (XrPanicCleanupProbe *)opaque;\n"
                    "    if (!probe || !result || requirement != UINT32_C(0) || \n"
                    "        operation != UINT32_C(0) || probe->close_calls >= UINT32_C(2) || \n"
                    "        handle != expected[probe->close_calls]) return 1;\n"
                    "    *result = UINT8_C(1);\n"
                    "    ++probe->close_calls;\n"
                    "    return 0;\n"
                    "}\n\n"
                    "int main(void) {\n"
                    "    XrPanicCleanupProbe probe = {0};\n"
                    "    XrAotContext context = {.provider_context = &probe, \n"
                    "                            .provider_call_typed = xr_probe_typed, .provider_dispose_typed = xr_probe_dispose};\n"
                    "    xr_probe_close_entry = xr_test_pipe_close;\n"
                    "    XrAotPanicInfo panic = {0};\n"
                    "    XrAotOutcome outcome = xr_aot_fn_%u(&context, &panic);\n"
                    "    if (outcome.kind != UINT32_C(3)) return 241;\n"
                    "    if (panic.code != UINT32_C(%u)) return 242;\n"
                    "    if (probe.close_calls != UINT32_C(2)) return 243;\n"
                    "    xr_aot_context_destroy(&context);\n"
                    "    return 173;\n"
                    "}\n",
                    entry, (unsigned) XR_ASSERTION_FAILURE_CONDITION_FALSE) > 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static uint32_t program_operation_successor_count(const XrValidatedProgram *program,
                                                  uint16_t operation_id, uint32_t successor_count) {
    uint32_t count = 0u;
    for (uint32_t function_index = 0u; program && function_index < program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                count += instruction->operation_id == operation_id &&
                         instruction->successor_count == successor_count;
            }
        }
    }
    return count;
}

static const XrTargetProviderContract *find_profile_provider(const XrTargetProfile *profile,
                                                             XrStableId contract_id) {
    for (size_t i = 0; profile && i < xr_target_profile_provider_count(profile); i++) {
        const XrTargetProviderContract *provider = xr_target_profile_provider(profile, i);
        if (provider && stable_id_equal(provider->contract_id, contract_id))
            return provider;
    }
    return NULL;
}

static const XrTargetProviderOperationContract *
find_profile_provider_operation(const XrTargetProviderContract *provider, XrStableId operation_id) {
    for (uint16_t i = 0; provider && i < provider->operation_count; i++) {
        if (stable_id_equal(provider->operations[i].stable_id, operation_id))
            return &provider->operations[i];
    }
    return NULL;
}

static XrProviderCallStatus clock_provider_probe(void *context, int64_t *result_out) {
    ClockProviderProbe *probe = context;
    if (!probe || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    ++probe->calls;
    if (probe->reject)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = probe->nanos;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus provider_trap_clock_probe(void *context, int64_t *result_out) {
    ProviderTrapCleanupProbe *probe = context;
    if (!probe || !result_out || probe->events % 5u != 0u)
        return XR_PROVIDER_CALL_FAILED;
    ++probe->events;
    ++probe->clock_calls;
    return XR_PROVIDER_CALL_FAILED;
}

static XrProviderCallStatus provider_trap_close_probe(void *context, int64_t handle,
                                                      bool *result_out) {
    ProviderTrapCleanupProbe *probe = context;
    if (!probe || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    uint32_t phase = probe->events % 5u;
    if (phase < 1u || phase > 4u || handle != probe->close_handles[phase - 1u])
        return XR_PROVIDER_CALL_FAILED;
    *result_out = true;
    ++probe->events;
    ++probe->close_calls;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus pipe_close_provider_probe(void *context, int64_t handle,
                                                      bool *result_out) {
    PipeProviderProbe *probe = context;
    if (!probe || !result_out)
        return XR_PROVIDER_CALL_FAILED;
    int64_t expected = (probe->close_calls & 1u) == 0u ? probe->read_handle : probe->write_handle;
    if (handle != expected)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = probe->close_results[probe->close_calls & 1u];
    ++probe->close_calls;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus pipe_close_sequence_probe(void *context, int64_t handle,
                                                      bool *result_out) {
    PipeCloseSequenceProbe *probe = context;
    if (!probe || !result_out || probe->calls >= probe->expected_count ||
        handle != probe->expected_handles[probe->calls])
        return XR_PROVIDER_CALL_FAILED;
    *result_out = true;
    ++probe->calls;
    return XR_PROVIDER_CALL_OK;
}

static void pipe_close_sequence_reset(PipeCloseSequenceProbe *probe, const int64_t *handles,
                                      uint32_t count) {
    probe->expected_handles = handles;
    probe->expected_count = count;
    probe->calls = 0u;
}

static XrProviderCallStatus clock_provider_unary_probe(void *context, int64_t argument,
                                                       int64_t *result_out) {
    ClockProviderProbe *probe = context;
    if (!probe || !result_out || argument != probe->expected_argument)
        return XR_PROVIDER_CALL_FAILED;
    ++probe->calls;
    if (probe->reject)
        return XR_PROVIDER_CALL_FAILED;
    *result_out = probe->nanos;
    return XR_PROVIDER_CALL_OK;
}

static XrProviderCallStatus pipe_provider_probe(void *context, bool *present_out,
                                                int64_t *first_out, int64_t *second_out) {
    PipeProviderProbe *probe = context;
    if (!probe || !present_out || !first_out || !second_out)
        return XR_PROVIDER_CALL_FAILED;
    ++probe->open_calls;
    *present_out = probe->present;
    *first_out = probe->read_handle;
    *second_out = probe->write_handle;
    return XR_PROVIDER_CALL_OK;
}

static bool write_source_file(const char *path, const char *source) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t size = strlen(source);
    bool written = fwrite(source, 1u, size, file) == size;
    return fclose(file) == 0 && written;
}

static XrFingerprint semantic_fingerprint(const char *text) {
    XrCoreIrKey key = xr_core_ir_key(text, strlen(text));
    XrFingerprint fingerprint;
    memcpy(fingerprint.bytes, key.bytes, sizeof(fingerprint.bytes));
    return fingerprint;
}

static void source_build_fixture_free(SourceBuildFixture *fixture) {
    if (!fixture)
        return;
    if (fixture->isolate && fixture->session && fixture->original_session)
        (void) xr_compiler_session_attach_isolate(fixture->isolate, fixture->original_session);
    xr_compiler_session_delete(fixture->session);
    xray_vm_delete(fixture->isolate);
    xr_module_resolver_free(fixture->resolver);
    xr_free(fixture->entry_identity);
    xr_free(fixture->entry_logical_path);
    if (fixture->facade_path[0])
        xr_test_unlink(fixture->facade_path);
    if (fixture->dependency_path[0])
        xr_test_unlink(fixture->dependency_path);
    if (fixture->entry_path[0])
        xr_test_unlink(fixture->entry_path);
    if (fixture->directory[0])
        xr_test_rmdir(fixture->directory);
    memset(fixture, 0, sizeof(*fixture));
}

static bool source_build_fixture_init(SourceBuildFixture *fixture, const char *entry_source,
                                      const char *dependency_source) {
    if (!fixture || !entry_source)
        return false;
    memset(fixture, 0, sizeof(*fixture));
    if (xr_temp_dir_create("xray-program-source-build", fixture->directory,
                           sizeof(fixture->directory)) != 0)
        goto fail;
    /* The directory doubles as the script identity authority root. The graph
     * canonicalizes the entry path it is handed and compares it against this
     * root byte for byte, so the root itself must be the physical path: on
     * macOS TMPDIR lives under /var, which resolves to /private/var. */
    char absolute_directory[XR_TEST_PATH_MAX] = {0};
    if (!xr_test_realpath_buf(fixture->directory, absolute_directory, sizeof(absolute_directory)))
        goto fail;
    memcpy(fixture->directory, absolute_directory, strlen(absolute_directory) + 1u);
    int entry_length = snprintf(fixture->entry_path, sizeof(fixture->entry_path), "%s/main.xr",
                                fixture->directory);
    if (entry_length < 0 || (size_t) entry_length >= sizeof(fixture->entry_path) ||
        !write_source_file(fixture->entry_path, entry_source))
        goto fail;
    if (dependency_source) {
        int dependency_length = snprintf(fixture->dependency_path, sizeof(fixture->dependency_path),
                                         "%s/library.xr", fixture->directory);
        if (dependency_length < 0 ||
            (size_t) dependency_length >= sizeof(fixture->dependency_path) ||
            !write_source_file(fixture->dependency_path, dependency_source))
            goto fail;
    }

    XrVMConfig vm_config = {0};
    fixture->isolate = xray_vm_new_full(&vm_config);
    if (!fixture->isolate)
        goto fail;
    fixture->original_session = xr_compiler_session_current_for_isolate(fixture->isolate);
    XrCompilerSessionConfig session_config = {0};
    fixture->session = xr_compiler_session_new(&session_config);
    if (!fixture->session || xr_compiler_session_attach_isolate(
                                 fixture->isolate, fixture->session) != fixture->original_session)
        goto fail;
    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    if (!fixture->resolver)
        goto fail;

    fixture->authority.kind = XR_MODULE_IDENTITY_SCRIPT;
    fixture->authority.physical_root = fixture->directory;
    if (!xr_module_identity_from_source(&fixture->authority, fixture->entry_path,
                                        &fixture->entry_identity, &fixture->entry_logical_path))
        goto fail;
    XrFingerprint source_fingerprint;
    xr_module_source_fingerprint(entry_source, &source_fingerprint);
    fixture->input = (XrProgramSourceBuildInput) {
        .schema_version = XR_PROGRAM_SOURCE_BUILD_SCHEMA_VERSION,
        .budget = xr_program_source_build_default_budget(),
        .session = fixture->session,
        .resolver = fixture->resolver,
        .entry_source_path = fixture->entry_path,
        .entry_authority = &fixture->authority,
        .entry =
            {
                .kind = XR_PROGRAM_SOURCE_ENTRY_FUNCTION,
                .module_identity = fixture->entry_identity,
                .function_name = "answer",
                .source_content_fingerprint = source_fingerprint,
            },
        .source_profile = XR_PROGRAM_SOURCE_PROFILE_NATIVE_RELEASE,
        .semantic_profile_fingerprint = semantic_fingerprint("source-owner-native-release"),
    };
    return true;

fail:
    source_build_fixture_free(fixture);
    return false;
}

static bool source_build_fixture_add_facade(SourceBuildFixture *fixture,
                                            const char *facade_source) {
    if (!fixture || !facade_source || !fixture->directory[0] || fixture->facade_path[0])
        return false;
    int facade_length = snprintf(fixture->facade_path, sizeof(fixture->facade_path), "%s/facade.xr",
                                 fixture->directory);
    return facade_length >= 0 && (size_t) facade_length < sizeof(fixture->facade_path) &&
           write_source_file(fixture->facade_path, facade_source);
}

#include "xr_program_source_cleanup_checks.inc.c"

static void assert_products_equal(const XrProgramSourceProduct *first,
                                  const XrProgramSourceProduct *second) {
    ASSERT_NOT_NULL(first->artifact.bytes);
    ASSERT_NOT_NULL(second->artifact.bytes);
    ASSERT_NOT_NULL(first->program);
    ASSERT_NOT_NULL(second->program);
    ASSERT_EQ_UINT(first->artifact.size, second->artifact.size);
    ASSERT_EQ_INT(memcmp(first->artifact.bytes, second->artifact.bytes, first->artifact.size), 0);
    ASSERT_TRUE(xr_program_id_equal(xr_validated_program_id(first->program),
                                    xr_validated_program_id(second->program)));
    ASSERT_EQ_UINT(xr_validated_program_entry_function(first->program),
                   xr_validated_program_entry_function(second->program));
}

static void assert_source_build_ok(const XrProgramSourceBuildInput *input,
                                   XrProgramSourceProduct *product,
                                   XrProgramSourceDiagnostic *diagnostic) {
    XrProgramSourceBuildStatus status = xr_program_source_build(input, product, diagnostic);
    if (status != XR_PROGRAM_SOURCE_BUILD_OK) {
        fprintf(stderr,
                "source build failed: status=%s stage=%u module=%u underlying=%u "
                "writer=%u source=%s:%u message=%s\n",
                xr_program_source_build_status_name(status), (unsigned) diagnostic->stage,
                diagnostic->module_index, diagnostic->underlying_status,
                (unsigned) diagnostic->writer_status, diagnostic->source_path,
                diagnostic->source_line, diagnostic->message);
    }
    ASSERT_EQ_INT(status, XR_PROGRAM_SOURCE_BUILD_OK);
}

static void assert_retained_root_native(const XrProgramSourceProduct *product,
                                        const XrTargetProfile *profile, const uint32_t *functions,
                                        const char *path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(product->program, profile, &options, &ir, &diagnostic),
                  XR_BACKEND_OK);
    XrGeneratedC generated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic), XR_BACKEND_OK);
    if (path) {
        FILE *output = fopen(path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_GT(fprintf(output, "#define main xr_retained_default_main\n"), 0);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_GT(fprintf(output,
                          "\n#undef main\nint main(void) {\n"
                          "    XrAotModules modules[2] = {0};\n"
                          "    XrAotContext contexts[2] = {0};\n"
                          "    int failed = 0;\n"
                          "    for (unsigned int instance = 0; instance < 2; ++instance) {\n"
                          "        XrAotContext *ctx = &contexts[instance];\n"
                          "        ctx->modules = &modules[instance];\n"
                          "        for (int64_t expected = 41; expected <= 43; ++expected) {\n"
                          "            XrAotOutcome init = xr_aot_initialize_modules(ctx);\n"
                          "            if (init.kind != 0) { failed = 1; break; }\n"
                          "            XrAotOutcome value = xr_aot_fn_%u(ctx);\n"
                          "            XrAotOutcome read = xr_aot_fn_%u(ctx);\n"
                          "            if (value.kind != 0 || read.kind != 0 ||\n"
                          "                value.i64 != expected || read.i64 != expected) {\n"
                          "                failed = 2; break;\n"
                          "            }\n"
                          "        }\n"
                          "    }\n"
                          "    xr_aot_modules_clear(&modules[1]);\n"
                          "    xr_aot_modules_clear(&modules[0]);\n"
                          "    return failed;\n}\n",
                          functions[0], functions[1]),
                  0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

TEST(source_owner_explicit_bool_conditions_execute) {
    static const char source[] =
        "fn amount(present: bool) -> i64? { if (present) { return 0 }; return null }\n"
        "fn answer() -> i64 {\n"
        " var absent = amount(false)\n var zero = amount(true)\n var score: i64 = 0\n"
        " if (zero != null) { score += 1 }\n if (absent == null) { score += 2 }\n"
        " if (absent != null && absent > 0) { return -1 }\n"
        " var selected = zero != null ? zero : 99\n"
        " if (zero == null || zero != 0) { return -2 }\n"
        " for (var index = 0; index < 2; index++) { score += 4 }\n"
        " while (score < 13) { score += 1 }\n"
        " for (var index = 0; ; index++) { break }\n"
        " var extra = match (score) { value if (value == 13) -> 29\n _ -> 0 }\n"
        " return score + selected + extra\n}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    assert_detached_program_i64_result(product.program, profile, 42);
    assert_aot_fixture_backend_contract(product.program, profile, XR_SOURCE_FIXTURE_BOOL_CONDITIONS);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
}

TEST(source_owner_bare_nullable_condition_has_no_product) {
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture,
        "fn probe(value: i64?) -> i64 { if (value) { return 1 }; return 0 }\n"
        "fn answer() -> i64 { return probe(null) }\n", NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&fixture.input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
    ASSERT_EQ_UINT(diagnostic.underlying_status, XR_ERR_ANALYZE_CONDITION_TYPE);
    ASSERT_NULL(product.program);
    ASSERT_NULL(product.artifact.bytes);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_manifest_exports_retain_exact_detached_roots) {
    static const char source[] =
        "import \"./library\"\n"
        "fn answer() -> i64 { return 0 }\n"
        "fn bump() -> i64 { return library.bump() }\n"
        "fn read() -> i64 { return library.read() }\n"
        "fn unselected() -> i64 { return 999 }\n"
        "@test\nfn checkState() { assert(library.read() == 40) }\n";
    static const char library[] =
        "var counter: i64 = 40\n"
        "export fn bump() -> i64 { counter = counter + 1; return counter }\n"
        "export fn read() -> i64 { return counter }\n";
    static const char manifest[] =
        "[[export.c]]\nxray = \"read\"\nsymbol = \"read_counter\"\nheader = true\n"
        "[[export.c]]\nxray = \"bump\"\nsymbol = \"bump_counter\"\n"
        "visibility = \"hidden\"\nabi = \"hosted-vm-v1\"\n"
        "[[export.c]]\nxray = \"answer\"\nsymbol = \"answer\"\nabi = \"native\"\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, library));
    XrTomlValue *toml = xtoml_parse(manifest, sizeof(manifest) - 1u);
    ASSERT_NOT_NULL(toml);
    XrNativePackagePlan *plan = xr_native_package_plan_parse(toml, fixture.directory);
    xtoml_free(toml);
    ASSERT_NOT_NULL(plan);
    ASSERT_TRUE(plan->valid);
    xr_compiler_session_set_native_package_plan(fixture.session, plan);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    fixture.input.discover_exports = 1u;
    fixture.input.discover_tests = 1u;
    XrProgramSourceProduct product = {0}, combined = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_EQ_UINT(product.export_count, 3u);
    ASSERT_EQ_UINT(product.test_entry_count, 1u);
    ASSERT_EQ_UINT(product.retained_function_count, 0u);
    ASSERT_NULL(product.retained_function_ids);
    ASSERT_EQ_UINT(xr_validated_program_function_count(product.program), 8u);
    ASSERT_EQ_UINT(product.export_function_ids[2],
                   xr_validated_program_entry_function(product.program));
    XrProgramSourceEntryIdentity explicit_root = fixture.input.entry;
    explicit_root.function_name = "bump";
    fixture.input.retained_entries = &explicit_root;
    fixture.input.retained_entry_count = 1u;
    assert_source_build_ok(&fixture.input, &combined, &diagnostic);
    assert_products_equal(&product, &combined);
    ASSERT_EQ_UINT(combined.retained_function_ids[0], product.export_function_ids[1]);
    for (uint32_t index = 0u; index < 3u; ++index)
        ASSERT_EQ_UINT(combined.export_function_ids[index], product.export_function_ids[index]);
    xr_program_source_product_free(&combined);
    source_build_fixture_free(&fixture);
    xr_native_package_plan_free(plan);
    ASSERT_EQ_INT(strcmp(product.exports[0].xray_name, "read"), 0);
    ASSERT_EQ_INT(strcmp(product.exports[0].symbol, "read_counter"), 0);
    ASSERT_TRUE(product.exports[0].header);
    ASSERT_NULL(product.exports[0].abi);
    ASSERT_NULL(product.exports[0].visibility);
    ASSERT_FALSE(product.exports[1].header);
    ASSERT_EQ_INT(strcmp(product.exports[1].visibility, "hidden"), 0);
    ASSERT_EQ_INT(strcmp(product.exports[1].abi, "hosted-vm-v1"), 0);
    ASSERT_EQ_INT(strcmp(product.exports[2].abi, "native"), 0);
    XrVmCode *code = NULL;
    ASSERT_EQ_INT(xr_vm_code_build(product.program, profile, NULL, &code, NULL), XR_VM_CODE_OK);
    uint32_t functions[] = {product.export_function_ids[1], product.export_function_ids[0]};
    for (uint32_t index = 0u; index < 2u; ++index) {
        XrExecutionBindingInput binding = {
            .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = product.program, .profile = profile, .generation = 1u,
        };
        XrInstance *instance = NULL;
        ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
        XrVmOutcome checked = xr_vm_code_execute(
            code, instance, product.tests[0].function_id, NULL, 0u);
        ASSERT_EQ_INT(checked.kind, XR_VM_OUTCOME_RETURN);
        xr_vm_outcome_dispose(&checked);
        for (int64_t expected = 41; expected <= 43; ++expected) {
            for (uint32_t root = 0u; root < 2u; ++root) {
                XrVmOutcome result = xr_vm_code_execute(code, instance, functions[root], NULL, 0u);
                ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_RETURN);
                ASSERT_EQ_INT(result.value.kind, XR_VM_VALUE_I64);
                ASSERT_EQ_INT(result.value.as.i64, expected);
                xr_vm_outcome_dispose(&result);
            }
        }
        ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
    }
    xr_vm_code_free(code);
    assert_retained_root_native(&product, profile, functions,
                                source_fixture_output_path(XR_SOURCE_FIXTURE_MANIFEST_EXPORTS));
    xr_program_source_product_free(&product);
    ASSERT_NULL(product.exports);
    ASSERT_NULL(product.export_function_ids);
    ASSERT_EQ_UINT(product.export_count, 0u);
    xr_target_profile_free(profile);
}

TEST(source_owner_rejects_invalid_manifest_roots_without_partial_product) {
    static const char source[] =
        "fn answer() -> i64 { return 42 }\n"
        "fn generic<T>(value: T) -> T { return value }\n";
    static const char manifest[] =
        "[[export.c]]\nxray = \"answer\"\nsymbol = \"answer\"\n";
    for (uint32_t invalid = 0u; invalid < 6u; ++invalid) {
        SourceBuildFixture fixture;
        ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
        XrTomlValue *toml = xtoml_parse(manifest, sizeof(manifest) - 1u);
        ASSERT_NOT_NULL(toml);
        XrNativePackagePlan *plan = xr_native_package_plan_parse(toml, fixture.directory);
        xtoml_free(toml);
        ASSERT_NOT_NULL(plan);
        ASSERT_TRUE(plan->valid);
        char *original_name = plan->exports[0].xray_name;
        char *original_symbol = plan->exports[0].symbol;
        XrProgramSourceBuildStatus expected = XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED;
        fixture.input.discover_exports = 1u;
        switch (invalid) {
            case 0: plan->exports[0].xray_name = "absent"; break;
            case 1: plan->exports[0].xray_name = "generic"; break;
            case 2: plan->valid = false; break;
            case 3: plan->exports[0].symbol = NULL; break;
            case 4:
                fixture.input.discover_exports = 2u;
                expected = XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT;
                break;
            case 5:
                plan->export_count = XR_PROGRAM_LIMIT_FUNCTIONS + 1u;
                expected = XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT;
                break;
        }
        xr_compiler_session_set_native_package_plan(fixture.session, plan);
        XrProgramSourceProduct product = {0};
        XrProgramSourceDiagnostic diagnostic;
        ASSERT_EQ_INT(xr_program_source_build(&fixture.input, &product, &diagnostic), expected);
        ASSERT_TRUE(diagnostic.message[0] != '\0');
        ASSERT_NULL(product.program);
        ASSERT_NULL(product.artifact.bytes);
        ASSERT_NULL(product.exports);
        ASSERT_NULL(product.export_function_ids);
        ASSERT_EQ_UINT(product.export_count, 0u);
        plan->exports[0].xray_name = original_name;
        plan->exports[0].symbol = original_symbol;
        plan->export_count = 1u;
        source_build_fixture_free(&fixture);
        xr_native_package_plan_free(plan);
    }
}

TEST(source_owner_test_discovery_detaches_metadata_and_exact_ids) {
    static const char source[] =
        "import \"./library\"\n"
        "fn answer() -> i64 { return 0 }\n"
        "@test(timeout: 2)\nfn first() { assert(library.bump() == 41) }\n"
        "@test\nfn second() { assert(library.read() == 41) }\n"
        "@test(skip)\nfn skipped() {}\n"
        "@before_each\nfn beforeEach() {}\n"
        "@after_each\nfn afterEach() {}\n"
        "@before_all\nfn beforeAll() {}\n"
        "@after_all\nfn afterAll() {}\n";
    static const char library[] =
        "var counter: i64 = 40\n"
        "export fn bump() -> i64 { counter = counter + 1; return counter }\n"
        "export fn read() -> i64 { return counter }\n"
        "@test\nfn dependencyTest() { assert(false) }\n";
    static const char *names[] = {"first", "second", "skipped", "beforeEach", "afterEach",
                                   "beforeAll", "afterAll"};
    static const XrProgramSourceTestKind kinds[] = {
        XR_PROGRAM_TEST_CASE, XR_PROGRAM_TEST_CASE, XR_PROGRAM_TEST_SKIP,
        XR_PROGRAM_TEST_BEFORE_EACH, XR_PROGRAM_TEST_AFTER_EACH,
        XR_PROGRAM_TEST_BEFORE_ALL, XR_PROGRAM_TEST_AFTER_ALL,
    };
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, library));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    fixture.input.discover_tests = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    ASSERT_EQ_UINT(product.retained_function_count, 0u);
    ASSERT_NULL(product.retained_function_ids);
    ASSERT_EQ_UINT(product.test_entry_count, 7u);
    for (uint32_t index = 0u; index < 7u; ++index) {
        ASSERT_EQ_INT(strcmp(product.tests[index].name, names[index]), 0);
        ASSERT_EQ_INT(product.tests[index].kind, kinds[index]);
        ASSERT_EQ_UINT(product.tests[index].timeout_seconds, index == 0u ? 2u : 0u);
        ASSERT_LT(product.tests[index].function_id,
                  xr_validated_program_function_count(product.program));
        for (uint32_t prior = 0u; prior < index; ++prior)
            ASSERT_NE(product.tests[index].function_id, product.tests[prior].function_id);
    }
    XrVmCode *code = NULL;
    ASSERT_EQ_INT(xr_vm_code_build(product.program, profile, NULL, &code, NULL), XR_VM_CODE_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program, .profile = profile, .generation = 1u,
    };
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
    {
        for (uint32_t index = 0u; index < 2u; ++index) {
            XrVmOutcome result = xr_vm_code_execute(code, instance,
                                                     product.tests[index].function_id, NULL, 0u);
            ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_RETURN);
            ASSERT_EQ_INT(result.value.kind, XR_VM_VALUE_VOID);
            xr_vm_outcome_dispose(&result);
        }
    }
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
    xr_vm_code_free(code);
    xr_program_source_product_free(&product);
    ASSERT_NULL(product.tests);
    ASSERT_EQ_UINT(product.test_entry_count, 0u);
    xr_target_profile_free(profile);
}

TEST(source_owner_retained_roots_share_one_detached_program_and_instance) {
    static const char library[] =
        "var counter: i64 = 40\n"
        "export fn bump() -> i64 { counter = counter + 1; return counter }\n"
        "export fn read() -> i64 { return counter }\n";
    static const char source[] = "import \"./library\"\n"
                                 "fn answer() -> i64 { return 0 }\n"
                                 "fn testCase() -> i64 { return library.bump() }\n"
                                 "fn unselected() -> i64 { return 999 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, library));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    char *library_identity = NULL;
    char *library_path = NULL;
    ASSERT_TRUE(xr_module_identity_from_source(&fixture.authority, fixture.dependency_path,
                                               &library_identity, &library_path));
    XrProgramSourceEntryIdentity roots[2] = {fixture.input.entry, fixture.input.entry};
    roots[0].function_name = "testCase";
    roots[1].module_identity = library_identity;
    roots[1].function_name = "read";
    xr_module_source_fingerprint(library, &roots[1].source_content_fingerprint);
    fixture.input.retained_entries = roots;
    fixture.input.retained_entry_count = 2u;
    XrProgramSourceProduct first = {0}, reversed = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    ASSERT_NOT_NULL(first.program);
    ASSERT_EQ_UINT(first.retained_function_count, 2u);
    ASSERT_NOT_NULL(first.retained_function_ids);
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 6u);
    ASSERT_NE(first.retained_function_ids[0], first.retained_function_ids[1]);
    ASSERT_NE(first.retained_function_ids[0], xr_validated_program_entry_function(first.program));
    XrProgramSourceEntryIdentity swap = roots[0];
    roots[0] = roots[1];
    roots[1] = swap;
    assert_source_build_ok(&fixture.input, &reversed, &diagnostic);
    assert_products_equal(&first, &reversed);
    ASSERT_EQ_UINT(first.retained_function_ids[0], reversed.retained_function_ids[1]);
    ASSERT_EQ_UINT(first.retained_function_ids[1], reversed.retained_function_ids[0]);
    source_build_fixture_free(&fixture);
    xr_free(library_identity);
    xr_free(library_path);
    memset(roots, 0, sizeof(roots));
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(first.program, profile, &options, &code, NULL),
                      XR_VM_CODE_OK);
        for (uint32_t index = 0u; index < 2u; ++index) {
            XrExecutionBindingInput binding = {
                .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                .program = first.program,
                .profile = profile,
                .generation = 1u,
            };
            XrInstance *instance = NULL;
            ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
            for (int64_t expected = 41; expected <= 43; ++expected) {
                for (uint32_t root = 0u; root < 2u; ++root) {
                    XrVmOutcome result = xr_vm_code_execute(
                        code, instance, first.retained_function_ids[root], NULL, 0u);
                    ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_RETURN);
                    ASSERT_EQ_INT(result.value.kind, XR_VM_VALUE_I64);
                    ASSERT_EQ_INT(result.value.as.i64, expected);
                    xr_vm_outcome_dispose(&result);
                }
            }
            ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
        }
        xr_vm_code_free(code);
    }
    assert_retained_root_native(&first, profile, first.retained_function_ids,
                                source_fixture_output_path(XR_SOURCE_FIXTURE_RETAINED_ROOTS));
    xr_program_source_product_free(&reversed);
    xr_program_source_product_free(&first);
    ASSERT_NULL(first.retained_function_ids);
    ASSERT_EQ_UINT(first.retained_function_count, 0u);
    xr_target_profile_free(profile);
}

TEST(source_owner_rejects_invalid_retained_root_identity_without_partial_product) {
    static const char source[] = "fn answer() -> i64 { return 0 }\n"
                                 "fn kept() -> i64 { return 1 }\n";
    for (uint32_t invalid = 0u; invalid < 9u; ++invalid) {
        SourceBuildFixture fixture;
        ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
        char *wrong_identity = NULL;
        XrProgramSourceEntryIdentity roots[2] = {fixture.input.entry, fixture.input.entry};
        roots[0].function_name = "kept";
        roots[1] = roots[0];
        fixture.input.retained_entries = roots;
        fixture.input.retained_entry_count = 1u;
        XrProgramSourceBuildStatus expected = XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED;
        switch (invalid) {
            case 0:
                roots[0].source_content_fingerprint.bytes[0] ^= 1u;
                break;
            case 1:
                roots[0].function_name = "absent";
                break;
            case 2:
                fixture.input.retained_entry_count = 2u;
                break;
            case 3:
                roots[0].function_name = "answer";
                break;
            case 4:
                roots[0].reserved8[0] = 1u;
                expected = XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT;
                break;
            case 5:
                fixture.input.retained_entries = NULL;
                expected = XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT;
                break;
            case 6:
                fixture.input.retained_entry_count = XR_PROGRAM_LIMIT_FUNCTIONS;
                expected = XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT;
                break;
            case 8:
                ASSERT_TRUE(xr_module_identity_from_logical(&fixture.authority, "absent.xr",
                                                            &wrong_identity));
                roots[0].module_identity = wrong_identity;
                break;
            case 7:
                --fixture.input.schema_version;
                expected = XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT;
                break;
        }
        XrProgramSourceProduct product = {0};
        XrProgramSourceDiagnostic diagnostic;
        ASSERT_EQ_INT(xr_program_source_build(&fixture.input, &product, &diagnostic), expected);
        ASSERT_NULL(product.program);
        ASSERT_NULL(product.artifact.bytes);
        ASSERT_NULL(product.retained_function_ids);
        ASSERT_EQ_UINT(product.retained_function_count, 0u);
        xr_free(wrong_identity);
        source_build_fixture_free(&fixture);
    }
}

static void check_source_record_type_identity(void) {
    static const char *const sources[] = {
        "type State = { token:i64, enabled:bool }\nvar state:State?=null\nfn answer() -> i64 { return 42 }\n",
        "type Other = { enabled:bool, token:i64 }\nvar state:Other?=null\nfn answer() -> i64 { return 42 }\n",
        "type State = { token:i64, active:bool }\nvar state:State?=null\nfn answer() -> i64 { return 42 }\n",
        "type State = { const token:i64, enabled:bool }\nvar state:State?=null\nfn answer() -> i64 { return 42 }\n",
        "type State = { token:i64, enabled:i64 }\nvar state:State?=null\nfn answer() -> i64 { return 42 }\n",
    };
    XrCoreIrKey original = {{0}};
    for (uint32_t scenario = 0u; scenario < 5u; ++scenario) {
        SourceBuildFixture fixture;
        ASSERT_TRUE(source_build_fixture_init(&fixture, sources[scenario], NULL));
        XrProgramSourceProduct product = {0};
        XrProgramSourceDiagnostic diagnostic;
        assert_source_build_ok(&fixture.input, &product, &diagnostic);
        const XrValidatedType *record = NULL;
        for (uint32_t index = 0u; index < product.program->type_count; ++index) {
            const XrValidatedType *type = &product.program->types[index];
            if (type->kind == XR_CORE_IR_TYPE_RECORD_REFERENCE) {
                ASSERT_NULL(record);
                record = type;
            }
        }
        ASSERT_NOT_NULL(record);
        ASSERT_EQ_UINT(record->nominal_kind, XR_CORE_IR_NOMINAL_NONE);
        ASSERT_EQ_UINT(record->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
        ASSERT_EQ_UINT(record->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
        ASSERT_EQ_UINT(record->field_count, 2u);
        if (scenario == 0u)
            original = record->key;
        else
            ASSERT_EQ_UINT(xr_core_ir_key_equal(original, record->key), scenario == 1u);
        xr_program_source_product_free(&product);
        source_build_fixture_free(&fixture);
    }
}

static void check_source_resource_type_identity(void) {
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture,
        "import { NetConn } from net\nvar connection:NetConn?=null\nfn answer()->i64 { return 42 }\n", NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    const uint8_t expected[16] = {0x0b, 0x55, 0x1a, 0x7a, 0x09, 0x78, 0x02, 0x97,
                                  0x3f, 0x0b, 0x08, 0x7b, 0xd7, 0x96, 0x56, 0xc3};
    uint32_t found = 0u;
    for (uint32_t index = 0u; index < product.program->type_count; ++index) {
        const XrValidatedType *type = &product.program->types[index];
        if (type->kind != XR_CORE_IR_TYPE_PROVIDER_RESOURCE)
            continue;
        if (memcmp(type->resource_id.bytes, expected, 16u) == 0) {
            ++found;
            ASSERT_EQ_UINT(type->copy_contract, XR_CORE_IR_COPY_FORBIDDEN);
            ASSERT_EQ_UINT(type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
        }
    }
    ASSERT_EQ_UINT(found, 1u);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_single_module_is_deterministic_and_detached) {
    check_source_record_type_identity();
    check_source_resource_type_identity();
    static const char source[] =
        "fn answer() -> i64 { return 42 }\n"
        "fn choose(flag: bool) -> i64 { if (flag) { return answer() }; return 0 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XaAnalyzer *analyzer = xa_analyzer_new(fixture.session);
    ASSERT_NOT_NULL(analyzer);
    XrType *resource = xa_builtin_parse_type_string_for_module(analyzer, "net", "__NetConnStorage?");
    ASSERT_NOT_NULL(resource);
    ASSERT_EQ_INT(resource->kind, XR_KIND_INSTANCE);
    ASSERT_TRUE(resource->is_nullable);
    const uint8_t expected_resource[16] = {
        0x0b, 0x55, 0x1a, 0x7a, 0x09, 0x78, 0x02, 0x97,
        0x3f, 0x0b, 0x08, 0x7b, 0xd7, 0x96, 0x56, 0xc3};
    ASSERT_EQ_INT(memcmp(resource->instance.resource_id.bytes, expected_resource, 16u), 0);
    XrType *copied_resource = xr_type_copy(fixture.isolate, resource);
    ASSERT_NOT_NULL(copied_resource);
    ASSERT_TRUE(xr_type_equals(resource, copied_resource));
    ASSERT_TRUE(xr_type_is_subclass_of(resource, copied_resource));
    ASSERT_EQ_INT(memcmp(copied_resource->instance.resource_id.bytes, expected_resource, 16u), 0);
    copied_resource->instance.resource_id.bytes[0] ^= 1u;
    ASSERT_FALSE(xr_type_equals(resource, copied_resource));
    ASSERT_FALSE(xr_type_is_subclass_of(resource, copied_resource));
    ASSERT_FALSE(xr_type_is_subclass_of(copied_resource, resource));
    xa_analyzer_free(analyzer);
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    ASSERT_EQ_INT(diagnostic.status, XR_PROGRAM_SOURCE_BUILD_OK);
    fixture.input.budget.max_program_bytes = (uint32_t) first.artifact.size;
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 2u);
    ASSERT_EQ_UINT(first.program->module_count, 1u);
    ASSERT_NE(first.program->modules[0].initializer,
              xr_validated_program_entry_function(first.program));

    ASSERT_EQ_INT(xr_test_unlink(fixture.entry_path), 0);
    fixture.entry_path[0] = '\0';
    source_build_fixture_free(&fixture);
    size_t retained_size = 0u;
    const uint8_t *retained_bytes = xr_validated_program_bytes(first.program, &retained_size);
    ASSERT_NOT_NULL(retained_bytes);
    ASSERT_TRUE(retained_bytes != first.artifact.bytes);
    ASSERT_EQ_UINT(retained_size, first.artifact.size);
    ASSERT_EQ_INT(memcmp(retained_bytes, first.artifact.bytes, retained_size), 0);

    XrProgramDiagnostic admission_diagnostic;
    XrValidatedProgram *rejected = NULL;
    first.artifact.bytes[0] ^= UINT8_C(0xff);
    ASSERT_EQ_INT(xr_program_validate(first.artifact.bytes, first.artifact.size, NULL, &rejected,
                                      &admission_diagnostic),
                  XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED);
    ASSERT_NULL(rejected);
    ASSERT_EQ_INT(memcmp(retained_bytes, second.artifact.bytes, retained_size), 0);
    XrProgramVerifyBudget tighter_policy = xr_program_verify_default_budget();
    tighter_policy.max_work = 1u;
    ASSERT_EQ_INT(xr_program_validate(retained_bytes, retained_size, &tighter_policy, &rejected,
                                      &admission_diagnostic),
                  XR_PROGRAM_VERIFY_RESOURCE_LIMIT);
    ASSERT_NULL(rejected);
    xr_program_artifact_free(&first.artifact);
    retained_bytes = xr_validated_program_bytes(first.program, &retained_size);
    ASSERT_NOT_NULL(retained_bytes);
    ASSERT_EQ_UINT(retained_size, second.artifact.size);
    ASSERT_EQ_INT(memcmp(retained_bytes, second.artifact.bytes, retained_size), 0);

    xr_program_source_product_free(&second);
    ASSERT_NULL(second.artifact.bytes);
    ASSERT_NULL(second.program);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
}

TEST(source_owner_two_module_graph_is_deterministic) {
    static const char library_source[] =
        "enum Unused { First, Second }\n"
        "export interface ReadCounter { current() -> i64 }\n"
        "var offset: i64 = 1\n"
        "export fn increment(value: i64) -> i64 { return value + offset }\n"
        "export fn unused(value: i64) -> i64 { return value + 100 }\n";
    static const char entry_source[] =
        "import { ReadCounter as Counter, increment } from \"./library\"\n"
        "fn answer() -> i64 { return increment(41) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 4u);
    ASSERT_EQ_UINT(first.program->module_count, 2u);
    ASSERT_EQ_UINT(first.program->modules[0].dependency_count, 0u);
    ASSERT_EQ_UINT(first.program->modules[1].dependency_count, 1u);
    ASSERT_EQ_UINT(first.program->modules[1].dependencies[0], 0u);
    ASSERT_NE(first.program->modules[0].initializer, first.program->modules[1].initializer);
    ASSERT_EQ_UINT(first.program->modules[0].slot_count, 1u);
    ASSERT_EQ_UINT(first.program->modules[0].slots[0].type_id, XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(first.program->modules[1].slot_count, 0u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_PLACE_MODULE), 2u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_PLACE_INITIALIZE), 1u);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(first.program, profile, &options, &code, NULL),
                      XR_VM_CODE_OK);
        XrExecutionBindingInput binding = {
            .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
            .program = first.program,
            .profile = profile,
            .generation = 1u,
        };
        XrInstance *instance = NULL;
        ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
        for (uint32_t repeat = 0u; repeat < 2u; ++repeat) {
            XrVmOutcome result = xr_vm_code_execute(
                code, instance, xr_validated_program_entry_function(first.program), NULL, 0u);
            ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_RETURN);
            ASSERT_EQ_INT(result.value.kind, XR_VM_VALUE_I64);
            ASSERT_EQ_INT(result.value.as.i64, 42);
        }
        xr_vm_code_free(code);
        ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
    }
    xr_target_profile_free(profile);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_module_initializer_is_a_canonical_entry) {
    static const char source[] = "struct Box<T> { value: T }\n"
                                 "print(Box<i64>{value: 41}.value + 1)\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    fixture.input.entry.kind = XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER;
    fixture.input.entry.function_name = NULL;
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_EQ_UINT(xr_validated_program_function_count(product.program), 1u);
    ASSERT_EQ_UINT(xr_validated_program_entry_function(product.program), 0u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT),
                   1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_AGGREGATE_PROJECT), 1u);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_NOT_NULL(generated.bytes);
    ASSERT_NOT_NULL(strstr(generated.bytes, "int main(void)"));

    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

static void assert_cross_module_coroutine_program(const char *entry_source,
                                                  const char *library_source,
                                                  XrSourceFixtureId fixture_id) {
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);

    const XrValidatedProgram *program = first.program;
    uint32_t entry_function = xr_validated_program_entry_function(program);
    ASSERT_LT(entry_function, program->function_count);
    const XrValidatedFunction *entry = &program->functions[entry_function];
    ASSERT_EQ_UINT(entry->coroutine_state_count, 2u);
    ASSERT_EQ_UINT(entry->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(entry->effect_mask,
                   XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND);
    ASSERT_EQ_UINT(entry->capability_mask, XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION);

    const XrValidatedInstruction *coroutine_call = NULL;
    for (uint32_t block_index = 0u; block_index < entry->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *candidate = &block->instructions[instruction_index];
            if (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                ASSERT_NULL(coroutine_call);
                coroutine_call = candidate;
            }
        }
    }
    ASSERT_NOT_NULL(coroutine_call);
    ASSERT_EQ_INT(coroutine_call->immediate_kind, XR_CORE_IR_IMMEDIATE_COROUTINE_CALL);
    ASSERT_EQ_UINT(coroutine_call->immediate.coroutine_call.safepoint_id, 0u);
    ASSERT_EQ_UINT(coroutine_call->successor_count, 2u);
    const XrValidatedBlock *cancel = &entry->blocks[coroutine_call->successors[1]];
    ASSERT_EQ_UINT(cancel->argument_count, 0u);
    ASSERT_TRUE(cancel->instruction_count != 0u);
    ASSERT_EQ_UINT(cancel->instructions[cancel->instruction_count - 1u].operation_id,
                   XR_CORE_OP_CORE_CANCEL_PUBLISH);
    uint32_t child_function = coroutine_call->immediate.coroutine_call.function_id;
    ASSERT_LT(child_function, program->function_count);
    ASSERT_TRUE(child_function != entry_function);
    const XrValidatedFunction *child = &program->functions[child_function];
    ASSERT_EQ_UINT(child->parameter_count, 1u);
    ASSERT_EQ_UINT(child->parameter_types[0], XR_CORE_TYPE_I64);
    ASSERT_EQ_INT(child->parameter_modes[0], XR_PARAM_READ);
    ASSERT_EQ_UINT(child->result_type_id, XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(child->coroutine_state_count, 3u);
    ASSERT_EQ_UINT(child->coroutine_safepoint_count, 2u);
    ASSERT_EQ_UINT(child->effect_mask, XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND);
    ASSERT_EQ_UINT(child->capability_mask, XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(
        xr_vm_code_build(binding.program, binding.profile, NULL, &vm_code, &vm_diagnostic),
        XR_VM_CODE_OK);
    XrVmExecution *vm_execution = NULL;
    ASSERT_TRUE(xr_vm_execution_create(vm_code, instance, entry_function, NULL, 0u, &vm_execution));
    XrVmOutcome vm_suspend = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(vm_suspend.state_id, 1u);
    vm_suspend = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(vm_suspend.state_id, 1u);
    XrVmOutcome vm_return = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_return.value.as.i64, 7);
    xr_vm_execution_free(vm_execution);

    for (uint32_t child_point = 1u; child_point <= 2u; ++child_point) {
        XrVmExecution *vm_cancel = NULL;
        ASSERT_TRUE(
            xr_vm_execution_create(vm_code, instance, entry_function, NULL, 0u, &vm_cancel));
        for (uint32_t step = 0u; step < child_point; ++step)
            ASSERT_EQ_INT(xr_vm_execution_step(vm_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_vm_execution_cancel(vm_cancel).kind, XR_VM_OUTCOME_CANCELLED);
        xr_vm_execution_free(vm_cancel);
    }
    xr_vm_code_free(vm_code);

    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    XrBackendOptions options = xr_backend_default_options();
    ASSERT_EQ_INT(
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "child_active_0"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_entry_coroutine_cancel"));
    ASSERT_NULL(strstr(generated.bytes, "XrProto"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(fixture_id);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_cross_module_coroutine_call_has_one_program_and_private_executors) {
    static const char library_source[] = "export fn child(value: i64) -> i64 {\n"
                                         "  Coro.yield()\n"
                                         "  Coro.yield()\n"
                                         "  return value\n"
                                         "}\n";
    static const char entry_source[] = "import { child } from \"./library\"\n"
                                       "fn answer() -> i64 { return child(7) }\n";
    assert_cross_module_coroutine_program(entry_source, library_source,
                                          XR_SOURCE_FIXTURE_CROSS_MODULE_COROUTINE);
}

TEST(source_owner_cross_module_static_method_coroutine_has_one_program_and_private_executors) {
    static const char library_source[] = "export class Worker {\n"
                                         "  static child(value: i64) -> i64 {\n"
                                         "    Coro.yield()\n"
                                         "    Coro.yield()\n"
                                         "    return value\n"
                                         "  }\n"
                                         "}\n";
    static const char entry_source[] = "import \"./library\" as library\n"
                                       "fn answer() -> i64 { return library.Worker.child(7) }\n";
    assert_cross_module_coroutine_program(entry_source, library_source,
                                          XR_SOURCE_FIXTURE_CROSS_MODULE_STATIC_METHOD_COROUTINE);
}

TEST(source_owner_cross_module_instance_method_preserves_receiver_across_suspend) {
    static const char library_source[] = "export final class Worker {\n"
                                         "  value: i64\n"
                                         "  constructor(value: i64) { this.value = value }\n"
                                         "  child(add: i64) -> i64 {\n"
                                         "    Coro.yield()\n"
                                         "    Coro.yield()\n"
                                         "    return this.value + add\n"
                                         "  }\n"
                                         "}\n";
    static const char entry_source[] = "import { Worker } from \"./library\"\n"
                                       "fn answer() -> i64 {\n"
                                       "  var worker = Worker(40)\n"
                                       "  return worker.child(2)\n"
                                       "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED),
                   1u);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .generation = 1u,
    };
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
    {
        SourceClassLifecycleLog log = {0};
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = &log;
        options.lifecycle_event = record_source_class_lifecycle;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(product.program, profile, &options, &code, NULL),
                      XR_VM_CODE_OK);
        for (uint32_t cancel_after = 0u; cancel_after <= 2u; ++cancel_after) {
            log = (SourceClassLifecycleLog) {0};
            XrVmExecution *execution = NULL;
            ASSERT_TRUE(xr_vm_execution_create(code, instance,
                                               xr_validated_program_entry_function(product.program),
                                               NULL, 0u, &execution));
            uint32_t steps = cancel_after ? cancel_after : 2u;
            for (uint32_t step = 0u; step < steps; ++step) {
                ASSERT_EQ_INT(xr_vm_execution_step(execution).kind, XR_VM_OUTCOME_SUSPENDED);
                for (uint32_t event = 0u; event < log.count; ++event) {
                    ASSERT_TRUE(log.events[event].kind != XR_VM_EVENT_CLASS_FINALIZE);
                    ASSERT_TRUE(log.events[event].kind != XR_VM_EVENT_CLASS_RECLAIM);
                }
            }
            if (cancel_after) {
                ASSERT_EQ_INT(xr_vm_execution_cancel(execution).kind, XR_VM_OUTCOME_CANCELLED);
            } else {
                XrVmOutcome outcome = xr_vm_execution_step(execution);
                ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
                ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
                ASSERT_EQ_INT(outcome.value.as.i64, 42);
                xr_vm_outcome_dispose(&outcome);
            }
            ASSERT_FALSE(log.overflow);
            uint32_t constructed = 0u;
            for (uint32_t event = 0u; event < log.count; ++event) {
                if (log.events[event].kind != XR_VM_EVENT_CLASS_CONSTRUCT)
                    continue;
                ++constructed;
                uint64_t identity = log.events[event].identity;
                ASSERT_EQ_UINT(
                    source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_FINALIZE, identity), 1u);
                ASSERT_EQ_UINT(
                    source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_RECLAIM, identity), 1u);
            }
            ASSERT_EQ_UINT(constructed, 1u);
            xr_vm_execution_free(execution);
        }
        xr_vm_code_free(code);
    }
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
    assert_aot_fixture_backend_contract(product.program, profile,
                                        XR_SOURCE_FIXTURE_CROSS_MODULE_INSTANCE_METHOD_COROUTINE);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_function_parameter_callable_has_one_program_and_private_executors) {
    {
        SourceBuildFixture open;
        ASSERT_TRUE(source_build_fixture_init(
            &open, "fn answer(callback: fn(i64) -> i64) -> i64 { return callback(1) }\n", NULL));
        XrProgramSourceProduct rejected = {0};
        XrProgramSourceDiagnostic rejection;
        ASSERT_EQ_INT(xr_program_source_build(&open.input, &rejected, &rejection),
                      XR_PROGRAM_SOURCE_BUILD_PROGRAM_REJECTED);
        ASSERT_EQ_INT(rejection.stage, XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE);
        ASSERT_EQ_INT(rejection.writer_status, XR_PROGRAM_BUILD_INVALID_INPUT);
        ASSERT_NOT_NULL(strstr(rejection.message, "inconsistent callable effect evidence"));
        ASSERT_NULL(rejected.program);
        ASSERT_NULL(rejected.artifact.bytes);
        source_build_fixture_free(&open);
    }
    static const char source[] =
        "fn apply(value: i64, body: fn(i64) -> i64) -> i64 {\n"
        "  return body(value)\n"
        "}\n"
        "fn twice(value: i64) -> i64 { return value * 2 }\n"
        "fn answer() -> i64 {\n"
        "  const named = apply(21, twice)\n"
        "  const closure = apply(5, fn(value: i64) -> i64 { return value + 1 })\n"
        "  return named + closure\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (!product.program) {
        xr_program_source_product_free(&product);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    uint32_t entry = xr_validated_program_entry_function(product.program);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(
        xr_vm_code_build(binding.program, binding.profile, NULL, &vm_code, &vm_diagnostic),
        XR_VM_CODE_OK);
    XrVmOutcome vm_result = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm_result.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_result.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_result.value.as.i64, 48);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, ".function_id)"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_FUNCTION_PARAMETER);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }

    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    xr_vm_code_free(vm_code);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&product);

    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_generic_specializations_are_exact_program_functions) {
    static const char source[] = "fn identity<T>(value: T) -> T { return value }\n"
                                 "fn answer() -> i64 {\n"
                                 "  const explicit = identity<i64>(40)\n"
                                 "  const inferred = identity(1)\n"
                                 "  if (identity<bool>(true)) {\n"
                                 "    return explicit + inferred + 1\n"
                                 "  }\n"
                                 "  return 0\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    ASSERT_NOT_NULL(first.program);
    ASSERT_NOT_NULL(second.program);
    assert_products_equal(&first, &second);

    XrProgramSourceBuildInput open_entry_input = fixture.input;
    open_entry_input.entry.function_name = "identity";
    XrProgramSourceProduct rejected = {0};
    ASSERT_EQ_INT(xr_program_source_build(&open_entry_input, &rejected, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_PROGRAM_REJECTED);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE);
    ASSERT_EQ_INT(diagnostic.writer_status, XR_PROGRAM_BUILD_INVALID_INPUT);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "canonical-program entry"));
    ASSERT_NULL(rejected.artifact.bytes);
    ASSERT_NULL(rejected.program);

    const XrValidatedProgram *program = first.program;
    ASSERT_EQ_UINT(program->function_count, 4u);
    ASSERT_EQ_UINT(program->module_count, 1u);
    uint32_t entry = xr_validated_program_entry_function(program);
    ASSERT_LT(entry, program->function_count);
    const XrValidatedFunction *entry_function = &program->functions[entry];
    ASSERT_EQ_UINT(entry_function->parameter_count, 0u);
    ASSERT_EQ_UINT(entry_function->result_type_id, XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT), 3u);

    uint32_t i64_target = UINT32_MAX;
    uint32_t bool_target = UINT32_MAX;
    uint32_t i64_calls = 0u;
    uint32_t bool_calls = 0u;
    for (uint32_t block_index = 0u; block_index < entry_function->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry_function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT)
                continue;
            ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FUNCTION);
            ASSERT_LT(instruction->immediate.function_id, program->function_count);
            ASSERT_TRUE(instruction->immediate.function_id != entry);
            const XrValidatedFunction *callee =
                &program->functions[instruction->immediate.function_id];
            ASSERT_EQ_UINT(callee->parameter_count, 1u);
            ASSERT_EQ_INT(callee->parameter_modes[0], XR_PARAM_READ);
            ASSERT_EQ_UINT(callee->parameter_types[0], callee->result_type_id);
            ASSERT_EQ_UINT(instruction->result_type_id, callee->result_type_id);
            if (callee->result_type_id == XR_CORE_TYPE_I64) {
                if (i64_target == UINT32_MAX)
                    i64_target = instruction->immediate.function_id;
                ASSERT_EQ_UINT(instruction->immediate.function_id, i64_target);
                ++i64_calls;
            } else {
                ASSERT_EQ_UINT(callee->result_type_id, XR_CORE_TYPE_BOOL);
                if (bool_target == UINT32_MAX)
                    bool_target = instruction->immediate.function_id;
                ASSERT_EQ_UINT(instruction->immediate.function_id, bool_target);
                ++bool_calls;
            }
        }
    }
    ASSERT_EQ_UINT(i64_calls, 2u);
    ASSERT_EQ_UINT(bool_calls, 1u);
    ASSERT_TRUE(i64_target != UINT32_MAX);
    ASSERT_TRUE(bool_target != UINT32_MAX);
    ASSERT_TRUE(i64_target != bool_target);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    {
        XrVmCodeOptions vm_options = xr_vm_code_default_options();
        XrVmCode *vm_code = NULL;
        XrVmCodeDiagnostic vm_diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(binding.program, binding.profile, &vm_options, &vm_code,
                                       &vm_diagnostic),
                      XR_VM_CODE_OK);
        XrVmOutcome vm_result = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(vm_result.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_result.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_result.value.as.i64, 42);
        xr_vm_code_free(vm_code);
    }

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_GENERIC_SPECIALIZATION);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }

    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_generic_constraint_methods_have_exact_concrete_targets) {
    // The dependency owns the constraint and generic body, while one concrete implementor belongs
    // to the caller and shadows a dependency-private decoy. Selective-import aliases must retain
    // the generic declaration identity, and each specialization must retain the exact concrete
    // type declaration identity across the module boundary.
    static const char library_source[] =
        "export interface ReadCounter { current() -> i64 }\n"
        "export struct LeftCounter implements ReadCounter {\n"
        "  value: i64\n"
        "  current() -> i64 { return this.value }\n"
        "}\n"
        "struct LocalCounter implements ReadCounter {\n"
        "  value: i64\n"
        "  current() -> i64 { return 999 }\n"
        "}\n"
        "fn nested<U>() -> i64 { return 1 }\n"
        "export fn genericRead<T: ReadCounter>(counter: T) -> i64 {\n"
        "  return counter.current() + nested<Array<T>>() - 1\n"
        "}\n"
        "export class GenericReader {\n"
        "  bias: i64\n"
        "  constructor(bias: i64) { this.bias = bias }\n"
        "  genericRead<T: ReadCounter>(counter: T) -> i64 {\n"
        "    return this.bias + counter.current()\n"
        "  }\n"
        "  token<T>() -> i64 { return 999 }\n"
        "}\n"
        "export class GenericStaticReader {\n"
        "  static genericRead<T: ReadCounter>(counter: T) -> i64 {\n"
        "    return counter.current()\n"
        "  }\n"
        "}\n"
        "export class OtherStaticReader {\n"
        "  static genericRead<T: ReadCounter>(counter: T) -> i64 {\n"
        "    return counter.current() + 1\n"
        "  }\n"
        "}\n";
    static const char facade_source[] =
        "export { ReadCounter, LeftCounter, GenericReader, GenericStaticReader, "
        "OtherStaticReader, "
        "genericRead as readCounter } from "
        "\"./library\"\n";
    static const char entry_source[] =
        "import { ReadCounter, LeftCounter, GenericReader, GenericStaticReader, "
        "OtherStaticReader, readCounter } from \"./facade\"\n"
        "import \"./facade\" as counters\n"
        "struct LocalCounter implements ReadCounter {\n"
        "  value: i64\n"
        "  current() -> i64 { return this.value }\n"
        "}\n"
        "fn genericRead<T>(_counter: T) -> i64 { return 999 }\n"
        "class DecoyReader {\n"
        "  genericRead<T>(_counter: T) -> i64 { return 999 }\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var left = LeftCounter{value: 20}\n"
        "  var local = LocalCounter{value: 11}\n"
        "  var reader = GenericReader(0)\n"
        "  return readCounter<LeftCounter>(left) + readCounter<LocalCounter>(local) +\n"
        "         readCounter(local) + counters.readCounter(left) +\n"
        "         counters.readCounter(local) + reader.genericRead<LeftCounter>(left) +\n"
        "         reader.genericRead(local) + GenericStaticReader.genericRead<LeftCounter>(left) "
        "+\n"
        "         counters.GenericStaticReader.genericRead(local) + "
        "OtherStaticReader.genericRead(left) - 114\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    ASSERT_TRUE(source_build_fixture_add_facade(&fixture, facade_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    ASSERT_NOT_NULL(first.program);
    ASSERT_NOT_NULL(second.program);
    assert_products_equal(&first, &second);

    const XrValidatedProgram *program = first.program;
    ASSERT_EQ_UINT(program->function_count, 15u);
    ASSERT_EQ_UINT(program->module_count, 3u);
    for (uint32_t module = 0u; module < 3u; ++module) {
        ASSERT_EQ_UINT(program->modules[module].slot_count, 0u);
        ASSERT_EQ_UINT(program->functions[program->modules[module].initializer].result_type_id,
                       XR_CORE_TYPE_VOID);
    }
    ASSERT_EQ_UINT(program->interface_count, 0u);
    ASSERT_EQ_UINT(program->conformance_count, 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT), 19u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CALL_WITNESS_DIRECT), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_EXISTENTIAL_PACK), 0u);

    uint32_t entry = xr_validated_program_entry_function(program);
    ASSERT_LT(entry, program->function_count);
    const XrValidatedFunction *entry_function = &program->functions[entry];
    ASSERT_EQ_UINT(entry_function->parameter_count, 0u);
    ASSERT_EQ_UINT(entry_function->result_type_id, XR_CORE_TYPE_I64);
    uint32_t specializations[5] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t specialization_count = 0u;
    uint32_t method_specializations[2] = {UINT32_MAX, UINT32_MAX};
    uint32_t method_specialization_count = 0u;
    uint32_t entry_call_count = 0u;
    uint32_t free_call_count = 0u;
    uint32_t method_call_count = 0u;
    uint16_t class_receiver_type = XR_CORE_TYPE_VOID;
    for (uint32_t block_index = 0u; block_index < entry_function->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry_function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT)
                continue;
            ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FUNCTION);
            ASSERT_LT(instruction->immediate.function_id, program->function_count);
            const XrValidatedFunction *callee =
                &program->functions[instruction->immediate.function_id];
            ASSERT_EQ_UINT(callee->result_type_id, XR_CORE_TYPE_I64);
            ++entry_call_count;
            uint32_t *instances = specializations;
            uint32_t *instance_count = &specialization_count;
            if (callee->has_receiver) {
                ASSERT_EQ_UINT(instruction->operand_count, 2u);
                ASSERT_EQ_UINT(callee->parameter_count, 2u);
                ASSERT_EQ_INT(callee->receiver_mode, XR_PARAM_READ);
                instances = method_specializations;
                instance_count = &method_specialization_count;
                if (class_receiver_type == XR_CORE_TYPE_VOID)
                    class_receiver_type = callee->parameter_types[0];
                ASSERT_EQ_UINT(callee->parameter_types[0], class_receiver_type);
                ++method_call_count;
            } else {
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_EQ_UINT(callee->parameter_count, 1u);
                ++free_call_count;
            }
            for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
                ASSERT_EQ_INT(callee->parameter_modes[parameter], XR_PARAM_READ);
                ASSERT_EQ_UINT(entry_function->value_types[instruction->operands[parameter]],
                               callee->parameter_types[parameter]);
                ASSERT_EQ_INT(entry_function->value_categories[instruction->operands[parameter]],
                              XR_CORE_IR_VALUE);
                ASSERT_EQ_INT(entry_function->value_ownerships[instruction->operands[parameter]],
                              callee->has_receiver && parameter == 0u ? XR_CORE_IR_OWNER
                                                                      : XR_CORE_IR_NON_OWNER);
            }
            bool known = false;
            for (uint32_t specialization = 0u; specialization < *instance_count; ++specialization)
                known |= instances[specialization] == instruction->immediate.function_id;
            if (!known) {
                ASSERT_LT(*instance_count, callee->has_receiver ? 2u : 5u);
                instances[(*instance_count)++] = instruction->immediate.function_id;
            }
        }
    }
    ASSERT_EQ_UINT(entry_call_count, 10u);
    ASSERT_EQ_UINT(free_call_count, 8u);
    ASSERT_EQ_UINT(method_call_count, 2u);
    ASSERT_EQ_UINT(specialization_count, 5u);
    ASSERT_EQ_UINT(method_specialization_count, 2u);
    ASSERT_TRUE(class_receiver_type != XR_CORE_TYPE_VOID);
    const XrValidatedType *class_receiver = xr_validated_program_type(program, class_receiver_type);
    ASSERT_NOT_NULL(class_receiver);
    ASSERT_EQ_INT(class_receiver->kind, XR_CORE_IR_TYPE_CLASS_REFERENCE);
    ASSERT_EQ_INT(class_receiver->nominal_kind, XR_CORE_IR_NOMINAL_CLASS);
    ASSERT_EQ_INT(class_receiver->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
    ASSERT_EQ_INT(class_receiver->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
    ASSERT_EQ_UINT(class_receiver->field_count, 1u);
    ASSERT_EQ_UINT(class_receiver->field_types[0], XR_CORE_TYPE_I64);

    uint32_t class_construct_count = 0u;
    uint32_t class_owner_drop_count = 0u;
    uint32_t class_owner_value = UINT32_MAX;
    for (uint32_t block_index = 0u; block_index < entry_function->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry_function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT &&
                instruction->result_type_id == class_receiver_type) {
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_EQ_UINT(entry_function->value_types[instruction->operands[0]],
                               XR_CORE_TYPE_I64);
                ASSERT_EQ_INT(entry_function->value_categories[instruction->operands[0]],
                              XR_CORE_IR_VALUE);
                ASSERT_EQ_INT(entry_function->value_ownerships[instruction->operands[0]],
                              XR_CORE_IR_NON_OWNER);
                ASSERT_EQ_INT(instruction->result_category, XR_CORE_IR_VALUE);
                ASSERT_EQ_INT(instruction->result_ownership, XR_CORE_IR_OWNER);
                class_owner_value = instruction->result_id;
                ++class_construct_count;
            } else if (instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                       instruction->operand_count == 1u && class_owner_value != UINT32_MAX &&
                       entry_function->value_types[instruction->operands[0]] ==
                           class_receiver_type) {
                ++class_owner_drop_count;
            }
        }
    }
    ASSERT_EQ_UINT(class_construct_count, 1u);
    ASSERT_EQ_UINT(class_owner_drop_count, 1u);
    ASSERT_TRUE(class_owner_value != UINT32_MAX);
    ASSERT_TRUE(specializations[0] != specializations[1]);
    ASSERT_TRUE(specializations[2] != specializations[3]);
    ASSERT_TRUE(specializations[2] != specializations[4]);
    ASSERT_TRUE(specializations[3] != specializations[4]);
    ASSERT_TRUE(method_specializations[0] != method_specializations[1]);

    uint32_t method_targets[2] = {UINT32_MAX, UINT32_MAX};
    uint16_t constraint_argument_types[2] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    for (uint32_t specialization = 0u; specialization < 2u; ++specialization) {
        const XrValidatedFunction *caller = &program->functions[specializations[specialization]];
        constraint_argument_types[specialization] = caller->parameter_types[0];
        const XrValidatedType *receiver =
            xr_validated_program_type(program, constraint_argument_types[specialization]);
        ASSERT_NOT_NULL(receiver);
        ASSERT_EQ_INT(receiver->kind, XR_CORE_IR_TYPE_AGGREGATE);
        ASSERT_EQ_INT(receiver->nominal_kind, XR_CORE_IR_NOMINAL_STRUCT);
        ASSERT_EQ_INT(receiver->ownership, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL);
        ASSERT_EQ_INT(receiver->copy_contract, XR_CORE_IR_COPY_TRIVIAL);
        ASSERT_EQ_UINT(receiver->field_count, 1u);
        ASSERT_EQ_UINT(receiver->field_types[0], XR_CORE_TYPE_I64);
        uint32_t calls = 0u;
        uint32_t nested_calls = 0u;
        for (uint32_t block_index = 0u; block_index < caller->block_count; ++block_index) {
            const XrValidatedBlock *block = &caller->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT)
                    continue;
                ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FUNCTION);
                ASSERT_LT(instruction->immediate.function_id, program->function_count);
                const XrValidatedFunction *method =
                    &program->functions[instruction->immediate.function_id];
                if (!method->has_receiver) {
                    ASSERT_EQ_UINT(instruction->operand_count, 0u);
                    ++nested_calls;
                    continue;
                }
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_TRUE(method->has_receiver);
                ASSERT_EQ_INT(method->receiver_mode, XR_PARAM_READ);
                ASSERT_EQ_UINT(method->parameter_count, 1u);
                ASSERT_EQ_INT(method->parameter_modes[0], XR_PARAM_READ);
                ASSERT_EQ_UINT(method->parameter_types[0],
                               constraint_argument_types[specialization]);
                ASSERT_EQ_UINT(method->result_type_id, XR_CORE_TYPE_I64);
                ASSERT_EQ_UINT(caller->value_types[instruction->operands[0]],
                               constraint_argument_types[specialization]);
                ASSERT_EQ_INT(caller->value_categories[instruction->operands[0]], XR_CORE_IR_VALUE);
                method_targets[specialization] = instruction->immediate.function_id;
                ++calls;
            }
        }
        ASSERT_EQ_UINT(calls, 1u);
        ASSERT_EQ_UINT(nested_calls, 1u);
    }
    ASSERT_TRUE(constraint_argument_types[0] != constraint_argument_types[1]);
    ASSERT_TRUE(method_targets[0] != method_targets[1]);
    const XrValidatedType *left_type =
        xr_validated_program_type(program, constraint_argument_types[0]);
    const XrValidatedType *right_type =
        xr_validated_program_type(program, constraint_argument_types[1]);
    ASSERT_NOT_NULL(left_type);
    ASSERT_NOT_NULL(right_type);
    ASSERT_TRUE(!xr_core_ir_key_equal(left_type->key, right_type->key));

    uint32_t static_method_targets[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint16_t static_parameter_types[3] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    for (uint32_t specialization = 0u; specialization < 3u; ++specialization) {
        const XrValidatedFunction *method =
            &program->functions[specializations[specialization + 2u]];
        ASSERT_FALSE(method->has_receiver);
        ASSERT_EQ_UINT(method->parameter_count, 1u);
        ASSERT_EQ_INT(method->parameter_modes[0], XR_PARAM_READ);
        ASSERT_EQ_UINT(method->result_type_id, XR_CORE_TYPE_I64);
        static_parameter_types[specialization] = method->parameter_types[0];
        ASSERT_TRUE(static_parameter_types[specialization] == constraint_argument_types[0] ||
                    static_parameter_types[specialization] == constraint_argument_types[1]);

        uint32_t calls = 0u;
        for (uint32_t block_index = 0u; block_index < method->block_count; ++block_index) {
            const XrValidatedBlock *block = &method->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT)
                    continue;
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_LT(instruction->immediate.function_id, program->function_count);
                const XrValidatedFunction *target =
                    &program->functions[instruction->immediate.function_id];
                ASSERT_TRUE(target->has_receiver);
                ASSERT_EQ_UINT(target->parameter_count, 1u);
                ASSERT_EQ_UINT(target->parameter_types[0], static_parameter_types[specialization]);
                ASSERT_EQ_UINT(method->value_types[instruction->operands[0]],
                               static_parameter_types[specialization]);
                static_method_targets[specialization] = instruction->immediate.function_id;
                ++calls;
            }
        }
        ASSERT_EQ_UINT(calls, 1u);
    }
    ASSERT_TRUE(static_parameter_types[0] != static_parameter_types[1]);
    ASSERT_EQ_UINT(static_parameter_types[0], static_parameter_types[2]);
    ASSERT_TRUE(static_method_targets[0] != static_method_targets[1]);
    ASSERT_EQ_UINT(static_method_targets[0], static_method_targets[2]);
    for (uint32_t specialization = 0u; specialization < 3u; ++specialization)
        ASSERT_TRUE(static_method_targets[specialization] == method_targets[0] ||
                    static_method_targets[specialization] == method_targets[1]);

    uint32_t generic_method_targets[2] = {UINT32_MAX, UINT32_MAX};
    for (uint32_t specialization = 0u; specialization < 2u; ++specialization) {
        const XrValidatedFunction *method =
            &program->functions[method_specializations[specialization]];
        ASSERT_TRUE(method->has_receiver);
        ASSERT_EQ_INT(method->receiver_mode, XR_PARAM_READ);
        ASSERT_EQ_UINT(method->parameter_count, 2u);
        ASSERT_EQ_INT(method->parameter_modes[0], XR_PARAM_READ);
        ASSERT_EQ_INT(method->parameter_modes[1], XR_PARAM_READ);
        ASSERT_EQ_UINT(method->result_type_id, XR_CORE_TYPE_I64);
        if (specialization != 0u)
            ASSERT_EQ_UINT(method->parameter_types[0],
                           program->functions[method_specializations[0]].parameter_types[0]);
        ASSERT_TRUE(method->parameter_types[1] == constraint_argument_types[0] ||
                    method->parameter_types[1] == constraint_argument_types[1]);

        uint32_t calls = 0u;
        for (uint32_t block_index = 0u; block_index < method->block_count; ++block_index) {
            const XrValidatedBlock *block = &method->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT)
                    continue;
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_LT(instruction->immediate.function_id, program->function_count);
                const XrValidatedFunction *target =
                    &program->functions[instruction->immediate.function_id];
                ASSERT_TRUE(target->has_receiver);
                ASSERT_EQ_UINT(target->parameter_count, 1u);
                ASSERT_EQ_UINT(target->parameter_types[0], method->parameter_types[1]);
                ASSERT_EQ_UINT(method->value_types[instruction->operands[0]],
                               method->parameter_types[1]);
                ASSERT_EQ_INT(method->value_categories[instruction->operands[0]], XR_CORE_IR_VALUE);
                generic_method_targets[specialization] = instruction->immediate.function_id;
                ++calls;
            }
        }
        ASSERT_EQ_UINT(calls, 1u);
    }
    ASSERT_TRUE(generic_method_targets[0] != generic_method_targets[1]);
    ASSERT_TRUE((generic_method_targets[0] == method_targets[0] ||
                 generic_method_targets[0] == method_targets[1]) &&
                (generic_method_targets[1] == method_targets[0] ||
                 generic_method_targets[1] == method_targets[1]));

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    assert_vm_fixture_backend_contract(instance, program, profile,
                                       XR_SOURCE_FIXTURE_GENERIC_CONSTRAINT_METHOD);
    assert_aot_fixture_backend_contract(program, profile,
                                        XR_SOURCE_FIXTURE_GENERIC_CONSTRAINT_METHOD);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    source_build_fixture_free(&fixture);

    static const char namespace_entry_source[] =
        "import { ReadCounter, LeftCounter } from \"./facade\"\n"
        "import \"./facade\" as counters\n"
        "struct LocalCounter implements ReadCounter {\n"
        "  value: i64\n"
        "  current() -> i64 { return this.value }\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var left = LeftCounter{value: 41}\n"
        "  var local = LocalCounter{value: 1}\n"
        "  return counters.readCounter(left) + counters.readCounter(left) +\n"
        "         counters.readCounter(local) - 41\n"
        "}\n";
    SourceBuildFixture namespace_fixture;
    ASSERT_TRUE(
        source_build_fixture_init(&namespace_fixture, namespace_entry_source, library_source));
    ASSERT_TRUE(source_build_fixture_add_facade(&namespace_fixture, facade_source));
    namespace_fixture.input.semantic_profile_fingerprint =
        xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct namespace_product = {0};
    XrProgramSourceDiagnostic namespace_diagnostic;
    assert_source_build_ok(&namespace_fixture.input, &namespace_product, &namespace_diagnostic);
    assert_detached_program_i64_result(namespace_product.program, profile, 42);
    xr_program_source_product_free(&namespace_product);
    source_build_fixture_free(&namespace_fixture);

    static const char class_carrier_library_source[] = "export class Reader {\n"
                                                       "  value() -> i64 { return 10 }\n"
                                                       "}\n";
    static const char class_carrier_facade_source[] = "export { Reader } from \"./library\"\n";
    static const char class_carrier_entry_source[] =
        "import \"./library\" as direct\n"
        "import \"./facade\" as facade\n"
        "class LocalReader {\n"
        "  value() -> i64 { return 22 }\n"
        "}\n"
        "fn local_value() -> i64 {\n"
        "  var local = LocalReader()\n"
        "  return local.value()\n"
        "}\n"
        "fn direct_value() -> i64 {\n"
        "  var direct_reader = direct.Reader()\n"
        "  return direct_reader.value()\n"
        "}\n"
        "fn reexport_value() -> i64 {\n"
        "  var reexport_reader = facade.Reader()\n"
        "  return reexport_reader.value()\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  return local_value() + direct_value() + reexport_value()\n"
        "}\n";
    SourceBuildFixture class_carrier_fixture;
    ASSERT_TRUE(source_build_fixture_init(&class_carrier_fixture, class_carrier_entry_source,
                                          class_carrier_library_source));
    ASSERT_TRUE(
        source_build_fixture_add_facade(&class_carrier_fixture, class_carrier_facade_source));
    class_carrier_fixture.input.semantic_profile_fingerprint =
        xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct class_carrier_product = {0};
    XrProgramSourceDiagnostic class_carrier_diagnostic;
    assert_source_build_ok(&class_carrier_fixture.input, &class_carrier_product,
                           &class_carrier_diagnostic);
    ASSERT_EQ_UINT(
        program_operation_count(class_carrier_product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT),
        3u);
    ASSERT_EQ_UINT(
        program_operation_count(class_carrier_product.program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT),
        6u);
    uint32_t nominal_class_types = 0u;
    for (uint32_t type_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
         type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + class_carrier_product.program->type_count;
         ++type_id) {
        const XrValidatedType *type =
            xr_validated_program_type(class_carrier_product.program, (uint16_t) type_id);
        nominal_class_types += type && type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE &&
                               type->nominal_kind == XR_CORE_IR_NOMINAL_CLASS;
    }
    ASSERT_EQ_UINT(nominal_class_types, 2u);
    assert_detached_program_i64_result(class_carrier_product.program, profile, 42);
    xr_program_source_product_free(&class_carrier_product);
    source_build_fixture_free(&class_carrier_fixture);
    xr_target_profile_free(profile);

    static const char negative_library_source[] =
        "export interface ReadCounter { current() -> i64 }\n"
        "struct NotCounter implements ReadCounter {\n"
        "  value: i64\n"
        "  current() -> i64 { return 999 }\n"
        "}\n"
        "export fn genericRead<T: ReadCounter>(counter: T) -> i64 {\n"
        "  return counter.current()\n"
        "}\n";
    static const char negative_entry_source[] =
        "import { readCounter } from \"./facade\"\n"
        "struct NotCounter { value: i64 }\n"
        "fn genericRead<T>(_value: T) -> i64 { return 999 }\n"
        "fn answer() -> i64 { return readCounter(NotCounter{value: 42}) }\n";
    SourceBuildFixture negative_fixture;
    ASSERT_TRUE(source_build_fixture_init(&negative_fixture, negative_entry_source,
                                          negative_library_source));
    static const char negative_facade_source[] =
        "export { genericRead as readCounter } from \"./library\"\n";
    ASSERT_TRUE(source_build_fixture_add_facade(&negative_fixture, negative_facade_source));
    XrProgramSourceProduct rejected = {0};
    XrProgramSourceDiagnostic rejection;
    ASSERT_EQ_INT(xr_program_source_build(&negative_fixture.input, &rejected, &rejection),
                  XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
    ASSERT_EQ_INT(rejection.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
    ASSERT_EQ_UINT(rejection.module_index, 2u);
    ASSERT_EQ_UINT(rejection.underlying_status, XR_ERR_ANALYZE_GENERIC_CONSTRAINT);
    ASSERT_EQ_UINT(rejection.source_line, 4u);
    ASSERT_NOT_NULL(strstr(rejection.source_path, "main.xr"));
    ASSERT_NOT_NULL(strstr(rejection.message, "does not satisfy constraint"));
    ASSERT_NULL(rejected.artifact.bytes);
    ASSERT_NULL(rejected.program);
    source_build_fixture_free(&negative_fixture);

    static const char negative_method_library_source[] =
        "export interface ReadCounter { current() -> i64 }\n"
        "export class GenericReader {\n"
        "  read<T: ReadCounter>(counter: T) -> i64 { return counter.current() }\n"
        "  token<T>() -> i64 { return 999 }\n"
        "}\n";
    static const char negative_method_facade_source[] =
        "export { ReadCounter, GenericReader } from \"./library\"\n";
    static const char negative_method_entry_source[] =
        "import { GenericReader } from \"./facade\"\n"
        "struct NotCounter { value: i64 }\n"
        "fn answer() -> i64 {\n"
        "  var reader = GenericReader()\n"
        "  return reader.read<NotCounter>(NotCounter{value: 42})\n"
        "}\n";
    SourceBuildFixture negative_method_fixture;
    ASSERT_TRUE(source_build_fixture_init(&negative_method_fixture, negative_method_entry_source,
                                          negative_method_library_source));
    ASSERT_TRUE(
        source_build_fixture_add_facade(&negative_method_fixture, negative_method_facade_source));
    XrProgramSourceProduct method_rejected = {0};
    XrProgramSourceDiagnostic method_rejection;
    ASSERT_EQ_INT(xr_program_source_build(&negative_method_fixture.input, &method_rejected,
                                          &method_rejection),
                  XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
    ASSERT_EQ_INT(method_rejection.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
    ASSERT_EQ_UINT(method_rejection.module_index, 2u);
    ASSERT_EQ_UINT(method_rejection.underlying_status, XR_ERR_ANALYZE_GENERIC_CONSTRAINT);
    ASSERT_EQ_UINT(method_rejection.source_line, 5u);
    ASSERT_NOT_NULL(strstr(method_rejection.message, "does not satisfy constraint"));
    ASSERT_NULL(method_rejected.artifact.bytes);
    ASSERT_NULL(method_rejected.program);
    source_build_fixture_free(&negative_method_fixture);

    static const char uninferred_method_entry_source[] =
        "import { GenericReader } from \"./facade\"\n"
        "fn answer() -> i64 {\n"
        "  var reader = GenericReader()\n"
        "  return reader.token()\n"
        "}\n";
    SourceBuildFixture uninferred_method_fixture;
    ASSERT_TRUE(source_build_fixture_init(&uninferred_method_fixture,
                                          uninferred_method_entry_source,
                                          negative_method_library_source));
    ASSERT_TRUE(
        source_build_fixture_add_facade(&uninferred_method_fixture, negative_method_facade_source));
    XrProgramSourceProduct uninferred_rejected = {0};
    XrProgramSourceDiagnostic uninferred_rejection;
    ASSERT_EQ_INT(xr_program_source_build(&uninferred_method_fixture.input, &uninferred_rejected,
                                          &uninferred_rejection),
                  XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
    ASSERT_EQ_INT(uninferred_rejection.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
    ASSERT_EQ_UINT(uninferred_rejection.module_index, 2u);
    ASSERT_EQ_UINT(uninferred_rejection.underlying_status, XR_ERR_ANALYZE_GENERIC_COUNT);
    ASSERT_EQ_UINT(uninferred_rejection.source_line, 4u);
    ASSERT_NOT_NULL(strstr(uninferred_rejection.message, "Cannot infer type argument 'T'"));
    ASSERT_NOT_NULL(strstr(uninferred_rejection.message, "token"));
    ASSERT_NULL(uninferred_rejected.artifact.bytes);
    ASSERT_NULL(uninferred_rejected.program);
    source_build_fixture_free(&uninferred_method_fixture);

    static const char private_name_entry_source[] = "import { genericRead } from \"./facade\"\n"
                                                    "fn answer() -> i64 { return 0 }\n";
    SourceBuildFixture private_name_fixture;
    ASSERT_TRUE(source_build_fixture_init(&private_name_fixture, private_name_entry_source,
                                          negative_library_source));
    ASSERT_TRUE(source_build_fixture_add_facade(&private_name_fixture, negative_facade_source));
    XrProgramSourceProduct private_name_rejected = {0};
    XrProgramSourceDiagnostic private_name_rejection;
    ASSERT_EQ_INT(xr_program_source_build(&private_name_fixture.input, &private_name_rejected,
                                          &private_name_rejection),
                  XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
    ASSERT_EQ_INT(private_name_rejection.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
    ASSERT_EQ_UINT(private_name_rejection.module_index, 2u);
    ASSERT_NOT_NULL(strstr(private_name_rejection.message, "has no member 'genericRead'"));
    ASSERT_NULL(private_name_rejected.artifact.bytes);
    ASSERT_NULL(private_name_rejected.program);
    source_build_fixture_free(&private_name_fixture);
}

TEST(source_owner_value_struct_string_copy_preserves_original) {
    static const char source[] =
        "struct Report { label: string; footer: string; code: i64 }\n"
        "struct Envelope { prefix: string; report: Report; suffix: string }\n"
        "fn answer() -> i64 {\n"
        "  var first = Report{label: \"ready\", footer: \"end\", code: 40}\n"
        "  var second = first\n"
        "  second.label = \"go\"\n"
        "  second.code = 42\n"
        "  var wrapped = Envelope{prefix: \"start\", report: first, suffix: \"stop\"}\n"
        "  var rewritten = wrapped\n"
        "  rewritten.suffix = \"done\"\n"
        "  if (first.label == \"ready\" && second.label == \"go\" &&\n"
        "      first.footer == \"end\" && second.footer == \"end\" && first.code == 40 &&\n"
        "      wrapped.prefix == \"start\" && rewritten.prefix == \"start\" &&\n"
        "      wrapped.report.label == \"ready\" && rewritten.report.label == \"ready\" &&\n"
        "      wrapped.report.footer == \"end\" && rewritten.report.footer == \"end\" &&\n"
        "      wrapped.report.code == 40 && rewritten.report.code == 40 &&\n"
        "      wrapped.suffix == \"stop\" && rewritten.suffix == \"done\") {\n"
        "    return second.code\n"
        "  }\n"
        "  return 0\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_GT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_AGGREGATE_UPDATE), 0u);
    assert_detached_program_i64_result(product.program, profile, 42);
    assert_aot_fixture_backend_contract(product.program, profile,
                                        XR_SOURCE_FIXTURE_VALUE_STRUCT_STRING);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_generic_value_struct_specializations_are_exact_nominal_aggregates) {
    static const char library_source[] =
        "export struct Box<R> {\n"
        "  value: R\n"
        "  readAs<U>(_marker: U) -> R { return this.value }\n"
        "  forwardAs<U>(marker: U) -> R { return this.readAs<U>(marker) }\n"
        "}\n";
    static const char facade_source[] = "export { Box as OriginBox } from \"./library\"\n"
                                        "export struct Box<R> {\n"
                                        "  value: R\n"
                                        "  readAs<U>(_marker: U) -> R { return this.value }\n"
                                        "}\n";
    static const char entry_source[] =
        "import \"./library\" as direct\n"
        "import \"./facade\" as facade\n"
        "struct Box<R> {\n"
        "  value: R\n"
        "  readAs<U>(_marker: U) -> R { return this.value }\n"
        "}\n"
        "fn directValue<T>(value: T) -> T {\n"
        "  var box = direct.Box<T>{value: value}\n"
        "  return box.value\n"
        "}\n"
        "fn facadeValue<T>(value: T) -> T {\n"
        "  var box = facade.OriginBox<T>{value: value}\n"
        "  return box.forwardAs<bool>(false)\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var left = direct.Box<i64>{value: 38}\n"
        "  var same = facade.OriginBox<i64>{value: 1}\n"
        "  var other = facade.Box<i64>{value: 1}\n"
        "  var nestedOrigin = facadeValue<bool>(true)\n"
        "  var nestedDirect = directValue<bool>(true)\n"
        "  left.value = left.value + same.value + other.value\n"
        "  if (nestedOrigin && nestedDirect) {\n"
        "    return left.readAs(false) + same.readAs(false) + other.readAs(false)\n"
        "  }\n"
        "  return 0\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    ASSERT_TRUE(source_build_fixture_add_facade(&fixture, facade_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    ASSERT_NOT_NULL(first.program);
    ASSERT_NOT_NULL(second.program);
    assert_products_equal(&first, &second);

    const XrValidatedProgram *program = first.program;
    ASSERT_EQ_UINT(program->function_count, 10u);
    ASSERT_EQ_UINT(program->module_count, 3u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT), 7u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_OWNER_ALIAS), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_FIELD_LOAD), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_FIELD_PLACE), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_PLACE_EXCHANGE), 0u);
    uint32_t entry = xr_validated_program_entry_function(program);
    ASSERT_LT(entry, program->function_count);
    const XrValidatedFunction *function = &program->functions[entry];
    ASSERT_EQ_UINT(function->parameter_count, 0u);
    ASSERT_EQ_UINT(function->result_type_id, XR_CORE_TYPE_I64);

    uint16_t aggregate_types[3] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    uint32_t aggregate_constructs = 0u;
    uint32_t projects = 0u;
    uint32_t updates = 0u;
    uint32_t method_calls = 0u;
    uint32_t method_targets[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
        const XrValidatedBlock *block = &function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id == XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT) {
                const XrValidatedType *type =
                    xr_validated_program_type(program, instruction->result_type_id);
                ASSERT_NOT_NULL(type);
                ASSERT_EQ_INT(type->kind, XR_CORE_IR_TYPE_AGGREGATE);
                if (type->nominal_kind != XR_CORE_IR_NOMINAL_STRUCT)
                    continue;
                ASSERT_EQ_INT(type->nominal_kind, XR_CORE_IR_NOMINAL_STRUCT);
                ASSERT_EQ_INT(type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL);
                ASSERT_EQ_INT(type->copy_contract, XR_CORE_IR_COPY_TRIVIAL);
                ASSERT_EQ_UINT(type->field_count, 1u);
                ASSERT_NOT_NULL(type->field_types);
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_EQ_INT(instruction->result_ownership, XR_CORE_IR_NON_OWNER);
                ASSERT_LT(aggregate_constructs, 3u);
                aggregate_types[aggregate_constructs++] = instruction->result_type_id;
            } else if (instruction->operation_id == XR_CORE_OP_CORE_AGGREGATE_PROJECT) {
                ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FIELD);
                ASSERT_EQ_UINT(instruction->immediate.field_ordinal, 0u);
                ++projects;
            } else if (instruction->operation_id == XR_CORE_OP_CORE_AGGREGATE_UPDATE) {
                ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FIELD);
                ASSERT_EQ_UINT(instruction->immediate.field_ordinal, 0u);
                ++updates;
            } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT) {
                ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FUNCTION);
                ASSERT_LT(instruction->immediate.function_id, program->function_count);
                const XrValidatedFunction *callee =
                    &program->functions[instruction->immediate.function_id];
                if (!callee->has_receiver || callee->parameter_count == 0u)
                    continue;
                const XrValidatedType *receiver_type =
                    xr_validated_program_type(program, callee->parameter_types[0]);
                if (!receiver_type || receiver_type->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                    receiver_type->nominal_kind != XR_CORE_IR_NOMINAL_STRUCT)
                    continue;
                ASSERT_LT(method_calls, 3u);
                ASSERT_TRUE(callee->has_receiver);
                ASSERT_EQ_INT(callee->receiver_mode, XR_PARAM_READ);
                ASSERT_EQ_UINT(callee->parameter_count, 2u);
                ASSERT_EQ_UINT(instruction->operand_count, 2u);
                ASSERT_EQ_INT(callee->parameter_modes[0], XR_PARAM_READ);
                ASSERT_EQ_INT(callee->parameter_modes[1], XR_PARAM_READ);
                ASSERT_EQ_UINT(function->value_types[instruction->operands[0]],
                               callee->parameter_types[0]);
                ASSERT_EQ_UINT(function->value_types[instruction->operands[1]],
                               callee->parameter_types[1]);
                method_targets[method_calls++] = instruction->immediate.function_id;
            }
        }
    }
    ASSERT_EQ_UINT(aggregate_constructs, 3u);
    ASSERT_EQ_UINT(aggregate_types[0], aggregate_types[1]);
    ASSERT_TRUE(aggregate_types[0] != aggregate_types[2]);
    ASSERT_EQ_UINT(projects, 3u);
    ASSERT_EQ_UINT(updates, 1u);
    ASSERT_EQ_UINT(method_calls, 3u);
    const XrValidatedType *origin_i64_type = xr_validated_program_type(program, aggregate_types[0]);
    const XrValidatedType *decoy_i64_type = xr_validated_program_type(program, aggregate_types[2]);
    ASSERT_NOT_NULL(origin_i64_type);
    ASSERT_NOT_NULL(decoy_i64_type);
    ASSERT_EQ_UINT(origin_i64_type->field_types[0], XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(decoy_i64_type->field_types[0], XR_CORE_TYPE_I64);
    ASSERT_TRUE(!xr_core_ir_key_equal(origin_i64_type->key, decoy_i64_type->key));

    uint32_t origin_i64_calls = 0u;
    uint32_t decoy_i64_calls = 0u;
    uint32_t origin_i64_target = UINT32_MAX;
    uint32_t decoy_i64_target = UINT32_MAX;
    for (uint32_t call_index = 0u; call_index < method_calls; ++call_index) {
        const XrValidatedFunction *method = &program->functions[method_targets[call_index]];
        if (method->parameter_types[0] == aggregate_types[0]) {
            if (origin_i64_target == UINT32_MAX)
                origin_i64_target = method_targets[call_index];
            ASSERT_EQ_UINT(method_targets[call_index], origin_i64_target);
            ++origin_i64_calls;
        } else if (method->parameter_types[0] == aggregate_types[2]) {
            decoy_i64_target = method_targets[call_index];
            ++decoy_i64_calls;
        } else
            ASSERT_TRUE(false);
    }
    ASSERT_EQ_UINT(origin_i64_calls, 2u);
    ASSERT_EQ_UINT(decoy_i64_calls, 1u);
    ASSERT_LT(origin_i64_target, program->function_count);
    ASSERT_LT(decoy_i64_target, program->function_count);
    ASSERT_TRUE(origin_i64_target != decoy_i64_target);

    uint16_t nested_origin_types[2] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    uint32_t nested_origin_constructs = 0u;
    for (uint32_t function_index = 0u; function_index < program->function_count; ++function_index) {
        if (function_index == entry)
            continue;
        const XrValidatedFunction *candidate = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < candidate->block_count; ++block_index) {
            const XrValidatedBlock *block = &candidate->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT)
                    continue;
                const XrValidatedType *constructed_type =
                    xr_validated_program_type(program, instruction->result_type_id);
                ASSERT_NOT_NULL(constructed_type);
                if (constructed_type->nominal_kind != XR_CORE_IR_NOMINAL_STRUCT)
                    continue;
                ASSERT_LT(nested_origin_constructs, 2u);
                ASSERT_EQ_UINT(constructed_type->field_types[0], XR_CORE_TYPE_BOOL);
                nested_origin_types[nested_origin_constructs] = instruction->result_type_id;
                ++nested_origin_constructs;
            }
        }
    }
    ASSERT_EQ_UINT(nested_origin_constructs, 2u);
    ASSERT_EQ_UINT(nested_origin_types[0], nested_origin_types[1]);
    ASSERT_TRUE(nested_origin_types[0] != aggregate_types[0]);
    ASSERT_TRUE(nested_origin_types[0] != aggregate_types[2]);
    ASSERT_TRUE(nested_origin_types[1] != aggregate_types[0]);
    ASSERT_TRUE(nested_origin_types[1] != aggregate_types[2]);

    const XrValidatedType *origin_bool_type =
        xr_validated_program_type(program, nested_origin_types[0]);
    ASSERT_NOT_NULL(origin_bool_type);
    ASSERT_EQ_UINT(origin_bool_type->field_types[0], XR_CORE_TYPE_BOOL);
    ASSERT_TRUE(!xr_core_ir_key_equal(origin_i64_type->key, origin_bool_type->key));

    uint32_t nested_target = UINT32_MAX;
    uint32_t bool_forward_target = UINT32_MAX;
    uint32_t bool_read_target = UINT32_MAX;
    for (uint32_t function_index = 0u; function_index < program->function_count; ++function_index) {
        const XrValidatedFunction *method = &program->functions[function_index];
        if (!method->has_receiver || method->parameter_count != 2u ||
            method->parameter_types[0] != nested_origin_types[0])
            continue;
        uint32_t method_projects = 0u;
        uint32_t nested_calls = 0u;
        for (uint32_t block_index = 0u; block_index < method->block_count; ++block_index) {
            const XrValidatedBlock *block = &method->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                method_projects += block->instructions[instruction_index].operation_id ==
                                   XR_CORE_OP_CORE_AGGREGATE_PROJECT;
                if (block->instructions[instruction_index].operation_id ==
                    XR_CORE_OP_CORE_CALL_SEALED_DIRECT) {
                    ASSERT_EQ_INT(block->instructions[instruction_index].immediate_kind,
                                  XR_CORE_IR_IMMEDIATE_FUNCTION);
                    nested_target = block->instructions[instruction_index].immediate.function_id;
                    ++nested_calls;
                }
            }
        }
        ASSERT_EQ_UINT(method->parameter_types[1], XR_CORE_TYPE_BOOL);
        ASSERT_EQ_UINT(method->result_type_id, XR_CORE_TYPE_BOOL);
        if (nested_calls == 1u) {
            ASSERT_EQ_UINT(method_projects, 0u);
            bool_forward_target = function_index;
        } else {
            ASSERT_EQ_UINT(nested_calls, 0u);
            ASSERT_EQ_UINT(method_projects, 1u);
            bool_read_target = function_index;
        }
    }
    ASSERT_LT(bool_forward_target, program->function_count);
    ASSERT_LT(bool_read_target, program->function_count);
    ASSERT_TRUE(bool_forward_target != bool_read_target);
    ASSERT_EQ_UINT(nested_target, bool_read_target);

    uint32_t unique_targets[2] = {origin_i64_target, decoy_i64_target};
    for (uint32_t method_index = 0u; method_index < 2u; ++method_index) {
        const XrValidatedFunction *method = &program->functions[unique_targets[method_index]];
        uint32_t method_projects = 0u;
        uint32_t nested_calls = 0u;
        ASSERT_EQ_UINT(method->parameter_count, 2u);
        for (uint32_t block_index = 0u; block_index < method->block_count; ++block_index) {
            const XrValidatedBlock *block = &method->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                method_projects += block->instructions[instruction_index].operation_id ==
                                   XR_CORE_OP_CORE_AGGREGATE_PROJECT;
                nested_calls += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_CALL_SEALED_DIRECT;
            }
        }
        ASSERT_TRUE(method->parameter_types[0] == aggregate_types[0] ||
                    method->parameter_types[0] == aggregate_types[2]);
        ASSERT_EQ_UINT(method->parameter_types[1], XR_CORE_TYPE_BOOL);
        ASSERT_EQ_UINT(method->result_type_id, XR_CORE_TYPE_I64);
        ASSERT_EQ_UINT(method_projects, 1u);
        ASSERT_EQ_UINT(nested_calls, 0u);
    }
    ASSERT_LT(nested_target, program->function_count);
    ASSERT_TRUE(nested_target != origin_i64_target);
    ASSERT_TRUE(nested_target != decoy_i64_target);
    const XrValidatedFunction *nested_method = &program->functions[nested_target];
    ASSERT_TRUE(nested_method->has_receiver);
    ASSERT_EQ_UINT(nested_method->parameter_count, 2u);
    ASSERT_EQ_UINT(nested_method->parameter_types[0], nested_origin_types[0]);
    ASSERT_EQ_UINT(nested_method->parameter_types[1], XR_CORE_TYPE_BOOL);
    ASSERT_EQ_UINT(nested_method->result_type_id, XR_CORE_TYPE_BOOL);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    {
        XrVmCodeOptions vm_options = xr_vm_code_default_options();
        XrVmCode *vm_code = NULL;
        XrVmCodeDiagnostic vm_diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(binding.program, binding.profile, &vm_options, &vm_code,
                                       &vm_diagnostic),
                      XR_VM_CODE_OK);
        ASSERT_NOT_NULL(vm_code);
        XrVmOutcome vm_result = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(vm_result.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_result.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_result.value.as.i64, 42);
        xr_vm_code_free(vm_code);
    }

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_GENERIC_VALUE_STRUCT);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }

    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_generic_scalar_class_specializations_are_exact_class_references) {
    static const char library_source[] = "export class Box<T> {\n"
                                         "  value: T\n"
                                         "  constructor(value: T) { this.value = value }\n"
                                         "  get() -> T { return this.value }\n"
                                         "}\n";
    static const char facade_source[] = "export { Box as OriginBox } from \"./library\"\n"
                                        "export class Box<T> {\n"
                                        "  value: T\n"
                                        "  constructor(value: T) { this.value = value }\n"
                                        "  get() -> T { return this.value }\n"
                                        "}\n";
    static const char entry_source[] =
        "import \"./library\" as direct\n"
        "import \"./facade\" as facade\n"
        "class Box<T> {\n"
        "  value: T\n"
        "  constructor(value: T) { this.value = value }\n"
        "  get() -> T { return this.value }\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var a = direct.Box<i64>(20)\n"
        "  var b = direct.Box(1)\n"
        "  var c = facade.OriginBox<i64>(1)\n"
        "  var d = facade.OriginBox(1)\n"
        "  var e = facade.Box<i64>(10)\n"
        "  var f = Box<i64>(9)\n"
        "  var flag = facade.OriginBox<bool>(true)\n"
        "  if (flag.get()) {\n"
        "    return a.get() + b.get() + c.get() + d.get() + e.get() + f.get()\n"
        "  }\n"
        "  return 0\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    ASSERT_TRUE(source_build_fixture_add_facade(&fixture, facade_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    ASSERT_NOT_NULL(first.program);
    ASSERT_NOT_NULL(second.program);
    assert_products_equal(&first, &second);

    const XrValidatedProgram *program = first.program;
    uint16_t i64_class_types[3] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    uint16_t bool_class_type = XR_CORE_TYPE_VOID;
    uint32_t i64_class_count = 0u;
    uint32_t bool_class_count = 0u;
    for (uint32_t type_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
         type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + program->type_count; ++type_id) {
        const XrValidatedType *type = xr_validated_program_type(program, (uint16_t) type_id);
        if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE ||
            type->nominal_kind != XR_CORE_IR_NOMINAL_CLASS)
            continue;
        ASSERT_EQ_INT(type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
        ASSERT_EQ_INT(type->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
        ASSERT_EQ_UINT(type->field_count, 1u);
        ASSERT_NOT_NULL(type->field_types);
        if (type->field_types[0] == XR_CORE_TYPE_I64) {
            ASSERT_LT(i64_class_count, 3u);
            i64_class_types[i64_class_count++] = (uint16_t) type_id;
        } else if (type->field_types[0] == XR_CORE_TYPE_BOOL) {
            ASSERT_EQ_UINT(bool_class_count, 0u);
            bool_class_type = (uint16_t) type_id;
            ++bool_class_count;
        } else {
            ASSERT_TRUE(false);
        }
    }
    ASSERT_EQ_UINT(i64_class_count, 3u);
    ASSERT_EQ_UINT(bool_class_count, 1u);
    ASSERT_TRUE(bool_class_type != XR_CORE_TYPE_VOID);
    for (uint32_t left = 0u; left < i64_class_count; ++left) {
        const XrValidatedType *left_type =
            xr_validated_program_type(program, i64_class_types[left]);
        ASSERT_NOT_NULL(left_type);
        ASSERT_TRUE(!xr_core_ir_key_equal(
            left_type->key, xr_validated_program_type(program, bool_class_type)->key));
        for (uint32_t right = left + 1u; right < i64_class_count; ++right) {
            const XrValidatedType *right_type =
                xr_validated_program_type(program, i64_class_types[right]);
            ASSERT_NOT_NULL(right_type);
            ASSERT_TRUE(!xr_core_ir_key_equal(left_type->key, right_type->key));
        }
    }

    uint32_t entry = xr_validated_program_entry_function(program);
    ASSERT_LT(entry, program->function_count);
    const XrValidatedFunction *entry_function = &program->functions[entry];
    uint32_t construct_counts[3] = {0u, 0u, 0u};
    uint32_t bool_constructs = 0u;
    uint32_t class_calls = 0u;
    uint32_t class_drops = 0u;
    uint32_t unique_method_targets[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t unique_method_target_count = 0u;
    for (uint32_t block_index = 0u; block_index < entry_function->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry_function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id == XR_CORE_OP_CORE_CLASS_CONSTRUCT) {
                const XrValidatedType *type =
                    xr_validated_program_type(program, instruction->result_type_id);
                if (!type || type->nominal_kind != XR_CORE_IR_NOMINAL_CLASS)
                    continue;
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_EQ_INT(instruction->result_ownership, XR_CORE_IR_OWNER);
                ASSERT_EQ_INT(entry_function->value_ownerships[instruction->operands[0]],
                              XR_CORE_IR_NON_OWNER);
                if (instruction->result_type_id == bool_class_type) {
                    ASSERT_EQ_UINT(entry_function->value_types[instruction->operands[0]],
                                   XR_CORE_TYPE_BOOL);
                    ++bool_constructs;
                } else {
                    bool matched = false;
                    for (uint32_t index = 0u; index < i64_class_count; ++index) {
                        if (instruction->result_type_id != i64_class_types[index])
                            continue;
                        ASSERT_EQ_UINT(entry_function->value_types[instruction->operands[0]],
                                       XR_CORE_TYPE_I64);
                        ++construct_counts[index];
                        matched = true;
                    }
                    ASSERT_TRUE(matched);
                }
            } else if (instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT) {
                ASSERT_EQ_INT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_FUNCTION);
                ASSERT_LT(instruction->immediate.function_id, program->function_count);
                const XrValidatedFunction *callee =
                    &program->functions[instruction->immediate.function_id];
                if (!callee->has_receiver)
                    continue;
                ASSERT_EQ_UINT(callee->parameter_count, 1u);
                ASSERT_EQ_UINT(instruction->operand_count, 1u);
                ASSERT_EQ_INT(callee->receiver_mode, XR_PARAM_READ);
                ASSERT_EQ_UINT(entry_function->value_types[instruction->operands[0]],
                               callee->parameter_types[0]);
                bool known = false;
                for (uint32_t target = 0u; target < unique_method_target_count; ++target)
                    known |= unique_method_targets[target] == instruction->immediate.function_id;
                if (!known) {
                    ASSERT_LT(unique_method_target_count, 4u);
                    unique_method_targets[unique_method_target_count++] =
                        instruction->immediate.function_id;
                }
                ++class_calls;
            } else if (instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                       instruction->operand_count == 1u) {
                const XrValidatedType *operand_type = xr_validated_program_type(
                    program, entry_function->value_types[instruction->operands[0]]);
                if (operand_type && operand_type->nominal_kind == XR_CORE_IR_NOMINAL_CLASS)
                    ++class_drops;
            }
        }
    }
    uint32_t origin_type_index = UINT32_MAX;
    uint32_t singleton_i64_types = 0u;
    for (uint32_t index = 0u; index < i64_class_count; ++index) {
        if (construct_counts[index] == 4u)
            origin_type_index = index;
        else if (construct_counts[index] == 1u)
            ++singleton_i64_types;
    }
    ASSERT_LT(origin_type_index, i64_class_count);
    ASSERT_EQ_UINT(singleton_i64_types, 2u);
    ASSERT_EQ_UINT(bool_constructs, 1u);
    ASSERT_EQ_UINT(class_calls, 7u);
    ASSERT_EQ_UINT(unique_method_target_count, 4u);
    /* The flag is dropped before the branch. Each of the six i64 owners has one terminal drop on
     * the true edge and one on the false edge. */
    ASSERT_EQ_UINT(class_drops, 13u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_OWNER_COPY), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_OWNER_MOVE), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_AGGREGATE_UPDATE), 0u);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    assert_vm_fixture_backend_contract(instance, program, profile,
                                       XR_SOURCE_FIXTURE_GENERIC_SCALAR_CLASS);
    assert_aot_fixture_backend_contract(program, profile, XR_SOURCE_FIXTURE_GENERIC_SCALAR_CLASS);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

/* A constructor body that stores each field from any parameter or a scalar
 * literal folds into one class.construct: the operands follow the declared
 * field order, not the parameter order, and a literal is materialized as its
 * own constant instruction ahead of the construction. Before this, the fold
 * admitted only a body that stored field i from parameter i, so the ordinary
 * `this.value = 0` constructor was refused. */
TEST(source_owner_folds_constructor_literal_and_reordered_stores) {
    static const char source[] = "class Counter {\n"
                                 "  value: i64\n"
                                 "  constructor() { this.value = 0 }\n"
                                 "}\n"
                                 "class Pair {\n"
                                 "  a: i64\n"
                                 "  b: bool\n"
                                 "  constructor(x: i64) { this.b = true\n"
                                 "    this.a = x }\n"
                                 "}\n"
                                 "class Swap {\n"
                                 "  first: i64\n"
                                 "  second: i64\n"
                                 "  constructor(x: i64, y: i64) { this.second = x\n"
                                 "    this.first = y }\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var c = Counter()\n"
                                 "  var p = Pair(5)\n"
                                 "  var s = Swap(1, 10)\n"
                                 "  return c.value + p.a + s.first * 100 + s.second\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    const XrValidatedProgram *program = product.program;
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 3u);

    uint32_t counter_constructs = 0u;
    uint32_t pair_constructs = 0u;
    uint32_t swap_constructs = 0u;
    for (uint32_t function_index = 0u; function_index < program->function_count; ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        /* The operation that produced each value, so a construction operand
         * can be traced back to the constant instruction that materialized a
         * folded literal. */
        uint16_t *producers = xr_calloc(function->value_count, sizeof(*producers));
        ASSERT_NOT_NULL(producers);
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t index = 0u; index < block->instruction_count; ++index) {
                const XrValidatedInstruction *instruction = &block->instructions[index];
                if (instruction->result_id < function->value_count)
                    producers[instruction->result_id] = instruction->operation_id;
            }
        }
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t index = 0u; index < block->instruction_count; ++index) {
                const XrValidatedInstruction *instruction = &block->instructions[index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CLASS_CONSTRUCT)
                    continue;
                const XrValidatedType *type =
                    xr_validated_program_type(program, instruction->result_type_id);
                ASSERT_NOT_NULL(type);
                ASSERT_EQ_UINT(instruction->operand_count, type->field_count);
                for (uint32_t field = 0u; field < type->field_count; ++field)
                    ASSERT_EQ_UINT(function->value_types[instruction->operands[field]],
                                   type->field_types[field]);
                if (type->field_count == 1u) {
                    /* Counter: the single field is the literal 0. */
                    ASSERT_EQ_UINT(producers[instruction->operands[0]],
                                   XR_CORE_OP_CORE_CONSTANT_I64);
                    ++counter_constructs;
                } else if (type->field_types[1] == XR_CORE_TYPE_BOOL) {
                    /* Pair: field a from the argument (itself the literal 5 in the
                     * caller), field b from the constructor's own literal true, in
                     * declared field order although the body stores b first. */
                    ASSERT_EQ_UINT(producers[instruction->operands[1]],
                                   XR_CORE_OP_CORE_CONSTANT_BOOL);
                    ++pair_constructs;
                } else {
                    /* Swap: both fields from arguments, crossed. */
                    ASSERT_TRUE(instruction->operands[0] != instruction->operands[1]);
                    ++swap_constructs;
                }
            }
        }
        xr_free(producers);
    }
    ASSERT_EQ_UINT(counter_constructs, 1u);
    ASSERT_EQ_UINT(pair_constructs, 1u);
    ASSERT_EQ_UINT(swap_constructs, 1u);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_generic_nested_class_types_are_exact) {
    static const char direct_managed_struct_source[] =
        "class Leaf {\n"
        "  value: i64\n"
        "  constructor(value: i64) { this.value = value }\n"
        "}\n"
        "struct Bad { value: Leaf }\n"
        "fn answer() -> i64 {\n"
        "  var leaf = Leaf(1)\n"
        "  var bad = Bad{value: move leaf}\n"
        "  return 42\n"
        "}\n";
    static const char nested_managed_struct_source[] =
        "class Leaf {\n"
        "  value: i64\n"
        "  constructor(value: i64) { this.value = value }\n"
        "}\n"
        "struct Bad { value: Leaf? }\n"
        "fn answer() -> i64 {\n"
        "  var leaf = Leaf(1)\n"
        "  var bad = Bad{value: move leaf}\n"
        "  return 42\n"
        "}\n";
    const char *managed_struct_sources[] = {
        direct_managed_struct_source,
        nested_managed_struct_source,
    };
    for (uint32_t source = 0u; source < 2u; ++source) {
        SourceBuildFixture rejected_fixture;
        ASSERT_TRUE(
            source_build_fixture_init(&rejected_fixture, managed_struct_sources[source], NULL));
        XrProgramSourceProduct rejected = {0};
        XrProgramSourceDiagnostic rejection;
        ASSERT_EQ_INT(xr_program_source_build(&rejected_fixture.input, &rejected, &rejection),
                      XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
        ASSERT_EQ_INT(rejection.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
        ASSERT_NULL(rejected.artifact.bytes);
        ASSERT_NULL(rejected.program);
        source_build_fixture_free(&rejected_fixture);
    }

    static const char library_source[] = "export class Leaf {\n"
                                         "  value: i64\n"
                                         "  constructor(value: i64) { this.value = value }\n"
                                         "}\n"
                                         "export class Box<T> {\n"
                                         "  value: T\n"
                                         "  constructor(value: move T) { this.value = value }\n"
                                         "}\n";
    static const char facade_source[] = "export class Leaf {\n"
                                        "  value: i64\n"
                                        "  constructor(value: i64) { this.value = value }\n"
                                        "}\n"
                                        "export class Box<T> {\n"
                                        "  value: T\n"
                                        "  constructor(value: move T) { this.value = value }\n"
                                        "}\n";
    static const char entry_source[] =
        "import { Leaf as DirectLeaf, Box as DirectBox } from \"./library\"\n"
        "import { Leaf as FacadeLeaf, Box as FacadeBox } from \"./facade\"\n"
        "class Leaf {\n"
        "  value: i64\n"
        "  constructor(value: i64) { this.value = value }\n"
        "}\n"
        "class Box<T> {\n"
        "  value: T\n"
        "  constructor(value: move T) { this.value = value }\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var directLeaf = DirectLeaf(19)\n"
        "  var original = DirectBox<DirectLeaf>(move directLeaf)\n"
        "  var facadeLeaf = FacadeLeaf(6)\n"
        "  var facadeDecoy = FacadeBox<FacadeLeaf>(move facadeLeaf)\n"
        "  var localLeaf = Leaf(7)\n"
        "  var localDecoy = Box(move localLeaf)\n"
        "  return 42\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    ASSERT_TRUE(source_build_fixture_add_facade(&fixture, facade_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    ASSERT_NOT_NULL(first.program);
    ASSERT_NOT_NULL(second.program);
    assert_products_equal(&first, &second);

    const XrValidatedProgram *program = first.program;
    uint16_t leaf_types[3] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    uint16_t box_types[3] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    uint32_t leaf_count = 0u;
    uint32_t box_count = 0u;
    for (uint32_t type_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
         type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + program->type_count; ++type_id) {
        const XrValidatedType *type = xr_validated_program_type(program, (uint16_t) type_id);
        if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE || type->field_count != 1u)
            continue;
        ASSERT_EQ_INT(type->nominal_kind, XR_CORE_IR_NOMINAL_CLASS);
        ASSERT_EQ_INT(type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
        ASSERT_EQ_INT(type->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
        if (type->field_types[0] == XR_CORE_TYPE_I64) {
            ASSERT_LT(leaf_count, 3u);
            leaf_types[leaf_count++] = (uint16_t) type_id;
        } else {
            const XrValidatedType *field = xr_validated_program_type(program, type->field_types[0]);
            ASSERT_NOT_NULL(field);
            ASSERT_EQ_INT(field->kind, XR_CORE_IR_TYPE_CLASS_REFERENCE);
            ASSERT_LT(box_count, 3u);
            box_types[box_count++] = (uint16_t) type_id;
        }
    }
    ASSERT_EQ_UINT(leaf_count, 3u);
    ASSERT_EQ_UINT(box_count, 3u);
    for (uint32_t left = 0u; left < 3u; ++left) {
        const XrValidatedType *left_leaf = xr_validated_program_type(program, leaf_types[left]);
        const XrValidatedType *left_box = xr_validated_program_type(program, box_types[left]);
        ASSERT_NOT_NULL(left_leaf);
        ASSERT_NOT_NULL(left_box);
        for (uint32_t right = left + 1u; right < 3u; ++right) {
            ASSERT_TRUE(!xr_core_ir_key_equal(
                left_leaf->key, xr_validated_program_type(program, leaf_types[right])->key));
            ASSERT_TRUE(!xr_core_ir_key_equal(
                left_box->key, xr_validated_program_type(program, box_types[right])->key));
        }
    }

    uint32_t leaf_construct_counts[3] = {0u, 0u, 0u};
    uint32_t box_construct_counts[3] = {0u, 0u, 0u};
    for (uint32_t function_index = 0u; function_index < program->function_count; ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CLASS_CONSTRUCT)
                    continue;
                for (uint32_t type = 0u; type < 3u; ++type) {
                    leaf_construct_counts[type] += instruction->result_type_id == leaf_types[type];
                    box_construct_counts[type] += instruction->result_type_id == box_types[type];
                }
            }
        }
    }
    for (uint32_t type = 0u; type < 3u; ++type) {
        ASSERT_EQ_UINT(leaf_construct_counts[type], 1u);
        ASSERT_EQ_UINT(box_construct_counts[type], 1u);
    }
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_OWNER_COPY), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_OWNER_ALIAS), 0u);

    assert_detached_program_i64_result(first.program, profile, 42);

    assert_aot_fixture_backend_contract(first.program, profile,
                                        XR_SOURCE_FIXTURE_GENERIC_NESTED_CLASS);
    xr_target_profile_free(profile);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_exact_integer_constants_and_conversions) {
    static const char source[] = "fn widen(value: u8) -> i64 { return value as i64 }\n"
                                 "fn bits(value: u64) -> i64 { return value as i64 }\n"
                                 "var value: u8 = 255\n"
                                 "var high: u64 = (-1) as u64\n"
                                 "fn answer() -> i64 { return widen(value) + bits(high) + 1 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        ASSERT_TRUE(program_operation_count(product.program, XR_CORE_OP_CORE_INTEGER_CONVERT) >=
                    4u);
        assert_detached_program_i64_result(product.program, profile, 255);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_INTEGER_CONVERSIONS);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_integer_wrapping_arithmetic_uses_shared_primitives) {
    static const struct {
        const char *type;
        const char *minimum;
        const char *maximum;
        const char *add_result;
        const char *subtract_result;
        const char *multiply_result;
    } cases[] = {
        {"i8", "-128", "127", "-128", "127", "-2"},
        {"u8", "0", "255", "0", "255", "254"},
        {"i16", "-32768", "32767", "-32768", "32767", "-2"},
        {"u16", "0", "65535", "0", "65535", "65534"},
        {"i32", "-2147483648", "2147483647", "-2147483648", "2147483647", "-2"},
        {"u32", "0", "4294967295", "0", "4294967295", "4294967294"},
        {"i64", "-9223372036854775808", "9223372036854775807", "-9223372036854775808",
         "9223372036854775807", "-2"},
        {"u64", "0", "18446744073709551615", "0", "-1", "-2"},
    };
    char source[16384];
    size_t used = 0u;
    for (unsigned index = 0u; index < 8u; ++index) {
        const char *type = cases[index].type;
        int count =
            snprintf(source + used, sizeof(source) - used,
                     "fn add_%s(a: %s, b: %s) -> %s { return a + b }\n"
                     "fn sub_%s(a: %s, b: %s) -> %s { return a - b }\n"
                     "fn mul_%s(a: %s, b: %s) -> %s { return a * b }\n",
                     type, type, type, type, type, type, type, type, type, type, type, type);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char prefix[] = "fn mixed(a: u8, b: u16) -> u16 { return a + b }\n"
                          "fn answer() -> i64 {\n";
    ASSERT_TRUE(sizeof(prefix) <= sizeof(source) - used);
    memcpy(source + used, prefix, sizeof(prefix));
    used += sizeof(prefix) - 1u;
    for (unsigned index = 0u; index < 8u; ++index) {
        int count = snprintf(source + used, sizeof(source) - used,
                             "if ((add_%s(%s, 1) as i64) != %s) { return %u }\n"
                             "if ((sub_%s(%s, 1) as i64) != %s) { return %u }\n"
                             "if ((mul_%s(%s, 2) as i64) != %s) { return %u }\n",
                             cases[index].type, cases[index].maximum, cases[index].add_result,
                             index * 3u + 1u, cases[index].type, cases[index].minimum,
                             cases[index].subtract_result, index * 3u + 2u, cases[index].type,
                             cases[index].maximum, cases[index].multiply_result, index * 3u + 3u);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char suffix[] = "if ((mixed(255, 1) as i64) != 256) { return 25 }\nreturn 0\n}\n";
    ASSERT_TRUE(sizeof(suffix) <= sizeof(source) - used);
    memcpy(source + used, suffix, sizeof(suffix));
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 0);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_INTEGER_ARITHMETIC);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_integer_comparisons_preserve_signedness_and_order) {
    static const struct {
        const char *type, *low, *high;
    } bounds[] = {
        {"i8", "-128", "127"},
        {"u8", "0", "255"},
        {"i16", "-32768", "32767"},
        {"u16", "0", "65535"},
        {"i32", "-2147483648", "2147483647"},
        {"u32", "0", "4294967295"},
        {"i64", "-9223372036854775808", "9223372036854775807"},
        {"u64", "0", "18446744073709551615"},
    };
    static const struct {
        const char *symbol;
        bool expected[3];
    } predicates[] = {
        {"==", {false, false, true}}, {"!=", {true, true, false}}, {"<", {true, false, false}},
        {"<=", {true, false, true}},  {">", {false, true, false}}, {">=", {false, true, true}},
    };
    char source[32768];
    size_t used = 0u;
    for (unsigned type = 0u; type < 8u; ++type) {
        for (unsigned predicate = 0u; predicate < 6u; ++predicate) {
            int count = snprintf(source + used, sizeof(source) - used,
                                 "fn op%u_%s(a: %s, b: %s) -> bool { return a %s b }\n", predicate,
                                 bounds[type].type, bounds[type].type, bounds[type].type,
                                 predicates[predicate].symbol);
            ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
            used += (size_t) count;
        }
    }
    const char prefix[] = "fn mixed(a: u32, b: u64) -> bool { return a < b }\n"
                          "fn signedMixed(a: i8, b: i16) -> bool { return a < b }\n"
                          "fn answer() -> i64 {\n";
    ASSERT_TRUE(sizeof(prefix) <= sizeof(source) - used);
    memcpy(source + used, prefix, sizeof(prefix));
    used += sizeof(prefix) - 1u;
    for (unsigned type = 0u; type < 8u; ++type) {
        for (unsigned predicate = 0u; predicate < 6u; ++predicate) {
            for (unsigned order = 0u; order < 3u; ++order) {
                const char *left = order == 0u ? bounds[type].low : bounds[type].high;
                const char *right = order == 1u ? bounds[type].low : bounds[type].high;
                int count = snprintf(
                    source + used, sizeof(source) - used, "if (%sop%u_%s(%s, %s)) { return %u }\n",
                    predicates[predicate].expected[order] ? "!" : "", predicate, bounds[type].type,
                    left, right, 1u + type * 18u + predicate * 3u + order);
                ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
                used += (size_t) count;
            }
        }
    }
    const char suffix[] = "if (!mixed(4294967295, 18446744073709551615)) { return 145 }\n"
                          "if (!signedMixed(-128, 32767)) { return 146 }\n"
                          "if (!op2_u64(9223372036854775807, 9223372036854775808)) { return 147 }\n"
                          "if (op2_u64(9223372036854775808, 9223372036854775807)) { return 148 }\n"
                          "return 0\n}\n";
    ASSERT_TRUE(sizeof(suffix) <= sizeof(source) - used);
    memcpy(source + used, suffix, sizeof(suffix));
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 0);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_INTEGER_COMPARISONS);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_integer_division_and_remainder_preserve_value_rules) {
    static const struct {
        const char *type, *left, *right, *quotient, *remainder;
    } cases[] = {
        {"i8", "-128", "-1", "-128", "0"},
        {"i16", "-32768", "-1", "-32768", "0"},
        {"i32", "-2147483648", "-1", "-2147483648", "0"},
        {"i64", "-9223372036854775808", "-1", "-9223372036854775808", "0"},
        {"u8", "255", "3", "85", "0"},
        {"u16", "65535", "3", "21845", "0"},
        {"u32", "4294967295", "3", "1431655765", "0"},
        {"u64", "18446744073709551615", "10", "1844674407370955161", "5"},
    };
    char source[16384];
    size_t used = 0u;
    for (unsigned index = 0u; index < 8u; ++index) {
        const char *type = cases[index].type;
        int count = snprintf(source + used, sizeof(source) - used,
                             "fn div_%s(a: %s, b: %s) -> %s { return a / b }\n"
                             "fn rem_%s(a: %s, b: %s) -> %s { return a %% b }\n",
                             type, type, type, type, type, type, type, type);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char prefix[] = "fn answer() -> i64 {\n";
    memcpy(source + used, prefix, sizeof(prefix));
    used += sizeof(prefix) - 1u;
    for (unsigned index = 0u; index < 8u; ++index) {
        int count =
            snprintf(source + used, sizeof(source) - used,
                     "if ((div_%s(%s, %s) as i64) != %s) { return %u }\n"
                     "if ((rem_%s(%s, %s) as i64) != %s) { return %u }\n",
                     cases[index].type, cases[index].left, cases[index].right,
                     cases[index].quotient, index * 2u + 1u, cases[index].type, cases[index].left,
                     cases[index].right, cases[index].remainder, index * 2u + 2u);
        ASSERT_TRUE(count > 0 && (size_t) count < sizeof(source) - used);
        used += (size_t) count;
    }
    const char suffix[] = "if (div_i64(-7, 3) != -2) { return 17 }\n"
                          "if (rem_i64(-7, 3) != -1) { return 18 }\n"
                          "if (div_i64(7, -3) != -2) { return 19 }\n"
                          "if (rem_i64(7, -3) != 1) { return 20 }\n"
                          "return 0\n}\n";
    ASSERT_TRUE(sizeof(suffix) <= sizeof(source) - used);
    memcpy(source + used, suffix, sizeof(suffix));
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 0);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_INTEGER_DIVISION);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_array_index_reads_and_replaces_elements) {
    static const char source[] =
        "fn answer() -> i64 {\n"
        "  var values: Array<i64> = [10, 20, 30]\n"
        "  values[1] = 42\n"
        "  return values[1]\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_ARRAY_INDEX);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_narrow_array_elements_precede_allocation) {
    static const char source[] =
        "const bytes: Array<u8> = [0, 255, 42]\n"
        "var visits: i64 = 0\n"
        "fn next() -> u8 { visits = visits + 1; return visits as u8 }\n"
        "fn answer() -> i64 {\n"
        "  var ordered: Array<u8> = [next(), next(), next()]\n"
        "  const signed: Array<i8> = [-128, 127]\n"
        "  assert(ordered[0] == 1 && ordered[1] == 2 && ordered[2] == 3)\n"
        "  assert(visits == 3 && bytes[0] == 0 && bytes[1] == 255)\n"
        "  assert(signed[0] == -128 && signed[1] == 127)\n"
        "  return bytes[2] as i64\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_NARROW_ARRAY);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_integer_bitwise_exact_width) {
    static const char source[] =
        "fn count() -> i64 { return 65 }\n"
        "fn left() -> i8 { return -128 }\n"
        "fn answer() -> i64 {\n"
        "  var a: u8 = 255\n"
        "  var b: i16 = -256\n"
        "  var c: u32 = 4294967295\n"
        "  var wide: u64 = 1\n"
        "  var neg: i64 = -2\n"
        "  assert((left() >> 7) == -1)\n"
        "  assert((a >> 7) == 1 && (a << count()) == 254)\n"
        "  assert((b | (42 as i16)) == -214)\n"
        "  assert((c ^ (42 as u32)) == 4294967253)\n"
        "  assert((~(0 as i32)) == -1)\n"
        "  assert((wide << count()) == 2 && (wide << 64) == 1)\n"
        "  assert((neg >> -1) == -1 && (neg >> 64) == -2)\n"
        "  return (a & (42 as u8)) as i64\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_INTEGER_BITWISE);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_array_append_preserves_class_identity) {
    static const char source[] =
        "class Holder {\n"
        " value: i64\n"
        " constructor(value: i64) { this.value = value }\n"
        "}\n"
        "fn answer() -> i64 {\n"
        " var rows: Array<Holder> = []\n"
        " var original = Holder(19)\n"
        " rows.push(original)\n"
        " original.value = 42\n"
        " return rows[0].value\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_ARRAY_APPEND_CLASS_IDENTITY);
        xr_program_source_product_free(&product);
    }
    xr_target_profile_free(profile);
}

TEST(source_owner_static_method_declarations) {
    static const char source[] =
        "class First { static value() -> i64 { return 19 } }\n"
        "class Second { static value() -> i64 { return 23 } }\n"
        "fn answer() -> i64 { return First.value() + Second.value() }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_STATIC_METHOD_DECLARATIONS);
        xr_program_source_product_free(&product);
    }
    xr_target_profile_free(profile);
}

TEST(source_owner_tuple_array_elements) {
    static const char source[] =
        "fn answer() -> i64 {\n"
        " var rows: Array<(string, string)> = []\n"
        " var builder = StringBuilder()\n"
        " builder.append(\"abcdefghijklmnopqrstu\")\n"
        " rows.push((builder.toString(), builder.toString()))\n"
        " builder.clear()\n"
        " return len(rows[0].0) + len(rows[0].1)\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_TUPLE_ARRAY_ELEMENTS);
        xr_program_source_product_free(&product);
    }
    xr_target_profile_free(profile);
}

TEST(source_owner_repeated_tuple_elements) {
    static const char source[] =
        "fn duplicate(s: string) -> (string, string, string) { return (s, s, s) }\n"
        "fn answer() -> i64 {\n"
        " var builder = StringBuilder()\n"
        " builder.append(\"abcdefghijklmn\")\n"
        " var p = duplicate(builder.toString())\n"
        " builder.clear()\n"
        " return len(p.0) + len(p.1) + len(p.2)\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    if (product.program) {
        ASSERT_GT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_REPEATED_TUPLE_ELEMENTS);
        xr_program_source_product_free(&product);
    }
    xr_target_profile_free(profile);
}

TEST(source_owner_byte_comparison) {
    static const char source[] =
        "import crypto\n"
        "fn answer() -> i64 {\n"
        " var a: Array<u8> = [0, 128, 255]\n"
        " var b: Array<u8> = [0, 128, 255]\n"
        " var changed: Array<u8> = [0, 129, 255]\n"
        " var empty: Array<u8> = []\n"
        " var shorter: Array<u8> = [0, 128]\n"
        " assert(crypto.timingSafeEqualBytes(a, b))\n"
        " assert(crypto.timingSafeEqualBytes(a, a))\n"
        " assert(crypto.timingSafeEqualBytes(empty, empty))\n"
        " assert(!crypto.timingSafeEqualBytes(a, changed))\n"
        " assert(!crypto.timingSafeEqualBytes(a, shorter))\n"
        " assert(!crypto.timingSafeEqualBytes(empty, a))\n"
        " assert(!crypto.timingSafeEqualBytes(a, empty))\n"
        " assert(a[0] == 0 && a[1] == 128 && a[2] == 255)\n"
        " return 42\n"
        "}\n"
        ;
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0}; XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile, XR_SOURCE_FIXTURE_BYTE_COMPARISON);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture); xr_target_profile_free(profile);
}

TEST(source_owner_string_builder_snapshot) {
    static const char source[] =
        "fn answer() -> i64 {\n"
        "  var text = StringBuilder()\n"
        "  assert(text.toString() == \"\")\n"
        "  text.append(\"A\").append(42).append(true).append(1.5)\n"
        "  const saved = text.toString()\n"
        "  assert(saved == \"A42true1.5\")\n"
        "  text.clear().append(\"new\")\n"
        "  assert(text.toString() == \"new\")\n"
        "  assert(saved == \"A42true1.5\")\n"
        "  text.clear()\n"
        "  for (i in 0..3) { text.append(\"x\") }\n"
        "  assert(text.toString() == \"xxx\")\n"
        "  assert(make() == \"owned\")\n"
        "  assert(make() == \"owned\")\n"
        "  return 42\n"
        "}\n"
        "fn make() -> string {\n"
        "  var text = StringBuilder()\n"
        "  text.append(\"owned\")\n"
        "  return text.toString()\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_STRING_BUILDER);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_array_error_invoke) {
    static const char source[] =
        "fn make(n: i64) -> i64 {\n"
        "  if (n < -1) { throw CryptoError.InvalidLength }\n"
        "  var bytes = Array<u8>(n)\n"
        "  return len(bytes)\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var result = make(32)\n"
        "  try { result = make(-2) } catch (e: CryptoError) { result = result + 10 }\n"
        "  return result\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_ARRAY_ERROR_INVOKE);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_string_slice_scalar_range) {
    static const char source[] =
        "fn answer() -> i64 {\n"
        "  const s = \"Aé中🙂\"\n"
        "  assert(s.slice(1, 4) == \"é中🙂\")\n"
        "  assert(s.slice(2) == \"中🙂\")\n"
        "  assert(s.slice(4, 4) == \"\")\n"
        "  return 42\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_STRING_SLICE);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_array_runtime_length) {
    static const char source[] =
        "fn length() -> i64 { return 3 }\n"
        "fn answer() -> i64 {\n"
        "  var bytes = Array<u8>(length())\n"
        "  assert(len(bytes) == 3 && bytes[0] == 0 && bytes[2] == 0)\n"
        "  bytes[1] = 42\n"
        "  var flags = Array<bool>(length())\n"
        "  assert(!flags[1])\n"
        "  var floats = Array<f64>(length())\n"
        "  assert(floats[1] == 0.0)\n"
        "  return bytes[1] as i64\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (product.program) {
        assert_detached_program_i64_result(product.program, profile, 42);
        assert_aot_fixture_backend_contract(product.program, profile,
                                            XR_SOURCE_FIXTURE_ARRAY_RUNTIME_LENGTH);
        xr_program_source_product_free(&product);
    }
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_recursive_class_type_reservation_is_cycle_safe) {
    static const char source[] = "class Node {\n"
                                 "  next: Node?\n"
                                 "  constructor(next: Node?) { this.next = next }\n"
                                 "}\n"
                                 "fn answer(node: Node) -> i64 {\n"
                                 "  return 42\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);

    uint16_t class_type_id = XR_CORE_TYPE_VOID;
    uint32_t class_count = 0u;
    for (uint32_t type_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
         type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + product.program->type_count; ++type_id) {
        const XrValidatedType *type =
            xr_validated_program_type(product.program, (uint16_t) type_id);
        if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE)
            continue;
        class_type_id = (uint16_t) type_id;
        ++class_count;
    }
    ASSERT_EQ_UINT(class_count, 1u);
    const XrValidatedType *class_type = xr_validated_program_type(product.program, class_type_id);
    ASSERT_NOT_NULL(class_type);
    ASSERT_EQ_INT(class_type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
    ASSERT_EQ_INT(class_type->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
    ASSERT_EQ_UINT(class_type->field_count, 1u);
    const XrValidatedType *optional =
        xr_validated_program_type(product.program, class_type->field_types[0]);
    ASSERT_NOT_NULL(optional);
    ASSERT_EQ_INT(optional->kind, XR_CORE_IR_TYPE_VARIANT);
    ASSERT_EQ_INT(optional->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
    ASSERT_EQ_INT(optional->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
    ASSERT_EQ_UINT(optional->variant_count, 2u);
    ASSERT_EQ_UINT(optional->variants[1].payload_count, 1u);
    ASSERT_EQ_UINT(optional->variants[1].payload_types[0], class_type_id);

    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_mutual_class_type_reservation_is_cycle_safe) {
    static const char source[] = "class Left {\n"
                                 "  right: Right?\n"
                                 "  constructor(right: Right?) { this.right = right }\n"
                                 "}\n"
                                 "class Right {\n"
                                 "  left: Left?\n"
                                 "  constructor(left: Left?) { this.left = left }\n"
                                 "}\n"
                                 "fn answer(left: Left) -> i64 {\n"
                                 "  return 42\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);

    uint16_t class_type_ids[2] = {XR_CORE_TYPE_VOID, XR_CORE_TYPE_VOID};
    uint32_t class_count = 0u;
    for (uint32_t type_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
         type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + product.program->type_count; ++type_id) {
        const XrValidatedType *type =
            xr_validated_program_type(product.program, (uint16_t) type_id);
        if (!type || type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE)
            continue;
        ASSERT_LT(class_count, 2u);
        class_type_ids[class_count++] = (uint16_t) type_id;
    }
    ASSERT_EQ_UINT(class_count, 2u);
    for (uint32_t index = 0u; index < class_count; ++index) {
        const XrValidatedType *class_type =
            xr_validated_program_type(product.program, class_type_ids[index]);
        ASSERT_NOT_NULL(class_type);
        ASSERT_EQ_INT(class_type->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
        ASSERT_EQ_UINT(class_type->field_count, 1u);
        const XrValidatedType *optional =
            xr_validated_program_type(product.program, class_type->field_types[0]);
        ASSERT_NOT_NULL(optional);
        ASSERT_EQ_INT(optional->kind, XR_CORE_IR_TYPE_VARIANT);
        ASSERT_EQ_UINT(optional->variant_count, 2u);
        ASSERT_EQ_UINT(optional->variants[1].payload_count, 1u);
        ASSERT_EQ_UINT(optional->variants[1].payload_types[0], class_type_ids[1u - index]);
    }

    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_deep_class_chain_has_no_nominal_recursion_limit) {
    enum {
        CLASS_COUNT = 80,
        SOURCE_CAPACITY = 32768
    };
    char *source = xr_calloc(SOURCE_CAPACITY, 1u);
    ASSERT_NOT_NULL(source);
    size_t used = 0u;
    for (int32_t index = CLASS_COUNT - 1; index >= 0; --index) {
        int written = index == CLASS_COUNT - 1
                          ? snprintf(source + used, SOURCE_CAPACITY - used,
                                     "class C%d {\n  value: i64\n  constructor(value: i64) { "
                                     "this.value = value }\n}\n",
                                     index)
                          : snprintf(source + used, SOURCE_CAPACITY - used,
                                     "class C%d {\n  next: C%d?\n  constructor(next: C%d?) { "
                                     "this.next = next }\n}\n",
                                     index, index + 1, index + 1);
        ASSERT_TRUE(written > 0 && (size_t) written < SOURCE_CAPACITY - used);
        used += (size_t) written;
    }
    int written = snprintf(source + used, SOURCE_CAPACITY - used,
                           "fn answer(root: C0) -> i64 {\n  return 42\n}\n");
    ASSERT_TRUE(written > 0 && (size_t) written < SOURCE_CAPACITY - used);

    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);

    uint32_t class_count = 0u;
    for (uint32_t type_id = XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
         type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + product.program->type_count; ++type_id) {
        const XrValidatedType *type =
            xr_validated_program_type(product.program, (uint16_t) type_id);
        if (type && type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE) {
            ASSERT_EQ_INT(type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
            ASSERT_EQ_INT(type->copy_contract, XR_CORE_IR_COPY_EXPLICIT);
            ASSERT_EQ_UINT(type->field_count, 1u);
            ++class_count;
        }
    }
    ASSERT_EQ_UINT(class_count, CLASS_COUNT);

    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
    xr_free(source);
}

TEST(source_owner_class_alias_escape_projects_one_share) {
    static const char source[] = "class Cell {\n"
                                 "  value: i64\n"
                                 "  constructor(value: i64) { this.value = value }\n"
                                 "}\n"
                                 "class Holder {\n"
                                 "  child: Cell\n"
                                 "  constructor(child: Cell) { this.child = child }\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var cell = Cell(1)\n"
                                 "  var alias = cell\n"
                                 "  var holder = Holder(alias)\n"
                                 "  cell.value = 17\n"
                                 "  var replacement = Cell(42)\n"
                                 "  holder.child = replacement\n"
                                 "  return holder.child.value\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 3u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_ALIAS), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_PLACE), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_PLACE_EXCHANGE), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_LOAD), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);

    uint32_t entry = xr_validated_program_entry_function(product.program);
    ASSERT_LT(entry, product.program->function_count);
    const XrValidatedFunction *entry_function = &product.program->functions[entry];
    uint32_t affine_exchange_count = 0u;
    uint32_t trivial_exchange_count = 0u;
    for (uint32_t block_index = 0u; block_index < entry_function->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry_function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *exchange = &block->instructions[instruction_index];
            if (exchange->operation_id != XR_CORE_OP_CORE_PLACE_EXCHANGE)
                continue;
            ASSERT_TRUE(instruction_index != 0u);
            const XrValidatedInstruction *place = &block->instructions[instruction_index - 1u];
            ASSERT_EQ_UINT(place->operation_id, XR_CORE_OP_CORE_CLASS_FIELD_PLACE);
            ASSERT_EQ_UINT(exchange->operand_count, 2u);
            ASSERT_EQ_UINT(exchange->operands[0], place->result_id);
            if (exchange->result_ownership == XR_CORE_IR_NON_OWNER) {
                ++trivial_exchange_count;
                continue;
            }
            ASSERT_EQ_INT(exchange->result_ownership, XR_CORE_IR_OWNER);
            ASSERT_LT(instruction_index + 1u, block->instruction_count);
            const XrValidatedInstruction *drop = &block->instructions[instruction_index + 1u];
            ASSERT_EQ_UINT(drop->operation_id, XR_CORE_OP_CORE_OWNER_DROP);
            ASSERT_EQ_UINT(drop->operand_count, 1u);
            ASSERT_EQ_UINT(drop->operands[0], exchange->result_id);
            uint32_t matching_drop_count = 0u;
            for (uint32_t candidate_block = 0u; candidate_block < entry_function->block_count;
                 ++candidate_block) {
                const XrValidatedBlock *candidate = &entry_function->blocks[candidate_block];
                for (uint32_t candidate_index = 0u; candidate_index < candidate->instruction_count;
                     ++candidate_index) {
                    const XrValidatedInstruction *instruction =
                        &candidate->instructions[candidate_index];
                    matching_drop_count +=
                        instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                                instruction->operand_count == 1u &&
                                instruction->operands[0] == exchange->result_id
                            ? 1u
                            : 0u;
                }
            }
            ASSERT_EQ_UINT(matching_drop_count, 1u);
            ++affine_exchange_count;
        }
    }
    ASSERT_EQ_UINT(affine_exchange_count, 1u);
    ASSERT_EQ_UINT(trivial_exchange_count, 1u);

    SourceClassLifecycleLog log = {0};
    assert_source_class_lifecycle_result(product.program, profile, &log);
    ASSERT_TRUE(!log.overflow);
    uint32_t distinct_exchange = UINT32_MAX;
    for (uint32_t index = 0u; index < log.count; ++index) {
        ASSERT_TRUE(log.events[index].origin != XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
        if (log.events[index].kind == XR_VM_EVENT_PLACE_EXCHANGE &&
            log.events[index].identity != UINT64_MAX &&
            log.events[index].identity != log.events[index].related_identity)
            distinct_exchange = index;
    }
    ASSERT_TRUE(distinct_exchange != UINT32_MAX);
    ASSERT_LT(distinct_exchange + 1u, log.count);
    const XrVmLifecycleEvent *exchange_event = &log.events[distinct_exchange];
    const XrVmLifecycleEvent *drop_event = &log.events[distinct_exchange + 1u];
    ASSERT_EQ_INT(drop_event->kind, XR_VM_EVENT_OWNER_DROP);
    ASSERT_EQ_INT(drop_event->origin, XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION);
    ASSERT_EQ_UINT(drop_event->identity, exchange_event->identity);
    ASSERT_EQ_UINT(
        source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_FINALIZE, exchange_event->identity),
        1u);
    ASSERT_EQ_UINT(
        source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_RECLAIM, exchange_event->identity),
        1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_FINALIZE,
                                                exchange_event->related_identity),
                   1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_RECLAIM,
                                                exchange_event->related_identity),
                   1u);

    assert_aot_fixture_backend_contract(product.program, profile,
                                        XR_SOURCE_FIXTURE_CLASS_ALIAS_ESCAPE);
    xr_target_profile_free(profile);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_class_field_self_assignment_shares_before_exchange) {
    static const char source[] = "class Cell {\n"
                                 "  value: i64\n"
                                 "  constructor(value: i64) { this.value = value }\n"
                                 "}\n"
                                 "class Holder {\n"
                                 "  child: Cell\n"
                                 "  constructor(child: Cell) { this.child = child }\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var holder = Holder(Cell(42))\n"
                                 "  holder.child = holder.child\n"
                                 "  return holder.child.value\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_ALIAS), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_PLACE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_PLACE_EXCHANGE), 1u);

    uint32_t entry = xr_validated_program_entry_function(product.program);
    ASSERT_LT(entry, product.program->function_count);
    const XrValidatedFunction *entry_function = &product.program->functions[entry];
    uint32_t exchange_count = 0u;
    for (uint32_t block_index = 0u; block_index < entry_function->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry_function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *exchange = &block->instructions[instruction_index];
            if (exchange->operation_id != XR_CORE_OP_CORE_PLACE_EXCHANGE)
                continue;
            ASSERT_TRUE(instruction_index >= 2u);
            ASSERT_LT(instruction_index + 1u, block->instruction_count);
            const XrValidatedInstruction *share = &block->instructions[instruction_index - 2u];
            const XrValidatedInstruction *place = &block->instructions[instruction_index - 1u];
            const XrValidatedInstruction *drop = &block->instructions[instruction_index + 1u];
            ASSERT_EQ_UINT(share->operation_id, XR_CORE_OP_CORE_OWNER_ALIAS);
            ASSERT_EQ_UINT(place->operation_id, XR_CORE_OP_CORE_CLASS_FIELD_PLACE);
            ASSERT_EQ_UINT(drop->operation_id, XR_CORE_OP_CORE_OWNER_DROP);
            ASSERT_EQ_UINT(share->operand_count, 1u);
            ASSERT_EQ_UINT(exchange->operand_count, 2u);
            ASSERT_EQ_UINT(drop->operand_count, 1u);
            ASSERT_EQ_INT(entry_function->value_ownerships[share->operands[0]],
                          XR_CORE_IR_NON_OWNER);
            ASSERT_EQ_UINT(exchange->operands[0], place->result_id);
            ASSERT_EQ_UINT(exchange->operands[1], share->result_id);
            ASSERT_EQ_UINT(drop->operands[0], exchange->result_id);
            uint32_t matching_drop_count = 0u;
            for (uint32_t candidate_block = 0u; candidate_block < entry_function->block_count;
                 ++candidate_block) {
                const XrValidatedBlock *candidate = &entry_function->blocks[candidate_block];
                for (uint32_t candidate_index = 0u; candidate_index < candidate->instruction_count;
                     ++candidate_index) {
                    const XrValidatedInstruction *instruction =
                        &candidate->instructions[candidate_index];
                    matching_drop_count +=
                        instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                                instruction->operand_count == 1u &&
                                instruction->operands[0] == exchange->result_id
                            ? 1u
                            : 0u;
                }
            }
            ASSERT_EQ_UINT(matching_drop_count, 1u);
            ++exchange_count;
        }
    }
    ASSERT_EQ_UINT(exchange_count, 1u);

    SourceClassLifecycleLog log = {0};
    assert_source_class_lifecycle_result(product.program, profile, &log);
    ASSERT_TRUE(!log.overflow);
    uint32_t share_event = UINT32_MAX;
    for (uint32_t index = 0u; index < log.count; ++index) {
        ASSERT_TRUE(log.events[index].origin != XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN);
        if (log.events[index].kind == XR_VM_EVENT_CLASS_SHARE)
            share_event = index;
    }
    ASSERT_TRUE(share_event != UINT32_MAX);
    ASSERT_LT(share_event + 3u, log.count);
    ASSERT_EQ_INT(log.events[share_event + 1u].kind, XR_VM_EVENT_CLASS_FIELD_PLACE);
    ASSERT_EQ_INT(log.events[share_event + 2u].kind, XR_VM_EVENT_PLACE_EXCHANGE);
    ASSERT_EQ_INT(log.events[share_event + 3u].kind, XR_VM_EVENT_OWNER_DROP);
    ASSERT_EQ_UINT(log.events[share_event].identity, log.events[share_event + 2u].identity);
    ASSERT_EQ_UINT(log.events[share_event].identity, log.events[share_event + 2u].related_identity);
    ASSERT_EQ_UINT(log.events[share_event].identity, log.events[share_event + 3u].identity);
    ASSERT_EQ_UINT(source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_FINALIZE,
                                                log.events[share_event].identity),
                   1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_RECLAIM,
                                                log.events[share_event].identity),
                   1u);

    assert_aot_fixture_backend_contract(product.program, profile,
                                        XR_SOURCE_FIXTURE_CLASS_FIELD_SELF_ASSIGNMENT);
    xr_target_profile_free(profile);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_class_alias_borrows_coalesce_without_share) {
    static const char source[] = "class Cell {\n"
                                 "  value: i64\n"
                                 "  constructor(value: i64) { this.value = value }\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var cell = Cell(1)\n"
                                 "  var alias = cell\n"
                                 "  alias.value = 42\n"
                                 "  return cell.value\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_ALIAS), 0u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_PLACE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_PLACE_EXCHANGE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_LOAD), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);

    assert_detached_program_i64_result(product.program, profile, 42);

    assert_aot_fixture_backend_contract(product.program, profile,
                                        XR_SOURCE_FIXTURE_CLASS_ALIAS_BORROW);
    xr_target_profile_free(profile);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_class_alias_final_transfer_coalesces_to_move) {
    static const char source[] = "class Cell {\n"
                                 "  value: i64\n"
                                 "  constructor(value: i64) { this.value = value }\n"
                                 "}\n"
                                 "class Holder {\n"
                                 "  child: Cell\n"
                                 "  constructor(child: Cell) { this.child = child }\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var cell = Cell(42)\n"
                                 "  var alias = cell\n"
                                 "  var holder = Holder(alias)\n"
                                 "  return holder.child.value\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_ALIAS), 0u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_MOVE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_LOAD), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);

    assert_detached_program_i64_result(product.program, profile, 42);

    assert_aot_fixture_backend_contract(product.program, profile,
                                        XR_SOURCE_FIXTURE_CLASS_ALIAS_TRANSFER);
    xr_target_profile_free(profile);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_function_parameter_suspending_callable_has_one_program) {
    static const char source[] = "fn apply(value: i64, body: fn(i64) -> i64) -> i64 {\n"
                                 "  return body(value)\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  const factor = 2\n"
                                 "  const body = fn(value: i64) -> i64 {\n"
                                 "    Coro.yield()\n"
                                 "    return value * factor\n"
                                 "  }\n"
                                 "  const first = apply(20, body)\n"
                                 "  return first + body(1)\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);

    const XrValidatedProgram *program = product.program;
    uint32_t entry = xr_validated_program_entry_function(program);
    ASSERT_LT(entry, program->function_count);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT), 2u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED), 1u);
    const XrValidatedFunction *entry_function = &program->functions[entry];
    ASSERT_EQ_UINT(entry_function->coroutine_state_count, 3u);
    ASSERT_EQ_UINT(entry_function->coroutine_safepoint_count, 2u);

    const XrValidatedInstruction *pack = NULL;
    const XrValidatedInstruction *sealed_call = NULL;
    const XrValidatedInstruction *entry_indirect_call = NULL;
    uint32_t entry_indirect_block = UINT32_MAX;
    for (uint32_t block = 0u; block < entry_function->block_count; ++block) {
        const XrValidatedBlock *row = &entry_function->blocks[block];
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
            const XrValidatedInstruction *candidate = &row->instructions[instruction];
            if (candidate->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK) {
                ASSERT_NULL(pack);
                pack = candidate;
            } else if (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                ASSERT_NULL(sealed_call);
                sealed_call = candidate;
            } else if (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
                ASSERT_NULL(entry_indirect_call);
                entry_indirect_call = candidate;
                entry_indirect_block = block;
            }
        }
    }
    ASSERT_NOT_NULL(pack);
    ASSERT_NOT_NULL(sealed_call);
    ASSERT_NOT_NULL(entry_indirect_call);
    ASSERT_EQ_UINT(pack->operand_count, 1u);
    ASSERT_EQ_INT(pack->result_ownership, XR_CORE_IR_OWNER);
    ASSERT_EQ_INT(pack->immediate_kind, XR_CORE_IR_IMMEDIATE_FUNCTION);
    uint32_t callable_owner = pack->result_id;
    ASSERT_LT(callable_owner, entry_function->value_count);
    ASSERT_EQ_INT(entry_function->value_ownerships[callable_owner], XR_CORE_IR_OWNER);
    uint32_t sealed_safepoint_id = sealed_call->immediate.coroutine_call.safepoint_id;
    uint32_t indirect_safepoint_id = entry_indirect_call->immediate.u32;
    ASSERT_LT(sealed_safepoint_id, entry_function->coroutine_safepoint_count);
    ASSERT_LT(indirect_safepoint_id, entry_function->coroutine_safepoint_count);
    ASSERT_TRUE(sealed_safepoint_id != indirect_safepoint_id);
    const XrValidatedCoroutineSafepoint *sealed_point =
        &entry_function->coroutine_safepoints[sealed_safepoint_id];
    const XrValidatedCoroutineSafepoint *indirect_point =
        &entry_function->coroutine_safepoints[indirect_safepoint_id];
    ASSERT_EQ_UINT(sealed_point->live_value_count, 1u);
    uint32_t callable_frame_owner = sealed_point->live_value_ids[0];
    ASSERT_LT(callable_frame_owner, entry_function->value_count);
    ASSERT_EQ_INT(entry_function->value_ownerships[callable_frame_owner], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(entry_function->value_types[callable_frame_owner],
                   entry_function->value_types[callable_owner]);

    uint32_t apply_id = sealed_call->immediate.coroutine_call.function_id;
    ASSERT_LT(apply_id, program->function_count);
    const XrValidatedFunction *apply = &program->functions[apply_id];
    ASSERT_EQ_UINT(apply->parameter_count, 2u);
    ASSERT_EQ_INT(apply->parameter_modes[0], XR_PARAM_READ);
    ASSERT_EQ_INT(apply->parameter_modes[1], XR_PARAM_READ);
    ASSERT_EQ_UINT(sealed_call->operand_count, 4u);
    ASSERT_EQ_UINT(sealed_call->operands[1], callable_frame_owner);
    ASSERT_EQ_UINT(sealed_call->operands[2], callable_frame_owner);
    ASSERT_EQ_UINT(sealed_call->operands[3], callable_frame_owner);
    const XrValidatedBlock *sealed_normal = &entry_function->blocks[sealed_call->successors[0]];
    const XrValidatedBlock *sealed_cancel = &entry_function->blocks[sealed_call->successors[1]];
    ASSERT_EQ_UINT(sealed_normal->argument_count, 2u);
    ASSERT_EQ_INT(sealed_normal->argument_ownerships[1], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(sealed_cancel->argument_count, 1u);
    ASSERT_EQ_INT(sealed_cancel->argument_ownerships[0], XR_CORE_IR_OWNER);
    uint32_t sealed_normal_owner = sealed_normal->argument_ids[1];
    uint32_t normal_owner_drops = 0u;
    const XrValidatedInstruction *owner_forward_branch = NULL;
    for (uint32_t instruction = 0u; instruction < sealed_normal->instruction_count; ++instruction) {
        const XrValidatedInstruction *candidate = &sealed_normal->instructions[instruction];
        normal_owner_drops += candidate->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                                      candidate->operand_count == 1u &&
                                      candidate->operands[0] == sealed_normal_owner
                                  ? 1u
                                  : 0u;
        if (candidate->operation_id == XR_CORE_OP_CORE_BRANCH)
            owner_forward_branch = candidate;
    }
    ASSERT_EQ_UINT(normal_owner_drops, 0u);
    ASSERT_NOT_NULL(owner_forward_branch);
    ASSERT_EQ_UINT(owner_forward_branch->successor_count, 1u);
    ASSERT_EQ_UINT(owner_forward_branch->successors[0], entry_indirect_block);
    ASSERT_EQ_UINT(owner_forward_branch->operand_count, 3u);
    const XrValidatedBlock *forward_target = &entry_function->blocks[entry_indirect_block];
    ASSERT_EQ_UINT(forward_target->argument_count, owner_forward_branch->operand_count);
    uint32_t forwarded_owners = 0u;
    for (uint32_t i = 0u; i < forward_target->argument_count; ++i) {
        if (forward_target->argument_ownerships[i] == XR_CORE_IR_OWNER) {
            ASSERT_EQ_UINT(owner_forward_branch->operands[i], sealed_normal_owner);
            ASSERT_EQ_UINT(forward_target->argument_ids[i], entry_indirect_call->operands[0]);
            ++forwarded_owners;
        }
    }
    ASSERT_EQ_UINT(forwarded_owners, 1u);
    ASSERT_EQ_UINT(sealed_cancel->instruction_count, 3u);
    ASSERT_EQ_UINT(sealed_cancel->instructions[1].operation_id, XR_CORE_OP_CORE_OWNER_DROP);
    ASSERT_EQ_UINT(sealed_cancel->instructions[1].operands[0], sealed_cancel->argument_ids[0]);
    ASSERT_EQ_UINT(sealed_cancel->instructions[2].operation_id, XR_CORE_OP_CORE_CANCEL_PUBLISH);

    ASSERT_EQ_UINT(entry_indirect_call->operand_count, 5u);
    uint32_t indirect_owner = entry_indirect_call->operands[0];
    ASSERT_LT(indirect_owner, entry_function->value_count);
    ASSERT_EQ_INT(entry_function->value_ownerships[indirect_owner], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(entry_function->value_types[indirect_owner],
                   entry_function->value_types[callable_owner]);
    ASSERT_EQ_UINT(indirect_point->live_value_count, 2u);
    uint32_t indirect_owner_lives = 0u;
    uint32_t indirect_non_owner_lives = 0u;
    uint32_t indirect_owner_live_index = UINT32_MAX;
    for (uint32_t live = 0u; live < indirect_point->live_value_count; ++live) {
        uint32_t value = indirect_point->live_value_ids[live];
        ASSERT_LT(value, entry_function->value_count);
        ASSERT_EQ_UINT(entry_indirect_call->operands[2u + live], value);
        indirect_owner_lives +=
            entry_function->value_ownerships[value] == XR_CORE_IR_OWNER ? 1u : 0u;
        if (entry_function->value_ownerships[value] == XR_CORE_IR_OWNER)
            indirect_owner_live_index = live;
        indirect_non_owner_lives +=
            entry_function->value_ownerships[value] == XR_CORE_IR_NON_OWNER ? 1u : 0u;
    }
    ASSERT_EQ_UINT(indirect_owner_lives, 1u);
    ASSERT_EQ_UINT(indirect_non_owner_lives, 1u);
    ASSERT_EQ_UINT(entry_indirect_call->operands[4], indirect_owner);
    const XrValidatedBlock *indirect_normal =
        &entry_function->blocks[entry_indirect_call->successors[0]];
    const XrValidatedBlock *indirect_cancel =
        &entry_function->blocks[entry_indirect_call->successors[1]];
    ASSERT_EQ_UINT(indirect_normal->argument_count, 3u);
    ASSERT_LT(indirect_owner_live_index, indirect_point->live_value_count);
    uint32_t indirect_normal_owner = indirect_normal->argument_ids[1u + indirect_owner_live_index];
    ASSERT_EQ_INT(entry_function->value_ownerships[indirect_normal_owner], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(entry_function->value_types[indirect_normal_owner],
                   entry_function->value_types[indirect_owner]);
    ASSERT_EQ_UINT(indirect_cancel->argument_count, 1u);
    ASSERT_EQ_INT(indirect_cancel->argument_ownerships[0], XR_CORE_IR_OWNER);
    uint32_t indirect_normal_owner_drops = 0u;
    for (uint32_t instruction = 0u; instruction < indirect_normal->instruction_count;
         ++instruction) {
        const XrValidatedInstruction *candidate = &indirect_normal->instructions[instruction];
        indirect_normal_owner_drops += candidate->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                                               candidate->operand_count == 1u &&
                                               candidate->operands[0] == indirect_normal_owner
                                           ? 1u
                                           : 0u;
    }
    ASSERT_EQ_UINT(indirect_normal_owner_drops, 1u);
    ASSERT_EQ_UINT(indirect_cancel->instruction_count, 3u);
    ASSERT_EQ_UINT(indirect_cancel->instructions[1].operation_id, XR_CORE_OP_CORE_OWNER_DROP);
    ASSERT_EQ_UINT(indirect_cancel->instructions[1].operands[0], indirect_cancel->argument_ids[0]);
    ASSERT_EQ_UINT(indirect_cancel->instructions[2].operation_id, XR_CORE_OP_CORE_CANCEL_PUBLISH);

    const XrValidatedInstruction *indirect_call = NULL;
    for (uint32_t block = 0u; block < apply->block_count; ++block)
        for (uint32_t instruction = 0u; instruction < apply->blocks[block].instruction_count;
             ++instruction)
            if (apply->blocks[block].instructions[instruction].operation_id ==
                XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT) {
                ASSERT_NULL(indirect_call);
                indirect_call = &apply->blocks[block].instructions[instruction];
            }
    ASSERT_NOT_NULL(indirect_call);
    ASSERT_EQ_UINT(apply->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(apply->coroutine_safepoints[0].live_value_count, 0u);
    ASSERT_LT(indirect_call->operands[0], apply->value_count);
    ASSERT_EQ_INT(apply->value_ownerships[indirect_call->operands[0]], XR_CORE_IR_NON_OWNER);

    uint32_t closure_id = pack->immediate.function_id;
    ASSERT_LT(closure_id, program->function_count);
    ASSERT_TRUE(closure_id != entry && closure_id != apply_id);
    const XrValidatedFunction *closure = &program->functions[closure_id];
    ASSERT_EQ_UINT(closure->parameter_count, 2u);
    ASSERT_EQ_INT(closure->parameter_modes[0], XR_PARAM_READ);
    ASSERT_EQ_INT(closure->parameter_modes[1], XR_PARAM_READ);
    ASSERT_EQ_UINT(closure->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(closure->coroutine_safepoints[0].live_value_count, 2u);
    for (uint32_t live = 0u; live < closure->coroutine_safepoints[0].live_value_count; ++live) {
        uint32_t value = closure->coroutine_safepoints[0].live_value_ids[live];
        ASSERT_LT(value, closure->value_count);
        ASSERT_EQ_INT(closure->value_categories[value], XR_CORE_IR_VALUE);
        ASSERT_EQ_INT(closure->value_ownerships[value], XR_CORE_IR_NON_OWNER);
    }

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    {
        XrVmCodeOptions vm_options = xr_vm_code_default_options();
        XrVmCodeDiagnostic vm_diagnostic;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(
            xr_vm_code_build(binding.program, binding.profile, &vm_options, &code, &vm_diagnostic),
            XR_VM_CODE_OK);
        ASSERT_NOT_NULL(code);

        XrVmExecution *vm = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm));
        XrVmOutcome vm_first_suspended = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_first_suspended.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(vm_first_suspended.safepoint_id, sealed_safepoint_id);
        ASSERT_EQ_UINT(vm_first_suspended.state_id, sealed_point->resume_state_id);
        XrVmOutcome vm_second_suspended = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_second_suspended.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(vm_second_suspended.safepoint_id, indirect_safepoint_id);
        ASSERT_EQ_UINT(vm_second_suspended.state_id, indirect_point->resume_state_id);
        XrVmOutcome vm_return = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_return.value.as.i64, 42);
        xr_vm_execution_free(vm);

        XrVmExecution *vm_cancel = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm_cancel));
        ASSERT_EQ_INT(xr_vm_execution_step(vm_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_vm_execution_cancel(vm_cancel).kind, XR_VM_OUTCOME_CANCELLED);
        xr_vm_execution_free(vm_cancel);

        XrVmExecution *vm_second_cancel = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm_second_cancel));
        ASSERT_EQ_INT(xr_vm_execution_step(vm_second_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_vm_execution_step(vm_second_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_vm_execution_cancel(vm_second_cancel).kind, XR_VM_OUTCOME_CANCELLED);
        xr_vm_execution_free(vm_second_cancel);
        xr_vm_code_free(code);
    }

    XrBackendOptions backend_options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    XrBackendStatus backend_status =
        xr_backend_ir_build(program, profile, &backend_options, &backend_ir, &backend_diagnostic);
    if (backend_status != XR_BACKEND_OK)
        fprintf(stderr,
                "indirect coroutine AOT build failed: status=%s op=%u function=%u block=%u "
                "instruction=%u\n",
                xr_backend_status_name(backend_status), backend_diagnostic.operation_id,
                backend_diagnostic.function_id, backend_diagnostic.block_id,
                backend_diagnostic.instruction_id);
    ASSERT_EQ_INT(backend_status, XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "callable_capture_"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_alloc(xr_ctx"));
    ASSERT_NOT_NULL(strstr(generated.bytes, ".capture = (void *)callable_capture_"));
    ASSERT_NOT_NULL(strstr(generated.bytes, ".capture;"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path =
        source_fixture_output_path(XR_SOURCE_FIXTURE_FUNCTION_PARAMETER_SUSPENDING);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);

    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

#include "test_xr_program_process_provider.inc.c"

TEST(source_owner_process_provider_uses_generated_bindings_across_private_executors) {
    assert_process_provider_source(XR_SOURCE_FIXTURE_PROCESS_PROVIDER);
    assert_process_provider_source_rejections();
    assert_process_provider_name_is_not_authority();
}

typedef struct TextOutputCapture {
    uint8_t bytes[256];
    size_t size;
    uint32_t calls;
    uint32_t fail_at;
} TextOutputCapture;

static XrProviderCallStatus text_output_capture_write(void *context, const uint8_t *bytes,
                                                      size_t size) {
    TextOutputCapture *capture = context;
    if (!capture || (!bytes && size != 0u) || size > sizeof(capture->bytes) - capture->size)
        return XR_PROVIDER_CALL_FAILED;
    if (++capture->calls == capture->fail_at)
        return XR_PROVIDER_CALL_FAILED;
    if (size != 0u)
        memcpy(capture->bytes + capture->size, bytes, size);
    capture->size += size;
    return XR_PROVIDER_CALL_OK;
}

#include "xr_program_output_cleanup_checks.inc.c"

TEST(source_owner_module_error_defer_output_preserves_failure_and_owners) {
    assert_output_initializer_failure(false, XR_SOURCE_FIXTURE_MODULE_ERROR_OUTPUT);
}

TEST(source_owner_module_panic_defer_output_preserves_failure_and_owners) {
    assert_output_initializer_failure(true, XR_SOURCE_FIXTURE_MODULE_PANIC_OUTPUT);
}

static void assert_module_report(bool extended, XrSourceFixtureId fixture_id) {
    size_t size = 0u;
    char *entry = xr_file_read_all(extended ? XR_MODULE_REPORT_FIXTURE_DIR "/extended.xr"
                                            : XR_MODULE_REPORT_FIXTURE_DIR "/main.xr",
                                   "r", &size);
    char *library = xr_file_read_all(XR_MODULE_REPORT_FIXTURE_DIR "/library.xr", "r", &size);
    char *facade =
        extended ? xr_file_read_all(XR_MODULE_REPORT_FIXTURE_DIR "/facade.xr", "r", &size) : NULL;
    ASSERT_NOT_NULL(entry);
    ASSERT_NOT_NULL(library);
    if (extended)
        ASSERT_NOT_NULL(facade);
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry, library));
    if (extended)
        ASSERT_TRUE(source_build_fixture_add_facade(&fixture, facade));
    xr_free(entry);
    xr_free(library);
    xr_free(facade);
    fixture.input.entry.kind = XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER;
    fixture.input.entry.function_name = NULL;
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (!product.program) {
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    ASSERT_EQ_UINT(product.program->module_count, extended ? 3u : 2u);
    const char *expected =
        extended ? "library 40 ready\nfacade warm:41\nentry report\nrow:42|row:43 tail:44 44\n"
                 : "library 40 ready\nentry report\nalpha:41 beta:42 42\n";
    TextOutputCapture capture = {0};
    XrProgramProviderRequirementView requirement = {0};
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    ASSERT_TRUE(xr_validated_program_provider_requirement(product.program, 0u, &requirement));
    ASSERT_EQ_UINT(requirement.operation_count, 1u);
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE,
        .context = &capture,
    };
    operation.entry.output_write = text_output_capture_write;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(product.program, profile, &options, &code, NULL),
                      XR_VM_CODE_OK);
        for (uint32_t separate = 0u; separate < 2u; ++separate) {
            capture = (TextOutputCapture) {0};
            XrInstance *instance = NULL;
            ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
            for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
                XrVmOutcome result = xr_vm_code_execute(
                    code, instance, xr_validated_program_entry_function(product.program), NULL, 0u);
                ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_RETURN);
                ASSERT_EQ_INT(result.value.kind, XR_VM_VALUE_VOID);
                ASSERT_EQ_UINT(capture.calls, extended ? 4u : 3u);
                ASSERT_EQ_UINT(capture.size, strlen(expected));
                ASSERT_EQ_INT(memcmp(capture.bytes, expected, capture.size), 0);
            }
            ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
        }
        xr_vm_code_free(code);
    }
    assert_aot_fixture_backend_contract(product.program, profile, fixture_id);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_module_report_has_instance_state_and_ordered_text) {
    assert_module_report(false, XR_SOURCE_FIXTURE_MODULE_REPORT);
}

TEST(source_owner_module_report_extension_reuses_shared_state) {
    assert_module_report(true, XR_SOURCE_FIXTURE_MODULE_REPORT_EXTENDED);
}

static void assert_failure_observation_retirement(XrVmCode *code, XrInstance **instance_owner,
                                                   XrValidatedProgram *program, bool panic, bool has_message) {
    XrInstance *instance = *instance_owner;
    XrVmExecution *borrowed_frame = NULL;
    XrVmOutcome borrowed = {0};
    XrVmOutcome owned = {0};
    if (!panic || has_message) {
        owned = xr_vm_code_execute(code, instance,
            xr_validated_program_entry_function(program), NULL, 0u);
        ASSERT_EQ_INT(owned.kind, panic ? XR_VM_OUTCOME_PANIC : XR_VM_OUTCOME_ERROR);
        ASSERT_TRUE(xr_vm_execution_create(code, instance,
            xr_validated_program_entry_function(program), NULL, 0u, &borrowed_frame));
        borrowed = xr_vm_execution_step(borrowed_frame);
        ASSERT_EQ_INT(borrowed.kind, panic ? XR_VM_OUTCOME_PANIC : XR_VM_OUTCOME_ERROR);
        ASSERT_FALSE(borrowed.owns_dynamic_values);
        ASSERT_EQ_UINT(xr_execution_instance_lease_count(instance), 2u);
    }
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
    if (!panic || has_message) {
        ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL),
                      XR_EXECUTION_GENERATION_REJECTED);
        XrVmAggregateView error;
        XrVmStringView message;
        if (has_message) {
            ASSERT_TRUE(xr_vm_panic_message_view(&borrowed.panic_value.as.panic_info, &message));
        } else {
            ASSERT_TRUE(xr_vm_value_aggregate_view(&borrowed.error_value, &error));
            ASSERT_TRUE(xr_vm_value_string_view(&error.fields[1], &message));
        }
        ASSERT_EQ_UINT(message.size, 11u);
        ASSERT_TRUE(memcmp(message.bytes, "failed init", 11u) == 0);
        xr_vm_execution_free(borrowed_frame);
        ASSERT_EQ_UINT(xr_execution_instance_lease_count(instance), 1u);
        ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL),
                      XR_EXECUTION_GENERATION_REJECTED);
        if (has_message) {
            ASSERT_TRUE(xr_vm_panic_message_view(&owned.panic_value.as.panic_info, &message));
        } else {
            ASSERT_TRUE(xr_vm_value_aggregate_view(&owned.error_value, &error));
            ASSERT_TRUE(xr_vm_value_string_view(&error.fields[1], &message));
        }
        ASSERT_EQ_UINT(message.size, 11u);
        ASSERT_TRUE(memcmp(message.bytes, "failed init", 11u) == 0);
        xr_vm_outcome_dispose(&owned);
        ASSERT_EQ_UINT(xr_execution_instance_lease_count(instance), 0u);
    }
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(instance_owner, NULL), XR_EXECUTION_OK);
}

static void assert_module_initializer_failure(bool panic, XrSourceFixtureId fixture_id) {
    bool has_message = fixture_id == XR_SOURCE_FIXTURE_MODULE_INITIALIZER_PANIC_MESSAGE;
    char library[768];
    int length =
        snprintf(library, sizeof(library),
                 "enum InitFailure { Rejected { code: i64, message: string } }\n"
                 "class Cell { value: i64; constructor(value: i64) { this.value = value } }\n"
                 "var state = Cell(1)\n"
                 "fn initialize(ok: bool) -> i64 {\n"
                 "  var local = Cell(2)\n"
                 "  defer { local.value = 3 }\n"
                 "  if (!ok) { %s }\n"
                 "  return local.value\n"
                 "}\n"
                 "initialize(false)\n",
                 has_message ? "assert(ok, \"failed \" + \"init\")"
                 : panic ? "assert(ok)" : "throw InitFailure.Rejected { code: 7, message: \"failed init\" }");
    ASSERT_TRUE(length > 0 && (size_t) length < sizeof(library));
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(
        &fixture, "import \"./library\"\nfn answer() -> i64 { return 42 }\n", library));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(product.program->module_count, 2u);
    source_build_fixture_free(&fixture);
    if (!panic) {
        uint32_t named_errors = 0u;
        for (uint32_t type = 0u; type < product.program->type_count; ++type) {
            uint16_t type_id = (uint16_t) (XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + type);
            const char *name = xr_validated_program_type_display_name(product.program, type_id);
            if (!name || strcmp(name, "InitFailure") != 0)
                continue;
            ++named_errors;
            const char *variant =
                xr_validated_program_variant_display_name(product.program, type_id, 0u);
            ASSERT_TRUE(variant && strcmp(variant, "Rejected") == 0);
        }
        ASSERT_EQ_UINT(named_errors, 1u);
    }
    {
        SourceClassLifecycleLog log = {0};
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = &log;
        options.lifecycle_event = record_source_class_lifecycle;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(product.program, profile, &options, &code, NULL),
                      XR_VM_CODE_OK);
        for (uint32_t separate = 0u; separate < 2u; ++separate) {
            log = (SourceClassLifecycleLog) {0};
            XrExecutionBindingInput binding = {
                .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                .program = product.program,
                .profile = profile,
                .generation = 1u,
            };
            XrInstance *instance = NULL;
            ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
            uint32_t terminal_count = 0u;
            for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
                XrVmOutcome result = xr_vm_code_execute(
                    code, instance, xr_validated_program_entry_function(product.program), NULL, 0u);
                ASSERT_EQ_INT(result.kind, panic ? XR_VM_OUTCOME_PANIC : XR_VM_OUTCOME_ERROR);
                ASSERT_EQ_INT(result.trap, XR_VM_TRAP_NONE);
                if (!panic) {
                    XrVmAggregateView error;
                    XrVmStringView message;
                    ASSERT_TRUE(xr_vm_value_aggregate_view(&result.error_value, &error));
                    ASSERT_EQ_UINT(error.variant_ordinal, 0u);
                    ASSERT_EQ_UINT(error.field_count, 2u);
                    ASSERT_EQ_INT(error.fields[0].kind, XR_VM_VALUE_I64);
                    ASSERT_EQ_INT(error.fields[0].as.i64, 7);
                    ASSERT_TRUE(xr_vm_value_string_view(&error.fields[1], &message));
                    ASSERT_EQ_UINT(message.size, 11u);
                    ASSERT_TRUE(memcmp(message.bytes, "failed init", 11u) == 0);
                    ASSERT_TRUE(result.owns_dynamic_values);
                }
                ASSERT_EQ_INT(result.panic_value.kind,
                              panic ? XR_VM_VALUE_PANIC_INFO : XR_VM_VALUE_VOID);
                if (panic)
                    ASSERT_EQ_UINT(result.panic_value.as.panic_info.code, 1u);
                if (has_message) {
                    XrVmStringView message = {0};
                    ASSERT_TRUE(result.owns_dynamic_values);
                    ASSERT_TRUE(xr_vm_panic_message_view(&result.panic_value.as.panic_info, &message));
                    ASSERT_EQ_UINT(message.size, 11u);
                    ASSERT_EQ_INT(memcmp(message.bytes, "failed init", 11u), 0);
                }
                xr_vm_outcome_dispose(&result);
                ASSERT_FALSE(log.overflow);
                if (repeat == 0u)
                    terminal_count = log.count;
                ASSERT_EQ_UINT(log.count, terminal_count);
            }
            uint64_t created[2] = {0};
            uint32_t creates = 0u, finalized = 0u;
            for (uint32_t event = 0u; event < log.count; ++event) {
                const XrVmLifecycleEvent *observed = &log.events[event];
                if (observed->kind == XR_VM_EVENT_CLASS_CONSTRUCT) {
                    ASSERT_LT(creates, 2u);
                    created[creates++] = observed->identity;
                } else if (observed->kind == XR_VM_EVENT_CLASS_FINALIZE) {
                    ASSERT_EQ_UINT(creates, 2u);
                    ASSERT_LT(finalized, 2u);
                    ASSERT_EQ_UINT(observed->identity, created[1u - finalized]);
                    ++finalized;
                }
            }
            ASSERT_EQ_UINT(creates, 2u);
            ASSERT_EQ_UINT(finalized, 2u);
            for (uint32_t object = 0u; object < 2u; ++object) {
                ASSERT_EQ_UINT(
                    source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_FINALIZE, created[object]),
                    1u);
                ASSERT_EQ_UINT(
                    source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_RECLAIM, created[object]),
                    1u);
            }
            assert_failure_observation_retirement(code, &instance, product.program, panic, has_message);
            ASSERT_EQ_UINT(log.count, terminal_count);
        }
        xr_vm_code_free(code);
    }
    assert_aot_fixture_backend_contract(product.program, profile, fixture_id);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
}

TEST(source_owner_module_initializer_error_cleans_owned_state) {
    assert_module_initializer_failure(false, XR_SOURCE_FIXTURE_MODULE_INITIALIZER_ERROR);
}

TEST(source_owner_module_initializer_panic_message_retains_failed_instance) {
    assert_module_initializer_failure(true, XR_SOURCE_FIXTURE_MODULE_INITIALIZER_PANIC_MESSAGE);
}

TEST(source_owner_module_initializer_panic_cleans_owned_state) {
    assert_module_initializer_failure(true, XR_SOURCE_FIXTURE_MODULE_INITIALIZER_PANIC);
}

/* A complete string program: literals, a string parameter and result,
 * interpolation with an i64 piece, ordered comparison, a loop accumulating
 * through a mutable string place, and typed output of mixed operands.  The
 * VM and generated C are checked against independently specified output bytes. */
TEST(source_owner_text_program_is_exact_across_private_executors) {
    static const char source[] = "fn greet(name: string) -> string {\n"
                                 "  return \"hello, \" + name\n"
                                 "}\n"
                                 "fn describe(n: i64, tag: string) -> string {\n"
                                 "  return \"${tag}=${n}\"\n"
                                 "}\n"
                                 "fn tally(limit: i64) -> string {\n"
                                 "  var i = 0\n"
                                 "  var out = \"\"\n"
                                 "  while (i < limit) {\n"
                                 "    out = out + \"x\"\n"
                                 "    i = i + 1\n"
                                 "  }\n"
                                 "  return out\n"
                                 "}\n"
                                 "fn echo(v: string) -> string {\n"
                                 "  if (v == \"yes\") { return v }\n"
                                 "  return v\n"
                                 "}\n"
                                 "fn pick(flag: bool, a: string, b: string) -> string {\n"
                                 "  if (flag) { return a }\n"
                                 "  return b\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  const s = greet(\"world\")\n"
                                 "  print(s)\n"
                                 "  const d = describe(42, \"answer\")\n"
                                 "  print(d, s == \"hello, world\", 'x')\n"
                                 "  var code = 0\n"
                                 "  if (d < s) { code = 1 }\n"
                                 "  if (\"\" == \"\") { code = code + 2 }\n"
                                 "  const t = tally(3)\n"
                                 "  const e = echo(pick(code == 3, \"yes\", \"no\"))\n"
                                 "  print(\"code\", code, t, e, echo(pick(false, e, \"no\")))\n"
                                 "  if (t == \"xxx\") { code = code + 4 }\n"
                                 "  return code\n"
                                 "}\n";
    static const char expected_stdout[] = "hello, world\nanswer=42 true x\ncode 3 xxx yes no\n";
    const size_t expected_size = sizeof(expected_stdout) - 1u;
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 1u);
    ASSERT_TRUE(program_operation_count(first.program, XR_CORE_OP_CORE_STRING_CONCAT) != 0u);
    ASSERT_TRUE(program_operation_count(first.program, XR_CORE_OP_CORE_STRING_FROM_SCALAR) != 0u);
    ASSERT_TRUE(program_operation_count(first.program, XR_CORE_OP_CORE_COMPARE_STRING) != 0u);
    ASSERT_TRUE(program_operation_count(first.program, XR_CORE_OP_CORE_CONSTANT_RUNE) != 0u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_OUTPUT_GROUP), 3u);
    ASSERT_TRUE(program_operation_count(first.program, XR_CORE_OP_CORE_OWNER_DROP) != 0u);
    /* Both echo branches return the same READ parameter, while pick returns
     * distinct parameters. Each return owns a separate explicit copy. */
    ASSERT_TRUE(program_operation_count(first.program, XR_CORE_OP_CORE_OWNER_COPY) >= 4u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, 0u, &requirement));
    ASSERT_EQ_UINT(requirement.operation_count, 1u);
    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    TextOutputCapture capture = {0};
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE,
        .context = &capture,
    };
    operation.entry.output_write = text_output_capture_write;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(first.program);

    XrVmCodeOptions baseline_options = xr_vm_code_default_options();
    XrVmCode *baseline = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(binding.program, binding.profile, &baseline_options, &baseline,
                                   &vm_diagnostic),
                  XR_VM_CODE_OK);
    XrVmOutcome baseline_result = xr_vm_code_execute(baseline, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(baseline_result.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(baseline_result.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(baseline_result.value.as.i64, 7);
    XrInstance *isolated_instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &isolated_instance, NULL), XR_EXECUTION_OK);
    XrVmOutcome isolated_result = xr_vm_code_execute(baseline, isolated_instance, entry, NULL, 0u);
    ASSERT_EQ_INT(isolated_result.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(isolated_result.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(isolated_result.value.as.i64, 7);
    ASSERT_EQ_UINT(isolated_result.steps, baseline_result.steps);
    ASSERT_EQ_UINT(capture.calls, 6u);
    ASSERT_EQ_UINT(capture.size, expected_size * 2u);
    for (size_t copy = 0u; copy < 2u; ++copy)
        ASSERT_EQ_INT(memcmp(capture.bytes + copy * expected_size, expected_stdout, expected_size),
                      0);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "XR_TEXT_KERNEL_H"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_string_concat"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_text_group_render"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_free(xr_ctx, v"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_TEXT_PROGRAM);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }

    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    xr_vm_outcome_dispose(&isolated_result);
    xr_vm_outcome_dispose(&baseline_result);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(isolated_instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(isolated_instance, NULL), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&isolated_instance, NULL), XR_EXECUTION_OK);
    xr_vm_code_free(baseline);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_clock_provider_is_exact_across_private_executors) {
    static const char source[] = "import time\n"
                                 "fn answer() -> i64 {\n"
                                 "  const wall = time.now()\n"
                                 "  const monotonic = time.monotonic()\n"
                                 "  const cpu = time.clock()\n"
                                 "  const offset = time.localOffsetAt(0)\n"
                                 "  if (wall <= 0) { return 0 }\n"
                                 "  if (monotonic <= 0) { return 0 }\n"
                                 "  if (cpu < 0) { return 0 }\n"
                                 "  if (offset < -1440) { return 0 }\n"
                                 "  if (offset > 1440) { return 0 }\n"
                                 "  return 1\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    assert_generated_provider_execution(first.program, profile);
    ASSERT_EQ_UINT(first.program->module_count, 2u);
    /* Both module initializers join the five reachable ordinary functions. */
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 7u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 1u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, 0u, &requirement));
    ASSERT_EQ_UINT(requirement.operation_count, 4u);
    XrStableId expected_contract;
    XrStableId expected_operations[4];
    static const char *operation_keys[] = {
        XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY,
        XR_PROVIDER_CLOCK_MONOTONIC_NANOS_OPERATION_KEY,
        XR_PROVIDER_CLOCK_PROCESS_CPU_NANOS_OPERATION_KEY,
        XR_PROVIDER_CLOCK_UTC_OFFSET_MINUTES_AT_OPERATION_KEY,
    };
    XrFingerprint key_digest;
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_CLOCK_CONTRACT_KEY, &expected_contract, &key_digest));
    for (uint32_t i = 0u; i < 4u; ++i)
        ASSERT_TRUE(xr_stable_id_from_key(operation_keys[i], &expected_operations[i], &key_digest));
    ASSERT_TRUE(stable_id_equal(requirement.contract_id, expected_contract));

    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    ClockProviderProbe probes[4] = {
        {.nanos = INT64_C(73000000)},
        {.nanos = INT64_C(11000000)},
        {.nanos = INT64_C(5000000)},
        {.nanos = INT64_C(7), .expected_argument = 0},
    };
    XrProviderOperationBinding operations[4] = {0};
    for (uint16_t operation_index = 0u; operation_index < requirement.operation_count;
         ++operation_index) {
        uint32_t expected_index = UINT32_MAX;
        for (uint32_t i = 0u; i < 4u; ++i) {
            if (stable_id_equal(requirement.operations[operation_index].operation_id,
                                expected_operations[i]))
                expected_index = i;
        }
        ASSERT_LT(expected_index, 4u);
        const XrTargetProviderOperationContract *contract_operation =
            find_profile_provider_operation(contract,
                                            requirement.operations[operation_index].operation_id);
        ASSERT_NOT_NULL(contract_operation);
        ASSERT_EQ_INT(contract_operation->call_abi.result.value_kind,
                      XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
        operations[operation_index].operation_id =
            requirement.operations[operation_index].operation_id;
        operations[operation_index].context = &probes[expected_index];
        if (expected_index == 3u) {
            ASSERT_EQ_UINT(contract_operation->call_abi.parameter_count, 1u);
            operations[operation_index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_UNARY;
            operations[operation_index].entry.i64_unary = clock_provider_unary_probe;
        } else {
            ASSERT_EQ_UINT(contract_operation->call_abi.parameter_count, 0u);
            operations[operation_index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[operation_index].entry.i64_nullary = clock_provider_probe;
        }
    }
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = operations,
        .operation_count = 4u,
    };
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(first.program);

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(
        xr_vm_code_build(binding.program, binding.profile, NULL, &vm_code, &vm_diagnostic),
        XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm.value.as.i64, 1);
    for (uint32_t i = 0u; i < 4u; ++i)
        ASSERT_EQ_UINT(probes[i].calls, 1u);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_typed"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_time_realtime_ns()"));
    ASSERT_NULL(strstr(generated.bytes, "xr_aot_host_realtime_nanos"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_CLOCK_PROVIDER);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }

    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    xr_vm_code_free(vm_code);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_provider_refusal_runs_nested_explicit_trap_cleanup) {
    static const char source[] =
        "import sys\n"
        "import time\n"
        "enum ClockFailure { Negative }\n"
        "fn rejectedFallible() -> i64 {\n"
        "  var inner = sys.Pipe(2147483642, 2147483643)\n"
        "  defer {\n"
        "    inner.closeRead()\n"
        "    inner.closeWrite()\n"
        "  }\n"
        "  var value = time.now()\n"
        "  if (value < 0) { throw ClockFailure.Negative }\n"
        "  return value\n"
        "}\n"
        "interface ClockReader { read() -> i64 }\n"
        "class RejectedClock implements ClockReader {\n"
        "  read() -> i64 { return rejectedFallible() }\n"
        "}\n"
        "fn invoke(body: fn() -> i64, reader: ClockReader, mode: i64) -> i64 {\n"
        "  var live = sys.Pipe(2147483646, 2147483647)\n"
        "  defer {\n"
        "    live.closeRead()\n"
        "    live.closeWrite()\n"
        "  }\n"
        "  var result = 0\n"
        "  if (mode == 1) {\n"
        "    try { result = body() }\n"
        "    catch (error: ClockFailure) { result = 0 }\n"
        "  } else if (mode == 2) {\n"
        "    try { result = reader.read() }\n"
        "    catch (error: ClockFailure) { result = 0 }\n"
        "  } else {\n"
        "    try { result = rejectedFallible() }\n"
        "    catch (error: ClockFailure) { result = 0 }\n"
        "  }\n"
        "  return result\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var reader: ClockReader = RejectedClock()\n"
        "  return invoke(rejectedFallible, reader, 1)\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_PROVIDER_CALL), 3u);
    ASSERT_EQ_UINT(
        program_operation_count(first.program, XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ), 1u);
    ASSERT_EQ_UINT(
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE, 4u),
        1u);
    ASSERT_EQ_UINT(
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_WITNESS_INVOKE, 4u),
        1u);
    ASSERT_EQ_UINT(
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_SEALED_INVOKE, 4u),
        1u);
    /* The three invokes and the clock wrapper each add a panic cleanup
     * frontier whose provider refusal must retain an independent trap exit. */
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_TRAP), 7u + 4u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 2u);

    XrStableId expected_clock_contract = {{0}};
    XrStableId expected_clock_operation = {{0}};
    XrStableId expected_io_contract = {{0}};
    XrStableId expected_close_operation = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_CLOCK_CONTRACT_KEY, &expected_clock_contract,
                                      &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY,
                                      &expected_clock_operation, &key_digest));
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &expected_io_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY,
                                      &expected_close_operation, &key_digest));

    ProviderTrapCleanupProbe probe = {
        .close_handles = {INT64_C(2147483642), INT64_C(2147483643), INT64_C(2147483646),
                          INT64_C(2147483647)},
    };
    XrProviderOperationBinding operations[2] = {0};
    XrProviderBinding providers[2] = {0};
    uint32_t clock_requirement = UINT32_MAX;
    uint32_t io_requirement = UINT32_MAX;
    for (uint32_t requirement_index = 0u; requirement_index < 2u; ++requirement_index) {
        XrProgramProviderRequirementView requirement = {0};
        ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, requirement_index,
                                                              &requirement));
        ASSERT_EQ_UINT(requirement.operation_count, 1u);
        const XrTargetProviderContract *contract =
            find_profile_provider(profile, requirement.contract_id);
        ASSERT_NOT_NULL(contract);
        operations[requirement_index].operation_id = requirement.operations[0].operation_id;
        operations[requirement_index].context = &probe;
        if (stable_id_equal(requirement.contract_id, expected_clock_contract)) {
            ASSERT_TRUE(
                stable_id_equal(requirement.operations[0].operation_id, expected_clock_operation));
            clock_requirement = requirement_index;
            operations[requirement_index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[requirement_index].entry.i64_nullary = provider_trap_clock_probe;
        } else {
            ASSERT_TRUE(stable_id_equal(requirement.contract_id, expected_io_contract));
            ASSERT_TRUE(
                stable_id_equal(requirement.operations[0].operation_id, expected_close_operation));
            io_requirement = requirement_index;
            operations[requirement_index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY;
            operations[requirement_index].entry.bool_i64_unary = provider_trap_close_probe;
        }
        providers[requirement_index].contract_id = requirement.contract_id;
        providers[requirement_index].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        providers[requirement_index].operations = &operations[requirement_index];
        providers[requirement_index].operation_count = 1u;
        ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(
                          contract, &providers[requirement_index].contract_fingerprint),
                      XR_RUNTIME_ABI_OK);
    }
    ASSERT_TRUE(clock_requirement != UINT32_MAX);
    ASSERT_TRUE(io_requirement != UINT32_MAX);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = providers,
        .provider_count = 2u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(first.program);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(
            xr_vm_code_build(binding.program, binding.profile, &options, &code, &diagnostic),
            XR_VM_CODE_OK);
        probe.events = 0u;
        probe.clock_calls = 0u;
        probe.close_calls = 0u;
        XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_TRAP);
        ASSERT_EQ_INT(outcome.trap, XR_VM_TRAP_PROVIDER_CALL_FAILED);
        ASSERT_EQ_UINT(probe.events, 5u);
        ASSERT_EQ_UINT(probe.clock_calls, 1u);
        ASSERT_EQ_UINT(probe.close_calls, 4u);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
    }
    assert_aot_provider_trap_cleanup(
        first.program, profile, entry, clock_requirement, io_requirement,
        source_fixture_output_path(XR_SOURCE_FIXTURE_PROVIDER_TRAP_CLEANUP));

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_child_coroutine_provider_failure_composes_static_cleanup) {
    static const char source[] = "import sys\n"
                                 "import time\n"
                                 "fn child() -> i64 {\n"
                                 "  var spare = sys.Pipe(2147483638, 2147483639)\n"
                                 "  defer { spare.closeRead(); spare.closeWrite() }\n"
                                 "  var inner = sys.Pipe(2147483642, 2147483643)\n"
                                 "  defer { inner.closeWrite() }\n"
                                 "  defer { inner.closeRead() }\n"
                                 "  Coro.yield()\n"
                                 "  return time.now()\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var outer = sys.Pipe(2147483646, 2147483647)\n"
                                 "  defer { outer.closeWrite() }\n"
                                 "  defer { outer.closeRead() }\n"
                                 "  return child() + 1\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (!product.program) {
        xr_program_source_product_free(&product);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED),
                   1u);
    ASSERT_EQ_UINT(program_operation_successor_count(product.program,
                                                     XR_CORE_OP_CORE_COROUTINE_CALL_SEALED, 4u),
                   1u);
    ASSERT_GT(
        program_operation_successor_count(product.program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT, 1u),
        0u);
    ASSERT_GT(program_operation_count(product.program, XR_CORE_OP_CORE_PROVIDER_CALL), 0u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 2u);

    XrStableId clock_contract = {{0}};
    XrStableId clock_operation = {{0}};
    XrStableId io_contract = {{0}};
    XrStableId close_operation = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_CLOCK_CONTRACT_KEY, &clock_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY,
                                      &clock_operation, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &io_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY, &close_operation,
                                      &key_digest));
    ChildCleanupProbe probe = {0};
    XrProviderOperationBinding operations[2] = {0};
    XrProviderBinding providers[2] = {0};
    uint32_t clock_requirement = UINT32_MAX;
    uint32_t io_requirement = UINT32_MAX;
    for (uint32_t index = 0u; index < 2u; ++index) {
        XrProgramProviderRequirementView requirement = {0};
        ASSERT_TRUE(
            xr_validated_program_provider_requirement(product.program, index, &requirement));
        ASSERT_EQ_UINT(requirement.operation_count, 1u);
        const XrTargetProviderContract *contract =
            find_profile_provider(profile, requirement.contract_id);
        ASSERT_NOT_NULL(contract);
        operations[index].operation_id = requirement.operations[0].operation_id;
        operations[index].context = &probe;
        if (stable_id_equal(requirement.contract_id, clock_contract)) {
            ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, clock_operation));
            clock_requirement = index;
            operations[index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[index].entry.i64_nullary = child_cleanup_clock_probe;
        } else {
            ASSERT_TRUE(stable_id_equal(requirement.contract_id, io_contract));
            ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, close_operation));
            io_requirement = index;
            operations[index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY;
            operations[index].entry.bool_i64_unary = child_cleanup_close_probe;
        }
        providers[index].contract_id = requirement.contract_id;
        providers[index].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        providers[index].operations = &operations[index];
        providers[index].operation_count = 1u;
        ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(
                          contract, &providers[index].contract_fingerprint),
                      XR_RUNTIME_ABI_OK);
    }
    ASSERT_TRUE(clock_requirement != UINT32_MAX);
    ASSERT_TRUE(io_requirement != UINT32_MAX);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .providers = providers,
        .provider_count = 2u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    uint32_t entry = xr_validated_program_entry_function(product.program);
    child_cleanup_check_vm(binding.program, binding.profile, instance, entry, &probe);
    child_cleanup_check_vm(binding.program, binding.profile, instance, entry, &probe);
    assert_aot_child_cleanup(
        product.program, profile, clock_requirement, io_requirement,
        source_fixture_output_path(XR_SOURCE_FIXTURE_CHILD_COROUTINE_TRAP_CLEANUP));

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&product);

    source_build_fixture_free(&fixture);

    /* A refusal in the first item enters one disjoint segment per later
     * fallible call.  The
     * empty item is a real program point, and the final
     * item needs a trivial value that no
     * preceding item otherwise uses. */
    static const char segmented_cleanup[] =
        "import sys\n"
        "fn answer() -> i64 {\n"
        "  var marker = 41\n"
        "  var pipe = sys.Pipe(2147483642, 2147483643)\n"
        "  defer { marker = marker + 1 }\n"
        "  defer {}\n"
        "  defer { pipe.closeRead(); pipe.closeWrite(); pipe.closeRead() }\n"
        "  return 43\n"
        "}\n";
    ASSERT_TRUE(source_build_fixture_init(&fixture, segmented_cleanup, NULL));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_PROVIDER_CALL), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT),
                   5u);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_shared_branching_cleanup_projects_private_cancel_graph) {
    /* A trap-capable operation before suspension claims the static handler for
     * failure.
     * Cancellation therefore needs a reason-private copy of the
     * complete branching cleanup
     * graph, not a single cloned entry block. */
    static const char branching_cleanup[] =
        "import sys\n"
        "fn branch(flag: bool) -> i64 {\n"
        "  var pipe = sys.Pipe(2147483642, 2147483643)\n"
        "  defer {\n"
        "    if (flag) { pipe.closeRead() } else { pipe.closeWrite() }\n"
        "    pipe.closeRead()\n"
        "    pipe.closeWrite()\n"
        "  }\n"
        "  var primed = pipe.closeRead()\n"
        "  if (primed) {\n"
        "    Coro.yield()\n"
        "    return 43\n"
        "  }\n"
        "  return 42\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  return branch(true) + branch(false)\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, branching_cleanup, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_YIELD), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED),
                   2u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    XrStableId io_contract = {{0}};
    XrStableId close_operation = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &io_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY, &close_operation,
                                      &key_digest));
    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(product.program, 0u, &requirement));
    ASSERT_TRUE(stable_id_equal(requirement.contract_id, io_contract));
    ASSERT_EQ_UINT(requirement.operation_count, 1u);
    ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, close_operation));
    const XrTargetProviderContract *contract = find_profile_provider(profile, io_contract);
    ASSERT_NOT_NULL(contract);
    BranchingCleanupProbe probe = {0};
    XrProviderOperationBinding operation = {
        .operation_id = close_operation,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
        .entry.bool_i64_unary = branching_cleanup_close_probe,
    };
    XrProviderBinding provider = {
        .contract_id = io_contract,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    uint32_t entry = xr_validated_program_entry_function(product.program);
    branching_cleanup_check_vm(binding.program, binding.profile, instance, entry, &probe);
    branching_cleanup_check_vm(binding.program, binding.profile, instance, entry, &probe);
    assert_aot_branching_cleanup(product.program, profile,
                                 source_fixture_output_path(XR_SOURCE_FIXTURE_BRANCHING_CLEANUP));

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_nested_cleanup_projects_reason_chain) {
    static const char nested_cleanup[] =
        "import sys\n"
        "fn nested(flag: bool) -> i64 {\n"
        "  var outer = sys.Pipe(2147483642, 2147483643)\n"
        "  var inner = sys.Pipe(2147483638, 2147483639)\n"
        "  defer {\n"
        "    var choose = flag\n"
        "    defer {\n"
        "      if (choose) { inner.closeRead() } else { inner.closeWrite() }\n"
        "      inner.closeRead()\n"
        "      inner.closeWrite()\n"
        "    }\n"
        "    outer.closeRead()\n"
        "    outer.closeWrite()\n"
        "  }\n"
        "  var primed = outer.closeRead()\n"
        "  if (primed) {\n"
        "    Coro.yield()\n"
        "    return 42\n"
        "  }\n"
        "  return 41\n"
        "}\n"
        "fn answer() -> i64 { return nested(true) + nested(false) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, nested_cleanup, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_YIELD), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED),
                   2u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    XrStableId io_contract = {{0}};
    XrStableId close_operation = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &io_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY, &close_operation,
                                      &key_digest));
    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(product.program, 0u, &requirement));
    ASSERT_TRUE(stable_id_equal(requirement.contract_id, io_contract));
    ASSERT_EQ_UINT(requirement.operation_count, 1u);
    ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, close_operation));
    const XrTargetProviderContract *contract = find_profile_provider(profile, io_contract);
    ASSERT_NOT_NULL(contract);
    BranchingCleanupProbe probe = {0};
    XrProviderOperationBinding operation = {
        .operation_id = close_operation,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
        .entry.bool_i64_unary = branching_cleanup_close_probe,
    };
    XrProviderBinding provider = {
        .contract_id = io_contract,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    uint32_t entry = xr_validated_program_entry_function(product.program);
    nested_cleanup_check_vm(binding.program, binding.profile, instance, entry, &probe);
    nested_cleanup_check_vm(binding.program, binding.profile, instance, entry, &probe);
    assert_aot_nested_cleanup(product.program, profile,
                              source_fixture_output_path(XR_SOURCE_FIXTURE_NESTED_CLEANUP));

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_pipe_provider_and_fieldwise_constructor_are_canonical) {
    static const char source[] =
        "import sys\n"
        "fn guardedAnd(guard: bool, divisor: i64) -> bool {\n"
        "  return guard && (1 / divisor > 0)\n"
        "}\n"
        "fn guardedOr(guard: bool, divisor: i64) -> bool {\n"
        "  return guard || (1 / divisor > 0)\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  var opened = sys.Pipe.open()\n"
        "  if (opened == null) { return 0 }\n"
        "  var ready = opened!\n"
        "  var live = move ready\n"
        "  var readHandle = live.readEnd()\n"
        "  var writeHandle = live.writeEnd()\n"
        "  var readClosed = live.closeRead()\n"
        "  var closedReadHandle = live.readEnd()\n"
        "  var writeClosed = live.closeWrite()\n"
        "  var closedWriteHandle = live.writeEnd()\n"
        "  var closed = (move live).close()\n"
        "  if (guardedAnd(false, 0)) { return 0 }\n"
        "  if (!guardedOr(true, 0)) { return 0 }\n"
        "  var handlesValid = readHandle >= 0 && writeHandle >= 0\n"
        "  if (!handlesValid || closedReadHandle != -1 || closedWriteHandle != -1) { return 0 }\n"
        "  if (!readClosed || !writeClosed || !closed) { return 0 }\n"
        "  return 1\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    assert_generated_provider_execution(first.program, profile);
    /* The root imports sys, which imports time: all three initializers join
     * the original nine reachable ordinary functions. */
    ASSERT_EQ_UINT(first.program->module_count, 3u);
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 12u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 1u);

    uint32_t aggregate_constructs = 0u;
    uint32_t aggregate_projects = 0u;
    uint32_t place_projects = 0u;
    uint32_t place_loads = 0u;
    uint32_t place_stores = 0u;
    uint32_t variant_projects = 0u;
    uint32_t sealed_calls = 0u;
    uint32_t class_constructs = 0u;
    uint32_t class_field_loads = 0u;
    uint32_t class_field_places = 0u;
    uint32_t place_exchanges = 0u;
    uint32_t logical_nots = 0u;
    uint32_t logical_ands = 0u;
    uint32_t logical_ors = 0u;
    for (uint32_t function_index = 0u; function_index < first.program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &first.program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                aggregate_constructs += block->instructions[instruction_index].operation_id ==
                                        XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT;
                aggregate_projects += block->instructions[instruction_index].operation_id ==
                                      XR_CORE_OP_CORE_AGGREGATE_PROJECT;
                place_projects += block->instructions[instruction_index].operation_id ==
                                  XR_CORE_OP_CORE_PLACE_PROJECT;
                place_loads += block->instructions[instruction_index].operation_id ==
                               XR_CORE_OP_CORE_PLACE_LOAD;
                place_stores += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_PLACE_STORE;
                variant_projects += block->instructions[instruction_index].operation_id ==
                                    XR_CORE_OP_CORE_VARIANT_PROJECT;
                sealed_calls += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_CALL_SEALED_DIRECT;
                class_constructs += block->instructions[instruction_index].operation_id ==
                                    XR_CORE_OP_CORE_CLASS_CONSTRUCT;
                class_field_loads += block->instructions[instruction_index].operation_id ==
                                     XR_CORE_OP_CORE_CLASS_FIELD_LOAD;
                class_field_places += block->instructions[instruction_index].operation_id ==
                                      XR_CORE_OP_CORE_CLASS_FIELD_PLACE;
                place_exchanges += block->instructions[instruction_index].operation_id ==
                                   XR_CORE_OP_CORE_PLACE_EXCHANGE;
                logical_nots += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_LOGICAL_NOT;
                logical_ands += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_LOGICAL_AND;
                logical_ors += block->instructions[instruction_index].operation_id ==
                               XR_CORE_OP_CORE_LOGICAL_OR;
            }
        }
    }
    ASSERT_EQ_UINT(aggregate_constructs, 0u);
    ASSERT_EQ_UINT(aggregate_projects, 2u);
    ASSERT_EQ_UINT(place_projects, 0u);
    ASSERT_EQ_UINT(place_loads, 0u);
    ASSERT_EQ_UINT(place_stores, 0u);
    ASSERT_EQ_UINT(variant_projects, 3u);
    ASSERT_EQ_UINT(sealed_calls, 8u);
    ASSERT_EQ_UINT(
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_SEALED_INVOKE, 2u),
        2u);
    ASSERT_EQ_UINT(class_constructs, 1u);
    ASSERT_EQ_UINT(class_field_loads, 10u);
    ASSERT_EQ_UINT(class_field_places, 2u);
    ASSERT_EQ_UINT(place_exchanges, 2u);
    ASSERT_TRUE(logical_nots != 0u);
    ASSERT_TRUE(logical_ands != 0u);
    ASSERT_TRUE(logical_ors != 0u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, 0u, &requirement));
    ASSERT_EQ_UINT(requirement.operation_count, 2u);
    XrStableId expected_contract = {{0}};
    XrStableId expected_open = {{0}};
    XrStableId expected_close = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &expected_contract, &key_digest));
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_OPEN_OPERATION_KEY, &expected_open, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY, &expected_close,
                                      &key_digest));
    ASSERT_TRUE(stable_id_equal(requirement.contract_id, expected_contract));

    uint32_t open_index = UINT32_MAX;
    uint32_t close_index = UINT32_MAX;
    for (uint32_t operation_index = 0u; operation_index < requirement.operation_count;
         ++operation_index) {
        if (stable_id_equal(requirement.operations[operation_index].operation_id, expected_open))
            open_index = operation_index;
        else if (stable_id_equal(requirement.operations[operation_index].operation_id,
                                 expected_close))
            close_index = operation_index;
    }
    ASSERT_TRUE(open_index != UINT32_MAX);
    ASSERT_TRUE(close_index != UINT32_MAX);

    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    const XrTargetProviderOperationContract *open_contract =
        find_profile_provider_operation(contract, expected_open);
    const XrTargetProviderOperationContract *close_contract =
        find_profile_provider_operation(contract, expected_close);
    ASSERT_NOT_NULL(open_contract);
    ASSERT_NOT_NULL(close_contract);
    ASSERT_EQ_UINT(open_contract->call_abi.parameter_count, 3u);
    ASSERT_EQ_INT(open_contract->call_abi.result.value_kind,
                  XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER);
    ASSERT_EQ_UINT(close_contract->call_abi.parameter_count, 1u);
    ASSERT_EQ_INT(close_contract->call_abi.parameters[0].value_kind,
                  XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
    ASSERT_EQ_INT(close_contract->call_abi.parameters[0].ownership,
                  XR_TARGET_PROVIDER_CALL_OWNERSHIP_CONSUMED);
    ASSERT_EQ_INT(close_contract->call_abi.result.value_kind,
                  XR_TARGET_PROVIDER_CALL_VALUE_UNSIGNED_INTEGER);
    ASSERT_EQ_UINT(close_contract->call_abi.result.width, 1u);
    ASSERT_EQ_INT(close_contract->call_abi.result.ownership,
                  XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE);
    ASSERT_EQ_UINT(close_contract->effect_flags, XR_TARGET_PROVIDER_EFFECT_IO);
    ASSERT_EQ_UINT(close_contract->lifetime_flags, XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED);
    ASSERT_EQ_UINT(close_contract->failure_flags, 0u);

    PipeProviderProbe probe = {
        .present = true,
        .read_handle = 17,
        .write_handle = 29,
        .close_results = {true, true},
    };
    XrProviderOperationBinding operations[2] = {0};
    for (uint32_t operation_index = 0u; operation_index < requirement.operation_count;
         ++operation_index) {
        operations[operation_index].operation_id =
            requirement.operations[operation_index].operation_id;
        operations[operation_index].context = &probe;
        if (operation_index == open_index) {
            operations[operation_index].trampoline_kind =
                XR_PROVIDER_TRAMPOLINE_OPTIONAL_I64_PAIR_NULLARY;
            operations[operation_index].entry.optional_i64_pair_nullary = pipe_provider_probe;
        } else {
            ASSERT_EQ_UINT(operation_index, close_index);
            operations[operation_index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY;
            operations[operation_index].entry.bool_i64_unary = pipe_close_provider_probe;
        }
    }
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = operations,
        .operation_count = 2u,
    };
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(first.program);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(
            xr_vm_code_build(binding.program, binding.profile, &options, &code, &diagnostic),
            XR_VM_CODE_OK);
        XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, 1);
        ASSERT_EQ_UINT(probe.close_calls, (0u + 1u) * 2u);
        ASSERT_EQ_UINT(probe.open_calls, 0u + 1u);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
    }
    assert_aot_fixture_backend_contract(first.program, profile, XR_SOURCE_FIXTURE_PIPE_PROVIDER);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_pipe_failed_close_consumes_endpoints_once) {
    static const char source[] =
        "import sys\n"
        "enum CleanupFailure { Negative { code: i64 } }\n"
        "fn fail(value: i64) -> i64 {\n"
        "  if (value < 0) { throw CleanupFailure.Negative { code: value } }\n"
        "  return value\n"
        "}\n"
        "fn doomed() -> i64 {\n"
        "  var live = sys.Pipe(2147483646, 2147483647)\n"
        "  defer {\n"
        "    live.closeRead()\n"
        "    live.closeWrite()\n"
        "  }\n"
        "  return fail(-2)\n"
        "}\n"
        "fn answer() -> i64 {\n"
        "  try { return doomed() } catch (error) { return 1 }\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_CALL_SEALED_INVOKE), 2u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_ERROR_PUBLISH), 2u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 1u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, 0u, &requirement));
    ASSERT_EQ_UINT(requirement.operation_count, 1u);
    XrStableId expected_contract = {{0}};
    XrStableId expected_close = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &expected_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY, &expected_close,
                                      &key_digest));
    ASSERT_TRUE(stable_id_equal(requirement.contract_id, expected_contract));
    ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, expected_close));

    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    const XrTargetProviderOperationContract *close_contract =
        find_profile_provider_operation(contract, expected_close);
    ASSERT_NOT_NULL(close_contract);
    ASSERT_EQ_UINT(close_contract->failure_flags, 0u);
    ASSERT_EQ_UINT(close_contract->lifetime_flags, XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED);

    PipeProviderProbe probe = {
        .read_handle = 2147483646,
        .write_handle = 2147483647,
        .close_results = {false, false},
    };
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
    };
    operation.entry.bool_i64_unary = pipe_close_provider_probe;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(first.program);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(
            xr_vm_code_build(binding.program, binding.profile, &options, &code, &diagnostic),
            XR_VM_CODE_OK);
        XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, 1);
        ASSERT_EQ_UINT(probe.close_calls, (0u + 1u) * 2u);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
    }
    assert_aot_fixture_backend_contract(first.program, profile,
                                        XR_SOURCE_FIXTURE_PIPE_CLOSE_FAILURE);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_pipe_uncaught_error_runs_cleanup) {
    static const char source[] =
        "import sys\n"
        "enum CleanupFailure { Negative { code: i64 } }\n"
        "fn fail(value: i64) -> i64 {\n"
        "  if (value < 0) { throw CleanupFailure.Negative { code: value } }\n"
        "  return value\n"
        "}\n"
        "fn doomed() -> i64 {\n"
        "  var live = sys.Pipe(2147483646, 2147483647)\n"
        "  defer {\n"
        "    live.closeRead()\n"
        "    live.closeWrite()\n"
        "  }\n"
        "  return fail(-2)\n"
        "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    fixture.input.entry.function_name = "doomed";
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_CALL_SEALED_INVOKE), 1u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_ERROR_PUBLISH), 2u);
    uint32_t entry = xr_validated_program_entry_function(first.program);
    ASSERT_LT(entry, first.program->function_count);
    const XrValidatedFunction *entry_function = &first.program->functions[entry];
    ASSERT_TRUE(entry_function->error_type_id != XR_CORE_TYPE_VOID);
    const XrValidatedType *error_type =
        xr_validated_program_type(first.program, entry_function->error_type_id);
    ASSERT_NOT_NULL(error_type);
    ASSERT_EQ_INT(error_type->kind, XR_CORE_IR_TYPE_VARIANT);
    ASSERT_EQ_UINT(error_type->variant_count, 1u);
    ASSERT_EQ_UINT(error_type->variants[0].payload_count, 1u);
    ASSERT_EQ_UINT(error_type->variants[0].payload_types[0], XR_CORE_TYPE_I64);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, 0u, &requirement));
    PipeProviderProbe probe = {
        .read_handle = 2147483646,
        .write_handle = 2147483647,
        .close_results = {false, false},
    };
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
    };
    operation.entry.bool_i64_unary = pipe_close_provider_probe;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    assert_uncaught_vm_error(binding.program, binding.profile, instance, entry,
                             entry_function->error_type_id, &probe);
    write_uncaught_error_aot(first.program, profile, entry, entry_function->error_type_id,
                             source_fixture_output_path(XR_SOURCE_FIXTURE_PIPE_UNCAUGHT_ERROR));

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

static void assert_builtin_panic_cleans_live_owners(uint32_t panic_code,
                                                    XrSourceFixtureId fixture_id, bool deferred,
                                                    uint32_t dispatch, bool suspends) {
    bool message = fixture_id == XR_SOURCE_FIXTURE_ASSERTION_MESSAGE_CLEANUP;
    bool fallible = dispatch >= 5u;
    if (fallible)
        dispatch -= 4u;
    const char *body =
        panic_code == 420u   ? "var divisor: u8 = 0\nif (ok) { divisor = 2 }\n"
                               "var result: u8 = 84 / divisor\n"
                               "return first.value + second.value + (result as i64) - 42\n"
        : panic_code == 421u ? "var divisor: u8 = 0\nif (ok) { divisor = 2 }\n"
                               "var result: u8 = 85 % divisor\n"
                               "return first.value + second.value + (result as i64) - 1\n"
                             : "assert(ok)\nreturn first.value + second.value\n";
    if (panic_code == 452u)
        body = "var size: i64 = -1\nif (ok) { size = 2 }\n"
               "var values = Array<u8>(size)\n"
               "assert(len(values) == 2 && values[1] == 0)\n"
               "return first.value + second.value\n";
    if (panic_code == 430u)
        body = "var end: i64 = 4\nif (ok) { end = 2 }\n"
               "const source = \"abc\"\nconst result = source.slice(0, end)\n"
               "assert(result == \"ab\")\nreturn first.value + second.value\n";
    if (message)
        body = "var message = \"assert \" + \"message\"\n"
               "assert(ok, message)\nreturn first.value + second.value\n";
    const char *call_body = dispatch == 4u   ? "var result = quotient(3, divisor)\n"
                            : dispatch == 1u ? "var result = quotient(divisor)\n"
                            : dispatch == 2u ? "var selected = quotient\n"
                                               "var result = selected(divisor)\n"
                                             : "var selected: Calculator = Divider{value: 84}\n"
                                               "var result = selected.calculate(divisor)\n";
    char source[4096];
    char indirect_body[1024];
    if (dispatch != 0u) {
        int size = snprintf(indirect_body, sizeof(indirect_body),
                            "var divisor: i64 = 0\nif (ok) { divisor = 2 }\n%s"
                            "return first.value + second.value + result - 42\n",
                            call_body);
        ASSERT_TRUE(size > 0 && (size_t) size < sizeof(indirect_body));
        body = indirect_body;
    }
    if (fallible) {
        const char *setup = dispatch == 2u   ? "var selected = quotient\n"
                            : dispatch == 3u ? "var selected: Calculator = Divider{value: 84}\n"
                                             : "";
        const char *callee = dispatch == 1u   ? "quotient"
                             : dispatch == 2u ? "selected"
                                              : "selected.calculate";
        int size = snprintf(indirect_body, sizeof(indirect_body),
                            "var divisor: i64 = 0\nif (ok) { divisor = 2 }\n%s"
                            "var result = 0\ntry { result = %s(divisor) }\n"
                            "catch (error: DivisionFailure) { result = 7 }\n"
                            "var caughtResult = 0\ntry { caughtResult = %s(-1) }\n"
                            "catch (error: DivisionFailure) { caughtResult = 7 }\n"
                            "assert(caughtResult == 7)\n"
                            "return first.value + second.value + result - 42\n",
                            setup, callee, callee);
        ASSERT_TRUE(size > 0 && (size_t) size < sizeof(indirect_body));
        body = indirect_body;
    }
    int written = snprintf(
        source, sizeof(source),
        "%s%sclass Cell {\nvalue: i64\n"
        "constructor(value: i64) { this.value = value }\n}\n"
        "fn checked(ok: bool) -> i64 {\n"
        "var first = Cell(20)\nvar second = Cell(22)\n%s%s}\n"
        "fn answer() -> i64 { return checked(false)%s }\n",
        fallible && panic_code == 452u ? "enum DivisionFailure { Negative }\n"
                                     "fn quotient(divisor: i64) -> i64 {\n"
                                     "if (divisor < 0) { throw DivisionFailure.Negative }\n"
                                     "const label = \"own\"\n"
                                     "var values = Array<u8>(divisor - 1)\n"
                                     "return len(values) + len(label) + 38\n}\n"
        : fallible && dispatch == 3u ? "enum DivisionFailure { Negative }\n"
                                     "interface Calculator { calculate(divisor: i64) -> i64 }\n"
                                     "struct Divider implements Calculator {\nvalue: i64\n"
                                     "calculate(divisor: i64) -> i64 {\n"
                                     "if (divisor < 0) { throw DivisionFailure.Negative }\n"
                                     "return this.value / divisor\n}\n}\n"
        : fallible && suspends     ? "enum DivisionFailure { Negative }\n"
                                     "fn quotient(divisor: i64) -> i64 {\n"
                                     "Coro.yield()\n"
                                     "if (divisor < 0) { throw DivisionFailure.Negative }\n"
                                     "return 84 / divisor\n}\n"
        : fallible                 ? "enum DivisionFailure { Negative }\n"
                                     "fn quotient(divisor: i64) -> i64 {\n"
                                     "if (divisor < 0) { throw DivisionFailure.Negative }\n"
                                     "return 84 / divisor\n}\n"
        : dispatch == 4u           ? "fn quotient(depth: i64, divisor: i64) -> i64 {\n"
                                     "if (depth == 0) { return 84 / divisor }\n"
                                     "return relay(depth - 1, divisor)\n}\n"
                                     "fn relay(depth: i64, divisor: i64) -> i64 {\n"
                                     "return quotient(depth, divisor)\n}\n"
        : dispatch == 3u           ? "interface Calculator { calculate(divisor: i64) -> i64 }\n"
                                     "struct Divider implements Calculator {\nvalue: i64\n"
                                     "calculate(divisor: i64) -> i64 { return this.value / divisor }\n}\n"
        : suspends                 ? "fn quotient(divisor: i64) -> i64 {\n"
                                     "Coro.yield()\nreturn 84 / divisor\n}\n"
        : dispatch != 0u           ? "fn quotient(divisor: i64) -> i64 { return 84 / divisor }\n"
                                   : "",
        deferred ? "var cleanupCount: i64 = 0\n"
                   "fn readCleanupCount() -> i64 { return cleanupCount }\n"
                 : "",
        deferred ? "defer { if (ok) { cleanupCount = 11 } "
                   "else { cleanupCount = 22 } }\n"
                 : "",
        body, deferred ? " + readCleanupCount()" : "");
    ASSERT_TRUE(written > 0 && (size_t) written < sizeof(source));
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(
        program_operation_successor_count(product.program,
                                          panic_code == XR_ASSERTION_FAILURE_CONDITION_FALSE
                                              ? XR_CORE_OP_CORE_ASSERT_CONDITION
                                              : panic_code == 452u ? XR_CORE_OP_CORE_ARRAY_ALLOCATE_DEFAULT
                                              : panic_code == 430u ? XR_CORE_OP_CORE_STRING_SLICE
                                              : XR_CORE_OP_CORE_INTEGER_DIVMOD,
                                          1u),
        1u);
    if (suspends) {
        ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_COROUTINE_YIELD),
                       1u);
        ASSERT_EQ_UINT(program_operation_successor_count(
                           product.program,
                           dispatch == 1u ? XR_CORE_OP_CORE_COROUTINE_CALL_SEALED
                                          : XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT,
                           fallible ? 5u : 4u),
                       fallible ? 2u : 1u);
    }
    if (dispatch != 0u && !suspends)
        ASSERT_TRUE(program_operation_count(
                        product.program,
                        dispatch == 1u || dispatch == 4u ? XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                        : dispatch == 2u                 ? XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE
                                         : XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) != 0u);
    if (dispatch != 0u && !fallible && !suspends)
        ASSERT_EQ_UINT(program_operation_successor_count(
                           product.program,
                           dispatch == 1u || dispatch == 4u ? XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                           : dispatch == 2u                 ? XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE
                                                            : XR_CORE_OP_CORE_CALL_WITNESS_INVOKE,
                           deferred ? 3u : 2u),
                       dispatch == 4u                ? 4u
                       : dispatch == 1u && !deferred ? 2u
                                                     : 1u);
    if (fallible && !suspends)
        ASSERT_EQ_UINT(program_operation_successor_count(
                           product.program,
                           dispatch == 1u   ? XR_CORE_OP_CORE_CALL_SEALED_INVOKE
                           : dispatch == 2u ? XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE
                                            : XR_CORE_OP_CORE_CALL_WITNESS_INVOKE,
                           deferred ? 4u : 3u),
                       2u);
    uint32_t checked = UINT32_MAX;
    uint32_t read_cleanup = UINT32_MAX;
    for (uint32_t function = 0u; function < product.program->function_count; ++function)
        if (product.program->functions[function].parameter_count == 1u &&
            product.program->functions[function].parameter_types[0] == XR_CORE_TYPE_BOOL)
            checked = function;
    if (deferred) {
        for (uint32_t function = 0u; function < product.program->function_count; ++function) {
            const XrValidatedFunction *candidate = &product.program->functions[function];
            if (candidate->parameter_count == 0u && candidate->result_type_id == XR_CORE_TYPE_I64 &&
                candidate->panic_type_id == XR_CORE_TYPE_VOID)
                read_cleanup = function;
        }
        ASSERT_TRUE(read_cleanup != UINT32_MAX);
    }
    ASSERT_TRUE(checked != UINT32_MAX);
    {
        SourceClassLifecycleLog log = {0};
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = &log;
        options.lifecycle_event = record_source_class_lifecycle;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(product.program, profile, &options, &code, NULL),
                      XR_VM_CODE_OK);
        for (uint32_t succeeds = 0u; succeeds < 2u; ++succeeds) {
            log = (SourceClassLifecycleLog) {0};
            XrExecutionBindingInput binding = {
                .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
                .program = product.program,
                .profile = profile,
                .generation = 1u,
            };
            XrInstance *instance = NULL;
            ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL), XR_EXECUTION_OK);
            XrVmValue argument = {.kind = XR_VM_VALUE_BOOL, .as.boolean = succeeds != 0u};
            XrVmExecution *execution = NULL;
            XrVmOutcome result = {0};
            if (suspends) {
                ASSERT_TRUE(
                    xr_vm_execution_create(code, instance, checked, &argument, 1u, &execution));
                result = xr_vm_execution_step(execution);
                ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_SUSPENDED);
                ASSERT_EQ_UINT(result.safepoint_id, 0u);
                ASSERT_EQ_UINT(result.state_id, 1u);
                for (uint32_t event = 0u; event < log.count; ++event) {
                    ASSERT_TRUE(log.events[event].kind != XR_VM_EVENT_CLASS_FINALIZE);
                    ASSERT_TRUE(log.events[event].kind != XR_VM_EVENT_CLASS_RECLAIM);
                }
                xr_vm_outcome_dispose(&result);
                result = xr_vm_execution_step(execution);
                if (fallible && succeeds) {
                    ASSERT_EQ_INT(result.kind, XR_VM_OUTCOME_SUSPENDED);
                    ASSERT_EQ_UINT(result.state_id, 2u);
                    for (uint32_t event = 0u; event < log.count; ++event) {
                        ASSERT_TRUE(log.events[event].kind != XR_VM_EVENT_CLASS_FINALIZE);
                        ASSERT_TRUE(log.events[event].kind != XR_VM_EVENT_CLASS_RECLAIM);
                    }
                    xr_vm_outcome_dispose(&result);
                    result = xr_vm_execution_step(execution);
                }
            } else {
                result = xr_vm_code_execute(code, instance, checked, &argument, 1u);
            }
            ASSERT_EQ_INT(result.kind, succeeds ? XR_VM_OUTCOME_RETURN : XR_VM_OUTCOME_PANIC);
            if (succeeds) {
                ASSERT_EQ_INT(result.value.kind, XR_VM_VALUE_I64);
                ASSERT_EQ_INT(result.value.as.i64, 42);
            } else {
                ASSERT_EQ_INT(result.panic_value.kind, XR_VM_VALUE_PANIC_INFO);
                ASSERT_EQ_UINT(result.panic_value.as.panic_info.code, panic_code);
            }
            if (!succeeds && message) {
                XrVmStringView view = {0};
                ASSERT_TRUE(xr_vm_panic_message_view(&result.panic_value.as.panic_info, &view));
                ASSERT_EQ_UINT(view.size, 14u);
                ASSERT_EQ_INT(memcmp(view.bytes, "assert message", 14u), 0);
            }
            uint64_t created[2] = {0};
            uint32_t creates = 0u, finalized = 0u;
            ASSERT_FALSE(log.overflow);
            for (uint32_t event = 0u; event < log.count; ++event) {
                const XrVmLifecycleEvent *observed = &log.events[event];
                if (observed->kind == XR_VM_EVENT_CLASS_CONSTRUCT) {
                    ASSERT_LT(creates, 2u);
                    created[creates++] = observed->identity;
                } else if (observed->kind == XR_VM_EVENT_CLASS_FINALIZE) {
                    ASSERT_EQ_UINT(creates, 2u);
                    ASSERT_LT(finalized, 2u);
                    if (!succeeds)
                        ASSERT_EQ_UINT(observed->identity, created[1u - finalized]);
                    ++finalized;
                }
            }
            ASSERT_EQ_UINT(creates, 2u);
            ASSERT_EQ_UINT(finalized, 2u);
            for (uint32_t object = 0u; object < 2u; ++object) {
                ASSERT_EQ_UINT(
                    source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_FINALIZE, created[object]),
                    1u);
                ASSERT_EQ_UINT(
                    source_class_lifecycle_count(&log, XR_VM_EVENT_CLASS_RECLAIM, created[object]),
                    1u);
            }
            xr_vm_outcome_dispose(&result);
            xr_vm_execution_free(execution);
            if (deferred) {
                XrVmOutcome observed = xr_vm_code_execute(code, instance, read_cleanup, NULL, 0u);
                ASSERT_EQ_INT(observed.kind, XR_VM_OUTCOME_RETURN);
                ASSERT_EQ_INT(observed.value.kind, XR_VM_VALUE_I64);
                ASSERT_EQ_INT(observed.value.as.i64, succeeds ? 11 : 22);
                xr_vm_outcome_dispose(&observed);
            }
            uint32_t terminal_count = log.count;
            ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
            ASSERT_EQ_UINT(log.count, terminal_count);
        }
        xr_vm_code_free(code);
    }
    write_builtin_panic_cleanup_aot(product.program, profile, checked, read_cleanup, panic_code,
                                    message, source_fixture_output_path(fixture_id));
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_assertion_message_survives_defer_and_owner_cleanup) {
    assert_builtin_panic_cleans_live_owners(XR_ASSERTION_FAILURE_CONDITION_FALSE,
                                            XR_SOURCE_FIXTURE_ASSERTION_MESSAGE_CLEANUP, true, 0u,
                                            false);
}

TEST(source_owner_assertion_cleans_live_owners_before_panic) {
    assert_builtin_panic_cleans_live_owners(XR_ASSERTION_FAILURE_CONDITION_FALSE,
                                            XR_SOURCE_FIXTURE_ASSERTION_OWNER_CLEANUP, false, 0u,
                                            false);
}

TEST(source_owner_array_invoke_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(452u, XR_SOURCE_FIXTURE_ARRAY_INVOKE_DEFER,
                                            true, 5u, false);
}

TEST(source_owner_array_default_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(452u, XR_SOURCE_FIXTURE_ARRAY_DEFAULT_DEFER,
                                            true, 0u, false);
}

TEST(source_owner_string_slice_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(430u, XR_SOURCE_FIXTURE_STRING_SLICE_DEFER,
                                            true, 0u, false);
}

TEST(source_owner_integer_division_zero_cleans_live_owners) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INTEGER_DIVISION_ZERO, false,
                                            0u, false);
}

TEST(source_owner_integer_remainder_zero_cleans_live_owners) {
    assert_builtin_panic_cleans_live_owners(421u, XR_SOURCE_FIXTURE_INTEGER_REMAINDER_ZERO, false,
                                            0u, false);
}

TEST(source_owner_integer_division_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INTEGER_DIVISION_DEFER, true,
                                            0u, false);
}

TEST(source_owner_assertion_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(1u, XR_SOURCE_FIXTURE_ASSERTION_DEFER, true, 0u, false);
}

TEST(source_owner_sealed_call_propagates_panic_and_cleans_owners) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_SEALED_CALL_PANIC, false, 1u,
                                            false);
}

TEST(source_owner_indirect_call_propagates_panic_and_cleans_owners) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INDIRECT_CALL_PANIC, false, 2u,
                                            false);
}

TEST(source_owner_witness_call_propagates_panic_and_cleans_owners) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_WITNESS_CALL_PANIC, false, 3u,
                                            false);
}

TEST(source_owner_recursive_calls_share_closed_panic_contract) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_RECURSIVE_CALL_PANIC, false, 4u,
                                            false);
}

TEST(source_owner_sealed_invoke_separates_error_and_panic) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_SEALED_INVOKE_PANIC, false, 5u,
                                            false);
}

TEST(source_owner_indirect_invoke_separates_error_and_panic) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INDIRECT_INVOKE_PANIC, false,
                                            6u, false);
}

TEST(source_owner_witness_invoke_separates_error_and_panic) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_WITNESS_INVOKE_PANIC, false, 7u,
                                            false);
}

TEST(source_owner_sealed_invoke_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_SEALED_INVOKE_DEFER, true, 5u,
                                            false);
}

TEST(source_owner_indirect_invoke_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INDIRECT_INVOKE_DEFER, true, 6u,
                                            false);
}

TEST(source_owner_witness_invoke_runs_conditional_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_WITNESS_INVOKE_DEFER, true, 7u,
                                            false);
}

TEST(source_owner_sealed_panic_only_call_runs_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_SEALED_PANIC_ONLY_DEFER, true,
                                            1u, false);
}

TEST(source_owner_indirect_panic_only_call_runs_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INDIRECT_PANIC_ONLY_DEFER, true,
                                            2u, false);
}

TEST(source_owner_witness_panic_only_call_runs_defer) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_WITNESS_PANIC_ONLY_DEFER, true,
                                            3u, false);
}

TEST(source_owner_sealed_coroutine_invoke_runs_defer_after_suspension) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_SEALED_COROUTINE_INVOKE, true,
                                            5u, true);
}

TEST(source_owner_indirect_coroutine_invoke_runs_defer_after_suspension) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INDIRECT_COROUTINE_INVOKE, true,
                                            6u, true);
}

TEST(source_owner_sealed_coroutine_panic_runs_defer_after_suspension) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_SEALED_COROUTINE_PANIC, true,
                                            1u, true);
}

TEST(source_owner_indirect_coroutine_panic_runs_defer_after_suspension) {
    assert_builtin_panic_cleans_live_owners(420u, XR_SOURCE_FIXTURE_INDIRECT_COROUTINE_PANIC, true,
                                            2u, true);
}

TEST(source_owner_lowers_defer_panic_cleanup_across_private_executors) {
    static const char source[] = "import sys\n"
                                 "fn doomed(divisor: i64) -> i64 {\n"
                                 "  var live = sys.Pipe(2147483646, 2147483647)\n"
                                 "  defer {\n"
                                 "    live.closeRead()\n"
                                 "    live.closeWrite()\n"
                                 "  }\n"
                                 "  assert(divisor != 0)\n"
                                 "  return 1\n"
                                 "}\n"
                                 "fn answer() -> i64 { return doomed(0) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(
        program_operation_successor_count(product.program, XR_CORE_OP_CORE_ASSERT_CONDITION, 1u),
        1u);
    ASSERT_GT(program_operation_count(product.program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT), 0u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(product.program, 0u, &requirement));
    static const int64_t expected_handles[] = {INT64_C(2147483646), INT64_C(2147483647)};
    PipeCloseSequenceProbe probe;
    pipe_close_sequence_reset(&probe, expected_handles, 2u);
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
    };
    operation.entry.bool_i64_unary = pipe_close_sequence_probe;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(product.program);

    pipe_close_sequence_reset(&probe, expected_handles, 2u);
    assert_vm_panic_cleanup(binding.program, binding.profile, instance, entry, &probe);
    assert_aot_panic_cleanup(product.program, profile, entry,
                             XR_SOURCE_FIXTURE_PANIC_DEFER_CLEANUP);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&product);

    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_recovers_and_reconstructs_place_backed_defer_for_resume_and_cancel) {
    static const char source[] = "import sys\n"
                                 "fn answer() -> i64 {\n"
                                 "  var live = sys.Pipe(2147483646, 2147483647)\n"
                                 "  defer {\n"
                                 "    live.closeRead()\n"
                                 "    live.closeWrite()\n"
                                 "  }\n"
                                 "  const readEnd = live.readEnd()\n"
                                 "  Coro.yield()\n"
                                 "  return readEnd - 2147483604\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_PLACE_LOCAL), 6u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_PLACE_TAKE), 0u);
    ASSERT_GT(program_operation_count(first.program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT), 0u);

    uint32_t entry = xr_validated_program_entry_function(first.program);
    ASSERT_LT(entry, first.program->function_count);
    const XrValidatedFunction *function = &first.program->functions[entry];
    ASSERT_EQ_UINT(function->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(function->coroutine_safepoints[0].live_value_count, 2u);
    const XrValidatedInstruction *yield = NULL;
    uint32_t recovery_block_count = 0u;
    for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
        const XrValidatedBlock *block = &function->blocks[block_index];
        const XrValidatedInstruction *block_local = NULL;
        const XrValidatedInstruction *block_borrow = NULL;
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *candidate = &block->instructions[instruction_index];
            if (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD) {
                ASSERT_NULL(yield);
                yield = candidate;
            }
            if (candidate->operation_id == XR_CORE_OP_CORE_PLACE_LOCAL)
                block_local = candidate;
            if (candidate->operation_id == XR_CORE_OP_CORE_PLACE_LOAD && block_local &&
                candidate->operand_count == 1u && candidate->operands[0] == block_local->result_id)
                block_borrow = candidate;
        }
        if (block_local && block_borrow && block->instruction_count != 0u &&
            block->instructions[block->instruction_count - 1u].operation_id ==
                XR_CORE_OP_CORE_BRANCH) {
            ASSERT_GT(block->instruction_count, 2u);
            ASSERT_EQ_UINT(block_local->operand_count, 1u);
            const XrValidatedInstruction *recovery_edge =
                &block->instructions[block->instruction_count - 1u];
            uint32_t owner_occurrences = 0u;
            uint32_t borrowed_occurrences = 0u;
            for (uint32_t operand = 0u; operand < recovery_edge->operand_count; ++operand) {
                owner_occurrences += recovery_edge->operands[operand] == block_local->operands[0];
                borrowed_occurrences += recovery_edge->operands[operand] == block_borrow->result_id;
            }
            ASSERT_EQ_UINT(owner_occurrences, 1u);
            ASSERT_EQ_UINT(borrowed_occurrences, 0u);
            ++recovery_block_count;
        }
    }
    ASSERT_GT(recovery_block_count, 0u);
    ASSERT_NOT_NULL(yield);
    ASSERT_EQ_UINT(yield->successor_count, 2u);
    const XrValidatedBlock *resume = &function->blocks[yield->successors[0]];
    ASSERT_EQ_UINT(resume->argument_count, 2u);
    uint32_t resume_owner_count = 0u;
    uint32_t resume_non_owner_count = 0u;
    for (uint32_t argument = 0u; argument < resume->argument_count; ++argument) {
        ASSERT_EQ_INT(resume->argument_categories[argument], XR_CORE_IR_VALUE);
        resume_owner_count += resume->argument_ownerships[argument] == XR_CORE_IR_OWNER;
        resume_non_owner_count += resume->argument_ownerships[argument] == XR_CORE_IR_NON_OWNER;
    }
    ASSERT_EQ_UINT(resume_owner_count, 1u);
    ASSERT_EQ_UINT(resume_non_owner_count, 1u);
    const XrValidatedBlock *cancel = &function->blocks[yield->successors[1]];
    ASSERT_EQ_UINT(cancel->argument_count, 1u);
    ASSERT_EQ_INT(cancel->argument_categories[0], XR_CORE_IR_VALUE);
    ASSERT_EQ_INT(cancel->argument_ownerships[0], XR_CORE_IR_OWNER);
    ASSERT_GT(cancel->instruction_count, 1u);
    ASSERT_EQ_INT(cancel->instructions[cancel->instruction_count - 1u].operation_id,
                  XR_CORE_OP_CORE_CANCEL_PUBLISH);
    uint32_t cancel_cleanup_calls = 0u;
    const XrValidatedBlock *trap = NULL;
    for (uint32_t instruction = 0u; instruction < cancel->instruction_count; ++instruction)
        cancel_cleanup_calls +=
            cancel->instructions[instruction].operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT;
    for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
        const XrValidatedBlock *candidate = &function->blocks[block_index];
        if (candidate->instruction_count == 0u ||
            candidate->instructions[candidate->instruction_count - 1u].operation_id !=
                XR_CORE_OP_CORE_TRAP)
            continue;
        uint32_t candidate_cleanup_calls = 0u;
        for (uint32_t instruction = 0u; instruction < candidate->instruction_count; ++instruction)
            candidate_cleanup_calls += candidate->instructions[instruction].operation_id ==
                                       XR_CORE_OP_CORE_CALL_SEALED_DIRECT;
        if (candidate_cleanup_calls == 2u) {
            ASSERT_NULL(trap);
            trap = candidate;
        }
    }
    ASSERT_EQ_UINT(cancel_cleanup_calls, 2u);
    ASSERT_NOT_NULL(trap);
    ASSERT_TRUE(trap != cancel);
    uint32_t trap_cleanup_calls = 0u;
    for (uint32_t instruction = 0u; instruction < trap->instruction_count; ++instruction)
        trap_cleanup_calls +=
            trap->instructions[instruction].operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT;
    ASSERT_EQ_UINT(trap_cleanup_calls, 2u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 1u);
    ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, 0u, &requirement));
    PipeProviderProbe probe = {
        .read_handle = 2147483646,
        .write_handle = 2147483647,
        .close_results = {true, true},
    };
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
    };
    operation.entry.bool_i64_unary = pipe_close_provider_probe;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        probe.close_calls = 0u;
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic vm_diagnostic;
        ASSERT_EQ_INT(
            xr_vm_code_build(binding.program, binding.profile, &options, &code, &vm_diagnostic),
            XR_VM_CODE_OK);
        XrVmExecution *vm = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm));
        ASSERT_EQ_INT(xr_vm_execution_step(vm).kind, XR_VM_OUTCOME_SUSPENDED);
        XrVmOutcome vm_return = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_return.value.as.i64, 42);
        ASSERT_EQ_UINT(probe.close_calls, 2u);
        xr_vm_execution_free(vm);

        XrVmExecution *vm_cancel = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm_cancel));
        ASSERT_EQ_INT(xr_vm_execution_step(vm_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_vm_execution_cancel(vm_cancel).kind, XR_VM_OUTCOME_CANCELLED);
        ASSERT_EQ_UINT(probe.close_calls, 4u);
        xr_vm_execution_free(vm_cancel);
        xr_vm_code_free(code);
    }
    assert_aot_pipe_cancel_cleanup(
        first.program, profile, source_fixture_output_path(XR_SOURCE_FIXTURE_PIPE_CANCEL_CLEANUP));

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_runs_each_dense_coroutine_state_across_private_executors) {
    static const char source[] = "fn answer() -> i64 {\n"
                                 "  Coro.yield()\n"
                                 "  Coro.yield()\n"
                                 "  return 42\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);
    uint32_t entry = xr_validated_program_entry_function(first.program);
    ASSERT_LT(entry, first.program->function_count);
    const XrValidatedFunction *function = &first.program->functions[entry];
    ASSERT_EQ_UINT(function->coroutine_state_count, 3u);
    ASSERT_EQ_UINT(function->coroutine_safepoint_count, 2u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_COROUTINE_YIELD), 2u);
    for (uint32_t safepoint = 0u; safepoint < 2u; ++safepoint) {
        ASSERT_EQ_UINT(function->coroutine_safepoints[safepoint].resume_state_id, safepoint + 1u);
        ASSERT_EQ_UINT(function->coroutine_safepoints[safepoint].live_value_count, 0u);
    }

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(binding.program, binding.profile, NULL, &code, &vm_diagnostic),
                  XR_VM_CODE_OK);
    XrVmExecution *vm = NULL;
    ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm));
    XrVmOutcome vm_outcome = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_outcome.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_outcome.state_id, 1u);
    ASSERT_EQ_UINT(vm_outcome.safepoint_id, 0u);
    vm_outcome = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_outcome.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_outcome.state_id, 2u);
    ASSERT_EQ_UINT(vm_outcome.safepoint_id, 1u);
    vm_outcome = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_outcome.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_outcome.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_outcome.value.as.i64, 42);
    xr_vm_execution_free(vm);

    for (uint32_t cancel_state = 1u; cancel_state <= 2u; ++cancel_state) {
        XrVmExecution *cancelled = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &cancelled));
        for (uint32_t step = 0u; step < cancel_state; ++step) {
            vm_outcome = xr_vm_execution_step(cancelled);
            ASSERT_EQ_INT(vm_outcome.kind, XR_VM_OUTCOME_SUSPENDED);
        }
        vm_outcome = xr_vm_execution_cancel(cancelled);
        ASSERT_EQ_INT(vm_outcome.kind, XR_VM_OUTCOME_CANCELLED);
        ASSERT_EQ_UINT(vm_outcome.state_id, cancel_state);
        xr_vm_execution_free(cancelled);
    }
    xr_vm_code_free(code);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "case UINT32_C(2)"));
    ASSERT_NOT_NULL(strstr(generated.bytes,
                           "return xr_aot_suspend(UINT32_C(1), UINT32_C(1), UINT32_C(0), "
                           "INT64_C(0))"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_MULTI_SAFEPOINT);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(output,
                    "\nint main(void) {\n"
                    "    XrAotEntryCoroutineFrame resumed;\n"
                    "    xr_aot_entry_coroutine_frame_initialize(&resumed, NULL, NULL);\n"
                    "    XrBackendNativeOutcome outcome = "
                    "xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (outcome.kind != UINT32_C(1) || outcome.state_id != UINT32_C(1) || "
                    "outcome.safepoint_id != UINT32_C(0)) return 255;\n"
                    "    outcome = xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (outcome.kind != UINT32_C(1) || outcome.state_id != UINT32_C(2) || "
                    "outcome.safepoint_id != UINT32_C(1)) return 254;\n"
                    "    outcome = xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (outcome.kind != 0 || outcome.value != INT64_C(42)) return 253;\n"
                    "    xr_aot_entry_coroutine_frame_dispose(&resumed);\n"
                    "    for (uint32_t cancel_state = UINT32_C(1); cancel_state <= UINT32_C(2); "
                    "++cancel_state) {\n"
                    "        XrAotEntryCoroutineFrame cancelled;\n"
                    "        xr_aot_entry_coroutine_frame_initialize(&cancelled, NULL, NULL);\n"
                    "        for (uint32_t step = 0; step < cancel_state; ++step) {\n"
                    "            outcome = xr_aot_entry_coroutine_step(&cancelled);\n"
                    "            if (outcome.kind != UINT32_C(1)) return 252;\n"
                    "        }\n"
                    "        outcome = xr_aot_entry_coroutine_cancel(&cancelled);\n"
                    "        if (outcome.kind != UINT32_C(3) || outcome.state_id != cancel_state) "
                    "return 251;\n"
                    "        xr_aot_entry_coroutine_frame_dispose(&cancelled);\n"
                    "    }\n"
                    "    return 227;\n"
                    "}\n") > 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_keeps_related_ref_parameter_places_stable_across_child_suspension) {
    static const char source[] = "import sys\n"
                                 "import time\n"
                                 "fn bump(value: ref i64, left: ref i64, right: ref i64) {\n"
                                 "  value = value + 4\n"
                                 "  left = left + 5\n"
                                 "  right = right + 6\n"
                                 "  Coro.yield()\n"
                                 "  value = value + 1\n"
                                 "  left = left + 2\n"
                                 "  right = right + 3\n"
                                 "  const stamp = time.now()\n"
                                 "  value = value + stamp - stamp\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var value = 10\n"
                                 "  var live = sys.Pipe(2147483642, 2147483643)\n"
                                 "  defer { live.closeRead(); live.closeWrite() }\n"
                                 "  const snapshot = live._readHandle\n"
                                 "  bump(ref value, ref live._readHandle, ref live._writeHandle)\n"
                                 "  return value + live.readEnd() + live.writeEnd() + snapshot\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program) {
        xr_program_source_product_free(&second);
        xr_program_source_product_free(&first);
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    assert_products_equal(&first, &second);

    uint32_t entry = xr_validated_program_entry_function(first.program);
    ASSERT_LT(entry, first.program->function_count);
    const XrValidatedFunction *function = &first.program->functions[entry];
    ASSERT_EQ_UINT(function->coroutine_state_count, 2u);
    ASSERT_EQ_UINT(function->coroutine_safepoint_count, 1u);
    const XrValidatedInstruction *coroutine_call = NULL;
    for (uint32_t block = 0u; block < function->block_count; ++block)
        for (uint32_t instruction = 0u; instruction < function->blocks[block].instruction_count;
             ++instruction)
            if (function->blocks[block].instructions[instruction].operation_id ==
                XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                ASSERT_NULL(coroutine_call);
                coroutine_call = &function->blocks[block].instructions[instruction];
            }
    ASSERT_NOT_NULL(coroutine_call);
    uint32_t child_id = coroutine_call->immediate.coroutine_call.function_id;
    ASSERT_LT(child_id, first.program->function_count);
    const XrValidatedFunction *child = &first.program->functions[child_id];
    ASSERT_EQ_UINT(child->parameter_count, 3u);
    for (uint32_t parameter = 0u; parameter < child->parameter_count; ++parameter)
        ASSERT_EQ_INT(child->parameter_modes[parameter], XR_PARAM_REF);
    ASSERT_EQ_UINT(child->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(child->coroutine_safepoints[0].live_value_count, 3u);
    for (uint32_t live = 0u; live < child->coroutine_safepoints[0].live_value_count; ++live) {
        uint32_t child_live = child->coroutine_safepoints[0].live_value_ids[live];
        ASSERT_LT(child_live, child->value_count);
        ASSERT_EQ_INT(child->value_categories[child_live], XR_CORE_IR_PLACE);
        ASSERT_EQ_INT(child->value_ownerships[child_live], XR_CORE_IR_NON_OWNER);
    }
    const XrValidatedCoroutineSafepoint *parent_point = &function->coroutine_safepoints[0];
    ASSERT_EQ_UINT(parent_point->live_value_count, 3u);
    ASSERT_EQ_UINT(coroutine_call->successor_count, 4u);
    ASSERT_EQ_UINT(child->error_type_id, XR_CORE_TYPE_VOID);
    ASSERT_EQ_UINT(child->panic_type_id, XR_CORE_TYPE_PANIC_INFO);
    const XrValidatedBlock *panic = &function->blocks[coroutine_call->successors[2]];
    ASSERT_EQ_UINT(panic->argument_types[0], XR_CORE_TYPE_PANIC_INFO);
    ASSERT_EQ_UINT(panic->argument_ownerships[0], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(coroutine_call->operand_count,
                   child->parameter_count + parent_point->live_value_count + 3u);
    uint32_t storage_roots[3] = {0};
    uint32_t field_ordinals[2] = {0};
    for (uint32_t parameter = 0u; parameter < child->parameter_count; ++parameter) {
        uint32_t place = coroutine_call->operands[parameter];
        ASSERT_LT(place, function->value_count);
        ASSERT_EQ_INT(function->value_categories[place], XR_CORE_IR_PLACE);
        ASSERT_EQ_INT(function->value_ownerships[place], XR_CORE_IR_NON_OWNER);
        ASSERT_NE(function->value_positions[place], 0u);
        uint32_t block = function->value_blocks[place];
        uint32_t position = function->value_positions[place] - 1u;
        ASSERT_LT(block, function->block_count);
        ASSERT_LT(position, function->blocks[block].instruction_count);
        const XrValidatedInstruction *definition = &function->blocks[block].instructions[position];
        ASSERT_EQ_UINT(definition->result_id, place);
        ASSERT_EQ_UINT(definition->operand_count, 1u);
        if (parameter == 0u) {
            ASSERT_EQ_UINT(definition->operation_id, XR_CORE_OP_CORE_PLACE_LOCAL);
            storage_roots[parameter] = definition->operands[0];
        } else {
            ASSERT_EQ_UINT(definition->operation_id, XR_CORE_OP_CORE_CLASS_FIELD_PLACE);
            ASSERT_EQ_INT(definition->immediate_kind, XR_CORE_IR_IMMEDIATE_FIELD);
            field_ordinals[parameter - 1u] = definition->immediate.field_ordinal;
            storage_roots[parameter] = xr_validated_function_scoped_affine_borrow_owner(
                first.program, function, place, block);
        }
        ASSERT_LT(storage_roots[parameter], function->value_count);
        ASSERT_EQ_INT(function->value_categories[storage_roots[parameter]], XR_CORE_IR_VALUE);
        uint32_t live_occurrences = 0u;
        for (uint32_t live = 0u; live < parent_point->live_value_count; ++live)
            live_occurrences += parent_point->live_value_ids[live] == storage_roots[parameter];
        ASSERT_EQ_UINT(live_occurrences, 1u);
    }
    ASSERT_NE(field_ordinals[0], field_ordinals[1]);
    ASSERT_EQ_UINT(storage_roots[1], storage_roots[2]);
    ASSERT_NE(storage_roots[0], storage_roots[1]);
    ASSERT_EQ_UINT(function->value_types[storage_roots[0]], XR_CORE_TYPE_I64);
    const XrValidatedType *class_type =
        xr_validated_program_type(first.program, function->value_types[storage_roots[1]]);
    ASSERT_NOT_NULL(class_type);
    ASSERT_EQ_INT(class_type->kind, XR_CORE_IR_TYPE_CLASS_REFERENCE);
    ASSERT_EQ_INT(class_type->nominal_kind, XR_CORE_IR_NOMINAL_CLASS);
    ASSERT_EQ_INT(class_type->ownership, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE);
    ASSERT_LT(field_ordinals[0], class_type->field_count);
    ASSERT_LT(field_ordinals[1], class_type->field_count);
    uint32_t snapshot_count = 0u;
    for (uint32_t live = 0u; live < parent_point->live_value_count; ++live) {
        uint32_t value = parent_point->live_value_ids[live];
        ASSERT_LT(value, function->value_count);
        ASSERT_EQ_INT(function->value_categories[value], XR_CORE_IR_VALUE);
        if (value != storage_roots[0] && value != storage_roots[1]) {
            ASSERT_EQ_UINT(function->value_types[value], XR_CORE_TYPE_I64);
            ++snapshot_count;
        }
    }
    ASSERT_EQ_UINT(snapshot_count, 1u);

    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(first.program), 2u);
    XrStableId clock_contract = {{0}};
    XrStableId clock_operation = {{0}};
    XrStableId io_contract = {{0}};
    XrStableId close_operation = {{0}};
    XrFingerprint key_digest;
    ASSERT_TRUE(
        xr_stable_id_from_key(XR_PROVIDER_CLOCK_CONTRACT_KEY, &clock_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY,
                                      &clock_operation, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &io_contract, &key_digest));
    ASSERT_TRUE(xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY, &close_operation,
                                      &key_digest));
    FieldRefCleanupProbe cleanup_probe = {0};
    XrProviderOperationBinding operations[2] = {0};
    XrProviderBinding providers[2] = {0};
    uint32_t clock_requirement = UINT32_MAX;
    uint32_t io_requirement = UINT32_MAX;
    for (uint32_t index = 0u; index < 2u; ++index) {
        XrProgramProviderRequirementView requirement = {0};
        ASSERT_TRUE(xr_validated_program_provider_requirement(first.program, index, &requirement));
        ASSERT_EQ_UINT(requirement.operation_count, 1u);
        const XrTargetProviderContract *contract =
            find_profile_provider(profile, requirement.contract_id);
        ASSERT_NOT_NULL(contract);
        operations[index].operation_id = requirement.operations[0].operation_id;
        operations[index].context = &cleanup_probe;
        if (stable_id_equal(requirement.contract_id, clock_contract)) {
            ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, clock_operation));
            clock_requirement = index;
            operations[index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[index].entry.i64_nullary = field_ref_cleanup_clock_probe;
        } else {
            ASSERT_TRUE(stable_id_equal(requirement.contract_id, io_contract));
            ASSERT_TRUE(stable_id_equal(requirement.operations[0].operation_id, close_operation));
            io_requirement = index;
            operations[index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY;
            operations[index].entry.bool_i64_unary = field_ref_cleanup_close_probe;
        }
        providers[index].contract_id = requirement.contract_id;
        providers[index].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        providers[index].operations = &operations[index];
        providers[index].operation_count = 1u;
        ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(
                          contract, &providers[index].contract_fingerprint),
                      XR_RUNTIME_ABI_OK);
    }
    ASSERT_TRUE(clock_requirement != UINT32_MAX);
    ASSERT_TRUE(io_requirement != UINT32_MAX);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .providers = providers,
        .provider_count = 2u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    field_ref_cleanup_check_vm(binding.program, binding.profile, instance, entry, &cleanup_probe);
    field_ref_cleanup_check_vm(binding.program, binding.profile, instance, entry, &cleanup_probe);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_0"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_1"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_2"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_typed"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_dispose_typed"));
    XrValidatedFunction *backend_parent = &backend_ir->program->functions[entry];
    XrValidatedInstruction *backend_call = NULL;
    for (uint32_t block = 0u; block < backend_parent->block_count; ++block)
        for (uint32_t instruction = 0u;
             instruction < backend_parent->blocks[block].instruction_count; ++instruction)
            if (backend_parent->blocks[block].instructions[instruction].operation_id ==
                XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                ASSERT_NULL(backend_call);
                backend_call = &backend_parent->blocks[block].instructions[instruction];
            }
    ASSERT_NOT_NULL(backend_call);
    ASSERT_EQ_UINT(backend_parent->coroutine_safepoint_count, 1u);
    XrValidatedCoroutineSafepoint *backend_point = &backend_parent->coroutine_safepoints[0];
    ASSERT_EQ_UINT(backend_point->live_value_count, 3u);
    uint32_t backend_child_id = backend_call->immediate.coroutine_call.function_id;
    ASSERT_LT(backend_child_id, backend_ir->program->function_count);
    uint32_t backend_parameter_count =
        backend_ir->program->functions[backend_child_id].parameter_count;
    ASSERT_EQ_UINT(backend_parameter_count, 3u);
    ASSERT_LT(backend_parameter_count + 1u, backend_call->operand_count);
    uint32_t saved_live = backend_point->live_value_ids[0];
    uint32_t saved_operand = backend_call->operands[backend_parameter_count];
    backend_point->live_value_ids[0] = backend_call->operands[0];
    backend_call->operands[backend_parameter_count] = backend_call->operands[0];
    ASSERT_FALSE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_EQ_INT(backend_diagnostic.status, XR_BACKEND_INVARIANT_REJECTED);
    backend_point->live_value_ids[0] = saved_live;
    backend_call->operands[backend_parameter_count] = saved_operand;
    ASSERT_TRUE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    uint32_t projected_place = backend_call->operands[1];
    ASSERT_LT(projected_place, function->value_count);
    ASSERT_NE(function->value_positions[projected_place], 0u);
    uint32_t project_block = function->value_blocks[projected_place];
    uint32_t project_position = function->value_positions[projected_place] - 1u;
    ASSERT_LT(project_block, backend_parent->block_count);
    ASSERT_LT(project_position, backend_parent->blocks[project_block].instruction_count);
    XrValidatedInstruction *backend_project =
        &backend_parent->blocks[project_block].instructions[project_position];
    ASSERT_EQ_UINT(backend_project->result_id, projected_place);
    ASSERT_EQ_UINT(backend_project->operation_id, XR_CORE_OP_CORE_CLASS_FIELD_PLACE);
    uint32_t saved_ordinal = backend_project->immediate.field_ordinal;
    backend_project->immediate.field_ordinal = class_type->field_count;
    ASSERT_FALSE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_EQ_INT(backend_diagnostic.status, XR_BACKEND_INVARIANT_REJECTED);
    backend_project->immediate.field_ordinal = saved_ordinal;
    ASSERT_TRUE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_REF_PARAMETER_COROUTINE);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
        ASSERT_TRUE(
            fprintf(output,
                    "\nstatic uint32_t xr_probe_mode;\n"
                    "static uint32_t xr_probe_count;\n"
                    "static int64_t xr_probe_events[3];\n"
                    "static int xr_probe_clock(void *context, uint32_t requirement, "
                    "uint32_t operation, int64_t *result) {\n"
                    "    (void)context;\n"
                    "    if (requirement != UINT32_C(%u) || operation != 0 || !result || "
                    "xr_probe_count != 0) return 1;\n"
                    "    xr_probe_events[xr_probe_count++] = 0;\n"
                    "    if (xr_probe_mode == 1) return 1;\n"
                    "    *result = INT64_C(42);\n"
                    "    return 0;\n"
                    "}\n"
                    "static int xr_probe_close(void *context, uint32_t requirement, "
                    "uint32_t operation, int64_t handle, uint8_t *result) {\n"
                    "    (void)context;\n"
                    "    if (requirement != UINT32_C(%u) || operation != 0 || !result || "
                    "xr_probe_count >= 3) return 1;\n"
                    "    xr_probe_events[xr_probe_count++] = handle;\n"
                    "    *result = UINT8_C(1);\n"
                    "    return 0;\n"
                    "}\n"
                    "int main(void) {\n"
                    "    static const uint32_t expected_kind[3] = {0, 2, 3};\n"
                    "    static const uint32_t expected_count[3] = {3, 3, 2};\n"
                    "    static const int64_t expected_events[3][3] = {\n"
                    "        {0, INT64_C(2147483649), INT64_C(2147483652)},\n"
                    "        {0, INT64_C(2147483649), INT64_C(2147483652)},\n"
                    "        {INT64_C(2147483647), INT64_C(2147483649), 0},\n"
                    "    };\n"
                    "    for (xr_probe_mode = 0; xr_probe_mode < 3; ++xr_probe_mode) {\n"
                    "        xr_probe_count = 0;\n"
                    "        XrAotEntryCoroutineFrame frame;\n"
                    "        xr_aot_entry_coroutine_frame_initialize(&frame, NULL, NULL);\n"
                    "        xr_probe_read_entry = xr_probe_clock;\n"
                    "        frame.context.provider_call_typed = xr_probe_typed;\n"
                    "        frame.context.provider_dispose_typed = xr_probe_dispose;\n"
                    "        xr_probe_close_entry = xr_probe_close;\n"
                    "        XrBackendNativeOutcome outcome = "
                    "xr_aot_entry_coroutine_step(&frame);\n"
                    "        if (outcome.kind != 1 || outcome.state_id != 1 || "
                    "outcome.safepoint_id != 0 || xr_probe_count != 0) return 255;\n"
                    "        outcome = xr_probe_mode == 2 "
                    "? xr_aot_entry_coroutine_cancel(&frame) "
                    ": xr_aot_entry_coroutine_step(&frame);\n"
                    "        xr_aot_entry_coroutine_frame_dispose(&frame);\n"
                    "        if (outcome.kind != expected_kind[xr_probe_mode]) return 254;\n"
                    "        if (outcome.kind == 0 && "
                    "outcome.value != INT64_C(6442450958)) return 253;\n"
                    "        if (outcome.kind == 2 && outcome.safepoint_id != 7) return 252;\n"
                    "        if (xr_probe_count != expected_count[xr_probe_mode]) return 251;\n"
                    "        for (uint32_t event = 0; event < xr_probe_count; ++event) {\n"
                    "            if (xr_probe_events[event] != "
                    "expected_events[xr_probe_mode][event]) return 250;\n"
                    "        }\n"
                    "    }\n"
                    "    return 230;\n"
                    "}\n",
                    clock_requirement, io_requirement) > 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

typedef struct ReadExistentialCoroutineProbe {
    uint32_t entry;
    uint32_t child_id;
    const XrValidatedFunction *parent;
    const XrValidatedFunction *child;
    const XrValidatedInstruction *call;
} ReadExistentialCoroutineProbe;

static bool inspect_read_existential_coroutine(const XrValidatedProgram *program,
                                               ReadExistentialCoroutineProbe *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->entry = xr_validated_program_entry_function(program);
    if (probe->entry >= program->function_count)
        return false;
    probe->parent = &program->functions[probe->entry];
    if (probe->parent->coroutine_state_count != 2u ||
        probe->parent->coroutine_safepoint_count != 1u)
        return false;
    uint32_t call_block = XR_PROGRAM_LOCATION_NONE;
    for (uint32_t block = 0u; block < probe->parent->block_count; ++block) {
        const XrValidatedBlock *candidate_block = &probe->parent->blocks[block];
        for (uint32_t instruction = 0u; instruction < candidate_block->instruction_count;
             ++instruction) {
            const XrValidatedInstruction *candidate = &candidate_block->instructions[instruction];
            if (candidate->operation_id != XR_CORE_OP_CORE_COROUTINE_CALL_SEALED)
                continue;
            if (probe->call)
                return false;
            probe->call = candidate;
            call_block = block;
        }
    }
    if (!probe->call || probe->call->immediate.coroutine_call.safepoint_id != 0u ||
        probe->call->operand_count < 2u)
        return false;
    probe->child_id = probe->call->immediate.coroutine_call.function_id;
    if (probe->child_id >= program->function_count)
        return false;
    probe->child = &program->functions[probe->child_id];
    if (probe->child->parameter_count != 1u || probe->child->parameter_modes[0] != XR_PARAM_READ ||
        probe->child->coroutine_safepoint_count != 1u ||
        probe->child->coroutine_safepoints[0].live_value_count != 1u)
        return false;
    const XrValidatedType *borrow_type =
        xr_validated_program_type(program, probe->child->parameter_types[0]);
    uint32_t child_live = probe->child->coroutine_safepoints[0].live_value_ids[0];
    if (!borrow_type || borrow_type->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        borrow_type->interface_use_kind != XR_CORE_IR_INTERFACE_EXISTENTIAL_READ ||
        child_live >= probe->child->value_count ||
        probe->child->value_types[child_live] != probe->child->parameter_types[0] ||
        probe->child->value_categories[child_live] != XR_CORE_IR_VALUE ||
        probe->child->value_ownerships[child_live] != XR_CORE_IR_NON_OWNER)
        return false;
    uint32_t owner = xr_validated_function_scoped_affine_borrow_owner(
        program, probe->parent, probe->call->operands[0], call_block);
    if (owner == XR_PROGRAM_LOCATION_NONE ||
        probe->parent->coroutine_safepoints[0].live_value_count != 1u ||
        probe->parent->coroutine_safepoints[0].live_value_ids[0] != owner ||
        probe->parent->value_categories[owner] != XR_CORE_IR_VALUE ||
        probe->parent->value_ownerships[owner] != XR_CORE_IR_OWNER)
        return false;
    const XrValidatedType *owner_type =
        xr_validated_program_type(program, probe->parent->value_types[owner]);
    return owner_type && owner_type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
           (owner_type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
            owner_type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) &&
           owner_type->interface_id == borrow_type->interface_id;
}

static bool run_read_existential_coroutine_executors(XrValidatedProgram *program,
                                                     XrTargetProfile *profile, uint32_t entry) {
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic diagnostic;
    XrInstance *instance = NULL;
    if (xr_execution_instance_create(&binding, &instance, &diagnostic) != XR_EXECUTION_OK ||
        !instance)
        return false;
    bool ok = false;
    XrVmCode *code = NULL;
    XrVmExecution *vm = NULL;
    XrVmExecution *vm_cancel = NULL;
    XrVmOutcome vm_outcome;
    XrVmCodeDiagnostic vm_diagnostic;

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        if (xr_vm_code_build(binding.program, binding.profile, &options, &code, &vm_diagnostic) !=
                XR_VM_CODE_OK ||
            !xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm))
            goto cleanup;
        vm_outcome = xr_vm_execution_step(vm);
        if (vm_outcome.kind != XR_VM_OUTCOME_SUSPENDED || vm_outcome.state_id != 1u ||
            vm_outcome.safepoint_id != 0u)
            goto cleanup;
        vm_outcome = xr_vm_execution_step(vm);
        if (vm_outcome.kind != XR_VM_OUTCOME_RETURN || vm_outcome.value.kind != XR_VM_VALUE_I64 ||
            vm_outcome.value.as.i64 != 42)
            goto cleanup;
        xr_vm_execution_free(vm);
        vm = NULL;
        if (!xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm_cancel) ||
            xr_vm_execution_step(vm_cancel).kind != XR_VM_OUTCOME_SUSPENDED ||
            xr_vm_execution_cancel(vm_cancel).kind != XR_VM_OUTCOME_CANCELLED)
            goto cleanup;
        xr_vm_execution_free(vm_cancel);
        vm_cancel = NULL;
        xr_vm_code_free(code);
        code = NULL;
    }
    ok = true;
cleanup:
    if (vm)
        xr_vm_execution_free(vm);
    if (vm_cancel)
        xr_vm_execution_free(vm_cancel);
    if (code)
        xr_vm_code_free(code);
    bool lifecycle_ok = xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK;
    lifecycle_ok =
        xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK && lifecycle_ok;
    lifecycle_ok =
        xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK && lifecycle_ok;
    return ok && lifecycle_ok;
}

static bool write_read_existential_coroutine_aot(const XrGeneratedC *generated,
                                                 XrSourceFixtureId fixture_id) {
    if (!source_fixture_output_path(fixture_id))
        return true;
    FILE *output = fopen(source_fixture_output_path(fixture_id), "wb");
    if (!output)
        return false;
    bool ok =
        fwrite(generated->bytes, 1u, generated->size, output) == generated->size &&
        fprintf(output, "\nint main(void) {\n"
                        "    XrAotEntryCoroutineFrame resumed;\n"
                        "    xr_aot_entry_coroutine_frame_initialize(&resumed, NULL, NULL);\n"
                        "    XrBackendNativeOutcome outcome = "
                        "xr_aot_entry_coroutine_step(&resumed);\n"
                        "    if (outcome.kind != UINT32_C(1) || outcome.state_id != UINT32_C(1) || "
                        "outcome.safepoint_id != UINT32_C(0)) return 255;\n"
                        "    outcome = xr_aot_entry_coroutine_step(&resumed);\n"
                        "    if (outcome.kind != UINT32_C(0) || outcome.value != INT64_C(42)) "
                        "return 254;\n"
                        "    xr_aot_entry_coroutine_frame_dispose(&resumed);\n"
                        "    XrAotEntryCoroutineFrame cancelled;\n"
                        "    xr_aot_entry_coroutine_frame_initialize(&cancelled, NULL, NULL);\n"
                        "    outcome = xr_aot_entry_coroutine_step(&cancelled);\n"
                        "    if (outcome.kind != UINT32_C(1)) return 253;\n"
                        "    outcome = xr_aot_entry_coroutine_cancel(&cancelled);\n"
                        "    if (outcome.kind != UINT32_C(3) || outcome.state_id != UINT32_C(1)) "
                        "return 252;\n"
                        "    xr_aot_entry_coroutine_frame_dispose(&cancelled);\n"
                        "    return 231;\n"
                        "}\n") > 0;
    return fclose(output) == 0 && ok;
}

static bool emit_read_existential_coroutine_aot(const XrValidatedProgram *program,
                                                const XrTargetProfile *profile,
                                                const ReadExistentialCoroutineProbe *probe,
                                                XrSourceFixtureId fixture_id) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    bool ok = false;
    if (xr_backend_ir_build(program, profile, &options, &ir, &diagnostic) != XR_BACKEND_OK ||
        !xr_backend_ir_binding_verify(ir, &diagnostic) ||
        xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) != XR_BACKEND_OK ||
        xr_backend_ir_emit_c(ir, false, &repeated, &diagnostic) != XR_BACKEND_OK ||
        generated.size != repeated.size ||
        memcmp(generated.bytes, repeated.bytes, generated.size) != 0 ||
        !strstr(generated.bytes, "child_active_0") || strstr(generated.bytes, "TargetPlan"))
        goto cleanup;
    XrValidatedFunction *parent = &ir->program->functions[probe->entry];
    XrValidatedInstruction *call = NULL;
    for (uint32_t block = 0u; block < parent->block_count; ++block) {
        XrValidatedBlock *candidate_block = &parent->blocks[block];
        for (uint32_t instruction = 0u; instruction < candidate_block->instruction_count;
             ++instruction) {
            XrValidatedInstruction *candidate = &candidate_block->instructions[instruction];
            if (candidate->operation_id != XR_CORE_OP_CORE_COROUTINE_CALL_SEALED)
                continue;
            if (call)
                goto cleanup;
            call = candidate;
        }
    }
    if (!call || parent->coroutine_safepoint_count != 1u ||
        parent->coroutine_safepoints[0].live_value_count != 1u)
        goto cleanup;
    XrValidatedCoroutineSafepoint *point = &parent->coroutine_safepoints[0];
    uint32_t parameter_count = ir->program->functions[probe->child_id].parameter_count;
    if (parameter_count != 1u || parameter_count >= call->operand_count)
        goto cleanup;
    uint32_t saved_live = point->live_value_ids[0];
    uint32_t saved_operand = call->operands[parameter_count];
    point->live_value_ids[0] = call->operands[0];
    call->operands[parameter_count] = call->operands[0];
    bool rejected = !xr_backend_ir_verify(ir, &diagnostic) &&
                    diagnostic.status == XR_BACKEND_INVARIANT_REJECTED;
    point->live_value_ids[0] = saved_live;
    call->operands[parameter_count] = saved_operand;
    ok = rejected && xr_backend_ir_verify(ir, &diagnostic) &&
         write_read_existential_coroutine_aot(&generated, fixture_id);
cleanup:
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    return ok;
}

TEST(source_owner_keeps_read_existential_root_across_child_suspension) {
    static const char source[] = "interface Reader { read() -> i64 }\n"
                                 "class One implements Reader {\n"
                                 "  read() -> i64 { return 42 }\n"
                                 "}\n"
                                 "fn delayed(reader: Reader) -> i64 {\n"
                                 "  Coro.yield()\n"
                                 "  return reader.read()\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var reader: Reader = One()\n"
                                 "  const result = delayed(reader)\n"
                                 "  return result + reader.read() - 42\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    if (!first.program || !second.program)
        goto cleanup;
    assert_products_equal(&first, &second);
    ReadExistentialCoroutineProbe probe;
    ASSERT_TRUE(inspect_read_existential_coroutine(first.program, &probe));
    ASSERT_TRUE(run_read_existential_coroutine_executors(first.program, profile, probe.entry));
    ASSERT_TRUE(emit_read_existential_coroutine_aot(first.program, profile, &probe,
                                                    XR_SOURCE_FIXTURE_READ_EXISTENTIAL_COROUTINE));
cleanup:
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_cross_module_coroutine_transfers_affine_resource_result) {
    static const char library_source[] = "import sys\n"
                                         "import { Pipe } from sys\n"
                                         "export fn child() -> Pipe {\n"
                                         "  var guard = sys.Pipe(2147483644, 2147483645)\n"
                                         "  defer {\n"
                                         "    guard.closeRead()\n"
                                         "    guard.closeWrite()\n"
                                         "  }\n"
                                         "  Coro.yield()\n"
                                         "  Coro.yield()\n"
                                         "  var result = sys.Pipe(2147483646, 2147483647)\n"
                                         "  return result\n"
                                         "}\n";
    static const char entry_source[] = "import sys\n"
                                       "import { Pipe } from sys\n"
                                       "import { child } from \"./library\"\n"
                                       "fn answer() -> i64 {\n"
                                       "  var live = child()\n"
                                       "  defer {\n"
                                       "    live.closeRead()\n"
                                       "    live.closeWrite()\n"
                                       "  }\n"
                                       "  Coro.yield()\n"
                                       "  Coro.yield()\n"
                                       "  return live.readEnd() - 2147483604\n"
                                       "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (!product.program) {
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }

    uint32_t entry = xr_validated_program_entry_function(product.program);
    ASSERT_LT(entry, product.program->function_count);
    const XrValidatedFunction *function = &product.program->functions[entry];
    ASSERT_EQ_UINT(function->coroutine_state_count, 4u);
    ASSERT_EQ_UINT(function->coroutine_safepoint_count, 3u);
    const XrValidatedInstruction *coroutine_call = NULL;
    for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
        const XrValidatedBlock *block = &function->blocks[block_index];
        for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
             ++instruction_index) {
            const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
            if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                ASSERT_NULL(coroutine_call);
                coroutine_call = instruction;
            }
        }
    }
    ASSERT_NOT_NULL(coroutine_call);
    ASSERT_EQ_UINT(coroutine_call->successor_count, 2u);
    uint32_t child_id = coroutine_call->immediate.coroutine_call.function_id;
    ASSERT_LT(child_id, product.program->function_count);
    const XrValidatedFunction *child = &product.program->functions[child_id];
    ASSERT_EQ_INT(child->result_ownership, XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(child->coroutine_state_count, 3u);
    ASSERT_EQ_UINT(child->coroutine_safepoint_count, 2u);
    ASSERT_EQ_UINT(child->coroutine_safepoints[0].live_value_count, 1u);
    ASSERT_EQ_UINT(child->coroutine_safepoints[1].live_value_count, 1u);
    ASSERT_EQ_UINT(function->coroutine_safepoints[0].live_value_count, 0u);
    ASSERT_EQ_UINT(function->coroutine_safepoints[1].live_value_count, 1u);
    ASSERT_EQ_UINT(function->coroutine_safepoints[2].live_value_count, 1u);
    const XrValidatedBlock *normal = &function->blocks[coroutine_call->successors[0]];
    const XrValidatedBlock *cancel = &function->blocks[coroutine_call->successors[1]];
    ASSERT_EQ_UINT(normal->argument_count, 1u);
    ASSERT_EQ_INT(normal->argument_types[0], child->result_type_id);
    ASSERT_EQ_INT(normal->argument_ownerships[0], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(cancel->argument_count, 0u);

    XrProgramProviderRequirementView requirement = {0};
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    ASSERT_TRUE(xr_validated_program_provider_requirement(product.program, 0u, &requirement));
    PipeCloseSequenceProbe probe = {0};
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operations[0].operation_id,
        .trampoline_kind = XR_PROVIDER_TRAMPOLINE_BOOL_I64_UNARY,
        .context = &probe,
    };
    operation.entry.bool_i64_unary = pipe_close_sequence_probe;
    XrProviderBinding provider = {
        .contract_id = requirement.contract_id,
        .behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL,
        .operations = &operation,
        .operation_count = 1u,
    };
    const XrTargetProviderContract *contract =
        find_profile_provider(profile, requirement.contract_id);
    ASSERT_NOT_NULL(contract);
    ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(contract, &provider.contract_fingerprint),
                  XR_RUNTIME_ABI_OK);
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        SourceVmCleanupLifecycle lifecycle = {0};
        options.lifecycle_context = &lifecycle;
        options.lifecycle_event = record_vm_cleanup_lifecycle;
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(
            xr_vm_code_build(binding.program, binding.profile, &options, &code, &diagnostic),
            XR_VM_CODE_OK);
        assert_affine_vm_executions(code, instance, entry, &probe, &lifecycle);
        lifecycle = (SourceVmCleanupLifecycle) {0};
        pipe_close_sequence_reset(&probe, affine_coroutine_close_handles, 2u);
        XrVmExecution *host_execution = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, child_id, NULL, 0u, &host_execution));
        ASSERT_EQ_INT(xr_vm_execution_step(host_execution).kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_vm_execution_step(host_execution).kind, XR_VM_OUTCOME_SUSPENDED);
        XrVmOutcome exported = xr_vm_execution_step(host_execution);
        ASSERT_EQ_INT(exported.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(exported.value.kind, XR_VM_VALUE_CLASS_REFERENCE);
        ASSERT_NOT_NULL(exported.value.as.class_reference);
        ASSERT_EQ_UINT(probe.calls, 2u);
        ASSERT_EQ_UINT(lifecycle.constructed, 2u);
        ASSERT_EQ_UINT(lifecycle.finalized, 1u);
        ASSERT_EQ_UINT(lifecycle.reclaimed, 1u);
        ASSERT_EQ_UINT(xr_execution_instance_lease_count(instance), 1u);
        xr_vm_code_free(code);
        ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
        ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_GENERATION_REJECTED);
        xr_vm_outcome_dispose(&exported);
        ASSERT_EQ_UINT(lifecycle.reclaimed, 1u);
        xr_vm_execution_free(host_execution);
        ASSERT_EQ_UINT(lifecycle.constructed, 2u);
        ASSERT_EQ_UINT(lifecycle.finalized, 2u);
        ASSERT_EQ_UINT(lifecycle.reclaimed, 2u);
        ASSERT_EQ_UINT(lifecycle.copied, 0u);
        ASSERT_EQ_UINT(lifecycle.teardown_events, 3u);
        ASSERT_EQ_UINT(probe.calls, 2u);
        ASSERT_EQ_UINT(xr_execution_instance_lease_count(instance), 0u);
    }
    assert_aot_affine_coroutine_cleanup(
        product.program, profile,
        source_fixture_output_path(XR_SOURCE_FIXTURE_AFFINE_COROUTINE_RESULT));

    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);

    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_time_sleep_has_typed_suspension_request) {
    static const char source[] = "import time\n"
                                 "fn answer() -> i64 { time.sleep(10); return 239 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = NULL;
    char profile_error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, profile_error,
                                                              sizeof(profile_error)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    XrProgramSourceBuildStatus status =
        xr_program_source_build(&fixture.input, &product, &diagnostic);
    if (status != XR_PROGRAM_SOURCE_BUILD_OK)
        fprintf(stderr, "time.sleep build failed: stage=%d writer=%d message=%s\n",
                (int) diagnostic.stage, (int) diagnostic.writer_status, diagnostic.message);
    ASSERT_EQ_INT(status, XR_PROGRAM_SOURCE_BUILD_OK);
    ASSERT_NOT_NULL(product.artifact.bytes);
    ASSERT_NOT_NULL(product.program);

    uint32_t entry = xr_validated_program_entry_function(product.program);
    ASSERT_LT(entry, product.program->function_count);
    const XrValidatedFunction *entry_function = &product.program->functions[entry];
    ASSERT_EQ_UINT(entry_function->effect_mask,
                   XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_CANCEL);
    ASSERT_EQ_UINT(entry_function->capability_mask,
                   XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION |
                       XR_CORE_CAPABILITY_RUNTIME_TIMER_SUSPENSION);

    const XrValidatedInstruction *timer_suspend = NULL;
    for (uint32_t function_index = 0u; function_index < product.program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &product.program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction = &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_COROUTINE_SUSPEND)
                    continue;
                ASSERT_NULL(timer_suspend);
                timer_suspend = instruction;
            }
        }
    }
    ASSERT_NOT_NULL(timer_suspend);
    ASSERT_EQ_INT(timer_suspend->immediate_kind, XR_CORE_IR_IMMEDIATE_COROUTINE_SUSPEND);
    ASSERT_EQ_UINT(timer_suspend->immediate.coroutine_suspend.request_kind,
                   XR_SUSPENSION_REQUEST_TIMER_AFTER_MS);
    ASSERT_EQ_UINT(timer_suspend->immediate.coroutine_suspend.request_operand_count, 1u);
    ASSERT_EQ_UINT(timer_suspend->successor_count, 2u);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = product.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(
        xr_vm_code_build(binding.program, binding.profile, NULL, &vm_code, &vm_diagnostic),
        XR_VM_CODE_OK);
    XrVmExecution *vm = NULL;
    ASSERT_TRUE(xr_vm_execution_create(vm_code, instance, entry, NULL, 0u, &vm));
    XrVmOutcome vm_suspend = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(vm_suspend.state_id, 1u);
    ASSERT_EQ_UINT(vm_suspend.suspension.kind, XR_SUSPENSION_REQUEST_TIMER_AFTER_MS);
    ASSERT_EQ_UINT(vm_suspend.suspension.operand_count, 1u);
    ASSERT_EQ_INT(vm_suspend.suspension.payload.timer_after_ms, 10);
    XrVmOutcome vm_return = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_return.value.as.i64, 239);
    xr_vm_execution_free(vm);

    ASSERT_TRUE(xr_vm_execution_create(vm_code, instance, entry, NULL, 0u, &vm));
    ASSERT_EQ_INT(xr_vm_execution_step(vm).kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_vm_execution_cancel(vm).kind, XR_VM_OUTCOME_CANCELLED);
    xr_vm_execution_free(vm);
    xr_vm_code_free(vm_code);

    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    XrBackendOptions backend_options = xr_backend_default_options();
    ASSERT_EQ_INT(xr_backend_ir_build(product.program, profile, &backend_options, &backend_ir,
                                      &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "UINT32_C(2), UINT32_C(1)"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "suspension_timer_after_ms"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_wait_timer"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_TIME_SLEEP);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);

    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_rejects_non_authoritative_entry_identity) {
    static const char source[] = "fn answer() -> i64 { return 42 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    char *wrong_identity = NULL;
    ASSERT_TRUE(xr_module_identity_from_logical(&fixture.authority, "other.xr", &wrong_identity));
    XrProgramSourceBuildInput input = fixture.input;
    input.entry.module_identity = wrong_identity;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "identity"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);

    input = fixture.input;
    input.entry.source_content_fingerprint.bytes[0] ^= UINT8_C(0x80);
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "fingerprint"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);

    input = fixture.input;
    input.entry.function_name = "missing";
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "source declarations"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);

    xr_free(wrong_identity);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_rejects_module_budget_before_analysis) {
    static const char library_source[] =
        "export fn increment(value: i64) -> i64 { return value + 1 }\n";
    static const char entry_source[] = "import { increment } from \"./library\"\n"
                                       "fn answer() -> i64 { return increment(41) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrProgramSourceBuildInput input = fixture.input;
    input.budget.max_modules = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH);
    ASSERT_EQ_UINT(diagnostic.underlying_status, 2u);
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_rejects_invalid_or_expanded_budget_request) {
    static const char source[] = "fn answer() -> i64 { return 42 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceBuildBudget defaults = xr_program_source_build_default_budget();
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    XrProgramSourceBuildInput input = fixture.input;

    input.budget.max_modules = defaults.max_modules + 1u;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT);
    input = fixture.input;
    input.budget.max_monomorphization_depth = defaults.max_monomorphization_depth + 1u;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT);
    input = fixture.input;
    input.budget.max_monomorphization_instances = defaults.max_monomorphization_instances + 1u;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT);
    input = fixture.input;
    input.budget.max_program_bytes = defaults.max_program_bytes + 1u;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT);
    input = fixture.input;
    input.budget.max_program_bytes = 0u;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT);
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_reports_exact_monomorphization_depth_budget) {
    static const char source[] = "class C0<T> {\n"
                                 "  value: T\n"
                                 "  constructor(value: T) { this.value = value }\n"
                                 "  get() -> T {\n"
                                 "    var next = C1<T>(this.value)\n"
                                 "    return next.get()\n"
                                 "  }\n"
                                 "}\n"
                                 "class C1<T> {\n"
                                 "  value: T\n"
                                 "  constructor(value: T) { this.value = value }\n"
                                 "  get() -> T {\n"
                                 "    var next = C2<T>(this.value)\n"
                                 "    return next.get()\n"
                                 "  }\n"
                                 "}\n"
                                 "class C2<T> {\n"
                                 "  value: T\n"
                                 "  constructor(value: T) { this.value = value }\n"
                                 "  get() -> T { return this.value }\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var root = C0<i64>(42)\n"
                                 "  return root.get()\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceBuildInput input = fixture.input;
    input.budget.max_monomorphization_depth = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION);
    ASSERT_EQ_UINT(diagnostic.underlying_status, XR_ERR_ANALYZE_MONO_DEPTH);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "E0389"));
    ASSERT_NOT_NULL(strstr(diagnostic.message, "deeper than 1 levels"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_reports_exact_monomorphization_instance_budget) {
    static const char source[] = "fn identity<T>(value: T) -> T { return value }\n"
                                 "fn answer() -> i64 {\n"
                                 "  var number = identity<i64>(41)\n"
                                 "  if (identity<bool>(true)) { return number + 1 }\n"
                                 "  return 0\n"
                                 "}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceBuildInput input = fixture.input;
    input.budget.max_monomorphization_instances = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION);
    ASSERT_EQ_UINT(diagnostic.underlying_status, XR_ERR_ANALYZE_MONO_BUDGET);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "E0388"));
    ASSERT_NOT_NULL(strstr(diagnostic.message, "budget of 1 generic instances"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_applies_instance_budget_across_module_graph) {
    static const char library_source[] =
        "export fn importedIdentity<T>(value: T) -> T { return value }\n";
    static const char entry_source[] =
        "import { importedIdentity } from \"./library\"\n"
        "fn localIdentity<T>(value: T) -> T { return value }\n"
        "fn answer() -> i64 { return importedIdentity<i64>(21) + localIdentity<i64>(21) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrProgramSourceBuildInput input = fixture.input;
    input.budget.max_monomorphization_instances = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION);
    ASSERT_EQ_UINT(diagnostic.module_index, 1u);
    ASSERT_EQ_UINT(diagnostic.underlying_status, XR_ERR_ANALYZE_MONO_BUDGET);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "E0388"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_rejects_program_bytes_over_request_budget) {
    static const char source[] = "fn answer() -> i64 { return 42 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceBuildInput input = fixture.input;
    input.budget.max_program_bytes = 1u;
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE);
    ASSERT_GT(diagnostic.underlying_status, 1u);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "Program size"));
    ASSERT_NOT_NULL(strstr(diagnostic.message, "limit 1 bytes"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_reports_structured_analysis_failure) {
    static const char source[] = "fn answer() -> i64 { return missing }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_program_source_build(&fixture.input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
    ASSERT_EQ_UINT(diagnostic.module_index, 0u);
    ASSERT_EQ_UINT(diagnostic.source_line, 1u);
    ASSERT_GT(diagnostic.source_column, 0u);
    ASSERT_NOT_NULL(strstr(diagnostic.source_path, "main.xr"));
    ASSERT_TRUE(diagnostic.message[0] != '\0');
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_channel_module_storage_has_exact_types) {
    static const char source[] =
        "type Subscription = { label: string, notifications: Channel<i64> }\n"
        "var subscriptions: Array<Subscription> = []\n"
        "var notifications: Channel<string>? = null\n"
        "fn answer() -> i64 {\n"
        " assert(len(subscriptions) == 0)\n"
        " assert(notifications == null)\n"
        " return len(subscriptions)\n}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_EQ_UINT(product.program->module_count, 1u);
    ASSERT_EQ_UINT(product.program->modules[0].slot_count, 2u);
    source_build_fixture_free(&fixture);
    assert_detached_program_i64_result(product.program, profile, 0);
    assert_aot_fixture_backend_contract(product.program, profile, XR_SOURCE_FIXTURE_CHANNEL_MODULE_TYPES);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
}

TEST(source_owner_imported_enum_catch_preserves_declaration_identity) {
    static const char library_source[] =
        "export enum Problem { Failed }\n"
        "export fn fail() -> i64 { throw Problem.Failed }\n";
    static const char source[] =
        "import { Problem as ImportedProblem, fail } from \"./library\"\n"
        "fn localFail() -> i64 { throw ImportedProblem.Failed }\n"
        "fn answer() -> i64 {\n"
        " var result = 0\n"
        " try { result = fail() } catch (e: ImportedProblem) { result = 20 }\n"
        " try { result = localFail() } catch (e: ImportedProblem) { result = result + 22 }\n"
        " assert(result == 42)\n"
        " return result\n}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, library_source));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    assert_detached_program_i64_result(product.program, profile, 42);
    assert_aot_fixture_backend_contract(product.program, profile, XR_SOURCE_FIXTURE_IMPORTED_ENUM_CATCH);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
}

TEST(source_owner_prelude_enum_catch_preserves_declaration_identity) {
    static const char library_source[] =
        "export fn fail() -> i64 { throw CryptoError.InvalidLength }\n";
    static const char source[] =
        "import { fail } from \"./library\"\n"
        "fn localFail() -> i64 { throw CryptoError.InvalidLength }\n"
        "fn answer() -> i64 {\n"
        " var result = 0\n"
        " try { result = fail() } catch (e: CryptoError) { result = 20 }\n"
        " try { result = localFail() } catch (e: CryptoError) { result = result + 22 }\n"
        " assert(result == 42)\n"
        " return result\n}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, library_source));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    source_build_fixture_free(&fixture);
    if (!product.program) {
        xr_program_source_product_free(&product);
        xr_target_profile_free(profile);
        return;
    }
    assert_detached_program_i64_result(product.program, profile, 42);
    assert_aot_fixture_backend_contract(product.program, profile, XR_SOURCE_FIXTURE_PRELUDE_ENUM_CATCH);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
}

TEST(source_owner_channel_handles_execute) {
    static const char source[] =
        "fn answer() -> i64 {\n"
        " const channel = Channel<string>(2)\n"
        " const alias = copy(channel)\n"
        " assert(!alias.isClosed)\n"
        " return 42\n}\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_EQ_UINT(product.program->module_count, 1u);
    bool constructed = false, queried = false;
    for (uint32_t f = 0u; f < product.program->function_count; ++f)
        for (uint32_t b = 0u; b < product.program->functions[f].block_count; ++b)
            for (uint32_t i = 0u; i < product.program->functions[f].blocks[b].instruction_count; ++i)
            {
                uint16_t operation = product.program->functions[f].blocks[b].instructions[i].operation_id;
                constructed |= operation == XR_CORE_OP_CORE_CHANNEL_CONSTRUCT;
                queried |= operation == XR_CORE_OP_CORE_CHANNEL_IS_CLOSED;
            }
    ASSERT_TRUE(constructed && queried);
    source_build_fixture_free(&fixture);
    assert_detached_program_i64_result(product.program, profile, 42);
    assert_aot_fixture_backend_contract(product.program, profile, XR_SOURCE_FIXTURE_CHANNEL_HANDLES);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
}

TEST(source_owner_atomic_ordering_preserves_exact_controls) {
    static const char *const names[] = {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"};
    static const uint32_t stores[] = {0u, 0u, 2u, 2u, 4u};
    for (uint32_t order = 0u; order < 5u; ++order) {
        char source[1024];
        int length = snprintf(source, sizeof(source),
            "fn answer() -> i64 {\n const counter = Atomic(40)\n"
            " counter.store(42, Ordering.%s)\n"
            " const old = counter.fetchSub(2, Ordering.%s)\n"
            " return old + counter.load(Ordering.%s)\n}\n",
            names[order], names[order], names[order]);
        ASSERT_TRUE(length > 0 && (size_t) length < sizeof(source));
        SourceBuildFixture fixture;
        ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
        XrProgramSourceProduct product = {0};
        XrProgramSourceDiagnostic diagnostic;
        assert_source_build_ok(&fixture.input, &product, &diagnostic);
        const XrValidatedProgram *program = product.program;
        uint32_t seen[3] = {0};
        for (uint32_t f = 0u; f < program->function_count; ++f) {
            const XrValidatedFunction *function = &program->functions[f];
            for (uint32_t b = 0u; b < function->block_count; ++b) {
                const XrValidatedBlock *block = &function->blocks[b];
                for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                    const XrValidatedInstruction *instruction = &block->instructions[i];
                    uint32_t slot, expected;
                    switch (instruction->operation_id) {
                        case XR_CORE_OP_CORE_ATOMIC_LOAD:
                            slot = 0u; expected = order; break;
                        case XR_CORE_OP_CORE_ATOMIC_EXCHANGE:
                            slot = 1u; expected = stores[order]; break;
                        case XR_CORE_OP_CORE_ATOMIC_UPDATE:
                            slot = 2u; expected = 8u + order; break;
                        default: continue;
                    }
                    ASSERT_EQ_UINT(instruction->immediate_kind, XR_CORE_IR_IMMEDIATE_U32);
                    ASSERT_EQ_UINT(instruction->immediate.u32, expected);
                    ++seen[slot];
                }
            }
        }
        for (uint32_t slot = 0u; slot < 3u; ++slot)
            ASSERT_EQ_UINT(seen[slot], 1u);
        source_build_fixture_free(&fixture);
        xr_program_source_product_free(&product);
    }
}

TEST_MAIN_BEGIN()
if (argc != 1) {
    if (argc == 3 && strcmp(argv[1], "--run-case") == 0) {
#define SELECT_SOURCE_CASE(name, fixture)                                                          \
    if (strcmp(argv[2], #name) == 0)                                                               \
        selected_source_case = #name;
        XR_SOURCE_CASES(SELECT_SOURCE_CASE)
#undef SELECT_SOURCE_CASE
        if (!selected_source_case) {
            fprintf(stderr, "unknown source case: %s\n", argv[2]);
            return 2;
        }
    } else {
        if (argc != 7 || strcmp(argv[1], "--emit-fixture") != 0 ||
            strcmp(argv[3], "--registry") != 0 || strcmp(argv[4], XR_SOURCE_REGISTRY_ID) != 0 ||
            strcmp(argv[5], "--output") != 0 || argv[6][0] == '\0') {
            fprintf(stderr,
                    "expected --run-case NAME or --emit-fixture ID --registry DIGEST --output "
                    "PATH\n");
            return 2;
        }
#define SELECT_SOURCE_FIXTURE(id, fixture)                                                         \
    if (strcmp(argv[2], #id) == 0)                                                                 \
        selected_source_fixture = fixture;
        XR_SOURCE_FIXTURES(SELECT_SOURCE_FIXTURE)
#undef SELECT_SOURCE_FIXTURE
        if (selected_source_fixture == XR_SOURCE_FIXTURE_NONE) {
            fprintf(stderr, "unknown source fixture: %s\n", argv[2]);
            return 2;
        }
        selected_source_output = argv[6];
    }
}
#define RUN_SOURCE_CASE(name, fixture)                                                             \
    do {                                                                                           \
        if ((selected_source_fixture == XR_SOURCE_FIXTURE_NONE && !selected_source_case) ||        \
            (selected_source_fixture != XR_SOURCE_FIXTURE_NONE &&                                  \
             selected_source_fixture == fixture) ||                                                \
            (selected_source_case && strcmp(selected_source_case, #name) == 0)) {                  \
            RUN_TEST(name);                                                                        \
        }                                                                                          \
    } while (0);
XR_SOURCE_CASES(RUN_SOURCE_CASE)
#undef RUN_SOURCE_CASE
if ((unsigned int) xr_tests_run !=
    (selected_source_fixture == XR_SOURCE_FIXTURE_NONE && !selected_source_case
         ? XR_SOURCE_CASE_COUNT
         : 1u)) {
    fprintf(stderr, "source case registry did not select exactly the expected cases\n");
    return 2;
}
TEST_MAIN_END()
