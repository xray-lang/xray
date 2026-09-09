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
#include "shared/xr_assertion_plan.h"
#include "execution/xr_execution.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "program/xr_program_source_build.h"
#include "program/xr_reference_evaluator.h"
#include "program/xr_validated_program_internal.h"
#include "runtime/abi/xr_builtin_provider_contract.h"
#include "runtime/abi/xr_runtime_target_profile.h"
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

static void branching_cleanup_check_vm(XrInstance *instance, uint32_t entry,
                                       BranchingCleanupProbe *probe, XrVmDecodePolicy policy) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    options.decode_policy = (uint8_t) policy;
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, &options, &code, &diagnostic), XR_VM_CODE_OK);
    for (uint32_t scenario = 0u;
         scenario < sizeof(branching_cleanup_scenarios) / sizeof(branching_cleanup_scenarios[0]);
         ++scenario) {
        const BranchingCleanupScenario *row = &branching_cleanup_scenarios[scenario];
        *probe = (BranchingCleanupProbe) {.refuse_call = row->refuse_call};
        XrVmExecution *execution = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        XrVmOutcome outcome = xr_vm_execution_step(execution);
        uint32_t suspension = 0u;
        while (outcome.kind == XR_VM_OUTCOME_SUSPENDED && suspension < 3u) {
            ASSERT_EQ_UINT(outcome.safepoint_id, suspension);
            outcome = row->cancel_suspension == suspension ? xr_vm_execution_cancel(execution)
                                                           : xr_vm_execution_step(execution);
            ++suspension;
        }
        xr_vm_execution_free(execution);
        if (row->expected == XR_BACKEND_EXECUTION_RETURN) {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
            ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
            ASSERT_EQ_INT(outcome.value.as.i64, 86);
        } else if (row->expected == XR_BACKEND_EXECUTION_CANCELLED) {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_CANCELLED);
        } else {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_TRAP);
            ASSERT_EQ_INT(outcome.trap, XR_VM_TRAP_PROVIDER_CALL_FAILED);
        }
        branching_cleanup_assert_trace(probe, row->expected_calls);
    }
    xr_vm_code_free(code);
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

static void nested_cleanup_check_vm(XrInstance *instance, uint32_t entry,
                                    BranchingCleanupProbe *probe, XrVmDecodePolicy policy) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    options.decode_policy = (uint8_t) policy;
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, &options, &code, &diagnostic), XR_VM_CODE_OK);
    for (uint32_t scenario = 0u;
         scenario < sizeof(nested_cleanup_scenarios) / sizeof(nested_cleanup_scenarios[0]);
         ++scenario) {
        const NestedCleanupScenario *row = &nested_cleanup_scenarios[scenario];
        *probe = (BranchingCleanupProbe) {.refuse_call = row->refuse_call};
        XrVmExecution *execution = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        XrVmOutcome outcome = xr_vm_execution_step(execution);
        uint32_t suspension = 0u;
        while (outcome.kind == XR_VM_OUTCOME_SUSPENDED && suspension < 3u) {
            ASSERT_EQ_UINT(outcome.safepoint_id, suspension);
            outcome = row->cancel_suspension == suspension ? xr_vm_execution_cancel(execution)
                                                           : xr_vm_execution_step(execution);
            ++suspension;
        }
        xr_vm_execution_free(execution);
        if (row->expected == XR_BACKEND_EXECUTION_RETURN) {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
            ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
            ASSERT_EQ_INT(outcome.value.as.i64, 84);
        } else if (row->expected == XR_BACKEND_EXECUTION_CANCELLED) {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_CANCELLED);
        } else {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_TRAP);
            ASSERT_EQ_INT(outcome.trap, XR_VM_TRAP_PROVIDER_CALL_FAILED);
        }
        nested_cleanup_assert_trace(probe, row->trace);
    }
    xr_vm_code_free(code);
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

static void child_cleanup_assert_vm(XrVmOutcome suspended, XrVmOutcome outcome,
                                    const ChildCleanupProbe *probe, uint32_t events_before_resume) {
    ASSERT_EQ_UINT(events_before_resume, 0u);
    ASSERT_EQ_INT(suspended.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(suspended.state_id, 1u);
    ASSERT_EQ_UINT(suspended.safepoint_id, 0u);
    if (probe->mode->expected == XR_BACKEND_EXECUTION_TRAP) {
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_TRAP);
        ASSERT_EQ_INT(outcome.trap, XR_VM_TRAP_PROVIDER_CALL_FAILED);
    } else if (probe->mode->expected == XR_BACKEND_EXECUTION_CANCELLED) {
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_CANCELLED);
    } else {
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, 43);
    }
    child_cleanup_assert_trace(probe);
}

static void child_cleanup_check_vm(XrInstance *instance, uint32_t entry, ChildCleanupProbe *probe,
                                   XrVmDecodePolicy policy) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    options.decode_policy = (uint8_t) policy;
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, &options, &code, &diagnostic), XR_VM_CODE_OK);
    for (uint32_t index = 0u; index < sizeof(child_cleanup_modes) / sizeof(child_cleanup_modes[0]);
         ++index) {
        const ChildCleanupMode *mode = &child_cleanup_modes[index];
        *probe = (ChildCleanupProbe) {.mode = mode};
        XrVmExecution *execution = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        XrVmOutcome suspended = xr_vm_execution_step(execution);
        uint32_t events_before_resume = probe->count;
        XrVmOutcome outcome = suspended;
        if (suspended.kind == XR_VM_OUTCOME_SUSPENDED)
            outcome =
                mode->cancel ? xr_vm_execution_cancel(execution) : xr_vm_execution_step(execution);
        xr_vm_execution_free(execution);
        child_cleanup_assert_vm(suspended, outcome, probe, events_before_resume);
    }
    xr_vm_code_free(code);
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

static void field_ref_cleanup_check_vm(XrInstance *instance, uint32_t entry,
                                       FieldRefCleanupProbe *probe, XrVmDecodePolicy policy) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    options.decode_policy = (uint8_t) policy;
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, &options, &code, &diagnostic), XR_VM_CODE_OK);
    for (uint32_t index = 0u;
         index < sizeof(field_ref_cleanup_modes) / sizeof(field_ref_cleanup_modes[0]); ++index) {
        const FieldRefCleanupMode *mode = &field_ref_cleanup_modes[index];
        *probe = (FieldRefCleanupProbe) {.mode = mode};
        XrVmExecution *execution = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        XrVmOutcome suspended = xr_vm_execution_step(execution);
        ASSERT_EQ_INT(suspended.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(suspended.state_id, 1u);
        ASSERT_EQ_UINT(suspended.safepoint_id, 0u);
        ASSERT_EQ_UINT(probe->count, 0u);
        XrVmOutcome outcome =
            mode->cancel ? xr_vm_execution_cancel(execution) : xr_vm_execution_step(execution);
        xr_vm_execution_free(execution);
        if (mode->expected == XR_BACKEND_EXECUTION_RETURN) {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
            ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
            ASSERT_EQ_INT(outcome.value.as.i64, INT64_C(6442450958));
        } else if (mode->expected == XR_BACKEND_EXECUTION_CANCELLED) {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_CANCELLED);
        } else {
            ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_TRAP);
            ASSERT_EQ_INT(outcome.trap, XR_VM_TRAP_PROVIDER_CALL_FAILED);
        }
        field_ref_cleanup_assert_trace(probe);
    }
    xr_vm_code_free(code);
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
    static unsigned int serial;
    if (!fixture || !entry_source)
        return false;
    memset(fixture, 0, sizeof(*fixture));
    (void) snprintf(fixture->directory, sizeof(fixture->directory),
                    "xr_program_source_build_%u_XXXXXX", serial++);
    if (!xr_test_mkdtemp(fixture->directory))
        goto fail;
    char absolute_directory[XR_TEST_PATH_MAX];
    if (!xr_test_realpath_buf(fixture->directory, absolute_directory, sizeof(absolute_directory)))
        goto fail;
    (void) snprintf(fixture->directory, sizeof(fixture->directory), "%s", absolute_directory);
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
        .max_modules = XR_PROGRAM_SOURCE_BUILD_DEFAULT_MAX_MODULES,
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
                "writer=%u verifier=%u message=%s\n",
                xr_program_source_build_status_name(status), (unsigned) diagnostic->stage,
                diagnostic->module_index, diagnostic->underlying_status,
                (unsigned) diagnostic->writer_status, (unsigned) diagnostic->verifier_status,
                diagnostic->message);
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
    static const char source[] = "print(42)\n";
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

TEST(source_owner_function_parameter_suspending_callable_has_one_program) {
    static const char source[] = "fn apply(value: i64, body: fn(i64) -> i64) -> i64 {\n"
                                 "  return body(value)\n"
                                 "}\n"
                                 "fn suspended(value: i64) -> i64 {\n"
                                 "  Coro.yield()\n"
                                 "  return value * 2\n"
                                 "}\n"
                                 "fn answer() -> i64 { return apply(21, suspended) }\n";
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
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT), 1u);
    ASSERT_EQ_UINT(program_operation_count(program, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED), 1u);
    const XrValidatedFunction *entry_function = &program->functions[entry];
    ASSERT_EQ_UINT(entry_function->coroutine_state_count, 2u);
    ASSERT_EQ_UINT(entry_function->coroutine_safepoint_count, 1u);

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
    XrReferenceOutcome reference_suspended = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_suspended.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_suspended.safepoint_id, 0u);
    ASSERT_EQ_UINT(reference_suspended.state_id, 1u);
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
        XrVmOutcome vm_suspended = xr_vm_execution_step(vm);
        ASSERT_EQ_INT(vm_suspended.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(vm_suspended.safepoint_id, reference_suspended.safepoint_id);
        ASSERT_EQ_UINT(vm_suspended.state_id, reference_suspended.state_id);
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

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_TRAP);
    ASSERT_EQ_INT(vm.trap, XR_VM_TRAP_PROVIDER_CALL_FAILED);
    ASSERT_EQ_UINT(probe.events, 10u);
    ASSERT_EQ_UINT(probe.clock_calls, 2u);
    ASSERT_EQ_UINT(probe.close_calls, 8u);
    xr_vm_outcome_dispose(&vm);

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
    ASSERT_NULL(strstr(generated.bytes, "int main(void)"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_i64_nullary"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_bool_i64_unary"));
    ASSERT_NOT_NULL(strstr(generated.bytes, ".trap == 7"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_PROVIDER_TRAP_CLEANUP);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(output,
                    "\nstatic uint32_t xr_probe_events;\n"
                    "static int xr_probe_refuse(void *context, uint32_t requirement, "
                    "uint32_t operation, int64_t *result) {\n"
                    "    (void)context;\n"
                    "    if (requirement != UINT32_C(%u) || operation != UINT32_C(0) || "
                    "!result || xr_probe_events != UINT32_C(0)) return 2;\n"
                    "    ++xr_probe_events;\n"
                    "    return 1;\n"
                    "}\n"
                    "static int xr_probe_close(void *context, uint32_t requirement, "
                    "uint32_t operation, int64_t handle, uint8_t *result) {\n"
                    "    (void)context;\n"
                    "    if (requirement != UINT32_C(%u) || operation != UINT32_C(0) || "
                    "!result) return 3;\n"
                    "    static const int64_t handles[] = {INT64_C(2147483642), "
                    "INT64_C(2147483643), INT64_C(2147483646), INT64_C(2147483647)};\n"
                    "    if (xr_probe_events < UINT32_C(1) || xr_probe_events > UINT32_C(4) || "
                    "handle != handles[xr_probe_events - UINT32_C(1)]) return 4;\n"
                    "    *result = UINT8_C(1);\n"
                    "    ++xr_probe_events;\n"
                    "    return 0;\n"
                    "}\n"
                    "int main(void) {\n"
                    "    XrAotContext context = {0};\n"
                    "    context.provider_call_i64_nullary = xr_probe_refuse;\n"
                    "    context.provider_call_bool_i64_unary = xr_probe_close;\n"
                    "    XrAotOutcome outcome = xr_aot_fn_%u(&context);\n"
                    "    int exit_code = outcome.kind == 1 && outcome.trap == 7 && "
                    "xr_probe_events == 5 ? 207 : 255;\n"
                    "    xr_aot_context_destroy(&context);\n"
                    "    return exit_code;\n"
                    "}\n",
                    clock_requirement, io_requirement, entry) > 0);
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
    child_cleanup_check_vm(instance, entry, &probe, XR_VM_DECODE_BASELINE_VIEW);
    child_cleanup_check_vm(instance, entry, &probe, XR_VM_DECODE_FIXED_ROWS);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_entry_coroutine_cancel"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_i64_nullary"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_bool_i64_unary"));
    const char *output_path =
        source_fixture_output_path(XR_SOURCE_FIXTURE_CHILD_COROUTINE_TRAP_CLEANUP);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(
                output,
                "\nstatic uint32_t xr_probe_mode;\n"
                "static uint32_t xr_probe_count;\n"
                "static int64_t xr_probe_events[8];\n"
                "static int64_t xr_probe_refused[2];\n"
                "static uint8_t xr_probe_false_close;\n"
                "static int xr_probe_clock(void *context, uint32_t requirement, uint32_t "
                "operation, "
                "int64_t *result) {\n"
                "    (void)context;\n"
                "    if (requirement != UINT32_C(%u) || operation != 0 || !result || "
                "xr_probe_count >= 8) return 1;\n"
                "    xr_probe_events[xr_probe_count++] = 0;\n"
                "    if (xr_probe_mode == 1) return 1;\n"
                "    *result = INT64_C(42000000);\n"
                "    return 0;\n"
                "}\n"
                "static int xr_probe_close(void *context, uint32_t requirement, uint32_t "
                "operation, "
                "int64_t handle, uint8_t *result) {\n"
                "    (void)context;\n"
                "    if (requirement != UINT32_C(%u) || operation != 0 || !result || "
                "xr_probe_count >= 8) return 1;\n"
                "    xr_probe_events[xr_probe_count++] = handle;\n"
                "    if (handle == xr_probe_refused[0] || handle == xr_probe_refused[1]) return "
                "1;\n"
                "    *result = xr_probe_false_close ? UINT8_C(0) : UINT8_C(1);\n"
                "    return 0;\n"
                "}\n"
                "int main(void) {\n"
                "    static const uint32_t expected_kind[17] = "
                "{0, 2, 3, 2, 2, 2, 2, 0, 3, 2, 2, 2, 2, 2, 2, 2, 2};\n"
                "    static const uint8_t cancel[17] = "
                "{0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};\n"
                "    static const uint8_t false_close[17] = "
                "{0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};\n"
                "    static const int64_t refused[17][2] = {{0, 0}, {0, 0}, {0, 0}, "
                "{INT64_C(2147483642), 0}, {INT64_C(2147483643), 0}, "
                "{INT64_C(2147483642), 0}, {INT64_C(2147483643), 0}, {0, 0}, {0, 0}, "
                "{INT64_C(2147483638), 0}, {INT64_C(2147483638), 0}, "
                "{INT64_C(2147483642), INT64_C(2147483643)}, "
                "{INT64_C(2147483642), INT64_C(2147483643)}, "
                "{INT64_C(2147483639), 0}, {INT64_C(2147483639), 0}, "
                "{INT64_C(2147483638), INT64_C(2147483639)}, "
                "{INT64_C(2147483638), INT64_C(2147483639)}};\n"
                "    static const int64_t handles[6] = {INT64_C(2147483642), "
                "INT64_C(2147483643), INT64_C(2147483638), INT64_C(2147483639), "
                "INT64_C(2147483646), INT64_C(2147483647)};\n"
                "    for (xr_probe_mode = 0; xr_probe_mode < 17; ++xr_probe_mode) {\n"
                "        xr_probe_count = 0;\n"
                "        xr_probe_refused[0] = refused[xr_probe_mode][0];\n"
                "        xr_probe_refused[1] = refused[xr_probe_mode][1];\n"
                "        xr_probe_false_close = false_close[xr_probe_mode];\n"
                "        XrAotEntryCoroutineFrame frame;\n"
                "        xr_aot_entry_coroutine_frame_initialize(&frame);\n"
                "        frame.context.provider_call_i64_nullary = xr_probe_clock;\n"
                "        frame.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                "        XrBackendNativeOutcome suspended = xr_aot_entry_coroutine_step(&frame);\n"
                "        if (suspended.kind != 1 || suspended.state_id != 1 || "
                "suspended.safepoint_id != 0 || xr_probe_count != 0) return 255;\n"
                "        XrBackendNativeOutcome outcome = cancel[xr_probe_mode] "
                "? xr_aot_entry_coroutine_cancel(&frame) : xr_aot_entry_coroutine_step(&frame);\n"
                "        xr_aot_entry_coroutine_frame_dispose(&frame);\n"
                "        if (outcome.kind != expected_kind[xr_probe_mode]) return 254;\n"
                "        if (outcome.kind == 0 && outcome.value != INT64_C(43)) return 253;\n"
                "        if (outcome.kind == 2 && outcome.safepoint_id != 7) return 252;\n"
                "        uint32_t start = cancel[xr_probe_mode] ? 0 : 1;\n"
                "        if (xr_probe_count != start + 6) return 251;\n"
                "        if (start != 0 && xr_probe_events[0] != 0) return 250;\n"
                "        for (uint32_t i = 0; i < 6; ++i) {\n"
                "            if (xr_probe_events[start + i] != handles[i]) return 249;\n"
                "        }\n"
                "    }\n"
                "    return 232;\n"
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
    branching_cleanup_check_vm(instance, entry, &probe, XR_VM_DECODE_BASELINE_VIEW);
    branching_cleanup_check_vm(instance, entry, &probe, XR_VM_DECODE_FIXED_ROWS);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
    bool observed_nonowner_cancel_input = false;
    XrBackendInstruction *tuple_probe = NULL;
    uint32_t tuple_probe_start = 0u;
    for (uint32_t function_id = 0u; function_id < backend_ir->function_count; ++function_id) {
        XrBackendFunction *function = &backend_ir->functions[function_id];
        for (uint32_t block_id = 0u; block_id < function->block_count; ++block_id) {
            XrBackendBlock *block = &function->blocks[block_id];
            for (uint32_t instruction_id = 0u; instruction_id < block->instruction_count;
                 ++instruction_id) {
                XrBackendInstruction *instruction = &block->instructions[instruction_id];
                uint32_t safepoint_id = UINT32_MAX;
                uint32_t live_start = 0u;
                if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD) {
                    safepoint_id = instruction->immediate.u32;
                } else if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                    safepoint_id = instruction->immediate.coroutine_call.safepoint_id;
                    uint32_t callee = instruction->immediate.coroutine_call.function_id;
                    ASSERT_LT(callee, backend_ir->function_count);
                    live_start = backend_ir->functions[callee].parameter_count;
                } else {
                    continue;
                }
                ASSERT_LT(safepoint_id, function->coroutine_safepoint_count);
                XrBackendCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint_id];
                ASSERT_TRUE(live_start <= instruction->operand_count);
                ASSERT_TRUE(point->live_value_count <= instruction->operand_count - live_start);
                if (!tuple_probe && point->live_value_count > 1u &&
                    instruction->operands[live_start] != instruction->operands[live_start + 1u]) {
                    tuple_probe = instruction;
                    tuple_probe_start = live_start;
                }
                ASSERT_TRUE(instruction->successor_count >= 2u);
                ASSERT_LT(instruction->successors[1], function->block_count);
                XrBackendBlock *cancel = &function->blocks[instruction->successors[1]];
                uint32_t cancel_start = live_start + point->live_value_count;
                ASSERT_TRUE(cancel_start <= instruction->operand_count);
                ASSERT_TRUE(cancel->argument_count <= instruction->operand_count - cancel_start);
                for (uint32_t argument = 0u; argument < cancel->argument_count; ++argument)
                    observed_nonowner_cancel_input |=
                        cancel->argument_ownerships[argument] == XR_CORE_IR_NON_OWNER;
            }
        }
    }
    ASSERT_TRUE(observed_nonowner_cancel_input);
    ASSERT_NOT_NULL(tuple_probe);
    uint32_t saved_tuple_value = tuple_probe->operands[tuple_probe_start + 1u];
    tuple_probe->operands[tuple_probe_start + 1u] = tuple_probe->operands[tuple_probe_start];
    ASSERT_FALSE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_EQ_INT(backend_diagnostic.status, XR_BACKEND_INVARIANT_REJECTED);
    tuple_probe->operands[tuple_probe_start + 1u] = saved_tuple_value;
    ASSERT_TRUE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &repeated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_BRANCHING_CLEANUP);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(
                output,
                "\nstatic uint32_t xr_probe_count;\n"
                "static uint32_t xr_probe_refuse_call;\n"
                "static int64_t xr_probe_events[8];\n"
                "static int xr_probe_close(void *context, uint32_t requirement, uint32_t "
                "operation, int64_t handle, uint8_t *result) {\n"
                "    (void)context;\n"
                "    if (requirement != UINT32_C(0) || operation != 0 || !result || "
                "xr_probe_count >= 8) return 1;\n"
                "    xr_probe_events[xr_probe_count++] = handle;\n"
                "    if (xr_probe_refuse_call != 0 && xr_probe_count == xr_probe_refuse_call) "
                "return 1;\n"
                "    *result = UINT8_C(1);\n"
                "    return 0;\n"
                "}\n"
                "int main(void) {\n"
                "    static const int32_t cancel_at[5] = {-1, 0, 1, -1, -1};\n"
                "    static const uint32_t refuse_call[5] = {0, 0, 0, 1, 4};\n"
                "    static const uint32_t expected_kind[5] = {0, 3, 3, 2, 2};\n"
                "    static const uint32_t expected_calls[5] = {4, 2, 4, 3, 5};\n"
                "    static const int64_t ordinary_events[5] = {INT64_C(2147483642), "
                "INT64_C(2147483643), INT64_C(2147483642), INT64_C(2147483643), "
                "INT64_C(2147483643)};\n"
                "    static const int64_t refusal_events[3] = {INT64_C(2147483642), "
                "INT64_C(2147483642), INT64_C(2147483643)};\n"
                "    for (uint32_t mode = 0; mode < 5; ++mode) {\n"
                "        xr_probe_count = 0;\n"
                "        xr_probe_refuse_call = refuse_call[mode];\n"
                "        XrAotEntryCoroutineFrame frame;\n"
                "        xr_aot_entry_coroutine_frame_initialize(&frame);\n"
                "        frame.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                "        XrBackendNativeOutcome outcome = xr_aot_entry_coroutine_step(&frame);\n"
                "        uint32_t suspension = 0;\n"
                "        while (outcome.kind == 1 && suspension < 3) {\n"
                "            outcome = cancel_at[mode] == (int32_t)suspension "
                "? xr_aot_entry_coroutine_cancel(&frame) "
                ": xr_aot_entry_coroutine_step(&frame);\n"
                "            ++suspension;\n"
                "        }\n"
                "        xr_aot_entry_coroutine_frame_dispose(&frame);\n"
                "        if (outcome.kind != expected_kind[mode]) return 255;\n"
                "        if (outcome.kind == 0 && outcome.value != INT64_C(86)) return 254;\n"
                "        if (outcome.kind == 2 && outcome.safepoint_id != 7) return 253;\n"
                "        if (xr_probe_count != expected_calls[mode]) return 252;\n"
                "        const int64_t *events = mode == 3 ? refusal_events : ordinary_events;\n"
                "        for (uint32_t event = 0; event < xr_probe_count; ++event) "
                "if (xr_probe_events[event] != events[event]) return 251;\n"
                "    }\n"
                "    return 233;\n"
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
    nested_cleanup_check_vm(instance, entry, &probe, XR_VM_DECODE_BASELINE_VIEW);
    nested_cleanup_check_vm(instance, entry, &probe, XR_VM_DECODE_FIXED_ROWS);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
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
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_NESTED_CLEANUP);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(
                output,
                "\nstatic uint32_t xr_probe_count;\n"
                "static uint32_t xr_probe_refuse_call;\n"
                "static int64_t xr_probe_events[16];\n"
                "static int xr_probe_close(void *context, uint32_t requirement, uint32_t "
                "operation, int64_t handle, uint8_t *result) {\n"
                "    (void)context;\n"
                "    if (requirement != UINT32_C(0) || operation != 0 || !result || "
                "xr_probe_count >= 16) return 1;\n"
                "    xr_probe_events[xr_probe_count++] = handle;\n"
                "    if (xr_probe_refuse_call != 0 && xr_probe_count == xr_probe_refuse_call) "
                "return 1;\n"
                "    *result = UINT8_C(1);\n"
                "    return 0;\n"
                "}\n"
                "int main(void) {\n"
                "    static const int32_t cancel_at[8] = {-1, 0, 1, -1, -1, -1, -1, -1};\n"
                "    static const uint32_t refuse_call[8] = {0, 0, 0, 1, 2, 4, 7, 8};\n"
                "    static const uint32_t expected_kind[8] = {0, 3, 3, 2, 2, 2, 2, 2};\n"
                "    static const uint32_t expected_calls[8] = {8, 4, 8, 5, 4, 4, 9, 8};\n"
                "    static const int64_t expected[8][9] = {\n"
                "        {2147483642,2147483643,2147483638,2147483639,"
                "2147483642,2147483643,2147483639,2147483638},\n"
                "        {2147483642,2147483643,2147483638,2147483639},\n"
                "        {2147483642,2147483643,2147483638,2147483639,"
                "2147483642,2147483643,2147483639,2147483638},\n"
                "        {2147483642,2147483642,2147483643,2147483638,2147483639},\n"
                "        {2147483642,2147483643,2147483638,2147483639},\n"
                "        {2147483642,2147483643,2147483638,2147483639},\n"
                "        {2147483642,2147483643,2147483638,2147483639,"
                "2147483642,2147483643,2147483639,2147483638,2147483639},\n"
                "        {2147483642,2147483643,2147483638,2147483639,"
                "2147483642,2147483643,2147483639,2147483638}\n"
                "    };\n"
                "    for (uint32_t mode = 0; mode < 8; ++mode) {\n"
                "        xr_probe_count = 0;\n"
                "        xr_probe_refuse_call = refuse_call[mode];\n"
                "        XrAotEntryCoroutineFrame frame;\n"
                "        xr_aot_entry_coroutine_frame_initialize(&frame);\n"
                "        frame.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                "        XrBackendNativeOutcome outcome = xr_aot_entry_coroutine_step(&frame);\n"
                "        uint32_t suspension = 0;\n"
                "        while (outcome.kind == 1 && suspension < 3) {\n"
                "            outcome = cancel_at[mode] == (int32_t)suspension "
                "? xr_aot_entry_coroutine_cancel(&frame) "
                ": xr_aot_entry_coroutine_step(&frame);\n"
                "            ++suspension;\n"
                "        }\n"
                "        xr_aot_entry_coroutine_frame_dispose(&frame);\n"
                "        if (outcome.kind != expected_kind[mode]) return 255;\n"
                "        if (outcome.kind == 0 && outcome.value != INT64_C(84)) return 254;\n"
                "        if (outcome.kind == 2 && outcome.safepoint_id != 7) return 253;\n"
                "        if (xr_probe_count != expected_calls[mode]) return 252;\n"
                "        for (uint32_t event = 0; event < xr_probe_count; ++event) "
                "if (xr_probe_events[event] != expected[mode][event]) return 251;\n"
                "    }\n"
                "    return 234;\n"
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
                logical_nots += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_LOGICAL_NOT;
                logical_ands += block->instructions[instruction_index].operation_id ==
                                XR_CORE_OP_CORE_LOGICAL_AND;
                logical_ors += block->instructions[instruction_index].operation_id ==
                               XR_CORE_OP_CORE_LOGICAL_OR;
            }
        }
    }
    ASSERT_EQ_UINT(aggregate_constructs, 1u);
    ASSERT_EQ_UINT(aggregate_projects, 8u);
    ASSERT_EQ_UINT(place_projects, 6u);
    ASSERT_EQ_UINT(place_loads, 6u);
    ASSERT_EQ_UINT(place_stores, 2u);
    ASSERT_EQ_UINT(variant_projects, 3u);
    ASSERT_EQ_UINT(sealed_calls, 10u);
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

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm.value.as.i64, reference.value.as.i64);
    ASSERT_EQ_UINT(probe.open_calls, 2u);
    ASSERT_EQ_UINT(probe.close_calls, 4u);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    XrBackendStatus backend_status =
        xr_backend_ir_build(first.program, profile, &options, &backend_ir, &backend_diagnostic);
    if (backend_status != XR_BACKEND_OK)
        fprintf(stderr,
                "Pipe lifecycle AOT build failed: status=%u operation=%u function=%u block=%u "
                "instruction=%u\n",
                (unsigned) backend_diagnostic.status, backend_diagnostic.operation_id,
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_pipe_open"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_pipe_close"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_PIPE_PROVIDER);
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

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm.value.as.i64, reference.value.as.i64);
    ASSERT_EQ_UINT(probe.close_calls, 4u);

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
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_pipe_close"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "out_error"));
    ASSERT_NULL(strstr(generated.bytes, "xr_aot_host_pipe_open"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_PIPE_CLOSE_FAILURE);
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

static void assert_uncaught_vm_error(XrInstance *instance, uint32_t entry, uint16_t error_type_id,
                                     PipeProviderProbe *probe) {
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &code, &diagnostic), XR_VM_CODE_OK);
    XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_ERROR);
    XrVmAggregateView error = {0};
    ASSERT_TRUE(xr_vm_value_aggregate_view(&outcome.error_value, &error));
    ASSERT_EQ_UINT(error.type_id, error_type_id);
    ASSERT_EQ_UINT(error.variant_ordinal, 0u);
    ASSERT_EQ_UINT(error.field_count, 1u);
    ASSERT_EQ_INT(error.fields[0].kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(error.fields[0].as.i64, -2);
    ASSERT_EQ_UINT(probe->close_calls, 4u);
    xr_vm_outcome_dispose(&outcome);
    xr_vm_code_free(code);
}

static void write_uncaught_error_aot(XrValidatedProgram *program, const XrTargetProfile *profile,
                                     uint32_t entry, uint16_t error_type_id,
                                     const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic), XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(ir, &diagnostic));
    XrGeneratedC generated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic), XR_BACKEND_OK);
    ASSERT_NULL(strstr(generated.bytes, "int main(void)"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "out_error"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(
                output,
                "\nstatic uint32_t xr_probe_calls;\n"
                "static int xr_probe_close(void *context, uint32_t requirement, uint32_t "
                "operation, "
                "int64_t argument, uint8_t *result) {\n"
                "    static const int64_t expected[2] = {INT64_C(2147483646), "
                "INT64_C(2147483647)};\n"
                "    (void)context;\n"
                "    if (requirement != 0 || operation != 0 || !result || xr_probe_calls >= 2 || "
                "argument != expected[xr_probe_calls]) return 1;\n"
                "    *result = 0;\n"
                "    ++xr_probe_calls;\n"
                "    return 0;\n"
                "}\n"
                "int main(void) {\n"
                "    XrAotContext context = {0};\n"
                "    context.provider_call_bool_i64_unary = xr_probe_close;\n"
                "    XrAotType%u error = {0};\n"
                "    XrAotOutcome outcome = xr_aot_fn_%u(&context, &error);\n"
                "    if (outcome.kind != 2 || error.tag != 0 || "
                "error.payload.case_0.f0 != -INT64_C(2) || xr_probe_calls != 2) return 255;\n"
                "    return 220;\n"
                "}\n",
                error_type_id, entry) > 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
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
    assert_uncaught_vm_error(instance, entry, entry_function->error_type_id, &probe);
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

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance, entry, NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_PANIC);
    ASSERT_EQ_INT(vm.panic_value.kind, XR_VM_VALUE_PANIC_INFO);
    ASSERT_EQ_UINT(vm.panic_value.as.panic_info, XR_ASSERTION_FAILURE_CONDITION_FALSE);
    ASSERT_EQ_UINT(probe.close_calls, 4u);
    xr_vm_outcome_dispose(&vm);
    xr_vm_code_free(vm_code);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
        XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, false, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_NOT_NULL(strstr(generated.bytes, "out_panic"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_bool_i64_unary"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "UINT32_C(1)"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "call_panic_"));
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
    const XrValidatedBlock *recovery_block = NULL;
    const XrValidatedInstruction *borrow = NULL;
    const XrValidatedInstruction *local = NULL;
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
            ASSERT_NULL(recovery_block);
            recovery_block = block;
            local = block_local;
            borrow = block_borrow;
        }
    }
    ASSERT_NOT_NULL(recovery_block);
    ASSERT_GT(recovery_block->instruction_count, 2u);
    ASSERT_NOT_NULL(local);
    ASSERT_NOT_NULL(borrow);
    ASSERT_EQ_UINT(local->operand_count, 1u);
    const XrValidatedInstruction *recovery_edge =
        &recovery_block->instructions[recovery_block->instruction_count - 1u];
    ASSERT_EQ_INT(recovery_edge->operation_id, XR_CORE_OP_CORE_BRANCH);
    uint32_t owner_occurrences = 0u;
    uint32_t borrowed_occurrences = 0u;
    for (uint32_t operand = 0u; operand < recovery_edge->operand_count; ++operand) {
        owner_occurrences += recovery_edge->operands[operand] == local->operands[0];
        borrowed_occurrences += recovery_edge->operands[operand] == borrow->result_id;
    }
    ASSERT_EQ_UINT(owner_occurrences, 1u);
    ASSERT_EQ_UINT(borrowed_occurrences, 0u);
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

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmExecution *vm = NULL;
    ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm));
    ASSERT_EQ_INT(xr_vm_execution_step(vm).kind, XR_VM_OUTCOME_SUSPENDED);
    XrVmOutcome vm_return = xr_vm_execution_step(vm);
    ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_return.value.as.i64, 42);
    ASSERT_EQ_UINT(probe.close_calls, 6u);
    xr_vm_execution_free(vm);

    XrVmExecution *vm_cancel = NULL;
    ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm_cancel));
    ASSERT_EQ_INT(xr_vm_execution_step(vm_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_vm_execution_cancel(vm_cancel).kind, XR_VM_OUTCOME_CANCELLED);
    ASSERT_EQ_UINT(probe.close_calls, 8u);
    xr_vm_execution_free(vm_cancel);
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_entry_coroutine_cancel"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_bool_i64_unary"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_PIPE_CANCEL_CLEANUP);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(output,
                    "\nstatic uint32_t xr_probe_calls;\n"
                    "static int xr_probe_close(void *context, uint32_t requirement, uint32_t "
                    "operation, int64_t argument, uint8_t *result) {\n"
                    "    static const int64_t expected[4] = {INT64_C(2147483646), "
                    "INT64_C(2147483647), INT64_C(2147483646), INT64_C(2147483647)};\n"
                    "    (void)context;\n"
                    "    if (requirement != 0 || operation != 0 || !result || xr_probe_calls >= 4 "
                    "|| argument != expected[xr_probe_calls]) return 1;\n"
                    "    *result = UINT8_C(1);\n"
                    "    ++xr_probe_calls;\n"
                    "    return 0;\n"
                    "}\n"
                    "int main(void) {\n"
                    "    XrAotEntryCoroutineFrame resumed;\n"
                    "    xr_aot_entry_coroutine_frame_initialize(&resumed);\n"
                    "    resumed.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                    "    XrBackendNativeOutcome suspended = "
                    "xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (suspended.kind != UINT32_C(1) || suspended.state_id != UINT32_C(1) || "
                    "suspended.safepoint_id != UINT32_C(0) || xr_probe_calls != 0) return 255;\n"
                    "    XrBackendNativeOutcome returned = xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (returned.kind != 0 || returned.value != INT64_C(42) || "
                    "xr_probe_calls != 2) return 254;\n"
                    "    xr_aot_entry_coroutine_frame_dispose(&resumed);\n"
                    "    XrAotEntryCoroutineFrame cancelled;\n"
                    "    xr_aot_entry_coroutine_frame_initialize(&cancelled);\n"
                    "    cancelled.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                    "    suspended = xr_aot_entry_coroutine_step(&cancelled);\n"
                    "    if (suspended.kind != UINT32_C(1) || xr_probe_calls != 2) return 253;\n"
                    "    XrBackendNativeOutcome stopped = "
                    "xr_aot_entry_coroutine_cancel(&cancelled);\n"
                    "    if (stopped.kind != UINT32_C(3) || stopped.state_id != UINT32_C(1) || "
                    "xr_probe_calls != 4) return 252;\n"
                    "    xr_aot_entry_coroutine_frame_dispose(&cancelled);\n"
                    "    return 226;\n"
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
        uint32_t projection_count = parameter == 0u ? 0u : 1u;
        for (uint32_t depth = 0u; depth <= projection_count; ++depth) {
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
            if (depth < projection_count) {
                ASSERT_EQ_UINT(definition->operation_id, XR_CORE_OP_CORE_PLACE_PROJECT);
                ASSERT_EQ_INT(definition->immediate_kind, XR_CORE_IR_IMMEDIATE_FIELD);
                field_ordinals[parameter - 1u] = definition->immediate.field_ordinal;
            } else {
                ASSERT_EQ_UINT(definition->operation_id, XR_CORE_OP_CORE_PLACE_LOCAL);
            }
            place = definition->operands[0];
        }
        ASSERT_LT(place, function->value_count);
        ASSERT_EQ_INT(function->value_categories[place], XR_CORE_IR_VALUE);
        storage_roots[parameter] = place;
        uint32_t live_occurrences = 0u;
        for (uint32_t live = 0u; live < parent_point->live_value_count; ++live)
            live_occurrences += parent_point->live_value_ids[live] == place;
        ASSERT_EQ_UINT(live_occurrences, 1u);
    }
    ASSERT_NE(field_ordinals[0], field_ordinals[1]);
    ASSERT_EQ_UINT(storage_roots[1], storage_roots[2]);
    ASSERT_NE(storage_roots[0], storage_roots[1]);
    ASSERT_EQ_UINT(function->value_types[storage_roots[0]], XR_CORE_TYPE_I64);
    const XrValidatedType *aggregate =
        xr_validated_program_type(first.program, function->value_types[storage_roots[1]]);
    ASSERT_NOT_NULL(aggregate);
    ASSERT_EQ_INT(aggregate->kind, XR_CORE_IR_TYPE_AGGREGATE);
    ASSERT_LT(field_ordinals[0], aggregate->field_count);
    ASSERT_LT(field_ordinals[1], aggregate->field_count);
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
    field_ref_cleanup_check_vm(instance, entry, &cleanup_probe, XR_VM_DECODE_BASELINE_VIEW);
    field_ref_cleanup_check_vm(instance, entry, &cleanup_probe, XR_VM_DECODE_FIXED_ROWS);

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
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_0"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_1"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_2"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_i64_nullary"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_bool_i64_unary"));
    XrBackendFunction *backend_parent = &backend_ir->functions[entry];
    XrBackendInstruction *backend_call = NULL;
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
    XrBackendCoroutineSafepoint *backend_point = &backend_parent->coroutine_safepoints[0];
    ASSERT_EQ_UINT(backend_point->live_value_count, 3u);
    uint32_t backend_child_id = backend_call->immediate.coroutine_call.function_id;
    ASSERT_LT(backend_child_id, backend_ir->function_count);
    uint32_t backend_parameter_count = backend_ir->functions[backend_child_id].parameter_count;
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
    XrBackendInstruction *backend_project =
        &backend_parent->blocks[project_block].instructions[project_position];
    ASSERT_EQ_UINT(backend_project->result_id, projected_place);
    ASSERT_EQ_UINT(backend_project->operation_id, XR_CORE_OP_CORE_PLACE_PROJECT);
    uint32_t saved_ordinal = backend_project->immediate.field_ordinal;
    backend_project->immediate.field_ordinal = aggregate->field_count;
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
                    "        xr_aot_entry_coroutine_frame_initialize(&frame);\n"
                    "        frame.context.provider_call_i64_nullary = xr_probe_clock;\n"
                    "        frame.context.provider_call_bool_i64_unary = xr_probe_close;\n"
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
    XrReferenceExecution *reference = NULL;
    XrReferenceExecution *reference_cancel = NULL;
    XrVmCode *code = NULL;
    XrVmExecution *vm = NULL;
    XrVmExecution *vm_cancel = NULL;
    XrReferenceOutcome reference_outcome;
    XrVmOutcome vm_outcome;
    XrVmCodeDiagnostic vm_diagnostic;
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
        xr_reference_execution_cancel(reference_cancel).kind != XR_REFERENCE_OUTCOME_CANCELLED ||
        xr_vm_code_build(instance, NULL, &code, &vm_diagnostic) != XR_VM_CODE_OK ||
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
    ok = true;
cleanup:
    if (reference)
        xr_reference_execution_free(reference);
    if (reference_cancel)
        xr_reference_execution_free(reference_cancel);
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
                        "    xr_aot_entry_coroutine_frame_initialize(&resumed);\n"
                        "    XrBackendNativeOutcome outcome = "
                        "xr_aot_entry_coroutine_step(&resumed);\n"
                        "    if (outcome.kind != UINT32_C(1) || outcome.state_id != UINT32_C(1) || "
                        "outcome.safepoint_id != UINT32_C(0)) return 255;\n"
                        "    outcome = xr_aot_entry_coroutine_step(&resumed);\n"
                        "    if (outcome.kind != UINT32_C(0) || outcome.value != INT64_C(42)) "
                        "return 254;\n"
                        "    xr_aot_entry_coroutine_frame_dispose(&resumed);\n"
                        "    XrAotEntryCoroutineFrame cancelled;\n"
                        "    xr_aot_entry_coroutine_frame_initialize(&cancelled);\n"
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
        !xr_backend_ir_translation_validate(ir, &diagnostic) ||
        xr_backend_ir_emit_c(ir, false, &generated, &diagnostic) != XR_BACKEND_OK ||
        xr_backend_ir_emit_c(ir, false, &repeated, &diagnostic) != XR_BACKEND_OK ||
        generated.size != repeated.size ||
        memcmp(generated.bytes, repeated.bytes, generated.size) != 0 ||
        !strstr(generated.bytes, "child_active_0") || strstr(generated.bytes, "TargetPlan"))
        goto cleanup;
    XrBackendFunction *parent = &ir->functions[probe->entry];
    XrBackendInstruction *call = NULL;
    for (uint32_t block = 0u; block < parent->block_count; ++block) {
        XrBackendBlock *candidate_block = &parent->blocks[block];
        for (uint32_t instruction = 0u; instruction < candidate_block->instruction_count;
             ++instruction) {
            XrBackendInstruction *candidate = &candidate_block->instructions[instruction];
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
    XrBackendCoroutineSafepoint *point = &parent->coroutine_safepoints[0];
    uint32_t parameter_count = ir->functions[probe->child_id].parameter_count;
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

static void assert_affine_vm_executions(const XrVmCode *code, XrInstance *instance, uint32_t entry,
                                        PipeCloseSequenceProbe *probe) {
    pipe_close_sequence_reset(probe, affine_coroutine_close_handles, 4u);
    XrVmExecution *execution = NULL;
    ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
    for (uint32_t step = 0u; step < 4u; ++step) {
        XrVmOutcome outcome = xr_vm_execution_step(execution);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_SUSPENDED);
        ASSERT_EQ_UINT(outcome.state_id, affine_coroutine_states[step]);
        ASSERT_EQ_UINT(outcome.safepoint_id, affine_coroutine_safepoints[step]);
        ASSERT_EQ_UINT(probe->calls, affine_coroutine_closes_before[step]);
    }
    XrVmOutcome returned = xr_vm_execution_step(execution);
    ASSERT_EQ_INT(returned.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(returned.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(returned.value.as.i64, 42);
    ASSERT_EQ_UINT(probe->calls, 4u);
    xr_vm_execution_free(execution);
    ASSERT_EQ_UINT(probe->calls, 4u);

    for (uint32_t cancel_after = 1u; cancel_after <= 4u; ++cancel_after) {
        uint32_t expected_closes = cancel_after <= 2u ? 2u : 4u;
        pipe_close_sequence_reset(probe, affine_coroutine_close_handles, expected_closes);
        execution = NULL;
        ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
        for (uint32_t step = 0u; step < cancel_after; ++step) {
            XrVmOutcome suspended = xr_vm_execution_step(execution);
            ASSERT_EQ_INT(suspended.kind, XR_VM_OUTCOME_SUSPENDED);
            ASSERT_EQ_UINT(suspended.state_id, affine_coroutine_states[step]);
            ASSERT_EQ_UINT(suspended.safepoint_id, affine_coroutine_safepoints[step]);
            ASSERT_EQ_UINT(probe->calls, affine_coroutine_closes_before[step]);
        }
        XrVmOutcome cancelled = xr_vm_execution_cancel(execution);
        ASSERT_EQ_INT(cancelled.kind, XR_VM_OUTCOME_CANCELLED);
        ASSERT_EQ_UINT(cancelled.state_id, affine_coroutine_states[cancel_after - 1u]);
        ASSERT_EQ_UINT(probe->calls, expected_closes);
        xr_vm_execution_free(execution);
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

    XrVmCode *code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &code, &vm_diagnostic), XR_VM_CODE_OK);
    assert_affine_vm_executions(code, instance, entry, &probe);
    xr_vm_code_free(code);

    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(
        xr_backend_ir_build(product.program, profile, &options, &backend_ir, &backend_diagnostic),
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "child_active_"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_bool_i64_unary"));
    ASSERT_NULL(strstr(generated.bytes, "xr_place_"));
    const char *output_path = source_fixture_output_path(XR_SOURCE_FIXTURE_AFFINE_COROUTINE_RESULT);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(
            fprintf(
                output,
                "\nstatic uint32_t xr_probe_calls;\n"
                "static const uint32_t xr_probe_states[4] = {UINT32_C(1), UINT32_C(1), "
                "UINT32_C(2), UINT32_C(3)};\n"
                "static const uint32_t xr_probe_safepoints[4] = {UINT32_C(0), UINT32_C(0), "
                "UINT32_C(1), UINT32_C(2)};\n"
                "static const uint32_t xr_probe_closes_before[4] = {UINT32_C(0), UINT32_C(0), "
                "UINT32_C(2), UINT32_C(2)};\n"
                "static int xr_probe_close(void *context, uint32_t requirement, uint32_t "
                "operation, int64_t argument, uint8_t *result) {\n"
                "    static const int64_t expected[16] = {\n"
                "        INT64_C(2147483644), INT64_C(2147483645), INT64_C(2147483646), "
                "INT64_C(2147483647),\n"
                "        INT64_C(2147483644), INT64_C(2147483645),\n"
                "        INT64_C(2147483644), INT64_C(2147483645),\n"
                "        INT64_C(2147483644), INT64_C(2147483645), INT64_C(2147483646), "
                "INT64_C(2147483647),\n"
                "        INT64_C(2147483644), INT64_C(2147483645), INT64_C(2147483646), "
                "INT64_C(2147483647)};\n"
                "    (void)context;\n"
                "    if (requirement != UINT32_C(0) || operation != UINT32_C(0) || !result || "
                "xr_probe_calls >= UINT32_C(16) || argument != expected[xr_probe_calls]) return "
                "1;\n"
                "    *result = UINT8_C(1);\n"
                "    ++xr_probe_calls;\n"
                "    return 0;\n"
                "}\n"
                "static int xr_probe_is_suspended(XrBackendNativeOutcome outcome, uint32_t "
                "observation, uint32_t expected_calls) {\n"
                "    return observation < UINT32_C(4) && outcome.kind == UINT32_C(1) && "
                "outcome.state_id == xr_probe_states[observation] && outcome.safepoint_id == "
                "xr_probe_safepoints[observation] && xr_probe_calls == expected_calls;\n"
                "}\n"
                "static int xr_probe_run_normal(void) {\n"
                "    uint32_t base = xr_probe_calls;\n"
                "    XrAotEntryCoroutineFrame frame;\n"
                "    xr_aot_entry_coroutine_frame_initialize(&frame);\n"
                "    frame.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                "    for (uint32_t observation = 0; observation < UINT32_C(4); ++observation) {\n"
                "        XrBackendNativeOutcome suspended = "
                "xr_aot_entry_coroutine_step(&frame);\n"
                "        if (!xr_probe_is_suspended(suspended, observation, base + "
                "xr_probe_closes_before[observation])) return 10 + (int)observation;\n"
                "    }\n"
                "    XrBackendNativeOutcome returned = xr_aot_entry_coroutine_step(&frame);\n"
                "    if (returned.kind != UINT32_C(0) || returned.value != INT64_C(42) || "
                "xr_probe_calls != base + UINT32_C(4)) return 14;\n"
                "    xr_aot_entry_coroutine_frame_dispose(&frame);\n"
                "    return xr_probe_calls == base + UINT32_C(4) ? 0 : 15;\n"
                "}\n"
                "static int xr_probe_run_cancel(uint32_t cancel_after) {\n"
                "    uint32_t base = xr_probe_calls;\n"
                "    XrAotEntryCoroutineFrame frame;\n"
                "    xr_aot_entry_coroutine_frame_initialize(&frame);\n"
                "    frame.context.provider_call_bool_i64_unary = xr_probe_close;\n"
                "    for (uint32_t observation = 0; observation < cancel_after; ++observation) {\n"
                "        XrBackendNativeOutcome suspended = "
                "xr_aot_entry_coroutine_step(&frame);\n"
                "        if (!xr_probe_is_suspended(suspended, observation, base + "
                "xr_probe_closes_before[observation])) return 20 + (int)observation;\n"
                "    }\n"
                "    uint32_t expected_closes = cancel_after <= UINT32_C(2) ? UINT32_C(2) : "
                "UINT32_C(4);\n"
                "    XrBackendNativeOutcome cancelled = xr_aot_entry_coroutine_cancel(&frame);\n"
                "    if (cancelled.kind != UINT32_C(3) || cancelled.state_id != "
                "xr_probe_states[cancel_after - UINT32_C(1)] || xr_probe_calls != base + "
                "expected_closes) return 24;\n"
                "    xr_aot_entry_coroutine_frame_dispose(&frame);\n"
                "    return xr_probe_calls == base + expected_closes ? 0 : 25;\n"
                "}\n"
                "int main(void) {\n"
                "    int result = xr_probe_run_normal();\n"
                "    if (result != 0) return result;\n"
                "    for (uint32_t cancel_after = UINT32_C(1); cancel_after <= UINT32_C(4); "
                "++cancel_after) {\n"
                "        result = xr_probe_run_cancel(cancel_after);\n"
                "        if (result != 0) return result + (int)(cancel_after * UINT32_C(10));\n"
                "    }\n"
                "    return xr_probe_calls == UINT32_C(16) ? 229 : 255;\n"
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
    input.max_modules = 1u;
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
    ASSERT_TRUE(diagnostic.message[0] != '\0');
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);
    source_build_fixture_free(&fixture);
}

TEST_MAIN_BEGIN()
if (argc != 1) {
    if (argc != 7 || strcmp(argv[1], "--emit-fixture") != 0 || strcmp(argv[3], "--registry") != 0 ||
        strcmp(argv[4], XR_SOURCE_REGISTRY_ID) != 0 || strcmp(argv[5], "--output") != 0 ||
        argv[6][0] == '\0') {
        fprintf(stderr, "expected --emit-fixture ID --registry DIGEST --output PATH\n");
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
#define RUN_SOURCE_CASE(name, fixture)                                                             \
    do {                                                                                           \
        if (selected_source_fixture == XR_SOURCE_FIXTURE_NONE ||                                   \
            selected_source_fixture == fixture) {                                                  \
            RUN_TEST(name);                                                                        \
        }                                                                                          \
    } while (0);
XR_SOURCE_CASES(RUN_SOURCE_CASE)
#undef RUN_SOURCE_CASE
if ((unsigned int) xr_tests_run !=
    (selected_source_fixture == XR_SOURCE_FIXTURE_NONE ? XR_SOURCE_CASE_COUNT : 1u)) {
    fprintf(stderr, "source case registry did not select exactly the expected cases\n");
    return 2;
}
TEST_MAIN_END()
