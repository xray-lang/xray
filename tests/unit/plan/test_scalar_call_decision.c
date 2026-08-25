/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_scalar_call_decision.c - Sealed scalar CallDecision tests
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/frontend/analyzer/xa_program_semantic_closure.h"
#include "../../../src/frontend/analyzer/xa_typed_program.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/module/xmodule_resolver.h"
#include "../../../src/plan/semantic/xr_program_semantic_closure_internal.h"
#include "../../../src/plan/target/xr_scalar_call_decision.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef enum FixtureMutation {
    FIXTURE_EXACT = 0,
    FIXTURE_OPAQUE_CALLEE_SIGNATURE,
    FIXTURE_OPAQUE_CALL_CONTRACT,
    FIXTURE_CAPABILITY,
} FixtureMutation;

static XrFingerprint fingerprint(const char *text) {
    XrFingerprint result;
    xr_sha256((const uint8_t *) text, strlen(text), result.bytes);
    return result;
}

static XrProgramSemanticClosure *build_closure(FixtureMutation mutation) {
    static const char source[] = "fn add1(value: i64) -> i64 { return value + 1 }\n"
                                 "fn root() -> i64 { return add1(41) }\n";
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    REQUIRE(session != NULL);
    XrModuleResolverConfig resolver_config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&resolver_config);
    REQUIRE(resolver != NULL);
    XrModuleGraph *graph = xr_module_graph_new(session, resolver);
    REQUIRE(graph != NULL);
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "scalar-call-decision",
    };
    char *graph_error = NULL;
    REQUIRE(xr_module_graph_build_source(graph, &authority, source, &graph_error) == 0);
    xr_free(graph_error);
    REQUIRE(xr_module_graph_topological_sort(graph) == 0 && !graph->has_cycle &&
            graph->spec_count == 1 && graph->entry_index >= 0);
    XrModuleSpec *spec = &graph->specs[graph->entry_index];
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    REQUIRE(analyzer != NULL);
    xa_analyzer_set_graph(analyzer, graph);
    xa_analyzer_analyze(analyzer, "scalar-call-decision.xr", spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diagnostic = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count);
         diagnostic; diagnostic = diagnostic->next)
        REQUIRE(diagnostic->severity != XR_DIAG_SEV_ERROR);
    XrHashMap *exports = NULL;
    REQUIRE(xa_analyzer_collect_export_symbols_checked(analyzer, spec->ast, &exports));
    spec->status = XR_MODSPEC_ANALYZED;
    XaTypedProgramPublishResult publication =
        xa_typed_program_publish(analyzer, spec->ast, NULL, 1);
    REQUIRE(publication.reason == XA_TYPED_PROGRAM_REASON_NONE && publication.program != NULL);

    char error[512] = {0};
    XrProgramSemanticClosure *closure = NULL;
    REQUIRE(
        xa_typed_program_build_scalar_closure(publication.program, &closure, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_verify(closure, error, sizeof(error)));

    xa_typed_program_free(publication.program);
    xa_analyzer_free(analyzer);
    xr_module_graph_free(graph);
    xr_module_resolver_free(resolver);
    xr_compiler_session_delete(session);

    XrProgramSemanticFunctionRecord *callee = NULL;
    for (uint32_t i = 0; i < closure->function_count; i++)
        if (closure->functions[i].flags == 0)
            callee = &closure->functions[i];
    REQUIRE(callee != NULL && closure->call_count == 1);
    if (mutation == FIXTURE_OPAQUE_CALLEE_SIGNATURE)
        callee->signature_fingerprint = fingerprint("opaque-i64-looking-signature");
    else if (mutation == FIXTURE_OPAQUE_CALL_CONTRACT)
        closure->calls[0].contract_fingerprint = fingerprint("opaque-direct-call-contract");
    else if (mutation == FIXTURE_CAPABILITY)
        callee->capability_mask = 1;
    return closure;
}

static void require_rejected(const XrScalarCallDecision *decision,
                             const XrProgramSemanticClosure *closure,
                             const XrTargetProfile *profile) {
    char error[512] = {0};
    REQUIRE(!xr_scalar_call_decision_verify(decision, closure, profile, error, sizeof(error)));
    REQUIRE(strstr(error, "XR_TARGET_1003") != NULL);
}

static void test_exact_decision(void) {
    XrProgramSemanticClosure *closure = build_closure(FIXTURE_EXACT);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    XrScalarCallDecision decision;
    char error[512] = {0};
    REQUIRE(xr_scalar_call_decision_build(closure, generation, profile, &decision, error,
                                          sizeof(error)));
    REQUIRE(xr_scalar_call_decision_verify(&decision, closure, profile, error, sizeof(error)));
    REQUIRE(decision.native_abi == XR_TARGET_ABI_WIN64_X86_64);
    REQUIRE(decision.calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL);
    REQUIRE(decision.target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL);
    REQUIRE(decision.entry_policy == XR_SCALAR_CALL_ENTRY_STATIC_DIRECT);
    REQUIRE(decision.argument.machine_rep == XR_MACHINE_REP_I64);
    REQUIRE(decision.argument.mode == XR_TARGET_CALL_VALUE);
    REQUIRE(decision.argument.slot_policy == XR_SCALAR_CALL_SLOT_REGISTER_ONLY);
    REQUIRE(decision.result.machine_rep == XR_MACHINE_REP_I64);
    REQUIRE(decision.result.mode == XR_TARGET_CALL_VALUE);
    REQUIRE(decision.result.slot_policy == XR_SCALAR_CALL_SLOT_REGISTER_ONLY);
    REQUIRE(decision.entry_cell_count == 0 && decision.adapter_count == 0 &&
            decision.cleanup_count == 0 && decision.error_channel_count == 0 &&
            decision.suspend_point_count == 0 && decision.capability_mask == 0);

    XrScalarCallDecision mutated = decision;
    mutated.calling_convention = XR_TARGET_CALL_CONVENTION_INVALID;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.target_kind = XR_TARGET_CALL_TARGET_INVALID;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.entry_policy = XR_SCALAR_CALL_ENTRY_POLICY_INVALID;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.native_abi = XR_TARGET_ABI_WASM;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.argument.machine_rep = XR_MACHINE_REP_U64;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.argument.mode = XR_TARGET_CALL_REFERENCE;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.argument.ownership = XR_TARGET_CALL_READ;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.result.slot_policy = XR_SCALAR_CALL_SLOT_POLICY_INVALID;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.adapter_count = 1;
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.generation_id.bytes[0] ^= UINT8_C(0x80);
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.callee_function.bytes[0] ^= UINT8_C(0x40);
    require_rejected(&mutated, closure, profile);
    mutated = decision;
    mutated.fingerprint.bytes[0] ^= UINT8_C(0x20);
    require_rejected(&mutated, closure, profile);

    XrTargetProfile *other_profile =
        xr_test_target_profile_build(true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(other_profile != NULL);
    require_rejected(&decision, closure, other_profile);
    xr_target_profile_free(other_profile);

    XrGenerationClosureId stale = generation;
    stale.bytes[0] ^= UINT8_C(0x01);
    REQUIRE(
        !xr_scalar_call_decision_build(closure, stale, profile, &mutated, error, sizeof(error)));
    xr_target_profile_free(profile);
    xr_program_semantic_closure_free(closure);
}

static void test_opaque_psc_fingerprints_are_not_authority(void) {
    const FixtureMutation mutations[] = {
        FIXTURE_OPAQUE_CALLEE_SIGNATURE,
        FIXTURE_OPAQUE_CALL_CONTRACT,
        FIXTURE_CAPABILITY,
    };
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        XrProgramSemanticClosure *closure = build_closure(mutations[i]);
        XrScalarCallDecision decision;
        char error[512] = {0};
        REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
        memset(error, 0, sizeof(error));
        REQUIRE(!xr_scalar_call_decision_build(closure,
                                               xr_program_semantic_closure_generation_id(closure),
                                               profile, &decision, error, sizeof(error)));
        REQUIRE(strstr(error, "verified authorities") != NULL);
        xr_program_semantic_closure_free(closure);
    }
    xr_target_profile_free(profile);
}

int main(void) {
    test_exact_decision();
    test_opaque_psc_fingerprints_are_not_authority();
    printf("scalar call decision tests: PASS\n");
    return 0;
}
