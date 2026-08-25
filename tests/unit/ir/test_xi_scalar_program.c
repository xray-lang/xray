/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_scalar_program.c - PSC/CallDecision to Xi binding KAT
 */

#include "../test_framework.h"

#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "frontend/analyzer/xa_program_semantic_closure.h"
#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "ir/xi_lower.h"
#include "ir/xi_scalar_program.h"
#include "ir/xi_scalar_semantic_plan.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_verify.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/target/xr_target_profile.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "toolchain/xcompiler_session.h"
#include <string.h>

static const char kScalarSource[] =
    "fn add1(value: i64) -> i64 { return value + 1 }\n"
    "fn root() -> i64 { return add1(41) }\n";

typedef struct ScalarFixture {
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    XrModuleSpec *spec;
    XaAnalyzer *analyzer;
    XaTypedProgram *typed;
} ScalarFixture;

static bool fixture_analyze(ScalarFixture *fixture,
                            XrCompilerSession *session,
                            const char *namespace_id) {
    memset(fixture, 0, sizeof(*fixture));
    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    if (!fixture->resolver)
        return false;
    fixture->graph = xr_module_graph_new(session, fixture->resolver);
    if (!fixture->graph)
        return false;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = namespace_id,
    };
    char *error = NULL;
    if (xr_module_graph_build_source(fixture->graph, &authority, kScalarSource,
                                     &error) != 0) {
        xr_free(error);
        return false;
    }
    xr_free(error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 ||
        fixture->graph->has_cycle || fixture->graph->spec_count != 1 ||
        fixture->graph->entry_index < 0)
        return false;
    fixture->spec =
        &fixture->graph->specs[fixture->graph->entry_index];
    fixture->analyzer = xa_analyzer_new(session);
    if (!fixture->analyzer)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    xa_analyzer_analyze(fixture->analyzer, "scalar-binding.xr",
                        fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(
             fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(
            fixture->analyzer, fixture->spec->ast, &exports))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    return true;
}

static bool fixture_publish(ScalarFixture *fixture) {
    XaTypedProgramPublishResult result = xa_typed_program_publish(
        fixture->analyzer, fixture->spec->ast, NULL, 1);
    fixture->typed = result.program;
    return result.reason == XA_TYPED_PROGRAM_REASON_NONE && result.program;
}

static void fixture_cleanup(ScalarFixture *fixture) {
    xa_typed_program_free(fixture->typed);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    xr_module_resolver_free(fixture->resolver);
    memset(fixture, 0, sizeof(*fixture));
}

static bool same_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static XiFunc *find_function(XiModule *module,
                             const XrProgramSemanticClosure *closure,
                             XrStableId identity) {
    for (uint16_t i = 0; module && i < module->nfuncs; i++) {
        XiFunc *function = module->functions[i];
        const XrProgramSemanticFunctionRecord *row =
            function ? xr_program_semantic_closure_function(
                           closure, function->psc_function_index)
                     : NULL;
        if (row && same_id(row->id, identity))
            return function;
    }
    return NULL;
}

static XiValue *find_bound_call(XiFunc *function) {
    for (uint32_t b = 0; function && b < function->nblocks; b++) {
        XiBlock *block = function->blocks[b];
        for (uint32_t i = 0; block && i < block->nvalues; i++) {
            XiValue *value = block->values[i];
            if (value && value->psc_call_index != XI_PSC_ROW_NONE)
                return value;
        }
    }
    return NULL;
}

static void mark_tree_optimized(XiFunc *function) {
    if (!function)
        return;
    function->stage = XI_STAGE_OPTIMIZED;
    for (uint16_t i = 0; i < function->nchildren; i++)
        mark_tree_optimized(function->children[i]);
}

static bool build_authorities(
    ScalarFixture *fixture, XrTargetProfile *profile,
    XrProgramSemanticClosure **closure, XrScalarCallDecision *decision,
    char *error, size_t error_size) {
    if (!xa_typed_program_build_scalar_closure(
            fixture->typed, closure, error, error_size))
        return false;
    return xr_scalar_call_decision_build(
               *closure,
               xr_program_semantic_closure_generation_id(*closure), profile,
               decision, error, error_size) &&
           xr_scalar_call_decision_verify(decision, *closure, profile, error,
                                          error_size);
}

TEST(stable_rows_survive_mutation_and_ownership_gates) {
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, session,
                                "xi-scalar-binding-direct"));
    ASSERT_TRUE(fixture_publish(&fixture));

    char error[512] = {0};
    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(
        &profile, error, sizeof(error)));
    XrProgramSemanticClosure *closure = NULL;
    XrScalarCallDecision decision = {0};
    ASSERT_TRUE(build_authorities(&fixture, profile, &closure, &decision,
                                  error, sizeof(error)));
    XrProgramSemanticClosure *retained =
        xr_program_semantic_closure_retain(closure);
    ASSERT_TRUE(retained == closure);

    XiScalarProgramInput input = {
        .closure = closure,
        .decision = &decision,
    };
    XiFunc *root =
        xi_lower_program(fixture.typed, NULL, false, &input);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(root->module);
    ASSERT_TRUE(xi_module_take_scalar_program(
        root->module, &closure, &decision, profile, error, sizeof(error)));
    ASSERT_NULL(closure);
    bool verified = xi_scalar_program_verify(root->module, profile, error,
                                             sizeof(error));
    if (!verified)
        fprintf(stderr, "Xi scalar verify failed: %s\n", error);
    ASSERT_TRUE(verified);

    XiFunc *caller = find_function(
        root->module, retained, decision.caller_function);
    XiFunc *callee = find_function(
        root->module, retained, decision.callee_function);
    XiValue *call = find_bound_call(caller);
    ASSERT_NOT_NULL(caller);
    ASSERT_NOT_NULL(callee);
    ASSERT_NOT_NULL(call);
    ASSERT_EQ_UINT(callee->inline_policy, XI_INLINE_PRESERVE_CALL);

    uint32_t saved_function_index = caller->psc_function_index;
    caller->psc_function_index = callee->psc_function_index;
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    caller->psc_function_index = saved_function_index;

    uint32_t saved_locator_kind = caller->psc_declaration_locator.kind;
    caller->psc_declaration_locator.kind ^= UINT32_C(1);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    caller->psc_declaration_locator.kind = saved_locator_kind;

    uint32_t saved_call_index = call->psc_call_index;
    call->psc_call_index = XI_PSC_ROW_NONE;
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    call->psc_call_index = saved_call_index;

    uint32_t saved_source_kind = call->source_kind;
    call->source_kind ^= UINT32_C(1);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    call->source_kind = saved_source_kind;

    root->module->scalar_call_decision->generation_id.bytes[0] ^=
        UINT8_C(0x40);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    root->module->scalar_call_decision->generation_id.bytes[0] ^=
        UINT8_C(0x40);
    root->module->scalar_call_decision->call_identity.bytes[0] ^=
        UINT8_C(0x20);
    ASSERT_FALSE(xi_scalar_program_verify(root->module, profile, error,
                                          sizeof(error)));
    root->module->scalar_call_decision->call_identity.bytes[0] ^=
        UINT8_C(0x20);
    ASSERT_TRUE(xi_scalar_program_verify(root->module, profile, error,
                                         sizeof(error)));

    mark_tree_optimized(root);
    ASSERT_TRUE(xi_module_set_identity(
        root->module,
        "memory-module-v1:id=26:xi-scalar-semantic-plan-v1"));
    XrSemanticPlan *semantic = NULL;
    bool semantic_built = xr_semantic_plan_build(
        root, &semantic, error, sizeof(error));
    if (!semantic_built)
        fprintf(stderr, "Scalar SemanticPlan build failed: %s\n", error);
    ASSERT_TRUE(semantic_built);
    ASSERT_NOT_NULL(semantic);
    ASSERT_TRUE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    ASSERT_EQ_UINT(xr_semantic_plan_function_count(semantic), 3);
    ASSERT_EQ_UINT(xr_semantic_plan_call_target_count(semantic), 1);
    XrFingerprint retained_fingerprint =
        xr_program_semantic_closure_fingerprint(retained);
    XrGenerationClosureId retained_generation =
        xr_program_semantic_closure_generation_id(retained);
    ASSERT_EQ_UINT(semantic->program_provenance.schema,
                   XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION);
    ASSERT_EQ_UINT(semantic->program_provenance.program_schema,
                   xr_program_semantic_closure_schema(retained));
    ASSERT_EQ_UINT(semantic->program_function_binding_count, 2);
    ASSERT_EQ_UINT(semantic->program_call_binding_count, 1);
    ASSERT_TRUE(xr_fingerprint_equal(
        semantic->program_provenance.program_fingerprint,
        retained_fingerprint));
    ASSERT_TRUE(memcmp(semantic->program_provenance.generation_identity.bytes,
                       retained_generation.bytes,
                       sizeof(retained_generation.bytes)) == 0);

    uint32_t saved_target =
        semantic->program_call_bindings[0].target_function;
    semantic->program_call_bindings[0].target_function =
        XR_SEMANTIC_INDEX_NONE;
    ASSERT_FALSE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    semantic->program_call_bindings[0].target_function = saved_target;
    semantic->program_provenance.program_fingerprint.bytes[0] ^=
        UINT8_C(0x80);
    ASSERT_FALSE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    semantic->program_provenance.program_fingerprint.bytes[0] ^=
        UINT8_C(0x80);
    uint32_t saved_provenance_schema = semantic->program_provenance.schema;
    semantic->program_provenance.schema = 0;
    ASSERT_FALSE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    semantic->program_provenance.schema = saved_provenance_schema;
    ASSERT_TRUE(xi_scalar_semantic_plan_verify(
        root, semantic, profile, error, sizeof(error)));
    XrFingerprint saved_plan_fingerprint = semantic->fingerprint;
    semantic->program_call_bindings[0].reserved = 1;
    xr_semantic_plan_compute_fingerprint(semantic, &semantic->fingerprint);
    ASSERT_FALSE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    semantic->program_call_bindings[0].reserved = 0;
    semantic->fingerprint = saved_plan_fingerprint;

    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    ASSERT_TRUE(xr_xsm_encode(semantic, &encoded, &encoded_size, error,
                              sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    ASSERT_TRUE(xr_xsm_decode(encoded, encoded_size, &decoded, error,
                              sizeof(error)));
    ASSERT_TRUE(decoded != NULL &&
                decoded->program_function_binding_count == 2 &&
                decoded->program_call_binding_count == 1 &&
                xr_fingerprint_equal(
                    xr_semantic_plan_fingerprint(decoded),
                    xr_semantic_plan_fingerprint(semantic)));
    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    ASSERT_TRUE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error,
                              sizeof(error)));
    ASSERT_EQ_UINT(roundtrip_size, encoded_size);
    ASSERT_TRUE(memcmp(roundtrip, encoded, encoded_size) == 0);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);

    ASSERT_TRUE(encoded_size > XR_XSM_HEADER_SIZE + 303u);
    uint8_t *hostile = (uint8_t *) xr_malloc(encoded_size);
    ASSERT_NOT_NULL(hostile);
    memcpy(hostile, encoded, encoded_size);
    semantic->program_call_bindings[0].reserved = 1;
    XrFingerprint hostile_fingerprint;
    xr_semantic_plan_compute_fingerprint(semantic, &hostile_fingerprint);
    semantic->program_call_bindings[0].reserved = 0;
    hostile[XR_XSM_HEADER_SIZE + 300u] = UINT8_C(1);
    memcpy(hostile + XR_XSM_HEADER_SIZE - XR_FINGERPRINT_BYTES,
           hostile_fingerprint.bytes, sizeof(hostile_fingerprint.bytes));
    xr_sha256(hostile + XR_XSM_HEADER_SIZE,
              encoded_size - XR_XSM_HEADER_SIZE, hostile + 24u);
    decoded = NULL;
    ASSERT_FALSE(xr_xsm_decode(hostile, encoded_size, &decoded, error,
                               sizeof(error)));
    ASSERT_NULL(decoded);
    xr_free(hostile);

    hostile = (uint8_t *) xr_malloc(encoded_size);
    ASSERT_NOT_NULL(hostile);
    memcpy(hostile, encoded, encoded_size);
    semantic->program_provenance.schema = 0;
    xr_semantic_plan_compute_fingerprint(semantic, &hostile_fingerprint);
    semantic->program_provenance.schema = saved_provenance_schema;
    memset(hostile + XR_XSM_HEADER_SIZE + 96u, 0, sizeof(uint32_t));
    memcpy(hostile + XR_XSM_HEADER_SIZE - XR_FINGERPRINT_BYTES,
           hostile_fingerprint.bytes, sizeof(hostile_fingerprint.bytes));
    xr_sha256(hostile + XR_XSM_HEADER_SIZE,
              encoded_size - XR_XSM_HEADER_SIZE, hostile + 24u);
    decoded = NULL;
    ASSERT_FALSE(xr_xsm_decode(hostile, encoded_size, &decoded, error,
                               sizeof(error)));
    ASSERT_NULL(decoded);
    xr_free(hostile);
    xr_free(encoded);

    XrProgramSemanticClosure *second = NULL;
    XrScalarCallDecision second_decision = {0};
    ASSERT_TRUE(build_authorities(&fixture, profile, &second,
                                  &second_decision, error, sizeof(error)));
    XrProgramSemanticClosure *second_owner = second;
    ASSERT_FALSE(xi_module_take_scalar_program(
        root->module, &second, &second_decision, profile, error,
        sizeof(error)));
    ASSERT_TRUE(second == second_owner);
    xr_program_semantic_closure_free(second);

    xi_func_free(root);
    ASSERT_TRUE(xr_semantic_plan_verify(semantic, error, sizeof(error)));
    xr_semantic_plan_free(semantic);
    ASSERT_TRUE(xr_program_semantic_closure_verify(retained, error,
                                                   sizeof(error)));
    xr_program_semantic_closure_free(retained);
    xr_target_profile_free(profile);
    fixture_cleanup(&fixture);
    xr_compiler_session_delete(session);
}

TEST(retain_rejects_mutable_closure) {
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1,
        .max_dependencies = 0,
        .max_types = 0,
        .max_functions = 2,
        .max_calls = 1,
    };
    XrFingerprint policy = {{0}};
    policy.bytes[0] = 1;
    XrProgramSemanticClosure *collecting = NULL;
    char error[256] = {0};
    ASSERT_TRUE(xr_program_semantic_closure_create(
        &limits, policy, &collecting, error, sizeof(error)));
    ASSERT_NULL(xr_program_semantic_closure_retain(collecting));
    xr_program_semantic_closure_free(collecting);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("PSC/CallDecision to Xi binding");
RUN_TEST(stable_rows_survive_mutation_and_ownership_gates);
RUN_TEST(retain_rejects_mutable_closure);
TEST_MAIN_END()
