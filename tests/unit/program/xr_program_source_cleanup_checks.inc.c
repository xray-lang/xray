/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_source_cleanup_checks.inc.c - Independent backend cleanup checks
 *
 * KEY CONCEPT:
 *   Execute each cleanup scenario in the VM and generated native
 *   code. Provider traces and cancellation outcomes are independent oracles.
 */

static void branching_cleanup_check_vm(const XrValidatedProgram *program,
                                       const XrTargetProfile *profile, XrInstance *instance,
                                       uint32_t entry, BranchingCleanupProbe *probe) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic), XR_VM_CODE_OK);
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

static void nested_cleanup_check_vm(const XrValidatedProgram *program,
                                    const XrTargetProfile *profile, XrInstance *instance,
                                    uint32_t entry, BranchingCleanupProbe *probe) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic), XR_VM_CODE_OK);
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

static void child_cleanup_check_vm(const XrValidatedProgram *program,
                                   const XrTargetProfile *profile, XrInstance *instance,
                                   uint32_t entry, ChildCleanupProbe *probe) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic), XR_VM_CODE_OK);
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

static void field_ref_cleanup_check_vm(const XrValidatedProgram *program,
                                       const XrTargetProfile *profile, XrInstance *instance,
                                       uint32_t entry, FieldRefCleanupProbe *probe) {
    XrVmCodeOptions options = xr_vm_code_default_options();
    XrVmCode *code = NULL;
    XrVmCodeDiagnostic diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic), XR_VM_CODE_OK);
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

static const uint32_t affine_coroutine_states[] = {1u, 1u, 2u, 3u};
static const uint32_t affine_coroutine_safepoints[] = {0u, 0u, 1u, 2u};
static const uint32_t affine_coroutine_closes_before[] = {0u, 0u, 2u, 2u};
static const int64_t affine_coroutine_close_handles[] = {
    2147483644,
    2147483645,
    2147483646,
    2147483647,
};

typedef struct SourceVmCleanupLifecycle {
    uint32_t constructed;
    uint32_t finalized;
    uint32_t reclaimed;
    uint32_t copied;
    uint32_t teardown_events;
} SourceVmCleanupLifecycle;

static void record_vm_cleanup_lifecycle(void *context, const XrVmLifecycleEvent *event) {
    SourceVmCleanupLifecycle *probe = context;
    probe->constructed += event->kind == XR_VM_EVENT_CLASS_CONSTRUCT;
    probe->finalized += event->kind == XR_VM_EVENT_CLASS_FINALIZE;
    probe->reclaimed += event->kind == XR_VM_EVENT_CLASS_RECLAIM;
    probe->copied += event->kind == XR_VM_EVENT_CLASS_COPY;
    probe->teardown_events += event->origin == XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN;
}

static void assert_vm_cleanup_lifecycle(const SourceVmCleanupLifecycle *probe, uint32_t expected) {
    ASSERT_EQ_UINT(probe->constructed, expected);
    ASSERT_EQ_UINT(probe->finalized, expected);
    ASSERT_EQ_UINT(probe->reclaimed, expected);
    ASSERT_EQ_UINT(probe->copied, 0u);
    ASSERT_EQ_UINT(probe->teardown_events, 0u);
}

static void assert_affine_vm_executions(const XrVmCode *code, XrInstance *instance, uint32_t entry,
                                        PipeCloseSequenceProbe *probe,
                                        SourceVmCleanupLifecycle *lifecycle) {
    *lifecycle = (SourceVmCleanupLifecycle) {0};
    pipe_close_sequence_reset(probe, affine_coroutine_close_handles, 4u);
    XrVmExecution *execution = NULL;
    ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &execution));
    for (uint32_t step = 0u; step < 4u; ++step) {
        XrVmOutcome outcome = xr_vm_execution_step(execution);
        if (outcome.kind != XR_VM_OUTCOME_SUSPENDED)
            fprintf(stderr,
                    "affine coroutine failed: step=%u outcome=%u state=%u safepoint=%u closes=%u\n",
                    step, (unsigned) outcome.kind, outcome.state_id, outcome.safepoint_id,
                    probe->calls);
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
    assert_vm_cleanup_lifecycle(lifecycle, 2u);
    xr_vm_execution_free(execution);
    assert_vm_cleanup_lifecycle(lifecycle, 2u);
    ASSERT_EQ_UINT(probe->calls, 4u);

    for (uint32_t cancel_after = 1u; cancel_after <= 4u; ++cancel_after) {
        *lifecycle = (SourceVmCleanupLifecycle) {0};
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
        assert_vm_cleanup_lifecycle(lifecycle, expected_closes / 2u);
        xr_vm_execution_free(execution);
        assert_vm_cleanup_lifecycle(lifecycle, expected_closes / 2u);
        ASSERT_EQ_UINT(probe->calls, expected_closes);
    }
}

static void assert_uncaught_vm_error(const XrValidatedProgram *program,
                                     const XrTargetProfile *profile, XrInstance *instance,
                                     uint32_t entry, uint16_t error_type_id,
                                     PipeProviderProbe *probe) {

    {
        probe->close_calls = 0u;
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCode *code = NULL;
        XrVmCodeDiagnostic diagnostic;
        ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &diagnostic),
                      XR_VM_CODE_OK);
        XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_ERROR);
        XrVmAggregateView error = {0};
        ASSERT_TRUE(xr_vm_value_aggregate_view(&outcome.error_value, &error));
        ASSERT_EQ_UINT(error.type_id, error_type_id);
        ASSERT_EQ_UINT(error.variant_ordinal, 0u);
        ASSERT_EQ_UINT(error.field_count, 1u);
        ASSERT_EQ_INT(error.fields[0].kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(error.fields[0].as.i64, -2);
        ASSERT_EQ_UINT(probe->close_calls, 2u);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
    }
}

static void write_builtin_panic_cleanup_aot(const XrValidatedProgram *program,
                                            const XrTargetProfile *profile, uint32_t checked,
                                            uint32_t read_cleanup, uint32_t panic_code,
                                            bool message, const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic), XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(ir, &diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &repeated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "int main(void)"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_TRUE(fputs("#define main xr_builtin_entry_main\n", output) >= 0);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(fputs(
            "\n#undef main\nstatic XrAotLifecycleEvent xr_events[64];\n"
            "static uint32_t xr_event_count;\n"
            "static void xr_record(void *context, const XrAotLifecycleEvent *event) {\n"
            "    (void)context;\n"
            "    if (xr_event_count < 64) xr_events[xr_event_count] = *event;\n"
            "    ++xr_event_count;\n"
            "}\n"
            "int main(void) {\n"
            "  for (uint32_t instance = 0; instance < 2; ++instance) {\n"
            "    XrAotContext context = {0};\n"
            "    context.lifecycle_event = xr_record;\n", output) >= 0);
        if (program->module_slot_count != 0u)
            ASSERT_TRUE(fputs("    XrAotModules modules = {0};\n"
                              "    modules.storage.modules = &modules; "
                              "context.modules = &modules;\n", output) >= 0);
        if (program->module_count != 0u)
            ASSERT_TRUE(fputs("    if (xr_aot_initialize_modules(&context).kind != 0) "
                              "return 250;\n", output) >= 0);
        ASSERT_TRUE(fputs(
            "    for (uint32_t iteration = 0; iteration < 4; ++iteration) {\n"
            "      uint8_t succeeds = (uint8_t)(iteration & 1u); XrAotPanicInfo panic = {0};\n"
            "      xr_event_count = 0;\n", output) >= 0);
        if (program->functions[checked].coroutine_safepoint_count != 0u) {
            ASSERT_TRUE(fprintf(output,
                "      XrAotCoroutineFrame%u frame = {0};\n"
                "      XrAotOutcome result = xr_aot_fn_%u_step(&context, &frame, 0, "
                "succeeds, &panic);\n"
                "      if (result.kind != 5 || frame.state != 1) return 249;\n"
                "      for (uint32_t i = 0; i < xr_event_count; ++i)\n"
                "        if (xr_events[i].kind == 8 || xr_events[i].kind == 9) return 248;\n"
                "      result = xr_aot_fn_%u_step(&context, &frame, 0, succeeds, &panic);\n",
                checked, checked, checked) > 0);
            if (program->functions[checked].coroutine_safepoint_count == 2u)
                ASSERT_TRUE(fprintf(output,
                    "      if (succeeds) {\n"
                    "        if (result.kind != 5 || frame.state != 2) return 247;\n"
                    "        for (uint32_t i = 0; i < xr_event_count; ++i)\n"
                    "          if (xr_events[i].kind == 8 || xr_events[i].kind == 9) return 246;\n"
                    "        result = xr_aot_fn_%u_step(&context, &frame, 0, succeeds, &panic);\n"
                    "      }\n", checked) > 0);
        } else {
            ASSERT_TRUE(fprintf(output,
                "      XrAotOutcome result = xr_aot_fn_%u(&context, succeeds, &panic);\n",
                checked) > 0);
        }
        ASSERT_TRUE(fprintf(output,
            "      if (succeeds ? result.kind != 0 || result.i64 != 42\n"
            "                   : result.kind != 3 || panic.code != %u) return 251;\n",
            panic_code) > 0);
        if (message)
            ASSERT_TRUE(fputs(
                "      if (!succeeds) {\n"
                "        if (!panic.message || panic.message->size != 14 ||\n"
                "            memcmp(panic.message->bytes, \"assert message\", 14) != 0) return 244;\n"
                "        xr_aot_free(&context, panic.message); panic.message = NULL;\n"
                "      }\n", output) >= 0);
        if (read_cleanup != UINT32_MAX)
            ASSERT_TRUE(fprintf(output,
                "      XrAotOutcome observed = xr_aot_fn_%u(&context);\n"
                "      if (observed.kind != 0 || observed.i64 != (succeeds ? 11 : 22)) "
                "return 252;\n", read_cleanup) > 0);
        ASSERT_TRUE(fputs(
            "      uint64_t created[2] = {0}; uint32_t creates = 0, finals = 0;\n"
            "      uint32_t finalized[2] = {0}, reclaimed[2] = {0};\n"
            "      if (xr_event_count > 64) return 253;\n"
            "      for (uint32_t i = 0; i < xr_event_count; ++i) {\n"
            "        const XrAotLifecycleEvent *event = &xr_events[i];\n"
            "        if (event->kind == 1) {\n"
            "          if (creates == 2) return 254;\n"
            "          created[creates++] = event->identity;\n"
            "        } else if (event->kind == 8 || event->kind == 9) {\n"
            "          if (creates != 2) return 255;\n"
            "          uint32_t object = event->identity == created[0] ? 0u : 1u;\n"
            "          if (event->identity != created[object]) return 255;\n"
            "          if (event->kind == 8) {\n"
            "            if (finals == 2 || (!succeeds && "
            "event->identity != created[1u-finals])) return 255;\n"
            "            ++finals; ++finalized[object];\n"
            "          } else ++reclaimed[object];\n"
            "        }\n"
            "      }\n"
            "      if (creates != 2 || finalized[0] != 1 || finalized[1] != 1 ||\n"
            "          reclaimed[0] != 1 || reclaimed[1] != 1 || context.allocations) return 255;\n"
            "    }\n"
            "    uint32_t before = xr_event_count;\n"
            "    xr_aot_context_destroy(&context);\n", output) >= 0);
        if (program->module_slot_count != 0u)
            ASSERT_TRUE(fputs("    xr_aot_modules_clear(&modules);\n", output) >= 0);
        ASSERT_TRUE(fputs(
            "    if (xr_event_count != before) return 255;\n"
            "  }\n"
            "  return xr_builtin_entry_main();\n"
            "}\n", output) >= 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static void write_probe_typed_host(FILE *output) {
    ASSERT_TRUE(fputs(
        "\n"
        "static int (*xr_probe_read_entry)(void *, uint32_t, uint32_t, int64_t *);\n"
        "static int (*xr_probe_close_entry)(void *, uint32_t, uint32_t, int64_t, uint8_t *);\n"
        "static void xr_probe_dispose(XrProviderValuePack *result) { *result = (XrProviderValuePack){0}; }\n"
        "static int xr_probe_typed(void *context, uint32_t requirement, uint32_t operation,\n"
        "                         const XrProviderValuePack *arguments, XrProviderValuePack *result) {\n"
        "    if (!arguments || !result || result->count) return 1;\n"
        "    if (!arguments->count && xr_probe_read_entry) {\n"
        "        int64_t value = 0;\n"
        "        if (xr_probe_read_entry(context, requirement, operation, &value) != 0) return 1;\n"
        "        result->count = 1; result->nodes[0].token = 3; result->nodes[0].as.i64 = value;\n"
        "        return 0;\n"
        "    }\n"
        "    if (arguments->count == 1 && arguments->nodes[0].token == 3 && xr_probe_close_entry) {\n"
        "        uint8_t value = 0;\n"
        "        if (xr_probe_close_entry(context, requirement, operation, arguments->nodes[0].as.i64, &value) != 0) return 1;\n"
        "        result->count = 1; result->nodes[0].token = 2; result->nodes[0].as.boolean = value != 0;\n"
        "        return 0;\n"
        "    }\n"
        "    return 1;\n"
        "}\n"
        , output) >= 0);
}

static void write_uncaught_error_aot(XrValidatedProgram *program, const XrTargetProfile *profile,
                                     uint32_t entry, uint16_t error_type_id,
                                     const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic), XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(ir, &diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, false, &generated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, false, &repeated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NULL(strstr(generated.bytes, "int main(void)"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "out_error"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                "    context.provider_call_typed = xr_probe_typed;\n"
                "    context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
                "    XrAotType%u error = {0};\n"
                "    XrAotOutcome outcome = xr_aot_fn_%u(&context, &error);\n"
                "    if (outcome.kind != 2 || error.tag != 0 || "
                "error.payload.case_0.f0 != -INT64_C(2) || xr_probe_calls != 2) return 255;\n"
                "    xr_aot_context_destroy(&context);\n"
                "    return 220;\n"
                "}\n",
                error_type_id, entry) > 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static void assert_aot_provider_trap_cleanup(const XrValidatedProgram *program,
                                             const XrTargetProfile *profile, uint32_t entry,
                                             uint32_t clock_requirement, uint32_t io_requirement,
                                             const char *output_path) {
    ASSERT_EQ_UINT(program->functions[entry].error_type_id, XR_CORE_TYPE_VOID);
    ASSERT_EQ_UINT(program->functions[entry].panic_type_id, XR_CORE_TYPE_PANIC_INFO);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
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
    ASSERT_NULL(strstr(generated.bytes, "int main(void)"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_typed"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_dispose_typed"));
    ASSERT_NOT_NULL(strstr(generated.bytes, ".trap == 7"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                    "    xr_probe_read_entry = xr_probe_refuse;\n"
                    "    context.provider_call_typed = xr_probe_typed;\n"
                "    context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
                    "    XrAotPanicInfo panic = {0};\n"
                    "    XrAotOutcome outcome = xr_aot_fn_%u(&context, &panic);\n"
                    "    int exit_code = outcome.kind == 1 && outcome.trap == 7 && "
                    "panic.code == UINT32_C(0) && xr_probe_events == 5 ? 207 : 255;\n"
                    "    xr_aot_context_destroy(&context);\n"
                    "    return exit_code;\n"
                    "}\n",
                    clock_requirement, io_requirement, entry) > 0);
        ASSERT_EQ_INT(fclose(output), 0);
    }

    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
}

static void assert_aot_child_cleanup(const XrValidatedProgram *program,
                                     const XrTargetProfile *profile, uint32_t clock_requirement,
                                     uint32_t io_requirement, const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_entry_coroutine_cancel"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_typed"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_dispose_typed"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                "        xr_aot_entry_coroutine_frame_initialize(&frame, NULL, NULL);\n"
                "        xr_probe_read_entry = xr_probe_clock;\n"
                "        frame.context.provider_call_typed = xr_probe_typed;\n"
                "    frame.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
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
}

static void assert_aot_branching_cleanup(const XrValidatedProgram *program,
                                         const XrTargetProfile *profile, const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(backend_ir, &backend_diagnostic));
    bool observed_nonowner_cancel_input = false;
    XrValidatedInstruction *tuple_probe = NULL;
    uint32_t tuple_probe_start = 0u;
    for (uint32_t function_id = 0u; function_id < backend_ir->program->function_count; ++function_id) {
        XrValidatedFunction *function = &backend_ir->program->functions[function_id];
        for (uint32_t block_id = 0u; block_id < function->block_count; ++block_id) {
            XrValidatedBlock *block = &function->blocks[block_id];
            for (uint32_t instruction_id = 0u; instruction_id < block->instruction_count;
                 ++instruction_id) {
                XrValidatedInstruction *instruction = &block->instructions[instruction_id];
                uint32_t safepoint_id = UINT32_MAX;
                uint32_t live_start = 0u;
                if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD) {
                    safepoint_id = instruction->immediate.u32;
                } else if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                    safepoint_id = instruction->immediate.coroutine_call.safepoint_id;
                    uint32_t callee = instruction->immediate.coroutine_call.function_id;
                    ASSERT_LT(callee, backend_ir->program->function_count);
                    live_start = backend_ir->program->functions[callee].parameter_count;
                } else {
                    continue;
                }
                ASSERT_LT(safepoint_id, function->coroutine_safepoint_count);
                XrValidatedCoroutineSafepoint *point =
                    &function->coroutine_safepoints[safepoint_id];
                ASSERT_TRUE(live_start <= instruction->operand_count);
                ASSERT_TRUE(point->live_value_count <= instruction->operand_count - live_start);
                if (!tuple_probe && point->live_value_count > 1u &&
                    instruction->operands[live_start] != instruction->operands[live_start + 1u]) {
                    tuple_probe = instruction;
                    tuple_probe_start = live_start;
                }
                ASSERT_TRUE(instruction->successor_count >= 2u);
                ASSERT_LT(instruction->successors[1], function->block_count);
                XrValidatedBlock *cancel = &function->blocks[instruction->successors[1]];
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
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                "        xr_aot_entry_coroutine_frame_initialize(&frame, NULL, NULL);\n"
                "        frame.context.provider_call_typed = xr_probe_typed;\n"
                "    frame.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
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
}

static void assert_aot_nested_cleanup(const XrValidatedProgram *program,
                                      const XrTargetProfile *profile, const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
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
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                "        xr_aot_entry_coroutine_frame_initialize(&frame, NULL, NULL);\n"
                "        frame.context.provider_call_typed = xr_probe_typed;\n"
                "    frame.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
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
}

static void assert_aot_pipe_cancel_cleanup(const XrValidatedProgram *program,
                                           const XrTargetProfile *profile,
                                           const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_entry_coroutine_cancel"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_typed"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                    "    xr_aot_entry_coroutine_frame_initialize(&resumed, NULL, NULL);\n"
                    "    resumed.context.provider_call_typed = xr_probe_typed;\n"
                "    resumed.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
                    "    XrBackendNativeOutcome suspended = "
                    "xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (suspended.kind != UINT32_C(1) || suspended.state_id != UINT32_C(1) || "
                    "suspended.safepoint_id != UINT32_C(0) || xr_probe_calls != 0) return 255;\n"
                    "    XrBackendNativeOutcome returned = xr_aot_entry_coroutine_step(&resumed);\n"
                    "    if (returned.kind != 0 || returned.value != INT64_C(42) || "
                    "xr_probe_calls != 2) return 254;\n"
                    "    xr_aot_entry_coroutine_frame_dispose(&resumed);\n"
                    "    XrAotEntryCoroutineFrame cancelled;\n"
                    "    xr_aot_entry_coroutine_frame_initialize(&cancelled, NULL, NULL);\n"
                    "    cancelled.context.provider_call_typed = xr_probe_typed;\n"
                "    cancelled.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
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
}

static void assert_aot_affine_coroutine_cleanup(const XrValidatedProgram *program,
                                                const XrTargetProfile *profile,
                                                const char *output_path) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *backend_ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &backend_ir, &backend_diagnostic),
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "child_active_"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "provider_call_typed"));
    ASSERT_NULL(strstr(generated.bytes, "xr_place_"));
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        write_probe_typed_host(output);
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
                "    xr_aot_entry_coroutine_frame_initialize(&frame, NULL, NULL);\n"
                "    frame.context.provider_call_typed = xr_probe_typed;\n"
                "    frame.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
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
                "    xr_aot_entry_coroutine_frame_initialize(&frame, NULL, NULL);\n"
                "    frame.context.provider_call_typed = xr_probe_typed;\n"
                "    frame.context.provider_dispose_typed = xr_probe_dispose;\n"
                "    xr_probe_close_entry = xr_probe_close;\n"
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
}
