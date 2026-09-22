/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_process_provider.inc.c - Source-owned generated process binding
 */

static void assert_generated_provider_execution(XrValidatedProgram *program,
                                                XrTargetProfile *profile) {
    uint32_t count = xr_validated_program_provider_requirement_count(program);
    size_t capacity = xr_stdlib_provider_count();
    ASSERT_TRUE(count != 0u && count <= capacity);
    XrProviderBinding *providers = xr_calloc(count, sizeof(*providers));
    XrProviderOperationBinding *operations = xr_calloc(capacity, sizeof(*operations));
    if (!providers || !operations) {
        xr_free(operations);
        xr_free(providers);
        ASSERT_TRUE(false);
        return;
    }
    size_t next = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        XrProgramProviderRequirementView requirement = {0};
        ASSERT_TRUE(xr_validated_program_provider_requirement(program, index, &requirement));
        ASSERT_TRUE(requirement.operation_count <= capacity - next);
        ASSERT_TRUE(requirement.operation_count <= UINT16_MAX);
        providers[index].contract_id = requirement.contract_id;
        providers[index].operations = operations + next;
        providers[index].operation_count = (uint16_t) requirement.operation_count;
        providers[index].behavior_flags = XR_PROVIDER_BEHAVIOR_FLAGS_ALL;
        for (uint32_t operation = 0u; operation < requirement.operation_count; ++operation) {
            const XrStdlibProviderDescriptor *descriptor = xr_stdlib_provider_find(
                requirement.contract_id, requirement.operations[operation].operation_id);
            uint32_t behavior = 0u;
            ASSERT_TRUE(
                xr_stdlib_provider_operation_binding(descriptor, &operations[next], &behavior));
            providers[index].behavior_flags &= behavior;
            ++next;
        }
        const XrTargetProviderContract *contract =
            find_profile_provider(profile, requirement.contract_id);
        ASSERT_NOT_NULL(contract);
        ASSERT_EQ_INT(xr_target_provider_contract_fingerprint(
                          contract, &providers[index].contract_fingerprint),
                      XR_RUNTIME_ABI_OK);
    }
    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = program,
        .profile = profile,
        .providers = providers,
        .provider_count = count,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    uint32_t entry = xr_validated_program_entry_function(program);

    {
        XrVmCodeOptions options = xr_vm_code_default_options();
        XrVmCodeDiagnostic vm_diagnostic;
        XrVmCode *code = NULL;
        ASSERT_EQ_INT(xr_vm_code_build(program, profile, &options, &code, &vm_diagnostic),
                      XR_VM_CODE_OK);
        XrVmOutcome outcome = xr_vm_code_execute(code, instance, entry, NULL, 0u);
        ASSERT_EQ_INT(outcome.kind, XR_VM_OUTCOME_RETURN);
        ASSERT_EQ_INT(outcome.value.kind, XR_VM_VALUE_I64);
        ASSERT_EQ_INT(outcome.value.as.i64, 1);
        xr_vm_outcome_dispose(&outcome);
        xr_vm_code_free(code);
    }
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic), XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_free(operations);
    xr_free(providers);
}

static void assert_process_provider_source_rejections(void) {
    static const char *sources[] = {
        "import os\nfn answer() -> i64 { return os.getpid(1) }\n",
        "import os\nfn answer() -> i64 { return os.__not_a_native() }\n",
    };
    for (size_t index = 0u; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        SourceBuildFixture fixture;
        ASSERT_TRUE(source_build_fixture_init(&fixture, sources[index], NULL));
        XrProgramSourceProduct product = {0};
        XrProgramSourceDiagnostic diagnostic = {0};
        ASSERT_EQ_INT(xr_program_source_build(&fixture.input, &product, &diagnostic),
                      XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED);
        ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_ANALYSIS);
        ASSERT_NULL(product.program);
        ASSERT_NULL(product.artifact.bytes);
        xr_program_source_product_free(&product);
        source_build_fixture_free(&fixture);
    }
}

static void assert_process_provider_name_is_not_authority(void) {
    static const char source[] = "import { __getpid } from \"./library\"\n"
                                 "fn answer() -> i64 { return __getpid() }\n";
    static const char library[] = "export fn __getpid() -> i64 { return 37 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, library));
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic = {0};
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 0u);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    assert_detached_program_i64_result(product.program, profile, 37);
    xr_target_profile_free(profile);
    xr_program_source_product_free(&product);
    source_build_fixture_free(&fixture);
}

static void assert_process_provider_source(XrSourceFixtureId fixture_id) {
    static const char source[] = "import os\n"
                                 "fn answer() -> i64 {\n"
                                 "  const first = os.getpid()\n"
                                 "  const second = os.getpid()\n"
                                 "  if (first <= 0 || first != second) { return 0 }\n"
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
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    if (!product.program) {
        xr_target_profile_free(profile);
        source_build_fixture_free(&fixture);
        return;
    }
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    XrProgramProviderRequirementView requirement = {0};
    ASSERT_TRUE(xr_validated_program_provider_requirement(product.program, 0u, &requirement));
    ASSERT_EQ_UINT(requirement.operation_count, 1u);
    const XrStdlibProviderDescriptor *descriptor =
        xr_stdlib_provider_find(requirement.contract_id, requirement.operations[0].operation_id);
    ASSERT_NOT_NULL(descriptor);
    ASSERT_EQ_INT(strcmp(descriptor->symbol, "os.__getpid"), 0);
    XrProviderOperationBinding operation = {0};
    uint32_t behavior = 0u;
    ASSERT_TRUE(xr_stdlib_provider_operation_binding(descriptor, &operation, &behavior));
    ASSERT_EQ_INT(operation.trampoline_kind, XR_PROVIDER_TRAMPOLINE_I64_NULLARY);
    assert_generated_provider_execution(product.program, profile);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendDiagnostic backend_diagnostic;
    XrBackendIR *ir = NULL;
    ASSERT_EQ_INT(xr_backend_ir_build(product.program, profile, &options, &ir, &backend_diagnostic),
                  XR_BACKEND_OK);
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &generated, &backend_diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(ir, true, &repeated, &backend_diagnostic), XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_os_core_getpid()"));
    ASSERT_NOT_NULL(strstr(generated.bytes, "xr_aot_host_typed"));
    ASSERT_NULL(strstr(generated.bytes, "xrt_os_getpid"));
    const char *output_path = source_fixture_output_path(fixture_id);
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output), generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(ir);
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    source_build_fixture_free(&fixture);
}
