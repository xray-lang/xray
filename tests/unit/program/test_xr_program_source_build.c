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
#include "execution/xr_execution.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "program/xr_program_source_build.h"
#include "program/xr_reference_evaluator.h"
#include "program/xr_validated_program_internal.h"
#include "toolchain/xcompiler_session.h"
#include "vm/xr_program_vm.h"
#include "xray_vm.h"
#include "plan/target_profile_test_fixture.h"

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

static const char *cross_module_coroutine_aot_output_path;

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
        (void) xr_compiler_session_attach_isolate(fixture->isolate,
                                                   fixture->original_session);
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
    if (!xr_test_realpath_buf(fixture->directory, absolute_directory,
                              sizeof(absolute_directory)))
        goto fail;
    (void) snprintf(fixture->directory, sizeof(fixture->directory), "%s",
                    absolute_directory);
    int entry_length = snprintf(fixture->entry_path, sizeof(fixture->entry_path), "%s/main.xr",
                                fixture->directory);
    if (entry_length < 0 || (size_t) entry_length >= sizeof(fixture->entry_path) ||
        !write_source_file(fixture->entry_path, entry_source))
        goto fail;
    if (dependency_source) {
        int dependency_length =
            snprintf(fixture->dependency_path, sizeof(fixture->dependency_path),
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
    fixture->original_session =
        xr_compiler_session_current_for_isolate(fixture->isolate);
    XrCompilerSessionConfig session_config = {0};
    fixture->session = xr_compiler_session_new(&session_config);
    if (!fixture->session ||
        xr_compiler_session_attach_isolate(fixture->isolate, fixture->session) !=
            fixture->original_session)
        goto fail;
    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    if (!fixture->resolver)
        goto fail;

    fixture->authority.kind = XR_MODULE_IDENTITY_SCRIPT;
    fixture->authority.physical_root = fixture->directory;
    if (!xr_module_identity_from_source(&fixture->authority, fixture->entry_path,
                                        &fixture->entry_identity,
                                        &fixture->entry_logical_path))
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
        .entry = {
            .kind = XR_PROGRAM_SOURCE_ENTRY_FUNCTION,
            .module_identity = fixture->entry_identity,
            .function_name = "answer",
            .source_content_fingerprint = source_fingerprint,
        },
        .source_profile = XR_PROGRAM_SOURCE_PROFILE_NATIVE_RELEASE,
        .semantic_profile_fingerprint =
            semantic_fingerprint("source-owner-native-release"),
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
    ASSERT_EQ_INT(memcmp(first->artifact.bytes, second->artifact.bytes,
                         first->artifact.size),
                  0);
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
        fprintf(stderr, "source build failed: status=%s stage=%u module=%u underlying=%u "
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

    ASSERT_EQ_INT(xr_test_unlink(fixture.entry_path), 0);
    fixture.entry_path[0] = '\0';
    size_t retained_size = 0u;
    const uint8_t *retained_bytes =
        xr_validated_program_bytes(first.program, &retained_size);
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
        "export fn increment(value: i64) -> i64 { return value + 1 }\n";
    static const char entry_source[] =
        "import { increment } from \"./library\"\n"
        "fn answer() -> i64 { return increment(41) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    assert_products_equal(&first, &second);
    ASSERT_GE(xr_validated_program_function_count(first.program), 2u);

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
    fixture.input.semantic_profile_fingerprint =
        xr_target_profile_target_semantics_id(profile);
    XrProgramSourceProduct product = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &product, &diagnostic);
    ASSERT_EQ_UINT(xr_validated_program_function_count(product.program), 1u);
    ASSERT_EQ_UINT(xr_validated_program_entry_function(product.program), 0u);
    ASSERT_EQ_UINT(xr_validated_program_provider_requirement_count(product.program), 1u);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    ASSERT_EQ_INT(xr_backend_ir_build(product.program, profile, &options, &backend_ir,
                                      &backend_diagnostic),
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

TEST(source_owner_cross_module_coroutine_call_has_one_program_and_private_executors) {
    static const char library_source[] =
        "export fn child(value: i64) -> i64 {\n"
        "  Coro.yield()\n"
        "  return value\n"
        "}\n";
    static const char entry_source[] =
        "import { child } from \"./library\"\n"
        "fn answer() -> i64 { return child(7) }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, entry_source, library_source));
    XrTargetProfile *profile =
        xr_test_target_profile_build_with_output(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    ASSERT_NOT_NULL(profile);
    ASSERT_TRUE(xr_compiler_session_set_target_profile(fixture.session, profile));
    fixture.input.semantic_profile_fingerprint =
        xr_target_profile_target_semantics_id(profile);

    XrProgramSourceProduct first = {0};
    XrProgramSourceProduct second = {0};
    XrProgramSourceDiagnostic diagnostic;
    assert_source_build_ok(&fixture.input, &first, &diagnostic);
    assert_source_build_ok(&fixture.input, &second, &diagnostic);
    assert_products_equal(&first, &second);

    const XrValidatedProgram *program = first.program;
    uint32_t entry_function = xr_validated_program_entry_function(program);
    ASSERT_LT(entry_function, program->function_count);
    const XrValidatedFunction *entry = &program->functions[entry_function];
    ASSERT_EQ_UINT(entry->coroutine_state_count, 2u);
    ASSERT_EQ_UINT(entry->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(entry->effect_mask, XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_SUSPEND);
    ASSERT_EQ_UINT(entry->capability_mask,
                   XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD);

    const XrValidatedInstruction *coroutine_call = NULL;
    for (uint32_t block_index = 0u; block_index < entry->block_count; ++block_index) {
        const XrValidatedBlock *block = &entry->blocks[block_index];
        for (uint32_t instruction_index = 0u;
             instruction_index < block->instruction_count; ++instruction_index) {
            const XrValidatedInstruction *candidate =
                &block->instructions[instruction_index];
            if (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED) {
                ASSERT_NULL(coroutine_call);
                coroutine_call = candidate;
            }
        }
    }
    ASSERT_NOT_NULL(coroutine_call);
    ASSERT_EQ_INT(coroutine_call->immediate_kind,
                  XR_CORE_IR_IMMEDIATE_COROUTINE_CALL);
    ASSERT_EQ_UINT(coroutine_call->immediate.coroutine_call.safepoint_id, 0u);
    uint32_t child_function = coroutine_call->immediate.coroutine_call.function_id;
    ASSERT_LT(child_function, program->function_count);
    ASSERT_TRUE(child_function != entry_function);
    const XrValidatedFunction *child = &program->functions[child_function];
    ASSERT_EQ_UINT(child->parameter_count, 1u);
    ASSERT_EQ_UINT(child->parameter_types[0], XR_CORE_TYPE_I64);
    ASSERT_EQ_INT(child->parameter_modes[0], XR_PARAM_READ);
    ASSERT_EQ_UINT(child->result_type_id, XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(child->coroutine_state_count, 2u);
    ASSERT_EQ_UINT(child->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(child->effect_mask, XR_CORE_EFFECT_SUSPEND);
    ASSERT_EQ_UINT(child->capability_mask,
                   XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD);

    XrExecutionBindingInput binding = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = first.program,
        .profile = profile,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&binding, &instance,
                                                &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);

    XrReferenceExecution *reference = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry_function, NULL, 0u,
                                               NULL, &reference));
    XrReferenceOutcome reference_suspend = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_suspend.kind, XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(reference_suspend.safepoint_id, 0u);
    ASSERT_EQ_UINT(reference_suspend.state_id, 1u);
    XrReferenceOutcome reference_return = xr_reference_execution_step(reference);
    ASSERT_EQ_INT(reference_return.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_return.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_return.value.as.i64, 7);
    xr_reference_execution_free(reference);

    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic),
                  XR_VM_CODE_OK);
    XrVmExecution *vm_execution = NULL;
    ASSERT_TRUE(xr_vm_execution_create(vm_code, instance, entry_function, NULL, 0u,
                                       &vm_execution));
    XrVmOutcome vm_suspend = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_suspend.kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_UINT(vm_suspend.safepoint_id, reference_suspend.safepoint_id);
    ASSERT_EQ_UINT(vm_suspend.state_id, reference_suspend.state_id);
    XrVmOutcome vm_return = xr_vm_execution_step(vm_execution);
    ASSERT_EQ_INT(vm_return.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_return.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_return.value.as.i64, reference_return.value.as.i64);
    xr_vm_execution_free(vm_execution);
    xr_vm_code_free(vm_code);

    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    XrBackendOptions options = xr_backend_default_options();
    ASSERT_EQ_INT(xr_backend_ir_build(first.program, profile, &options, &backend_ir,
                                      &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    XrGeneratedC repeated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated,
                                      &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &repeated,
                                      &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_EQ_UINT(generated.size, repeated.size);
    ASSERT_EQ_INT(memcmp(generated.bytes, repeated.bytes, generated.size), 0);
    ASSERT_NOT_NULL(strstr(generated.bytes, "child_active_0"));
    ASSERT_NULL(strstr(generated.bytes, "XrProto"));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    if (cross_module_coroutine_aot_output_path) {
        FILE *output = fopen(cross_module_coroutine_aot_output_path, "wb");
        ASSERT_NOT_NULL(output);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, output),
                       generated.size);
        ASSERT_EQ_INT(fclose(output), 0);
    }
    xr_generated_c_free(&repeated);
    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);

    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    xr_program_source_product_free(&second);
    xr_program_source_product_free(&first);
    source_build_fixture_free(&fixture);
    xr_target_profile_free(profile);
}

TEST(source_owner_rejects_non_authoritative_entry_identity) {
    static const char source[] = "fn answer() -> i64 { return 42 }\n";
    SourceBuildFixture fixture;
    ASSERT_TRUE(source_build_fixture_init(&fixture, source, NULL));
    char *wrong_identity = NULL;
    ASSERT_TRUE(xr_module_identity_from_logical(&fixture.authority, "other.xr",
                                                &wrong_identity));
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
    static const char entry_source[] =
        "import { increment } from \"./library\"\n"
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
if (argc == 2)
    cross_module_coroutine_aot_output_path = argv[1];
else if (argc != 1)
    return 2;
RUN_TEST(source_owner_single_module_is_deterministic_and_detached);
RUN_TEST(source_owner_two_module_graph_is_deterministic);
RUN_TEST(source_owner_cross_module_coroutine_call_has_one_program_and_private_executors);
RUN_TEST(source_owner_module_initializer_is_a_canonical_entry);
RUN_TEST(source_owner_rejects_non_authoritative_entry_identity);
RUN_TEST(source_owner_rejects_module_budget_before_analysis);
RUN_TEST(source_owner_reports_structured_analysis_failure);
TEST_MAIN_END()
