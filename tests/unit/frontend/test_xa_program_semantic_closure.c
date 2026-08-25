/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xa_program_semantic_closure.c - Source-backed pre-Xi PSC KAT
 */

#include "../test_framework.h"

#include "frontend/analyzer/xa_program_semantic_closure.h"
#include "frontend/analyzer/xa_scalar_program_authority_internal.h"
#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/parser/xast.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xast_walk.h"
#include "frontend/parser/xparse.h"
#include "base/xmalloc.h"
#include "module/xmodule.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "plan/semantic/xr_program_semantic_closure.h"
#include "plan/semantic/xr_scalar_call_semantics.h"
#include "shared/xr_exact_scalar_registry.h"
#include "toolchain/xcompiler_session.h"
#include <string.h>

static const char kScalarSource[] = "fn add1(value: i64) -> i64 { return value + 1 }\n"
                                    "fn root() -> i64 { return add1(41) }\n";

static const char kLeafAggregateSource[] =
    "struct Pair { left: i64; right: i64 }\n"
    "fn swap(value: Pair) -> Pair { return Pair{left: value.right, right: value.left} }\n"
    "fn root() -> Pair { return swap(Pair{left: 1, right: 41}) }\n";

typedef struct ScalarFixture {
    XrModuleGraph *graph;
    XrModuleSpec *spec;
    XaAnalyzer *analyzer;
    XaTypedProgram *typed;
} ScalarFixture;

static XrModuleResolver *g_resolver;
static XrCompilerSession *g_session;

static void setup(void) {
    XrCompilerSessionConfig session_config = {0};
    XrModuleResolverConfig resolver_config = {0};
    g_session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(g_session);
    g_resolver = xr_module_resolver_new(&resolver_config);
    ASSERT_NOT_NULL(g_resolver);
}

static void teardown(void) {
    xr_module_resolver_free(g_resolver);
    xr_compiler_session_delete(g_session);
    g_resolver = NULL;
    g_session = NULL;
}

static bool fixture_analyze(ScalarFixture *fixture, const char *source, const char *namespace_id) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->graph = xr_module_graph_new(g_session, g_resolver);
    if (!fixture->graph)
        return false;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = namespace_id,
    };
    char *error = NULL;
    if (xr_module_graph_build_source(fixture->graph, &authority, source, &error) != 0) {
        xr_free(error);
        return false;
    }
    xr_free(error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 || fixture->graph->has_cycle ||
        fixture->graph->spec_count != 1 || fixture->graph->entry_index < 0)
        return false;
    fixture->spec = &fixture->graph->specs[fixture->graph->entry_index];
    fixture->analyzer = xa_analyzer_new(g_session);
    if (!fixture->analyzer)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    xa_analyzer_analyze(fixture->analyzer, "<psc-scalar>", fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, fixture->spec->ast,
                                                    &exports))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    return true;
}

static bool fixture_publish(ScalarFixture *fixture) {
    XaTypedProgramPublishResult result =
        xa_typed_program_publish(fixture->analyzer, fixture->spec->ast, NULL, 1);
    fixture->typed = result.program;
    return result.reason == XA_TYPED_PROGRAM_REASON_NONE && result.program;
}

static void fixture_cleanup(ScalarFixture *fixture) {
    xa_typed_program_free(fixture->typed);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    memset(fixture, 0, sizeof(*fixture));
}

typedef struct FindCallContext {
    AstNode *call;
} FindCallContext;

static bool find_call_child(AstNode *child, void *context);

static bool find_call(AstNode *node, FindCallContext *context) {
    if (!node || context->call)
        return true;
    if (node->type == AST_CALL_EXPR) {
        context->call = node;
        return true;
    }
    return xr_ast_for_each_child(node, find_call_child, context);
}

static bool find_call_child(AstNode *child, void *context) {
    return find_call(child, (FindCallContext *) context);
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void expect_strict_call_locator_rejection(bool match_caller_end, const char *namespace_id) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, namespace_id));

    FindCallContext found = {0};
    ASSERT_TRUE(find_call(fixture.spec->ast, &found));
    ASSERT_NOT_NULL(found.call);
    ASSERT_EQ_UINT(fixture.spec->ast->type, AST_PROGRAM);
    AstNode *caller = fixture.spec->ast->as.program.statements[1];
    ASSERT_NOT_NULL(caller);
    ASSERT_EQ_UINT(caller->type, AST_FUNCTION_DECL);

    found.call->line = caller->line;
    found.call->column = caller->column;
    if (match_caller_end) {
        found.call->end_line = caller->end_line;
        found.call->end_column = caller->end_column;
    }

    ASSERT_TRUE(fixture_publish(&fixture));
    const XaScalarProgramAuthority *authority = xa_typed_program_scalar_authority(fixture.typed);
    ASSERT_NOT_NULL(authority);
    char error[256] = {0};
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));

    XrProgramSemanticClosure *closure = NULL;
    ASSERT_FALSE(
        xa_typed_program_build_scalar_closure(fixture.typed, &closure, error, sizeof(error)));
    ASSERT_NULL(closure);
    fixture_cleanup(&fixture);
}

TEST(source_backed_scalar_snapshot_builds_verified_closure) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-positive"));
    ASSERT_TRUE(fixture_publish(&fixture));
    const XaScalarProgramAuthority *authority = xa_typed_program_scalar_authority(fixture.typed);
    ASSERT_NOT_NULL(authority);
    char error[256] = {0};
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    ASSERT_TRUE(fingerprint_equal(xa_scalar_program_authority_module(authority)->source_fingerprint,
                                  fixture.spec->source_content_fingerprint));

    XrProgramSemanticClosure *closure = NULL;
    ASSERT_TRUE(
        xa_typed_program_build_scalar_closure(fixture.typed, &closure, error, sizeof(error)));
    ASSERT_NOT_NULL(closure);
    ASSERT_TRUE(xr_program_semantic_closure_is_verified(closure));
    ASSERT_EQ_UINT(xr_program_semantic_closure_module_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_dependency_count(closure), 0);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_count(closure), 0);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_call_count(closure), 1);
    XrScalarI64FunctionContract nullary;
    XrScalarI64FunctionContract unary;
    XrFingerprint direct_call;
    ASSERT_TRUE(xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_NULLARY, &nullary));
    ASSERT_TRUE(xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY, &unary));
    ASSERT_TRUE(xr_scalar_i64_call_contract(&unary, &direct_call));
    uint32_t roots = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        bool entry = (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0;
        const XrScalarI64FunctionContract *expected = entry ? &nullary : &unary;
        roots += entry;
        const XaScalarFunctionAuthority *source_function = NULL;
        for (uint32_t j = 0; j < XA_SCALAR_PROGRAM_FUNCTION_COUNT; j++) {
            const XaScalarFunctionAuthority *candidate =
                xa_scalar_program_authority_function(authority, j);
            if (candidate &&
                memcmp(candidate->declaration_identity.bytes, row->declaration_identity.bytes,
                       sizeof(row->declaration_identity.bytes)) == 0)
                source_function = candidate;
        }
        ASSERT_NOT_NULL(source_function);
        ASSERT_EQ_UINT(row->declaration_locator.kind, source_function->declaration_span.kind);
        ASSERT_EQ_UINT(row->declaration_locator.start_line,
                       source_function->declaration_span.start_line);
        ASSERT_EQ_UINT(row->declaration_locator.start_column,
                       source_function->declaration_span.start_column);
        ASSERT_EQ_UINT(row->declaration_locator.end_line,
                       source_function->declaration_span.end_line);
        ASSERT_EQ_UINT(row->declaration_locator.end_column,
                       source_function->declaration_span.end_column);
        ASSERT_EQ_UINT(row->capability_mask, 0);
        ASSERT_TRUE(fingerprint_equal(row->signature_fingerprint, expected->signature_fingerprint));
        ASSERT_TRUE(fingerprint_equal(row->effect_fingerprint, expected->effect_fingerprint));
    }
    ASSERT_EQ_UINT(roots, 1);
    ASSERT_TRUE(fingerprint_equal(
        xr_program_semantic_closure_call(closure, 0)->contract_fingerprint, direct_call));
    const XaScalarCallAuthority *source_call = xa_scalar_program_authority_call(authority);
    const XrProgramSemanticCallRecord *projected_call =
        xr_program_semantic_closure_call(closure, 0);
    ASSERT_NOT_NULL(source_call);
    ASSERT_EQ_UINT(projected_call->locator.kind, source_call->callsite_span.kind);
    ASSERT_EQ_UINT(projected_call->locator.start_line, source_call->callsite_span.start_line);
    ASSERT_EQ_UINT(projected_call->locator.start_column, source_call->callsite_span.start_column);
    ASSERT_EQ_UINT(projected_call->locator.end_line, source_call->callsite_span.end_line);
    ASSERT_EQ_UINT(projected_call->locator.end_column, source_call->callsite_span.end_column);
    ASSERT_TRUE(projected_call->locator.end_line > projected_call->locator.start_line ||
                (projected_call->locator.end_line == projected_call->locator.start_line &&
                 projected_call->locator.end_column > projected_call->locator.start_column));
    xr_program_semantic_closure_free(closure);
    fixture_cleanup(&fixture);
}

TEST(source_backed_leaf_aggregate_publishes_typed_psc) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kLeafAggregateSource, "psc-leaf-aggregate"));
    ASSERT_TRUE(fixture_publish(&fixture));
    ASSERT_NULL(xa_typed_program_scalar_authority(fixture.typed));
    const XrProgramSemanticClosure *closure =
        xa_typed_program_program_semantic_closure(fixture.typed);
    ASSERT_NOT_NULL(closure);
    char error[256] = {0};
    ASSERT_TRUE(xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    ASSERT_EQ_UINT(xr_program_semantic_closure_family(closure),
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_field_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_parameter_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_call_count(closure), 1);
    uint32_t scalar_rows = 0;
    uint32_t aggregate_rows = 0;
    XrStableId aggregate_id = {{0}};
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        ASSERT_NOT_NULL(row);
        if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
            scalar_rows++;
            ASSERT_EQ_UINT(row->exact_scalar, XR_EXACT_SCALAR_I64);
            ASSERT_EQ_UINT(row->field_count, 0);
        } else {
            aggregate_rows++;
            aggregate_id = row->id;
            ASSERT_EQ_UINT(row->kind, XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE);
            ASSERT_EQ_UINT(row->field_count, 2);
        }
    }
    ASSERT_EQ_UINT(scalar_rows, 1);
    ASSERT_EQ_UINT(aggregate_rows, 1);
    uint32_t entries = 0;
    uint32_t callees = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        ASSERT_NOT_NULL(row);
        ASSERT_TRUE(
            memcmp(row->return_type.bytes, aggregate_id.bytes, sizeof(aggregate_id.bytes)) == 0);
        if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            entries++;
            ASSERT_EQ_UINT(row->parameter_count, 0);
        } else {
            callees++;
            ASSERT_EQ_UINT(row->flags, 0);
            ASSERT_EQ_UINT(row->parameter_count, 1);
            const XrProgramSemanticFunctionParameterRecord *parameter =
                xr_program_semantic_closure_function_parameter(closure, row->parameter_begin);
            ASSERT_NOT_NULL(parameter);
            ASSERT_EQ_UINT(parameter->mode, XR_PARAM_READ);
            ASSERT_TRUE(
                memcmp(parameter->type.bytes, aggregate_id.bytes, sizeof(aggregate_id.bytes)) == 0);
        }
    }
    ASSERT_EQ_UINT(entries, 1);
    ASSERT_EQ_UINT(callees, 1);
    fixture_cleanup(&fixture);
}

TEST(strict_call_locator_boundaries_fail_after_source_republication) {
    expect_strict_call_locator_rejection(false, "psc-scalar-call-shares-caller-start");
    expect_strict_call_locator_rejection(true, "psc-scalar-call-equals-caller-span");
}

TEST(published_snapshot_is_pointer_free_and_ignores_node_ids) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-owned"));
    ASSERT_TRUE(fixture_publish(&fixture));
    char error[256] = {0};
    XrProgramSemanticClosure *first = NULL;
    ASSERT_TRUE(xa_typed_program_build_scalar_closure(fixture.typed, &first, error, sizeof(error)));
    XrFingerprint first_fingerprint = xr_program_semantic_closure_fingerprint(first);

    FindCallContext found = {0};
    ASSERT_TRUE(find_call(fixture.spec->ast, &found));
    ASSERT_NOT_NULL(found.call);
    uint32_t original_node_id = found.call->node_id;
    int original_line = found.call->line;
    found.call->node_id += 1000;
    found.call->line += 100;
    const XaResolvedCall *resolved = xa_analyzer_get_resolved_call(fixture.analyzer, found.call);
    ASSERT_NULL(resolved);

    XrProgramSemanticClosure *second = NULL;
    ASSERT_TRUE(
        xa_typed_program_build_scalar_closure(fixture.typed, &second, error, sizeof(error)));
    ASSERT_TRUE(
        fingerprint_equal(first_fingerprint, xr_program_semantic_closure_fingerprint(second)));
    found.call->node_id = original_node_id;
    found.call->line = original_line;

    xr_program_semantic_closure_free(second);
    xr_program_semantic_closure_free(first);
    fixture_cleanup(&fixture);
}

TEST(snapshot_projects_after_analyzer_and_graph_teardown) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-lifetime"));
    ASSERT_TRUE(fixture_publish(&fixture));
    XaTypedProgram *typed = fixture.typed;
    fixture.typed = NULL;
    xa_analyzer_free(fixture.analyzer);
    fixture.analyzer = NULL;
    xr_module_graph_free(fixture.graph);
    fixture.graph = NULL;
    fixture.spec = NULL;

    char error[256] = {0};
    XrProgramSemanticClosure *closure = NULL;
    ASSERT_TRUE(xa_typed_program_build_scalar_closure(typed, &closure, error, sizeof(error)));
    ASSERT_TRUE(xr_program_semantic_closure_is_verified(closure));
    xr_program_semantic_closure_free(closure);
    xa_typed_program_free(typed);
}

TEST(compiler_only_graph_refuses_runtime_preload) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-no-runtime"));
    XrModule **modules = NULL;
    ASSERT_FALSE(xr_module_graph_preload(NULL, fixture.graph, &modules));
    ASSERT_NULL(modules);
    fixture_cleanup(&fixture);
}

TEST(call_authority_mutations_fail_independent_verification) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-mutation"));
    ASSERT_TRUE(fixture_publish(&fixture));
    XaScalarProgramAuthority *authority =
        (XaScalarProgramAuthority *) xa_typed_program_scalar_authority(fixture.typed);
    ASSERT_NOT_NULL(authority);
    char error[256] = {0};

    authority->call.caller_declaration_identity.bytes[0] ^= UINT8_C(0x40);
    ASSERT_FALSE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    authority->call.caller_declaration_identity.bytes[0] ^= UINT8_C(0x40);
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));

    authority->call.callsite_span.start_column++;
    ASSERT_FALSE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    authority->call.callsite_span.start_column--;
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    fixture_cleanup(&fixture);
}

TEST(incomplete_or_ambiguous_source_authority_fails_closed) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-negative"));
    AstNode *first = fixture.spec->ast->as.program.statements[0];
    AstNode *second = fixture.spec->ast->as.program.statements[1];
    XaScalarSourceSpan first_span = {
        .kind = first->type,
        .start_line = (uint32_t) first->line,
        .start_column = (uint32_t) first->column,
        .end_line = (uint32_t) first->end_line,
        .end_column = (uint32_t) first->end_column,
    };
    second->line = (int) first_span.start_line;
    second->column = (int) first_span.start_column;
    second->end_line = (int) first_span.end_line;
    second->end_column = (int) first_span.end_column;
    XaTypedProgramPublishResult duplicate =
        xa_typed_program_publish(fixture.analyzer, fixture.spec->ast, NULL, 1);
    ASSERT_NULL(duplicate.program);
    ASSERT_EQ_INT(duplicate.reason, XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY);
    XrProgramSemanticClosure *closure = NULL;
    char error[256] = {0};
    ASSERT_NULL(closure);
    fixture_cleanup(&fixture);

    XaAnalyzer *graphless = xa_analyzer_new(g_session);
    AstNode *program = xr_parse(g_session, kScalarSource);
    ASSERT_NOT_NULL(program);
    xa_analyzer_analyze(graphless, "graphless.xr", program);
    XaTypedProgramPublishResult result = xa_typed_program_publish(graphless, program, NULL, 0);
    ASSERT_NULL(result.program);
    ASSERT_EQ_INT(result.reason, XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY);
    xa_analyzer_free(graphless);
    xr_program_destroy(program);

    static const char outside_family[] = "fn echo(value: string) -> string { return value }\n"
                                         "fn root() -> string { return echo(\"x\") }\n";
    ASSERT_TRUE(fixture_analyze(&fixture, outside_family, "psc-nonscalar-family"));
    ASSERT_TRUE(fixture_publish(&fixture));
    ASSERT_NULL(xa_typed_program_scalar_authority(fixture.typed));
    ASSERT_FALSE(
        xa_typed_program_build_scalar_closure(fixture.typed, &closure, error, sizeof(error)));
    fixture_cleanup(&fixture);
}

TEST(resolved_target_mismatch_does_not_publish_scalar_authority) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-target"));
    FindCallContext found = {0};
    ASSERT_TRUE(find_call(fixture.spec->ast, &found));
    XaResolvedCall *resolved =
        (XaResolvedCall *) xa_analyzer_get_resolved_call(fixture.analyzer, found.call);
    ASSERT_NOT_NULL(resolved);
    resolved->target_symbol_id++;
    XaTypedProgramPublishResult result =
        xa_typed_program_publish(fixture.analyzer, fixture.spec->ast, NULL, 1);
    ASSERT_NULL(result.program);
    ASSERT_EQ_INT(result.reason, XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY);
    fixture_cleanup(&fixture);
}

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("source-backed program semantic closure");
RUN_TEST(source_backed_scalar_snapshot_builds_verified_closure);
RUN_TEST(source_backed_leaf_aggregate_publishes_typed_psc);
RUN_TEST(strict_call_locator_boundaries_fail_after_source_republication);
RUN_TEST(published_snapshot_is_pointer_free_and_ignores_node_ids);
RUN_TEST(snapshot_projects_after_analyzer_and_graph_teardown);
RUN_TEST(compiler_only_graph_refuses_runtime_preload);
RUN_TEST(call_authority_mutations_fail_independent_verification);
RUN_TEST(incomplete_or_ambiguous_source_authority_fails_closed);
RUN_TEST(resolved_target_mismatch_does_not_publish_scalar_authority);
teardown();
TEST_MAIN_END()
