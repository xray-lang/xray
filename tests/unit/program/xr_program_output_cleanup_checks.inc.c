/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_output_cleanup_checks.inc.c - Source output refusal ownership checks
 */

static void assert_output_class_cleanup(const SourceClassLifecycleLog *log, uint32_t count) {
    uint64_t created[2] = {0};
    uint32_t creates = 0u, finalized = 0u;
    ASSERT_FALSE(log->overflow);
    for (uint32_t event = 0u; event < log->count; ++event) {
        const XrVmLifecycleEvent *observed = &log->events[event];
        if (observed->kind == XR_VM_EVENT_CLASS_CONSTRUCT) {
            ASSERT_LT(creates, count);
            created[creates++] = observed->identity;
        } else if (observed->kind == XR_VM_EVENT_CLASS_FINALIZE) {
            ASSERT_EQ_UINT(creates, count);
            ASSERT_LT(finalized, count);
            ASSERT_EQ_UINT(observed->identity, created[count - 1u - finalized]);
            ++finalized;
        }
    }
    ASSERT_EQ_UINT(creates, count);
    ASSERT_EQ_UINT(finalized, count);
    for (uint32_t object = 0u; object < count; ++object) {
        ASSERT_EQ_UINT(
            source_class_lifecycle_count(log, XR_VM_EVENT_CLASS_FINALIZE, created[object]), 1u);
        ASSERT_EQ_UINT(
            source_class_lifecycle_count(log, XR_VM_EVENT_CLASS_RECLAIM, created[object]), 1u);
    }
}

static void assert_output_initializer_vm(XrValidatedProgram *program, XrTargetProfile *profile,
                                         bool panic) {
    TextOutputCapture capture = {0};
    XrProgramProviderRequirementView requirement = {0};
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(program), 1u);
    ASSERT_TRUE(xr_validated_program_provider_requirement(program, 0u, &requirement));
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
        .program = program,
        .profile = profile,
        .providers = &provider,
        .provider_count = 1u,
        .generation = 1u,
    };
    {
        SourceClassLifecycleLog log = {0};
        XrVmCodeOptions options = xr_vm_code_default_options();
        options.lifecycle_context = &log;
        options.lifecycle_event = record_source_class_lifecycle;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, NULL), XR_VM_CODE_OK);
        for (uint32_t fail = 0u; fail < 4u; ++fail) {
            for (uint32_t separate = 0u; separate < 2u; ++separate) {
                log = (SourceClassLifecycleLog) {0};
                capture = (TextOutputCapture) {.fail_at = fail};
                XrInstance *instance = NULL;
                ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, NULL),
                              XR_EXECUTION_OK);
                uint32_t terminal_count = 0u;
                for (uint32_t repeat = 0u; repeat < 3u; ++repeat) {
                    XrVmOutcome result = xr_vm_code_execute(
                        code, instance, xr_validated_program_entry_function(program), NULL, 0u);
                    ASSERT_EQ_INT(result.kind, fail ? XR_VM_OUTCOME_TRAP
                                                       : panic ? XR_VM_OUTCOME_PANIC
                                                               : XR_VM_OUTCOME_ERROR);
                    ASSERT_EQ_INT(result.trap, fail ? XR_VM_TRAP_PROVIDER_CALL_FAILED
                                                       : XR_VM_TRAP_NONE);
                    if (!fail && !panic) {
                        XrVmAggregateView error;
                        ASSERT_TRUE(xr_vm_value_aggregate_view(&result.error_value, &error));
                        ASSERT_EQ_UINT(error.field_count, 1u);
                        ASSERT_EQ_INT(error.fields[0].kind, XR_VM_VALUE_I64);
                        ASSERT_EQ_INT(error.fields[0].as.i64, 7);
                    }
                    xr_vm_outcome_dispose(&result);
                    const char *expected =
                        fail == 1u   ? ""
                        : fail == 2u ? "library ready\ndone temporary\n"
                        : fail == 3u ? "library ready\ncleanup temporary\n"
                                     : "library ready\ncleanup temporary\ndone temporary\n";
                    ASSERT_EQ_UINT(capture.calls, fail == 1u ? 1u : 3u);
                    ASSERT_EQ_UINT(capture.size, strlen(expected));
                    ASSERT_EQ_INT(memcmp(capture.bytes, expected, capture.size), 0);
                    if (repeat == 0u)
                        terminal_count = log.count;
                    ASSERT_EQ_UINT(log.count, terminal_count);
                }
                assert_output_class_cleanup(&log, fail == 1u ? 1u : 2u);
                ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, NULL), XR_EXECUTION_OK);
                ASSERT_EQ_INT(xr_execution_instance_retire(instance, NULL), XR_EXECUTION_OK);
                ASSERT_EQ_INT(xr_execution_instance_free(&instance, NULL), XR_EXECUTION_OK);
                ASSERT_EQ_UINT(log.count, terminal_count);
            }
        }
        xr_vm_code_free(code);
    }
}

static void assert_output_initializer_native(const XrValidatedProgram *program,
                                             const XrTargetProfile *profile,
                                             XrSourceFixtureId fixture_id, bool panic) {
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(program, profile, &options, &ir, &diagnostic), XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_binding_verify(ir, &diagnostic));
    XrGeneratedC generated = {0}, repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &generated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &repeated, &diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    const char *path = source_fixture_output_path(fixture_id);
    if (path) {
        size_t size = 0u;
        char *harness = xr_file_read_all(XR_OUTPUT_TRAP_NATIVE_HARNESS, "rb", &size);
        ASSERT_NOT_NULL(harness);
        FILE *output = fopen(path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_TRUE(fputs("#include <stdlib.h>\n"
                          "static void *output_trap_malloc(size_t size);\n"
                          "static void output_trap_free(void *pointer);\n"
                          "#define malloc output_trap_malloc\n#define free output_trap_free\n"
                          "#define main xr_output_fixture_default_main\n",
                          output) >= 0);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_TRUE(fputs("\n#undef main\n#undef malloc\n#undef free\n", output) >= 0);
        ASSERT_GT(fprintf(output, "#define XR_OUTPUT_PANIC %u\n", panic ? 1u : 0u), 0);
        if (!panic) {
            uint16_t error_type = XR_CORE_TYPE_VOID;
            for (uint32_t type = 0u; type < program->type_count; ++type) {
                const XrValidatedType *candidate = &program->types[type];
                const char *name = xr_validated_program_type_display_name(program,
                                                                          candidate->type_id);
                if (name && strcmp(name, "InitFailure") == 0)
                    error_type = candidate->type_id;
            }
            ASSERT_TRUE(error_type != XR_CORE_TYPE_VOID);
            ASSERT_GT(fprintf(output, "#define XR_OUTPUT_ERROR_TYPE XrAotType%u\n"
                                      "#define XR_OUTPUT_ERROR_ID %u\n", error_type, error_type), 0);
        }
        ASSERT_EQ_UINT(fwrite(harness, 1u, size, output), size);
        ASSERT_EQ_INT(fclose(output), 0);
        xr_free(harness);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
}

static void assert_output_initializer_failure(bool panic, XrSourceFixtureId fixture_id) {
    char library[1024];
    int length = snprintf(
        library, sizeof(library),
        "enum InitFailure { Rejected { code: i64 } }\n"
        "class Cell {\n"
        "  value: string\n"
        "  constructor(value: string) { this.value = value }\n"
        "}\n"
        "var state = Cell(\"ready\")\n"
        "fn initialize(ok: bool) -> i64 {\n"
        "  var transient = Cell(\"temporary\")\n"
        "  defer { print(\"cleanup\", transient.value); print(\"done\", transient.value) }\n"
        "  if (!ok) { %s }\n"
        "  return 1\n"
        "}\n"
        "print(\"library\", state.value)\n"
        "initialize(false)\n",
        panic ? "assert(ok)" : "throw InitFailure.Rejected { code: 7 }");
    ASSERT_TRUE(length > 0 && (size_t) length < sizeof(library));
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(
        &fixture, "import \"./library\"\nfn answer() -> i64 { return 42 }\n", library));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_NOT_NULL(product.program);
    ASSERT_EQ_UINT(product.program->module_count, 2u);
    assert_output_initializer_vm(product.program, profile, panic);
    assert_output_initializer_native(product.program, profile, fixture_id, panic);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}
