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
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "program/xr_program_source_build.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

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
RUN_TEST(source_owner_single_module_is_deterministic_and_detached);
RUN_TEST(source_owner_two_module_graph_is_deterministic);
RUN_TEST(source_owner_rejects_non_authoritative_entry_identity);
RUN_TEST(source_owner_rejects_module_budget_before_analysis);
RUN_TEST(source_owner_reports_structured_analysis_failure);
TEST_MAIN_END()
