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
#include "aot/program/xr_backend_ir.h"
#include "aot/program/xr_backend_ir_internal.h"
#include "core/xr_core_spec_gen.h"
#include "shared/xr_assertion_plan.h"
#include "execution/xr_execution.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "os/os_temp.h"
#include "program/xr_program_source_build.h"
#include "program/xr_reference_evaluator.h"
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

static void branching_cleanup_check_reference(XrInstance *instance, uint32_t entry,
                                              BranchingCleanupProbe *probe) {
    for (uint32_t scenario = 0u;
         scenario < sizeof(branching_cleanup_scenarios) / sizeof(branching_cleanup_scenarios[0]);
         ++scenario) {
        const BranchingCleanupScenario *row = &branching_cleanup_scenarios[scenario];
        *probe = (BranchingCleanupProbe) {.refuse_call = row->refuse_call};
        XrReferenceExecution *execution = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
        XrReferenceOutcome outcome = xr_reference_execution_step(execution);
        uint32_t suspension = 0u;
        while (outcome.kind == XR_REFERENCE_OUTCOME_SUSPENDED && suspension < 3u) {
            ASSERT_EQ_UINT(outcome.safepoint_id, suspension);
            outcome = row->cancel_suspension == suspension
                          ? xr_reference_execution_cancel(execution)
                          : xr_reference_execution_step(execution);
            ++suspension;
        }
        xr_reference_execution_free(execution);
        if (row->expected == XR_BACKEND_EXECUTION_RETURN) {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_RETURN);
            ASSERT_EQ_INT(outcome.value.kind, XR_REFERENCE_VALUE_I64);
            ASSERT_EQ_INT(outcome.value.as.i64, 86);
        } else if (row->expected == XR_BACKEND_EXECUTION_CANCELLED) {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_CANCELLED);
        } else {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_TRAP);
            ASSERT_EQ_INT(outcome.trap, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
        }
        branching_cleanup_assert_trace(probe, row->expected_calls);
    }
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

static void nested_cleanup_check_reference(XrInstance *instance, uint32_t entry,
                                           BranchingCleanupProbe *probe) {
    for (uint32_t scenario = 0u;
         scenario < sizeof(nested_cleanup_scenarios) / sizeof(nested_cleanup_scenarios[0]);
         ++scenario) {
        const NestedCleanupScenario *row = &nested_cleanup_scenarios[scenario];
        *probe = (BranchingCleanupProbe) {.refuse_call = row->refuse_call};
        XrReferenceExecution *execution = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
        XrReferenceOutcome outcome = xr_reference_execution_step(execution);
        uint32_t suspension = 0u;
        while (outcome.kind == XR_REFERENCE_OUTCOME_SUSPENDED && suspension < 3u) {
            ASSERT_EQ_UINT(outcome.safepoint_id, suspension);
            outcome = row->cancel_suspension == suspension
                          ? xr_reference_execution_cancel(execution)
                          : xr_reference_execution_step(execution);
            ++suspension;
        }
        xr_reference_execution_free(execution);
        if (row->expected == XR_BACKEND_EXECUTION_RETURN) {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_RETURN);
            ASSERT_EQ_INT(outcome.value.kind, XR_REFERENCE_VALUE_I64);
            ASSERT_EQ_INT(outcome.value.as.i64, 84);
        } else if (row->expected == XR_BACKEND_EXECUTION_CANCELLED) {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_CANCELLED);
        } else {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_TRAP);
            ASSERT_EQ_INT(outcome.trap, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
        }
        nested_cleanup_assert_trace(probe, row->trace);
    }
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

static void child_cleanup_assert_reference(XrReferenceOutcome suspended, XrReferenceOutcome outcome,
                                           const ChildCleanupProbe *probe) {
    ASSERT_EQ_INT(suspended.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(suspended.state_id, 1u);
    ASSERT_EQ_UINT(suspended.safepoint_id, 0u);
    if (probe->mode->expected == XR_BACKEND_EXECUTION_TRAP) {
        ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_TRAP);
        ASSERT_EQ_INT(outcome.trap, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
    } else if (probe->mode->expected == XR_BACKEND_EXECUTION_CANCELLED) {
        ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_CANCELLED);
    } else {
        ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_REFERENCE_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, 43);
    }
    child_cleanup_assert_trace(probe);
}

static void child_cleanup_check_reference(XrInstance *instance, uint32_t entry,
                                          ChildCleanupProbe *probe) {
    for (uint32_t index = 0u; index < sizeof(child_cleanup_modes) / sizeof(child_cleanup_modes[0]);
         ++index) {
        const ChildCleanupMode *mode = &child_cleanup_modes[index];
        *probe = (ChildCleanupProbe) {.mode = mode};
        XrReferenceExecution *execution = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
        XrReferenceOutcome suspended = xr_reference_execution_step(execution);
        uint32_t events_before_resume = probe->count;
        XrReferenceOutcome outcome = suspended;
        if (suspended.kind == XR_REFERENCE_OUTCOME_SUSPENDED)
            outcome = mode->cancel ? xr_reference_execution_cancel(execution)
                                   : xr_reference_execution_step(execution);
        xr_reference_execution_free(execution);
        ASSERT_EQ_UINT(events_before_resume, 0u);
        child_cleanup_assert_reference(suspended, outcome, probe);
    }
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

static void field_ref_cleanup_check_reference(XrInstance *instance, uint32_t entry,
                                              FieldRefCleanupProbe *probe) {
    for (uint32_t index = 0u;
         index < sizeof(field_ref_cleanup_modes) / sizeof(field_ref_cleanup_modes[0]); ++index) {
        const FieldRefCleanupMode *mode = &field_ref_cleanup_modes[index];
        *probe = (FieldRefCleanupProbe) {.mode = mode};
        XrReferenceExecution *execution = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
        XrReferenceOutcome suspended = xr_reference_execution_step(execution);
        ASSERT_EQ_INT(suspended.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(suspended.state_id, 1u);
        ASSERT_EQ_UINT(suspended.safepoint_id, 0u);
        ASSERT_EQ_UINT(probe->count, 0u);
        XrReferenceOutcome outcome = mode->cancel ? xr_reference_execution_cancel(execution)
                                                  : xr_reference_execution_step(execution);
        xr_reference_execution_free(execution);
        if (mode->expected == XR_BACKEND_EXECUTION_RETURN) {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_RETURN);
            ASSERT_EQ_INT(outcome.value.kind, XR_REFERENCE_VALUE_I64);
            ASSERT_EQ_INT(outcome.value.as.i64, INT64_C(6442450958));
        } else if (mode->expected == XR_BACKEND_EXECUTION_CANCELLED) {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_CANCELLED);
        } else {
            ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_TRAP);
            ASSERT_EQ_INT(outcome.trap, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
        }
        field_ref_cleanup_assert_trace(probe);
    }
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
    XrReferenceLifecycleEvent events[128];
    uint32_t count;
    bool overflow;
} SourceClassLifecycleLog;

static void record_source_class_lifecycle(void *context,
                                          const XrReferenceLifecycleEvent *event) {
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
                                             XrReferenceLifecycleEventKind kind,
                                             uint64_t identity) {
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

static void assert_vm_rejects_inactive_operation(XrInstance *instance,
                                                 const XrValidatedProgram *program,
                                                 uint16_t expected_operation) {
    const XrVmDecodePolicy policies[] = {XR_VM_DECODE_BASELINE_VIEW, XR_VM_DECODE_FIXED_ROWS};
    XrVmCodeDiagnostic first = {0};
    for (uint32_t index = 0u; index < sizeof(policies) / sizeof(policies[0]); ++index) {
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.decode_policy = policies[index];
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(instance, &options, &code, &diagnostic),
                      XR_VM_CODE_UNSUPPORTED_OPERATION);
        ASSERT_NULL(code);
        ASSERT_EQ_INT(diagnostic.status, XR_VM_CODE_UNSUPPORTED_OPERATION);
        ASSERT_EQ_UINT(diagnostic.operation_id, expected_operation);
        ASSERT_TRUE(program_instruction_matches(program, diagnostic.function_id,
                                                diagnostic.block_id, diagnostic.instruction_id,
                                                diagnostic.operation_id));
        const XrCoreOperationSpec *spec =
            xr_core_spec_operation_by_id(diagnostic.operation_id);
        ASSERT_NOT_NULL(spec);
        if (spec)
            ASSERT_EQ_INT(spec->vm_status, XR_CORE_COVERAGE_NOT_YET_ACTIVE);
        if (index == 0u)
            first = diagnostic;
        else {
            ASSERT_EQ_UINT(diagnostic.operation_id, first.operation_id);
            ASSERT_EQ_UINT(diagnostic.function_id, first.function_id);
            ASSERT_EQ_UINT(diagnostic.block_id, first.block_id);
            ASSERT_EQ_UINT(diagnostic.instruction_id, first.instruction_id);
        }
    }
}

static void assert_vm_fixture_backend_contract(XrInstance *instance,
                                               const XrValidatedProgram *program,
                                               XrSourceFixtureId fixture) {
    uint16_t expected_operation =
        xr_source_fixture_unsupported_operation(fixture, XR_SOURCE_BACKEND_VM);
    if (expected_operation != 0u) {
        assert_vm_rejects_inactive_operation(instance, program, expected_operation);
        return;
    }

    const XrVmDecodePolicy policies[] = {XR_VM_DECODE_BASELINE_VIEW, XR_VM_DECODE_FIXED_ROWS};
    for (uint32_t index = 0u; index < sizeof(policies) / sizeof(policies[0]); ++index) {
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.decode_policy = policies[index];
        XrVmCodeDiagnostic diagnostic;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(instance, &options, &code, &diagnostic), XR_VM_CODE_OK);
        ASSERT_NOT_NULL(code);
        ASSERT_EQ_INT(diagnostic.status, XR_VM_CODE_OK);
        ASSERT_EQ_INT(xr_vm_code_decode_policy(code), policies[index]);
        xr_vm_code_free(code);
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
                                            diagnostic.instruction_id,
                                            diagnostic.operation_id));
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
    ASSERT_TRUE(xr_backend_ir_translation_validate(ir, &diagnostic));

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

static bool reference_clock_provider_call(void *context, uint32_t requirement_index,
                                          uint32_t operation_index, int64_t *result_out) {
    const XrExecutionLease *lease = context;
    return xr_execution_lease_provider_call_i64_nullary(lease, requirement_index, operation_index,
                                                        result_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_clock_provider_call_unary(void *context, uint32_t requirement_index,
                                                uint32_t operation_index, int64_t argument,
                                                int64_t *result_out) {
    const XrExecutionLease *lease = context;
    return xr_execution_lease_provider_call_i64_unary(lease, requirement_index, operation_index,
                                                      argument,
                                                      result_out) == XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_pipe_provider_call(void *context, uint32_t requirement_index,
                                         uint32_t operation_index, bool *present_out,
                                         int64_t *first_out, int64_t *second_out) {
    const XrExecutionLease *lease = context;
    return xr_execution_lease_provider_call_optional_i64_pair_nullary(
               lease, requirement_index, operation_index, present_out, first_out, second_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
}

static bool reference_pipe_close_provider_call(void *context, uint32_t requirement_index,
                                               uint32_t operation_index, int64_t handle,
                                               bool *result_out) {
    const XrExecutionLease *lease = context;
    return xr_execution_lease_provider_call_bool_i64_unary(lease, requirement_index,
                                                           operation_index, handle, result_out) ==
           XR_EXECUTION_PROVIDER_CALL_OK;
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
                "writer=%u verifier=%u source=%s:%u message=%s\n",
                xr_program_source_build_status_name(status), (unsigned) diagnostic->stage,
                diagnostic->module_index, diagnostic->underlying_status,
                (unsigned) diagnostic->writer_status, (unsigned) diagnostic->verifier_status,
                diagnostic->source_path, diagnostic->source_line, diagnostic->message);
    }
    ASSERT_EQ_INT(status, XR_PROGRAM_SOURCE_BUILD_OK);
}

TEST(source_owner_single_module_is_deterministic_and_detached) {
    static const char source[] =
        "fn answer() -> i64 { return 42 }\n"
        "fn choose(flag: bool) -> i64 { if (flag) { return answer() }; return 0 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    ASSERT_EQ_INT(diagnostic.status, XR_PROGRAM_SOURCE_BUILD_OK);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 1u);

    ASSERT_EQ_INT(xr_test_unlink(fixture.entry_path), 0);
    fixture.entry_path[0] = '\0';
    size_t retained_size = 0u;
    const uint8_t *retained_bytes = xr_validated_program_bytes(first.program, &retained_size);
    ASSERT_NOT_NULL(retained_bytes);
    ASSERT_EQ_UINT(retained_size, first.artifact.size);
    ASSERT_EQ_INT(memcmp(retained_bytes, first.artifact.bytes, retained_size), 0);
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
    source_build_fixture_free(&fixture);
}

TEST(source_owner_two_module_graph_is_deterministic) {
    static const char library_source[] =
        "export fn increment(value: i64) -> i64 { return value + 1 }\n"
        "export fn unused(value: i64) -> i64 { return value + 100 }\n";
    static const char entry_source[] = "import { increment } from \"./library\"\n"
                                       "fn answer() -> i64 { return increment(41) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    assert_products_equal(&first, &second);
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 2u);

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
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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

    XrReferenceExecution *reference = NULL;
    ASSERT_TRUE(
        xr_reference_execution_create(instance, entry_function, NULL, 0u, NULL, &reference));
    XrReferenceOutcome reference_suspend = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_suspend.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(reference_suspend.state_id, 1u);
    reference_suspend = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_suspend.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(reference_suspend.state_id, 1u);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_return.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_return.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_return.value.as.i64, 7);
    xr_reference_execution_free(reference);

    for (uint32_t child_point = 1u; child_point <= 2u; ++child_point) {
        XrReferenceExecution *reference_cancel = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry_function, NULL, 0u, NULL,
                                                  &reference_cancel));
        for (uint32_t step = 0u; step < child_point; ++step)
            ASSERT_EQ_INT(xr_reference_execution_step(reference_cancel).kind,
                          XR_REFERENCE_OUTCOME_SUSPENDED);
        ASSERT_EQ_INT(xr_reference_execution_cancel(reference_cancel).kind,
                      XR_REFERENCE_OUTCOME_CANCELLED);
        xr_reference_execution_free(reference_cancel);
    }

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmExecution *vm_execution = NULL;
    ASSERT_TRUE(xr_vm_execution_create(vm_code, instance, entry_function, NULL, 0u, &vm_execution));
    XrVmOutcome vm_suspend = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, reference_suspend.safepoint_id);
    ASSERT_EQ_UINT(vm_suspend.state_id, reference_suspend.state_id);
    vm_suspend = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, reference_suspend.safepoint_id);
    ASSERT_EQ_UINT(vm_suspend.state_id, reference_suspend.state_id);
    XrVmOutcome vm_return = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_return.value.as.i64, reference_return.value.as.i64);
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
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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

TEST(source_owner_function_parameter_callable_has_one_program_and_private_executors) {
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

    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference =
        xr_reference_evaluate(product.program, entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 48);

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
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm_result = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm_result.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_result.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_result.value.as.i64, reference.value.as.i64);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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
    ASSERT_EQ_UINT(program->function_count, 3u);
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

    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

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
    static const XrVmDecodePolicy policies[] = {
        XR_VM_DECODE_BASELINE_VIEW,
        XR_VM_DECODE_FIXED_ROWS,
    };
    for (uint32_t policy = 0u; policy < sizeof(policies) / sizeof(policies[0]); ++policy) {
        XrVmCodeOptions vm_options = xr_vm_code_default_options();
        vm_options.decode_policy = (uint8_t) policies[policy];
        XrVmCode *vm_code = NULL;
        XrVmCodeDiagnostic vm_diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(instance, &vm_options, &vm_code, &vm_diagnostic),
                      XR_VM_CODE_OK);
        ASSERT_EQ_INT(xr_vm_code_decode_policy(vm_code), policies[policy]);
        XrVmOutcome vm_result = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(vm_result.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_result.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_result.value.as.i64, reference.value.as.i64);
        xr_vm_code_free(vm_code);
    }

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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
    ASSERT_EQ_UINT(program->function_count, 12u);
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

    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

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
    assert_vm_fixture_backend_contract(instance, program,
                                                 XR_SOURCE_FIXTURE_GENERIC_CONSTRAINT_METHOD);
    assert_aot_fixture_backend_contract(program, profile,
                                                  XR_SOURCE_FIXTURE_GENERIC_CONSTRAINT_METHOD);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
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
    XrProgramSourceProduct namespace_product = {0};
    XrProgramSourceDiagnostic namespace_diagnostic;
    assert_source_build_ok(&namespace_fixture.input, &namespace_product, &namespace_diagnostic);
    uint32_t namespace_entry = xr_validated_program_entry_function(namespace_product.program);
    XrReferenceOutcome namespace_reference = xr_reference_evaluate(
        namespace_product.program, namespace_entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(namespace_reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(namespace_reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(namespace_reference.value.as.i64, 42);
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
    uint32_t class_carrier_entry =
        xr_validated_program_entry_function(class_carrier_product.program);
    XrReferenceOutcome class_carrier_reference = xr_reference_evaluate(
        class_carrier_product.program, class_carrier_entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(class_carrier_reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(class_carrier_reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(class_carrier_reference.value.as.i64, 42);
    xr_program_source_product_free(&class_carrier_product);
    source_build_fixture_free(&class_carrier_fixture);

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

TEST(source_owner_generic_value_struct_specializations_are_exact_nominal_aggregates) {
    static const char library_source[] =
        "export struct Box<R> {\n"
        "  value: R\n"
        "  readAs<U>(_marker: U) -> R { return this.value }\n"
        "  forwardAs<U>(marker: U) -> R { return this.readAs<U>(marker) }\n"
        "}\n";
    static const char facade_source[] =
        "export { Box as OriginBox } from \"./library\"\n"
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
    ASSERT_EQ_UINT(program->function_count, 7u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CALL_SEALED_DIRECT), 7u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 0u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_SHARE), 0u);
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
    const XrValidatedType *origin_i64_type =
        xr_validated_program_type(program, aggregate_types[0]);
    const XrValidatedType *decoy_i64_type =
        xr_validated_program_type(program, aggregate_types[2]);
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
    for (uint32_t function_index = 0u; function_index < program->function_count;
         ++function_index) {
        if (function_index == entry)
            continue;
        const XrValidatedFunction *candidate = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < candidate->block_count; ++block_index) {
            const XrValidatedBlock *block = &candidate->blocks[block_index];
            for (uint32_t instruction_index = 0u;
                 instruction_index < block->instruction_count; ++instruction_index) {
                const XrValidatedInstruction *instruction =
                    &block->instructions[instruction_index];
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
    for (uint32_t function_index = 0u; function_index < program->function_count;
         ++function_index) {
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
            for (uint32_t instruction_index = 0u;
                 instruction_index < block->instruction_count; ++instruction_index) {
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

    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

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
    static const XrVmDecodePolicy policies[] = {
        XR_VM_DECODE_BASELINE_VIEW,
        XR_VM_DECODE_FIXED_ROWS,
    };
    for (uint32_t policy = 0u; policy < sizeof(policies) / sizeof(policies[0]); ++policy) {
        XrVmCodeOptions vm_options = xr_vm_code_default_options();
        vm_options.decode_policy = (uint8_t) policies[policy];
        XrVmCode *vm_code = NULL;
        XrVmCodeDiagnostic vm_diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(instance, &vm_options, &vm_code, &vm_diagnostic),
                      XR_VM_CODE_OK);
        ASSERT_NOT_NULL(vm_code);
        XrVmOutcome vm_result = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(vm_result.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_result.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_result.value.as.i64, reference.value.as.i64);
        xr_vm_code_free(vm_code);
    }

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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

    XrReferenceProfile reference_profile = {.pointer_width = 64u};
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, NULL, 0u, &reference_profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

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
    assert_vm_fixture_backend_contract(instance, program,
                                                 XR_SOURCE_FIXTURE_GENERIC_SCALAR_CLASS);
    assert_aot_fixture_backend_contract(program, profile,
                                                  XR_SOURCE_FIXTURE_GENERIC_SCALAR_CLASS);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
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
        ASSERT_TRUE(source_build_fixture_init(&rejected_fixture, managed_struct_sources[source],
                                              NULL));
        XrProgramSourceProduct rejected = {0};
        XrProgramSourceDiagnostic rejection;
        ASSERT_EQ_INT(xr_program_source_build(&rejected_fixture.input, &rejected, &rejection),
                      XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
        ASSERT_EQ_INT(rejection.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
        ASSERT_NULL(rejected.artifact.bytes);
        ASSERT_NULL(rejected.program);
        source_build_fixture_free(&rejected_fixture);
    }

    static const char library_source[] =
        "export class Leaf {\n"
        "  value: i64\n"
        "  constructor(value: i64) { this.value = value }\n"
        "}\n"
        "export class Box<T> {\n"
        "  value: T\n"
        "  constructor(value: move T) { this.value = value }\n"
        "}\n";
    static const char facade_source[] =
        "export class Leaf {\n"
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
            const XrValidatedType *field =
                xr_validated_program_type(program, type->field_types[0]);
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
                const XrValidatedInstruction *instruction =
                    &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CLASS_CONSTRUCT)
                    continue;
                for (uint32_t type = 0u; type < 3u; ++type) {
                    leaf_construct_counts[type] +=
                        instruction->result_type_id == leaf_types[type];
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
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_CLASS_SHARE), 0u);

    uint32_t entry = xr_validated_program_entry_function(program);
    XrReferenceOutcome reference =
        xr_reference_evaluate(program, entry, NULL, 0u, NULL, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    source_build_fixture_free(&fixture);
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
    const XrValidatedType *class_type =
        xr_validated_program_type(product.program, class_type_id);
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
    enum { CLASS_COUNT = 80, SOURCE_CAPACITY = 32768 };
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
    static const char source[] =
        "class Cell {\n"
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
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 3u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_SHARE), 1u);
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
                for (uint32_t candidate_index = 0u;
                     candidate_index < candidate->instruction_count; ++candidate_index) {
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
    XrReferenceProviderBinding binding = {
        .lifecycle_context = &log,
        .lifecycle_event = record_source_class_lifecycle,
    };
    XrReferenceOutcome reference = xr_reference_evaluate_bound(
        product.program, entry, NULL, 0u, NULL, NULL, &binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);
    ASSERT_TRUE(!log.overflow);
    uint32_t distinct_exchange = UINT32_MAX;
    for (uint32_t index = 0u; index < log.count; ++index) {
        ASSERT_TRUE(log.events[index].origin != XR_REFERENCE_EVENT_ORIGIN_DOMAIN_TEARDOWN);
        if (log.events[index].kind == XR_REFERENCE_EVENT_PLACE_EXCHANGE &&
            log.events[index].identity != UINT64_MAX &&
            log.events[index].identity != log.events[index].related_identity)
            distinct_exchange = index;
    }
    ASSERT_TRUE(distinct_exchange != UINT32_MAX);
    ASSERT_LT(distinct_exchange + 1u, log.count);
    const XrReferenceLifecycleEvent *exchange_event = &log.events[distinct_exchange];
    const XrReferenceLifecycleEvent *drop_event = &log.events[distinct_exchange + 1u];
    ASSERT_EQ_INT(drop_event->kind, XR_REFERENCE_EVENT_OWNER_DROP);
    ASSERT_EQ_INT(drop_event->origin, XR_REFERENCE_EVENT_ORIGIN_PROGRAM_OPERATION);
    ASSERT_EQ_UINT(drop_event->identity, exchange_event->identity);
    ASSERT_EQ_UINT(source_class_lifecycle_count(
                       &log, XR_REFERENCE_EVENT_CLASS_FINALIZE, exchange_event->identity),
                   1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(
                       &log, XR_REFERENCE_EVENT_CLASS_RECLAIM, exchange_event->identity),
                   1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(
                       &log, XR_REFERENCE_EVENT_CLASS_FINALIZE,
                       exchange_event->related_identity),
                   1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(
                       &log, XR_REFERENCE_EVENT_CLASS_RECLAIM,
                       exchange_event->related_identity),
                   1u);

    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_class_field_self_assignment_shares_before_exchange) {
    static const char source[] =
        "class Cell {\n"
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
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_SHARE), 1u);
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
            ASSERT_EQ_UINT(share->operation_id, XR_CORE_OP_CORE_CLASS_SHARE);
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
                for (uint32_t candidate_index = 0u;
                     candidate_index < candidate->instruction_count; ++candidate_index) {
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
    XrReferenceProviderBinding binding = {
        .lifecycle_context = &log,
        .lifecycle_event = record_source_class_lifecycle,
    };
    XrReferenceOutcome reference = xr_reference_evaluate_bound(
        product.program, entry, NULL, 0u, NULL, NULL, &binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);
    ASSERT_TRUE(!log.overflow);
    uint32_t share_event = UINT32_MAX;
    for (uint32_t index = 0u; index < log.count; ++index) {
        ASSERT_TRUE(log.events[index].origin != XR_REFERENCE_EVENT_ORIGIN_DOMAIN_TEARDOWN);
        if (log.events[index].kind == XR_REFERENCE_EVENT_CLASS_SHARE)
            share_event = index;
    }
    ASSERT_TRUE(share_event != UINT32_MAX);
    ASSERT_LT(share_event + 3u, log.count);
    ASSERT_EQ_INT(log.events[share_event + 1u].kind, XR_REFERENCE_EVENT_CLASS_FIELD_PLACE);
    ASSERT_EQ_INT(log.events[share_event + 2u].kind, XR_REFERENCE_EVENT_PLACE_EXCHANGE);
    ASSERT_EQ_INT(log.events[share_event + 3u].kind, XR_REFERENCE_EVENT_OWNER_DROP);
    ASSERT_EQ_UINT(log.events[share_event].identity,
                   log.events[share_event + 2u].identity);
    ASSERT_EQ_UINT(log.events[share_event].identity,
                   log.events[share_event + 2u].related_identity);
    ASSERT_EQ_UINT(log.events[share_event].identity,
                   log.events[share_event + 3u].identity);
    ASSERT_EQ_UINT(source_class_lifecycle_count(
                       &log, XR_REFERENCE_EVENT_CLASS_FINALIZE,
                       log.events[share_event].identity),
                   1u);
    ASSERT_EQ_UINT(source_class_lifecycle_count(
                       &log, XR_REFERENCE_EVENT_CLASS_RECLAIM,
                       log.events[share_event].identity),
                   1u);

    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_class_alias_borrows_coalesce_without_share) {
    static const char source[] =
        "class Cell {\n"
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
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_SHARE), 0u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_PLACE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_PLACE_EXCHANGE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_LOAD), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);

    XrReferenceProfile profile = {.pointer_width = 64u};
    uint32_t entry = xr_validated_program_entry_function(product.program);
    XrReferenceOutcome reference =
        xr_reference_evaluate(product.program, entry, NULL, 0u, &profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

TEST(source_owner_class_alias_final_transfer_coalesces_to_move) {
    static const char source[] =
        "class Cell {\n"
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
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_CONSTRUCT), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_SHARE), 0u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_MOVE), 1u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_CLASS_FIELD_LOAD), 2u);
    ASSERT_EQ_UINT(program_operation_count(product.program, XR_CORE_OP_CORE_OWNER_COPY), 0u);

    XrReferenceProfile profile = {.pointer_width = 64u};
    uint32_t entry = xr_validated_program_entry_function(product.program);
    XrReferenceOutcome reference =
        xr_reference_evaluate(product.program, entry, NULL, 0u, &profile, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

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
    ASSERT_EQ_UINT(owner_forward_branch->operands[0], sealed_normal_owner);
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
    for (uint32_t live = 0u; live < indirect_point->live_value_count; ++live) {
        uint32_t value = indirect_point->live_value_ids[live];
        ASSERT_LT(value, entry_function->value_count);
        ASSERT_EQ_UINT(entry_indirect_call->operands[2u + live], value);
        indirect_owner_lives +=
            entry_function->value_ownerships[value] == XR_CORE_IR_OWNER ? 1u : 0u;
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
    ASSERT_EQ_INT(indirect_normal->argument_ownerships[1], XR_CORE_IR_OWNER);
    ASSERT_EQ_UINT(indirect_cancel->argument_count, 1u);
    ASSERT_EQ_INT(indirect_cancel->argument_ownerships[0], XR_CORE_IR_OWNER);
    uint32_t indirect_normal_owner_drops = 0u;
    for (uint32_t instruction = 0u; instruction < indirect_normal->instruction_count;
         ++instruction) {
        const XrValidatedInstruction *candidate = &indirect_normal->instructions[instruction];
        indirect_normal_owner_drops +=
            candidate->operation_id == XR_CORE_OP_CORE_OWNER_DROP &&
                    candidate->operand_count == 1u &&
                    candidate->operands[0] == indirect_normal->argument_ids[1]
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

    XrReferenceExecution *reference = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
    XrReferenceOutcome reference_first_suspended = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_first_suspended.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_first_suspended.safepoint_id, sealed_safepoint_id);
    ASSERT_EQ_UINT(reference_first_suspended.state_id, sealed_point->resume_state_id);
    XrReferenceOutcome reference_second_suspended = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_second_suspended.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_second_suspended.safepoint_id, indirect_safepoint_id);
    ASSERT_EQ_UINT(reference_second_suspended.state_id, indirect_point->resume_state_id);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_return.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_return.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_return.value.as.i64, 42);
    xr_reference_execution_free(reference);

    XrReferenceExecution *reference_cancel = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference_cancel));
    ASSERT_EQ_INT(xr_reference_execution_step(reference_cancel).kind,
                  XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_reference_execution_cancel(reference_cancel).kind,
                  XR_REFERENCE_OUTCOME_CANCELLED);
    xr_reference_execution_free(reference_cancel);

    XrReferenceExecution *reference_second_cancel = NULL;
    ASSERT_TRUE(
        xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference_second_cancel));
    ASSERT_EQ_INT(xr_reference_execution_step(reference_second_cancel).kind,
                  XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_reference_execution_step(reference_second_cancel).kind,
                  XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_reference_execution_cancel(reference_second_cancel).kind,
                  XR_REFERENCE_OUTCOME_CANCELLED);
    xr_reference_execution_free(reference_second_cancel);

    const XrVmDecodePolicy policies[] = {
        XR_VM_DECODE_BASELINE_VIEW,
        XR_VM_DECODE_FIXED_ROWS,
    };
    for (uint32_t policy = 0u; policy < 2u; ++policy) {
        XrVmCodeOptions vm_options = xr_vm_code_default_options();
        vm_options.decode_policy = policies[policy];
        XrVmCodeDiagnostic vm_diagnostic;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(instance, &vm_options, &code, &vm_diagnostic),
                      XR_VM_CODE_OK);
        ASSERT_NOT_NULL(code);
        ASSERT_EQ_INT(xr_vm_code_decode_policy(code), policies[policy]);

        XrVmExecution *vm = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm));
        XrVmOutcome vm_first_suspended = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_first_suspended.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(vm_first_suspended.safepoint_id, reference_first_suspended.safepoint_id);
        ASSERT_EQ_UINT(vm_first_suspended.state_id, reference_first_suspended.state_id);
        XrVmOutcome vm_second_suspended = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_second_suspended.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(vm_second_suspended.safepoint_id, reference_second_suspended.safepoint_id);
        ASSERT_EQ_UINT(vm_second_suspended.state_id, reference_second_suspended.state_id);
        XrVmOutcome vm_return = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(vm_return.value.as.i64, reference_return.value.as.i64);
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
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 5u);
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
            if (stable_id_equal(requirement.operation_ids[operation_index], expected_operations[i]))
                expected_index = i;
        }
        ASSERT_LT(expected_index, 4u);
        const XrTargetProviderOperationContract *contract_operation =
            find_profile_provider_operation(contract, requirement.operation_ids[operation_index]);
        ASSERT_NOT_NULL(contract_operation);
        ASSERT_EQ_INT(contract_operation->call_abi.result.value_kind,
                      XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER);
        operations[operation_index].operation_id = requirement.operation_ids[operation_index];
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

    XrExecutionLease lease = {0};
    ASSERT_TRUE(xr_execution_instance_acquire(instance, &lease));
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .call_i64_unary = reference_clock_provider_call_unary,
        .call_i64_nullary = reference_clock_provider_call,
    };
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(first.program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 1);
    for (uint32_t i = 0u; i < 4u; ++i)
        ASSERT_EQ_UINT(probes[i].calls, 1u);
    ASSERT_TRUE(xr_execution_lease_release(&lease));

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm.value.as.i64, reference.value.as.i64);
    for (uint32_t i = 0u; i < 4u; ++i)
        ASSERT_EQ_UINT(probes[i].calls, 2u);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_clock_nullary"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_realtime_nanos"));
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
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE, 3u),
        1u);
    ASSERT_EQ_UINT(
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_WITNESS_INVOKE, 3u),
        1u);
    ASSERT_EQ_UINT(
        program_operation_successor_count(first.program, XR_CORE_OP_CORE_CALL_SEALED_INVOKE, 3u),
        1u);
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_TRAP), 7u);
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
        operations[requirement_index].operation_id = requirement.operation_ids[0];
        operations[requirement_index].context = &probe;
        if (stable_id_equal(requirement.contract_id, expected_clock_contract)) {
            ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], expected_clock_operation));
            clock_requirement = requirement_index;
            operations[requirement_index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[requirement_index].entry.i64_nullary = provider_trap_clock_probe;
        } else {
            ASSERT_TRUE(stable_id_equal(requirement.contract_id, expected_io_contract));
            ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], expected_close_operation));
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

    XrExecutionLease lease = {0};
    ASSERT_TRUE(xr_execution_instance_acquire(instance, &lease));
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .call_i64_nullary = reference_clock_provider_call,
        .call_bool_i64_unary = reference_pipe_close_provider_call,
    };
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(first.program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_TRAP);
    ASSERT_EQ_INT(reference.trap, XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED);
    ASSERT_EQ_UINT(probe.events, 5u);
    ASSERT_EQ_UINT(probe.clock_calls, 1u);
    ASSERT_EQ_UINT(probe.close_calls, 4u);
    xr_reference_outcome_dispose(&reference);
    ASSERT_TRUE(xr_execution_lease_release(&lease));

    ProviderTrapCleanupProbe reference_probe = probe;
    assert_vm_fixture_backend_contract(instance, first.program,
                                                 XR_SOURCE_FIXTURE_PROVIDER_TRAP_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_fixture_backend_contract(first.program, profile,
                                                  XR_SOURCE_FIXTURE_PROVIDER_TRAP_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
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
                                                     XR_CORE_OP_CORE_COROUTINE_CALL_SEALED, 3u),
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
        operations[index].operation_id = requirement.operation_ids[0];
        operations[index].context = &probe;
        if (stable_id_equal(requirement.contract_id, clock_contract)) {
            ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], clock_operation));
            clock_requirement = index;
            operations[index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[index].entry.i64_nullary = child_cleanup_clock_probe;
        } else {
            ASSERT_TRUE(stable_id_equal(requirement.contract_id, io_contract));
            ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], close_operation));
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
    child_cleanup_check_reference(instance, entry, &probe);
    ChildCleanupProbe reference_probe = probe;
    assert_vm_fixture_backend_contract(
        instance, product.program, XR_SOURCE_FIXTURE_CHILD_COROUTINE_TRAP_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_fixture_backend_contract(
        product.program, profile, XR_SOURCE_FIXTURE_CHILD_COROUTINE_TRAP_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
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
    ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], close_operation));
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
    branching_cleanup_check_reference(instance, entry, &probe);
    BranchingCleanupProbe reference_probe = probe;
    assert_vm_fixture_backend_contract(instance, product.program,
                                                 XR_SOURCE_FIXTURE_BRANCHING_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_fixture_backend_contract(product.program, profile,
                                                  XR_SOURCE_FIXTURE_BRANCHING_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
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
    ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], close_operation));
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
    nested_cleanup_check_reference(instance, entry, &probe);
    BranchingCleanupProbe reference_probe = probe;
    assert_vm_fixture_backend_contract(instance, product.program,
                                                 XR_SOURCE_FIXTURE_NESTED_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_fixture_backend_contract(product.program, profile,
                                                  XR_SOURCE_FIXTURE_NESTED_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
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
    ASSERT_EQ_UINT(xr_validated_program_function_count(first.program), 9u);
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
    ASSERT_EQ_UINT(sealed_calls, 10u);
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
        if (stable_id_equal(requirement.operation_ids[operation_index], expected_open))
            open_index = operation_index;
        else if (stable_id_equal(requirement.operation_ids[operation_index], expected_close))
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
        operations[operation_index].operation_id = requirement.operation_ids[operation_index];
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

    XrExecutionLease lease = {0};
    ASSERT_TRUE(xr_execution_instance_acquire(instance, &lease));
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .call_bool_i64_unary = reference_pipe_close_provider_call,
        .call_optional_i64_pair_nullary = reference_pipe_provider_call,
    };
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(first.program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 1);
    ASSERT_EQ_UINT(probe.open_calls, 1u);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    ASSERT_TRUE(xr_execution_lease_release(&lease));

    assert_vm_fixture_backend_contract(instance, first.program,
                                                 XR_SOURCE_FIXTURE_PIPE_PROVIDER);
    ASSERT_EQ_UINT(probe.open_calls, 1u);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    assert_aot_fixture_backend_contract(first.program, profile,
                                                  XR_SOURCE_FIXTURE_PIPE_PROVIDER);
    ASSERT_EQ_UINT(probe.open_calls, 1u);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
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
    ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], expected_close));

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
        .operation_id = requirement.operation_ids[0],
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

    XrExecutionLease lease = {0};
    ASSERT_TRUE(xr_execution_instance_acquire(instance, &lease));
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .call_bool_i64_unary = reference_pipe_close_provider_call,
    };
    XrReferenceOutcome reference =
        xr_reference_evaluate_bound(first.program, entry, NULL, 0u, NULL, NULL, &reference_binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 1);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    ASSERT_TRUE(xr_execution_lease_release(&lease));

    assert_vm_fixture_backend_contract(instance, first.program,
                                                 XR_SOURCE_FIXTURE_PIPE_CLOSE_FAILURE);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    assert_aot_fixture_backend_contract(first.program, profile,
                                                  XR_SOURCE_FIXTURE_PIPE_CLOSE_FAILURE);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

static void assert_uncaught_reference_error(XrValidatedProgram *program, XrInstance *instance,
                                            uint32_t entry, uint16_t error_type_id,
                                            PipeProviderProbe *probe) {
    XrExecutionLease lease = {0};
    ASSERT_TRUE(xr_execution_instance_acquire(instance, &lease));
    XrReferenceProviderBinding binding = {
        .context = &lease,
        .call_bool_i64_unary = reference_pipe_close_provider_call,
    };
    XrReferenceOutcome outcome =
        xr_reference_evaluate_bound(program, entry, NULL, 0u, NULL, NULL, &binding);
    ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_ERROR);
    XrReferenceAggregateView error = {0};
    ASSERT_TRUE(xr_reference_value_aggregate_view(&outcome.error_value, &error));
    ASSERT_EQ_UINT(error.type_id, error_type_id);
    ASSERT_EQ_UINT(error.variant_ordinal, 0u);
    ASSERT_EQ_UINT(error.field_count, 1u);
    ASSERT_EQ_INT(error.fields[0].kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(error.fields[0].as.i64, -2);
    ASSERT_EQ_UINT(probe->close_calls, 2u);
    xr_reference_outcome_dispose(&outcome);
    ASSERT_TRUE(xr_execution_lease_release(&lease));
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
        .operation_id = requirement.operation_ids[0],
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

    assert_uncaught_reference_error(first.program, instance, entry, entry_function->error_type_id,
                                    &probe);
    assert_vm_fixture_backend_contract(instance, first.program,
                                                 XR_SOURCE_FIXTURE_PIPE_UNCAUGHT_ERROR);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    assert_aot_fixture_backend_contract(first.program, profile,
                                                  XR_SOURCE_FIXTURE_PIPE_UNCAUGHT_ERROR);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
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
    PipeProviderProbe probe = {
        .read_handle = 2147483646,
        .write_handle = 2147483647,
        .close_results = {true, true},
    };
    XrProviderOperationBinding operation = {
        .operation_id = requirement.operation_ids[0],
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

    XrExecutionLease lease = {0};
    ASSERT_TRUE(xr_execution_instance_acquire(instance, &lease));
    XrReferenceProviderBinding reference_binding = {
        .context = &lease,
        .call_bool_i64_unary = reference_pipe_close_provider_call,
    };
    XrReferenceOutcome reference = xr_reference_evaluate_bound(product.program, entry, NULL, 0u,
                                                               NULL, NULL, &reference_binding);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_PANIC);
    ASSERT_EQ_INT(reference.panic_value.kind, XR_REFERENCE_VALUE_PANIC_INFO);
    ASSERT_EQ_UINT(reference.panic_value.as.panic_info, XR_ASSERTION_FAILURE_CONDITION_FALSE);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    xr_reference_outcome_dispose(&reference);
    ASSERT_TRUE(xr_execution_lease_release(&lease));

    PipeProviderProbe reference_probe = probe;
    assert_vm_rejects_inactive_operation(instance, product.program,
                                         XR_CORE_OP_CORE_CLASS_FIELD_LOAD);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_rejects_inactive_operation(product.program, profile,
                                          XR_CORE_OP_CORE_CLASS_FIELD_LOAD);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);

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
        .operation_id = requirement.operation_ids[0],
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

    XrReferenceExecution *reference = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
    ASSERT_EQ_INT(xr_reference_execution_step(reference).kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_return.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_return.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_return.value.as.i64, 42);
    ASSERT_EQ_UINT(probe.close_calls, 2u);
    xr_reference_execution_free(reference);

    XrReferenceExecution *reference_cancel = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference_cancel));
    ASSERT_EQ_INT(xr_reference_execution_step(reference_cancel).kind,
                  XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_reference_execution_cancel(reference_cancel).kind,
                  XR_REFERENCE_OUTCOME_CANCELLED);
    ASSERT_EQ_UINT(probe.close_calls, 4u);
    xr_reference_execution_free(reference_cancel);

    PipeProviderProbe reference_probe = probe;
    assert_vm_fixture_backend_contract(instance, first.program,
                                                 XR_SOURCE_FIXTURE_PIPE_CANCEL_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_fixture_backend_contract(first.program, profile,
                                                  XR_SOURCE_FIXTURE_PIPE_CANCEL_CLEANUP);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);

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

    XrReferenceExecution *reference = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
    XrReferenceOutcome reference_outcome = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_outcome.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_outcome.state_id, 1u);
    ASSERT_EQ_UINT(reference_outcome.safepoint_id, 0u);
    reference_outcome = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_outcome.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_outcome.state_id, 2u);
    ASSERT_EQ_UINT(reference_outcome.safepoint_id, 1u);
    reference_outcome = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_outcome.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_outcome.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_outcome.value.as.i64, 42);
    xr_reference_execution_free(reference);

    for (uint32_t cancel_state = 1u; cancel_state <= 2u; ++cancel_state) {
        XrReferenceExecution *cancelled = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &cancelled));
        for (uint32_t step = 0u; step < cancel_state; ++step) {
            reference_outcome = xr_reference_execution_step(cancelled);
            ASSERT_EQ_INT(reference_outcome.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
        }
        reference_outcome = xr_reference_execution_cancel(cancelled);
        ASSERT_EQ_INT(reference_outcome.kind, XR_REFERENCE_OUTCOME_CANCELLED);
        ASSERT_EQ_UINT(reference_outcome.state_id, cancel_state);
        xr_reference_execution_free(cancelled);
    }

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &code, &vm_diagnostic), XR_VM_CODE_OK);
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
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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
                    "    xr_aot_entry_coroutine_frame_initialize(&resumed);\n"
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
                    "        xr_aot_entry_coroutine_frame_initialize(&cancelled);\n"
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
    ASSERT_EQ_UINT(coroutine_call->successor_count, 3u);
    ASSERT_EQ_UINT(coroutine_call->operand_count,
                   child->parameter_count + parent_point->live_value_count + 2u);
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
        const XrValidatedInstruction *definition =
            &function->blocks[block].instructions[position];
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
            live_occurrences +=
                parent_point->live_value_ids[live] == storage_roots[parameter];
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
        operations[index].operation_id = requirement.operation_ids[0];
        operations[index].context = &cleanup_probe;
        if (stable_id_equal(requirement.contract_id, clock_contract)) {
            ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], clock_operation));
            clock_requirement = index;
            operations[index].trampoline_kind = XR_PROVIDER_TRAMPOLINE_I64_NULLARY;
            operations[index].entry.i64_nullary = field_ref_cleanup_clock_probe;
        } else {
            ASSERT_TRUE(stable_id_equal(requirement.contract_id, io_contract));
            ASSERT_TRUE(stable_id_equal(requirement.operation_ids[0], close_operation));
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

    field_ref_cleanup_check_reference(instance, entry, &cleanup_probe);
    FieldRefCleanupProbe reference_probe = cleanup_probe;
    assert_vm_fixture_backend_contract(instance, first.program,
                                                 XR_SOURCE_FIXTURE_REF_PARAMETER_COROUTINE);
    ASSERT_EQ_INT(memcmp(&cleanup_probe, &reference_probe, sizeof(cleanup_probe)), 0);

    assert_aot_fixture_backend_contract(first.program, profile,
                                                  XR_SOURCE_FIXTURE_REF_PARAMETER_COROUTINE);
    ASSERT_EQ_INT(memcmp(&cleanup_probe, &reference_probe, sizeof(cleanup_probe)), 0);

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

static bool run_read_existential_coroutine_reference(XrValidatedProgram *program,
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
    XrReferenceExecution *reference = NULL;
    XrReferenceExecution *reference_cancel = NULL;
    XrReferenceOutcome reference_outcome;
    if (!xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference))
        goto cleanup;
    reference_outcome = xr_reference_execution_step(reference);
    if (reference_outcome.kind != XR_REFERENCE_OUTCOME_SUSPENDED ||
        reference_outcome.state_id != 1u || reference_outcome.safepoint_id != 0u)
        goto cleanup;
    reference_outcome = xr_reference_execution_step(reference);
    if (reference_outcome.kind != XR_REFERENCE_OUTCOME_RETURN ||
        reference_outcome.value.kind != XR_REFERENCE_VALUE_I64 ||
        reference_outcome.value.as.i64 != 42)
        goto cleanup;
    xr_reference_execution_free(reference);
    reference = NULL;
    if (!xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference_cancel) ||
        xr_reference_execution_step(reference_cancel).kind != XR_REFERENCE_OUTCOME_SUSPENDED ||
        xr_reference_execution_cancel(reference_cancel).kind != XR_REFERENCE_OUTCOME_CANCELLED)
        goto cleanup;
    assert_vm_fixture_backend_contract(
        instance, program, XR_SOURCE_FIXTURE_READ_EXISTENTIAL_COROUTINE);
    assert_aot_fixture_backend_contract(
        program, profile, XR_SOURCE_FIXTURE_READ_EXISTENTIAL_COROUTINE);
    ok = true;
cleanup:
    if (reference)
        xr_reference_execution_free(reference);
    if (reference_cancel)
        xr_reference_execution_free(reference_cancel);
    bool lifecycle_ok = xr_execution_instance_begin_drain(instance, &diagnostic) == XR_EXECUTION_OK;
    lifecycle_ok =
        xr_execution_instance_retire(instance, &diagnostic) == XR_EXECUTION_OK && lifecycle_ok;
    lifecycle_ok =
        xr_execution_instance_free(&instance, &diagnostic) == XR_EXECUTION_OK && lifecycle_ok;
    return ok && lifecycle_ok;
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
    ASSERT_TRUE(run_read_existential_coroutine_reference(first.program, profile, probe.entry));
    ASSERT_TRUE(source_fixture_output_path(XR_SOURCE_FIXTURE_READ_EXISTENTIAL_COROUTINE) ==
                selected_source_output);
cleanup:
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}

static const uint32_t affine_coroutine_states[] = {1u, 1u, 2u, 3u};
static const uint32_t affine_coroutine_safepoints[] = {0u, 0u, 1u, 2u};
static const uint32_t affine_coroutine_closes_before[] = {0u, 0u, 2u, 2u};
static const int64_t affine_coroutine_close_handles[] = {
    2147483644,
    2147483645,
    2147483646,
    2147483647,
};

static void assert_affine_reference_executions(XrInstance *instance, uint32_t entry,
                                               PipeCloseSequenceProbe *probe) {
    pipe_close_sequence_reset(probe, affine_coroutine_close_handles, 4u);
    XrReferenceExecution *execution = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
    for (uint32_t step = 0u; step < 4u; ++step) {
        XrReferenceOutcome outcome = xr_reference_execution_step(execution);
        ASSERT_EQ_INT(outcome.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(outcome.state_id, affine_coroutine_states[step]);
        ASSERT_EQ_UINT(outcome.safepoint_id, affine_coroutine_safepoints[step]);
        ASSERT_EQ_UINT(probe->calls, affine_coroutine_closes_before[step]);
    }
    XrReferenceOutcome returned = xr_reference_execution_step(execution);
    ASSERT_EQ_INT(returned.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(returned.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(returned.value.as.i64, 42);
    ASSERT_EQ_UINT(probe->calls, 4u);
    xr_reference_execution_free(execution);
    ASSERT_EQ_UINT(probe->calls, 4u);

    for (uint32_t cancel_after = 1u; cancel_after <= 4u; ++cancel_after) {
        uint32_t expected_closes = cancel_after <= 2u ? 2u : 4u;
        pipe_close_sequence_reset(probe, affine_coroutine_close_handles, expected_closes);
        execution = NULL;
        ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &execution));
        for (uint32_t step = 0u; step < cancel_after; ++step) {
            XrReferenceOutcome suspended = xr_reference_execution_step(execution);
            ASSERT_EQ_INT(suspended.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
            ASSERT_EQ_UINT(suspended.state_id, affine_coroutine_states[step]);
            ASSERT_EQ_UINT(suspended.safepoint_id, affine_coroutine_safepoints[step]);
            ASSERT_EQ_UINT(probe->calls, affine_coroutine_closes_before[step]);
        }
        XrReferenceOutcome cancelled = xr_reference_execution_cancel(execution);
        ASSERT_EQ_INT(cancelled.kind, XR_REFERENCE_OUTCOME_CANCELLED);
        ASSERT_EQ_UINT(cancelled.state_id, affine_coroutine_states[cancel_after - 1u]);
        ASSERT_EQ_UINT(probe->calls, expected_closes);
        xr_reference_execution_free(execution);
        ASSERT_EQ_UINT(probe->calls, expected_closes);
    }
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
        .operation_id = requirement.operation_ids[0],
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

    assert_affine_reference_executions(instance, entry, &probe);
    PipeCloseSequenceProbe reference_probe = probe;
    assert_vm_fixture_backend_contract(instance, product.program,
                                                 XR_SOURCE_FIXTURE_AFFINE_COROUTINE_RESULT);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);
    assert_aot_fixture_backend_contract(product.program, profile,
                                                  XR_SOURCE_FIXTURE_AFFINE_COROUTINE_RESULT);
    ASSERT_EQ_INT(memcmp(&probe, &reference_probe, sizeof(probe)), 0);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
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

    XrReferenceExecution *reference = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
    XrReferenceOutcome reference_suspend = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_suspend.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(reference_suspend.state_id, 1u);
    ASSERT_EQ_UINT(reference_suspend.suspension.kind, XR_SUSPENSION_REQUEST_TIMER_AFTER_MS);
    ASSERT_EQ_UINT(reference_suspend.suspension.operand_count, 1u);
    ASSERT_EQ_INT(reference_suspend.suspension.payload.timer_after_ms, 10);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_return.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_return.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_return.value.as.i64, 239);
    xr_reference_execution_free(reference);

    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference));
    ASSERT_EQ_INT(xr_reference_execution_step(reference).kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_reference_execution_cancel(reference).kind, XR_REFERENCE_OUTCOME_CANCELLED);
    xr_reference_execution_free(reference);

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmExecution *vm = NULL;
    ASSERT_TRUE(xr_vm_execution_create(vm_code, instance, entry, NULL, 0u, &vm));
    XrVmOutcome vm_suspend = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, reference_suspend.safepoint_id);
    ASSERT_EQ_UINT(vm_suspend.state_id, reference_suspend.state_id);
    ASSERT_EQ_UINT(vm_suspend.suspension.kind, reference_suspend.suspension.kind);
    ASSERT_EQ_UINT(vm_suspend.suspension.operand_count, reference_suspend.suspension.operand_count);
    ASSERT_EQ_INT(vm_suspend.suspension.payload.timer_after_ms,
                  reference_suspend.suspension.payload.timer_after_ms);
    XrVmOutcome vm_return = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_return.value.as.i64, reference_return.value.as.i64);
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
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
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
        if (strcmp(argv[2], #id) == 0)                                                             \
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
        if ((selected_source_fixture == XR_SOURCE_FIXTURE_NONE && !selected_source_case) ||         \
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
