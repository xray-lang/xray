/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xa_typed_program.c - Verified analyzer publication contract
 */

#include "../test_framework.h"

#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xparse.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

static XrVMRuntime *g_isolate = NULL;
static XrCompilerSession *g_session = NULL;

static void setup(void) {
    XrVMConfig vm_config = {0};
    g_isolate = xray_vm_new_full(&vm_config);
    g_session = xr_compiler_session_current_for_isolate(g_isolate);
    ASSERT_NOT_NULL(g_session);
}

static void teardown(void) {
    xray_vm_delete(g_isolate);
    g_isolate = NULL;
    g_session = NULL;
}

static AstNode *parse_and_analyze(XaAnalyzer *analyzer, const char *file, const char *source) {
    AstNode *program = xr_parse(g_session, source);
    if (program)
        xa_analyzer_analyze(analyzer, file, program);
    return program;
}

TEST(publishes_verified_current_snapshot) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(analyzer, "typed.xr", "var value = 42\n");
    ASSERT_NOT_NULL(program);

    uint64_t revision = analyzer->semantic_revision;
    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, program, NULL, 7);

    ASSERT_NOT_NULL(result.program);
    ASSERT_EQ_INT(result.reason, XA_TYPED_PROGRAM_REASON_NONE);
    ASSERT_TRUE(xa_typed_program_is_verified(result.program));
    ASSERT_TRUE(xa_typed_program_is_current(result.program));
    ASSERT_EQ_UINT(xa_typed_program_semantic_revision(result.program), revision);
    ASSERT_EQ_PTR(xa_typed_program_syntax(result.program), program);
    ASSERT_EQ_PTR(xa_typed_program_semantics(result.program), analyzer);
    ASSERT_EQ_UINT(xa_typed_program_module_id(result.program), 7);

    xa_typed_program_free(result.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(reanalysis_invalidates_old_snapshot) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *first = parse_and_analyze(analyzer, "first.xr", "var first = 1\n");
    ASSERT_NOT_NULL(first);
    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, first, NULL, 0);
    ASSERT_NOT_NULL(result.program);
    ASSERT_TRUE(xa_typed_program_is_current(result.program));

    AstNode *second = parse_and_analyze(analyzer, "second.xr", "var second = 2\n");
    ASSERT_NOT_NULL(second);
    ASSERT_FALSE(xa_typed_program_is_current(result.program));

    xa_typed_program_free(result.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(second);
    xr_program_destroy(first);
}

TEST(conversion_snapshot_is_owned_and_immutable) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(analyzer, "conversion.xr", "var value = 42\n");
    ASSERT_NOT_NULL(program);

    XrConversionWitness original = {
        .kind = XR_CONVERSION_LOSSLESS_WIDEN,
        .source_scalar_rep = XR_NATIVE_U8,
        .target_scalar_rep = XR_NATIVE_U64,
        .is_implicit = true,
    };
    xa_analyzer_set_node_conversion(analyzer, program, &original);
    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, program, NULL, 0);
    ASSERT_NOT_NULL(result.program);

    XrConversionWitness replacement = {
        .kind = XR_CONVERSION_EXPLICIT_TRUNCATE,
        .source_scalar_rep = XR_NATIVE_U64,
        .target_scalar_rep = XR_NATIVE_U8,
    };
    xa_analyzer_set_node_conversion(analyzer, program, &replacement);

    XrConversionWitness published = {0};
    ASSERT_TRUE(xa_typed_program_conversion(result.program, program, &published));
    ASSERT_EQ_INT(published.kind, original.kind);
    ASSERT_EQ_INT(published.source_scalar_rep, original.source_scalar_rep);
    ASSERT_EQ_INT(published.target_scalar_rep, original.target_scalar_rep);
    ASSERT_TRUE(published.is_implicit);

    xa_typed_program_free(result.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(error_diagnostic_blocks_publication) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(analyzer, "error.xr", "var value = 42\n");
    ASSERT_NOT_NULL(program);

    XrLocation location = {.file = "error.xr", .line = 3, .column = 5};
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                               "synthetic semantic failure", &location);
    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, program, NULL, 0);

    ASSERT_NULL(result.program);
    ASSERT_EQ_INT(result.reason, XA_TYPED_PROGRAM_REASON_ANALYZER_ERROR);
    ASSERT_EQ_UINT(result.source_line, 3);
    ASSERT_STR_EQ(result.detail, "synthetic semantic failure");

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("typed program publication");
RUN_TEST(publishes_verified_current_snapshot);
RUN_TEST(reanalysis_invalidates_old_snapshot);
RUN_TEST(conversion_snapshot_is_owned_and_immutable);
RUN_TEST(error_diagnostic_blocks_publication);
teardown();
TEST_MAIN_END()
