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
static const char *function_parameter_aot_output_path;
static const char *clock_provider_aot_output_path;
static const char *provider_trap_cleanup_aot_output_path;
static const char *pipe_provider_aot_output_path;
static const char *pipe_close_failure_aot_output_path;
static const char *pipe_uncaught_error_aot_output_path;
static const char *pipe_cancel_cleanup_aot_output_path;
static const char *multi_safepoint_aot_output_path;
static const char *affine_coroutine_result_aot_output_path;
static const char *ref_parameter_coroutine_aot_output_path;

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
                                                  const char *library_source) {
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
    ASSERT_EQ_UINT(entry->capability_mask, XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD);

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
    ASSERT_EQ_UINT(child->capability_mask, XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD);

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
    if (cross_module_coroutine_aot_output_path) {
        FILE *output = fopen(cross_module_coroutine_aot_output_path, "wb");
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
    assert_cross_module_coroutine_program(entry_source, library_source);
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
    assert_cross_module_coroutine_program(entry_source, library_source);
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
    if (function_parameter_aot_output_path) {
        FILE *output = fopen(function_parameter_aot_output_path, "wb");
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
    if (clock_provider_aot_output_path) {
        FILE *output = fopen(clock_provider_aot_output_path, "wb");
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
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_TRAP), 2u);
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
    if (provider_trap_cleanup_aot_output_path) {
        FILE *output = fopen(provider_trap_cleanup_aot_output_path, "wb");
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
    if (pipe_provider_aot_output_path) {
        FILE *output = fopen(pipe_provider_aot_output_path, "wb");
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
    if (pipe_close_failure_aot_output_path) {
        FILE *output = fopen(pipe_close_failure_aot_output_path, "wb");
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
                             pipe_uncaught_error_aot_output_path);
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
    ASSERT_EQ_UINT(program_operation_count(first.program, XR_CORE_OP_CORE_PLACE_LOCAL), 4u);
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
        ASSERT_NULL(trap);
        trap = candidate;
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
    if (pipe_cancel_cleanup_aot_output_path) {
        FILE *output = fopen(pipe_cancel_cleanup_aot_output_path, "wb");
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "return xr_aot_make(5, UINT32_C(1), 0)"));
    if (multi_safepoint_aot_output_path) {
        FILE *output = fopen(multi_safepoint_aot_output_path, "wb");
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

TEST(source_owner_keeps_ref_parameter_place_stable_across_child_suspension) {
    static const char source[] = "fn bump(value: ref i64) {\n"
                                 "  Coro.yield()\n"
                                 "  value = value + 1\n"
                                 "}\n"
                                 "fn answer() -> i64 {\n"
                                 "  var value = 41\n"
                                 "  bump(ref value)\n"
                                 "  return value\n"
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
    ASSERT_EQ_UINT(child->parameter_count, 1u);
    ASSERT_EQ_INT(child->parameter_modes[0], XR_PARAM_REF);
    ASSERT_EQ_UINT(child->coroutine_safepoint_count, 1u);
    ASSERT_EQ_UINT(child->coroutine_safepoints[0].live_value_count, 1u);
    uint32_t child_live = child->coroutine_safepoints[0].live_value_ids[0];
    ASSERT_LT(child_live, child->value_count);
    ASSERT_EQ_INT(child->value_categories[child_live], XR_CORE_IR_PLACE);
    ASSERT_EQ_INT(child->value_ownerships[child_live], XR_CORE_IR_NON_OWNER);

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
    ASSERT_EQ_INT(reference_outcome.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference_outcome.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference_outcome.value.as.i64, 42);
    xr_reference_execution_free(reference);

    XrReferenceExecution *reference_cancel = NULL;
    ASSERT_TRUE(xr_reference_execution_create(instance, entry, NULL, 0u, NULL, &reference_cancel));
    ASSERT_EQ_INT(xr_reference_execution_step(reference_cancel).kind,
                  XR_REFERENCE_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_reference_execution_cancel(reference_cancel).kind,
                  XR_REFERENCE_OUTCOME_CANCELLED);
    xr_reference_execution_free(reference_cancel);

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
    ASSERT_EQ_INT(vm_outcome.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm_outcome.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm_outcome.value.as.i64, 42);
    xr_vm_execution_free(vm);

    XrVmExecution *vm_cancel = NULL;
    ASSERT_TRUE(xr_vm_execution_create(code, instance, entry, NULL, 0u, &vm_cancel));
    ASSERT_EQ_INT(xr_vm_execution_step(vm_cancel).kind, XR_VM_OUTCOME_SUSPENDED);
    ASSERT_EQ_INT(xr_vm_execution_cancel(vm_cancel).kind, XR_VM_OUTCOME_CANCELLED);
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
    ASSERT_NOT_NULL(strstr(generated.bytes, "int64_t * parameter_0"));
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
    ASSERT_EQ_UINT(backend_point->live_value_count, 1u);
    uint32_t backend_child_id = backend_call->immediate.coroutine_call.function_id;
    ASSERT_LT(backend_child_id, backend_ir->function_count);
    uint32_t backend_parameter_count = backend_ir->functions[backend_child_id].parameter_count;
    ASSERT_EQ_UINT(backend_parameter_count, 1u);
    ASSERT_LT(backend_parameter_count, backend_call->operand_count);
    uint32_t saved_live = backend_point->live_value_ids[0];
    uint32_t saved_operand = backend_call->operands[backend_parameter_count];
    backend_point->live_value_ids[0] = backend_call->operands[0];
    backend_call->operands[backend_parameter_count] = backend_call->operands[0];
    ASSERT_FALSE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_EQ_INT(backend_diagnostic.status, XR_BACKEND_INVARIANT_REJECTED);
    backend_point->live_value_ids[0] = saved_live;
    backend_call->operands[backend_parameter_count] = saved_operand;
    ASSERT_TRUE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_NULL(strstr(generated.bytes, "TargetPlan"));
    if (ref_parameter_coroutine_aot_output_path) {
        FILE *output = fopen(ref_parameter_coroutine_aot_output_path, "wb");
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
                    "    return 230;\n"
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
    if (affine_coroutine_result_aot_output_path) {
        FILE *output = fopen(affine_coroutine_result_aot_output_path, "wb");
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

TEST(source_owner_keeps_reachable_unlowered_sleep_fail_closed) {
    static const char source[] = "import time\n"
                                 "fn answer() -> i64 { time.sleep(1); return 0 }\n";
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
    ASSERT_EQ_INT(xr_program_source_build(&fixture.input, &product, &diagnostic),
                  XR_PROGRAM_SOURCE_BUILD_PROGRAM_REJECTED);
    ASSERT_EQ_INT(diagnostic.stage, XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE);
    ASSERT_EQ_INT(diagnostic.writer_status, XR_PROGRAM_BUILD_INVALID_INPUT);
    ASSERT_NOT_NULL(strstr(diagnostic.message, "inconsistent callable effect evidence"));
    ASSERT_NULL(product.artifact.bytes);
    ASSERT_NULL(product.program);

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
if (argc >= 2)
    cross_module_coroutine_aot_output_path = argv[1];
if (argc >= 3)
    function_parameter_aot_output_path = argv[2];
if (argc >= 4)
    clock_provider_aot_output_path = argv[3];
if (argc >= 5)
    provider_trap_cleanup_aot_output_path = argv[4];
if (argc >= 6)
    pipe_provider_aot_output_path = argv[5];
if (argc >= 7)
    pipe_close_failure_aot_output_path = argv[6];
if (argc >= 8)
    pipe_uncaught_error_aot_output_path = argv[7];
if (argc >= 9)
    pipe_cancel_cleanup_aot_output_path = argv[8];
if (argc >= 10)
    multi_safepoint_aot_output_path = argv[9];
if (argc >= 11)
    affine_coroutine_result_aot_output_path = argv[10];
if (argc >= 12)
    ref_parameter_coroutine_aot_output_path = argv[11];
if (argc > 12)
    return 2;
RUN_TEST(source_owner_single_module_is_deterministic_and_detached);
RUN_TEST(source_owner_two_module_graph_is_deterministic);
RUN_TEST(source_owner_cross_module_coroutine_call_has_one_program_and_private_executors);
RUN_TEST(source_owner_cross_module_static_method_coroutine_has_one_program_and_private_executors);
RUN_TEST(source_owner_function_parameter_callable_has_one_program_and_private_executors);
RUN_TEST(source_owner_clock_provider_is_exact_across_private_executors);
RUN_TEST(source_owner_provider_refusal_runs_nested_explicit_trap_cleanup);
RUN_TEST(source_owner_pipe_provider_and_fieldwise_constructor_are_canonical);
RUN_TEST(source_owner_pipe_failed_close_consumes_endpoints_once);
RUN_TEST(source_owner_pipe_uncaught_error_runs_cleanup);
RUN_TEST(source_owner_lowers_defer_panic_cleanup_across_private_executors);
RUN_TEST(source_owner_recovers_and_reconstructs_place_backed_defer_for_resume_and_cancel);
RUN_TEST(source_owner_runs_each_dense_coroutine_state_across_private_executors);
RUN_TEST(source_owner_keeps_ref_parameter_place_stable_across_child_suspension);
RUN_TEST(source_owner_cross_module_coroutine_transfers_affine_resource_result);
RUN_TEST(source_owner_keeps_reachable_unlowered_sleep_fail_closed);
RUN_TEST(source_owner_module_initializer_is_a_canonical_entry);
RUN_TEST(source_owner_rejects_non_authoritative_entry_identity);
RUN_TEST(source_owner_rejects_module_budget_before_analysis);
RUN_TEST(source_owner_reports_structured_analysis_failure);
TEST_MAIN_END()
