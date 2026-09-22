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
#include "frontend/analyzer/xa_node_table.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xparse.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

#include <string.h>

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

TEST(call_error_effect_snapshot_preserves_flow_sensitive_fact) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(analyzer, "call-effect.xr",
                                         "fn run() -> i64 {\n"
                                         "  var action = fn() -> i64 { return 42 }\n"
                                         "  return action()\n"
                                         "}\n");
    ASSERT_NOT_NULL(program);
    ASSERT_EQ_INT(program->type, AST_PROGRAM);
    ASSERT_EQ_INT(program->as.program.count, 1);
    AstNode *run = program->as.program.statements[0];
    ASSERT_NOT_NULL(run);
    ASSERT_EQ_INT(run->type, AST_FUNCTION_DECL);
    AstNode *body = run->as.function_decl.body;
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_INT(body->type, AST_BLOCK);
    ASSERT_EQ_INT(body->as.block.count, 2);
    AstNode *return_statement = body->as.block.statements[1];
    ASSERT_NOT_NULL(return_statement);
    ASSERT_EQ_INT(return_statement->type, AST_RETURN_STMT);
    ASSERT_EQ_INT(return_statement->as.return_stmt.value_count, 1);
    AstNode *call = return_statement->as.return_stmt.values[0];
    ASSERT_NOT_NULL(call);
    ASSERT_EQ_INT(call->type, AST_CALL_EXPR);

    XaCallErrorEffectFact analyzed = {0};
    ASSERT_TRUE(xa_analyzer_get_call_error_effect(analyzer, call, &analyzed));
    ASSERT_EQ_INT(analyzed.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_EQ_INT(analyzed.completeness, XA_EFFECT_COMPLETE);
    ASSERT_EQ_UINT(analyzed.unknown_reasons, XA_UNKNOWN_NONE);
    ASSERT_TRUE(analyzed.effect_id != XA_EFFECT_NONE);

    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, program, NULL, 0);
    ASSERT_NOT_NULL(result.program);

    XaCallErrorEffectFact replacement = {
        .effect_id = analyzed.effect_id,
        .throw_effect = XR_FN_EFFECT_MAY_THROW,
        .completeness = XA_EFFECT_INCOMPLETE,
        .unknown_reasons = XA_UNKNOWN_DYNAMIC_CALL_TARGET,
    };
    ASSERT_TRUE(xa_analyzer_set_call_error_effect(analyzer, call, &replacement));

    XaCallErrorEffectFact published = {0};
    ASSERT_TRUE(xa_typed_program_call_error_effect(result.program, call, &published));
    ASSERT_EQ_INT(published.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_EQ_INT(published.completeness, XA_EFFECT_COMPLETE);
    ASSERT_EQ_UINT(published.unknown_reasons, XA_UNKNOWN_NONE);
    ASSERT_EQ_UINT(published.effect_id, analyzed.effect_id);

    xa_typed_program_free(result.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(function_expr_effect_snapshot_is_distinct_and_immutable) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(analyzer, "function-expr-effect.xr",
                                         "fn run() -> i64 {\n"
                                         "  var action = fn() -> i64 { return 42 }\n"
                                         "  return action()\n"
                                         "}\n");
    ASSERT_NOT_NULL(program);
    AstNode *run = program->as.program.statements[0];
    ASSERT_NOT_NULL(run);
    AstNode *body = run->as.function_decl.body;
    ASSERT_NOT_NULL(body);
    AstNode *binding = body->as.block.statements[0];
    ASSERT_NOT_NULL(binding);
    AstNode *function_expr = binding->as.var_decl.initializer;
    ASSERT_NOT_NULL(function_expr);
    ASSERT_EQ_INT(function_expr->type, AST_FUNCTION_EXPR);

    XaBodyEffectFact analyzed = {0};
    ASSERT_TRUE(xa_analyzer_get_body_effect(analyzer, function_expr, &analyzed));
    ASSERT_TRUE(analyzed.effect_id != XA_EFFECT_NONE);
    ASSERT_EQ_INT(analyzed.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_EQ_INT(analyzed.completeness, XA_EFFECT_COMPLETE);
    ASSERT_EQ_UINT(analyzed.unknown_reasons, XA_UNKNOWN_NONE);
    const XaEffectSummary *summary = xa_effect_db_get(analyzer->effect_db, analyzed.effect_id);
    ASSERT_NOT_NULL(summary);
    ASSERT_TRUE(xa_effect_summary_is_complete(summary));
    ASSERT_TRUE(xa_effect_summary_is_nothrow(summary));
    XaCallErrorEffectFact unrelated_call_fact = {0};
    ASSERT_FALSE(
        xa_analyzer_get_call_error_effect(analyzer, function_expr, &unrelated_call_fact));

    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, program, NULL, 0);
    ASSERT_NOT_NULL(result.program);

    XaBodyEffectFact replacement = {
        .effect_id = analyzed.effect_id,
        .throw_effect = XR_FN_EFFECT_MAY_THROW,
        .completeness = XA_EFFECT_INCOMPLETE,
        .unknown_reasons = XA_UNKNOWN_DYNAMIC_CALL_TARGET,
    };
    ASSERT_TRUE(xa_analyzer_set_body_effect(analyzer, function_expr, &replacement));

    XaBodyEffectFact published = {0};
    ASSERT_TRUE(xa_typed_program_body_effect(result.program, function_expr, &published));
    ASSERT_EQ_UINT(published.effect_id, analyzed.effect_id);
    ASSERT_EQ_INT(published.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_EQ_INT(published.completeness, XA_EFFECT_COMPLETE);
    ASSERT_EQ_UINT(published.unknown_reasons, XA_UNKNOWN_NONE);

    xa_typed_program_free(result.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(target_query_snapshot_preserves_compiler_owned_identity) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program =
        parse_and_analyze(analyzer, "target-query.xr", "var bits: u16 = target.pointerBits\n");
    ASSERT_NOT_NULL(program);
    ASSERT_EQ_UINT(analyzer->diagnostic_count, 0);
    AstNode *decl = program->as.program.statements[0];
    AstNode *query = decl && decl->type == AST_VAR_DECL ? decl->as.var_decl.initializer : NULL;
    ASSERT_NOT_NULL(query);

    XaTargetQueryFact analyzer_fact = {0};
    ASSERT_TRUE(xa_analyzer_get_target_query(analyzer, query, &analyzer_fact));
    ASSERT_EQ_UINT(analyzer_fact.namespace_id, XA_TARGET_NAMESPACE_TARGET);
    ASSERT_EQ_UINT(analyzer_fact.query_id, XA_TARGET_QUERY_POINTER_BITS);
    ASSERT_EQ_UINT(analyzer_fact.result_native_type, XR_NATIVE_U16);
    ASSERT_EQ_UINT(analyzer_fact.complete, 1);

    XaTypedProgramPublishResult result = xa_typed_program_publish(analyzer, program, NULL, 0);
    ASSERT_NOT_NULL(result.program);
    xa_analyzer_clear_target_query(analyzer, query);
    XaTargetQueryFact frozen = {0};
    ASSERT_TRUE(xa_typed_program_target_query(result.program, query, &frozen));
    ASSERT_EQ_UINT(frozen.namespace_id, XA_TARGET_NAMESPACE_TARGET);
    ASSERT_EQ_UINT(frozen.query_id, XA_TARGET_QUERY_POINTER_BITS);
    ASSERT_EQ_UINT(frozen.result_native_type, XR_NATIVE_U16);
    ASSERT_EQ_UINT(frozen.complete, 1);

    xa_typed_program_free(result.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

static bool has_target_namespace_redeclaration(XaAnalyzer *analyzer) {
    int count = 0;
    for (XaDiagnostic *diagnostic = xa_analyzer_get_diagnostics(analyzer, &count); diagnostic;
         diagnostic = diagnostic->next) {
        if (diagnostic->severity == XR_DIAG_SEV_ERROR && diagnostic->message &&
            strstr(diagnostic->message, "compiler-owned target namespace"))
            return true;
    }
    return false;
}

TEST(target_namespace_is_unshadowable_at_every_source_binding_gate) {
    static const struct {
        const char *name;
        const char *source;
    } cases[] = {
        {"top-level", "var target = 1\n"},
        {"local", "fn read() -> i64 {\n var target = 7\n return target\n}\n"},
        {"parameter", "fn read(target: i64) -> i64 { return target }\n"},
        {"const", "const target = 7\n"},
        {"catch",
         "enum Failure { Bad }\n"
         "fn read() {\n try { throw Failure.Bad } catch (target) { return }\n}\n"},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        XaAnalyzer *analyzer = xa_analyzer_new(g_session);
        AstNode *program = parse_and_analyze(analyzer, cases[index].name, cases[index].source);
        ASSERT_NOT_NULL(program);
        ASSERT_TRUE(has_target_namespace_redeclaration(analyzer));
        xa_analyzer_free(analyzer);
        xr_program_destroy(program);
    }
}

TEST(target_enum_member_and_query_share_one_canonical_type) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(
        analyzer, "target-os-member.xr",
        "fn is_darwin() -> bool { return target.os == TargetOs.Darwin }\n");
    ASSERT_NOT_NULL(program);
    ASSERT_EQ_UINT(analyzer->diagnostic_count, 0);
    AstNode *function = program->as.program.statements[0];
    AstNode *body = function ? function->as.function_decl.body : NULL;
    AstNode *ret = body && body->type == AST_BLOCK ? body->as.block.statements[0] : NULL;
    AstNode *equal = ret && ret->type == AST_RETURN_STMT ? ret->as.return_stmt.values[0] : NULL;
    ASSERT_NOT_NULL(equal);
    ASSERT_EQ_INT(equal->type, AST_BINARY_EQ);
    XaTargetQueryFact fact = {0};
    ASSERT_TRUE(xa_analyzer_get_target_query(analyzer, equal->as.binary.left, &fact));
    ASSERT_EQ_UINT(fact.query_id, XA_TARGET_QUERY_OPERATING_SYSTEM);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(top_level_call_publishes_flow_sensitive_error_effect) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program = parse_and_analyze(analyzer, "top-level-call-error-effect.xr",
                                         "var action = fn() -> i64 { return 42 }\n"
                                         "var result = action()\n");
    ASSERT_NOT_NULL(program);
    ASSERT_EQ_INT(program->type, AST_PROGRAM);
    ASSERT_EQ_INT(program->as.program.count, 2);
    AstNode *result = program->as.program.statements[1];
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_INT(result->type, AST_VAR_DECL);
    AstNode *call = result->as.var_decl.initializer;
    ASSERT_NOT_NULL(call);
    ASSERT_EQ_INT(call->type, AST_CALL_EXPR);

    XaCallErrorEffectFact fact = {0};
    ASSERT_TRUE(xa_analyzer_get_call_error_effect(analyzer, call, &fact));
    ASSERT_EQ_INT(fact.throw_effect, XR_FN_EFFECT_NO_THROW);
    ASSERT_EQ_INT(fact.completeness, XA_EFFECT_COMPLETE);
    ASSERT_EQ_UINT(fact.unknown_reasons, XA_UNKNOWN_NONE);
    ASSERT_TRUE(fact.effect_id != XA_EFFECT_NONE);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
}

TEST(module_body_effect_snapshot_preserves_exact_escaping_errors) {
    XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_MEMORY,
        .module_identity = "memory-module-v1:id=18:initializer-effect",
    };
    ASSERT_TRUE(xr_compiler_session_set_compile_unit_identity(g_session, &identity));
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    AstNode *program =
        parse_and_analyze(analyzer, "initializer-effect.xr",
                          "enum InitFailure { Rejected { code: i64 } }\n"
                          "fn fail() -> i64 { throw InitFailure.Rejected { code: 7 } }\n"
                          "fail()\n");
    ASSERT_NOT_NULL(program);
    xa_analyzer_print_errors(analyzer, "initializer-effect.xr");
    ASSERT_EQ_UINT(analyzer->diagnostic_count, 0u);
    XaBodyEffectFact analyzed = {0};
    ASSERT_TRUE(xa_analyzer_get_body_effect(analyzer, program, &analyzed));
    ASSERT_EQ_INT(analyzed.throw_effect, XR_FN_EFFECT_MAY_THROW);
    const XaEffectSummary *effect = xa_effect_db_get(analyzer->effect_db, analyzed.effect_id);
    ASSERT_NOT_NULL(effect);
    ASSERT_EQ_INT(effect->error_set_completeness, XA_EFFECT_COMPLETE);
    ASSERT_EQ_UINT(effect->error_unknown_reasons, XA_UNKNOWN_NONE);
    ASSERT_EQ_UINT(effect->escaping.count, 1u);
    XaTypedProgramPublishResult published = xa_typed_program_publish(analyzer, program, NULL, 0);
    ASSERT_NOT_NULL(published.program);
    XaBodyEffectFact changed = analyzed;
    changed.throw_effect = XR_FN_EFFECT_NO_THROW;
    ASSERT_TRUE(xa_analyzer_set_body_effect(analyzer, program, &changed));
    XaBodyEffectFact snapshot = {0};
    ASSERT_TRUE(xa_typed_program_body_effect(published.program, program, &snapshot));
    ASSERT_EQ_UINT(snapshot.effect_id, analyzed.effect_id);
    ASSERT_EQ_INT(snapshot.throw_effect, XR_FN_EFFECT_MAY_THROW);
    xa_typed_program_free(published.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    ASSERT_TRUE(xr_compiler_session_set_compile_unit_identity(g_session, NULL));
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
RUN_TEST(call_error_effect_snapshot_preserves_flow_sensitive_fact);
RUN_TEST(function_expr_effect_snapshot_is_distinct_and_immutable);
RUN_TEST(target_query_snapshot_preserves_compiler_owned_identity);
RUN_TEST(target_namespace_is_unshadowable_at_every_source_binding_gate);
RUN_TEST(target_enum_member_and_query_share_one_canonical_type);
RUN_TEST(top_level_call_publishes_flow_sensitive_error_effect);
RUN_TEST(module_body_effect_snapshot_preserves_exact_escaping_errors);
RUN_TEST(error_diagnostic_blocks_publication);
teardown();
TEST_MAIN_END()
