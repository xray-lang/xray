/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_scalar_call_decision.c - Sealed scalar CallDecision tests
 */

#include "../../../src/plan/semantic/xr_program_semantic_closure.h"
#include "../../../src/plan/semantic/xr_scalar_call_semantics.h"
#include "../../../src/plan/target/xr_scalar_call_decision.h"
#include "../../../src/base/xsha256.h"
#include "target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                              \
        }                                                                         \
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

static XrStableId stable_id(const char *text) {
    XrStableId result;
    XrFingerprint full;
    REQUIRE(xr_stable_id_from_key(text, &result, &full));
    return result;
}

static XrProgramSemanticClosure *build_closure(FixtureMutation mutation) {
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1,
        .max_dependencies = 0,
        .max_types = 0,
        .max_functions = 2,
        .max_calls = 1,
    };
    char error[512] = {0};
    XrProgramSemanticClosure *closure = NULL;
    REQUIRE(xr_program_semantic_closure_create(
        &limits, fingerprint("scalar-call-policy"), &closure, error,
        sizeof(error)));
    XrStableId module = stable_id("module:scalar-call");
    XrProgramSemanticModuleInput module_input = {
        .module_identity = module,
        .source_fingerprint = fingerprint("scalar-call-source"),
        .export_fingerprint = fingerprint("scalar-call-empty-exports"),
    };
    REQUIRE(xr_program_semantic_closure_add_module(
        closure, &module_input, error, sizeof(error)));

    XrScalarI64FunctionContract nullary;
    XrScalarI64FunctionContract unary;
    REQUIRE(xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_NULLARY,
                                            &nullary));
    REQUIRE(xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY,
                                            &unary));
    XrProgramSemanticFunctionInput caller = {
        .module_identity = module,
        .declaration_identity = stable_id("declaration:scalar-call:entry"),
        .concrete_instance_identity = stable_id("instance:scalar-call:entry"),
        .signature_fingerprint = nullary.signature_fingerprint,
        .effect_fingerprint = nullary.effect_fingerprint,
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
    };
    XrProgramSemanticFunctionInput callee = {
        .module_identity = module,
        .declaration_identity = stable_id("declaration:scalar-call:callee"),
        .concrete_instance_identity = stable_id("instance:scalar-call:callee"),
        .signature_fingerprint = unary.signature_fingerprint,
        .effect_fingerprint = unary.effect_fingerprint,
        .capability_mask = mutation == FIXTURE_CAPABILITY ? 1 : 0,
    };
    if (mutation == FIXTURE_OPAQUE_CALLEE_SIGNATURE)
        callee.signature_fingerprint = fingerprint("opaque-i64-looking-signature");
    XrStableId caller_id;
    XrStableId callee_id;
    REQUIRE(xr_program_semantic_closure_add_function(
        closure, &caller, &caller_id, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_add_function(
        closure, &callee, &callee_id, error, sizeof(error)));

    XrFingerprint call_contract;
    REQUIRE(xr_scalar_i64_call_contract(&unary, &call_contract));
    if (mutation == FIXTURE_OPAQUE_CALL_CONTRACT)
        call_contract = fingerprint("opaque-direct-call-contract");
    XrProgramSemanticCallInput call = {
        .callsite_identity = stable_id("callsite:scalar-call:entry:callee"),
        .caller_function = caller_id,
        .callee_function = callee_id,
        .contract_fingerprint = call_contract,
    };
    XrStableId call_id;
    REQUIRE(xr_program_semantic_closure_add_call(
        closure, &call, &call_id, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_freeze(closure, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    return closure;
}

static void require_rejected(const XrScalarCallDecision *decision,
                             const XrProgramSemanticClosure *closure,
                             const XrTargetProfile *profile) {
    char error[512] = {0};
    REQUIRE(!xr_scalar_call_decision_verify(decision, closure, profile, error,
                                            sizeof(error)));
    REQUIRE(strstr(error, "XR_TARGET_1003") != NULL);
}

static void test_exact_decision(void) {
    XrProgramSemanticClosure *closure = build_closure(FIXTURE_EXACT);
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrGenerationClosureId generation =
        xr_program_semantic_closure_generation_id(closure);
    XrScalarCallDecision decision;
    char error[512] = {0};
    REQUIRE(xr_scalar_call_decision_build(closure, generation, profile, &decision,
                                          error, sizeof(error)));
    REQUIRE(xr_scalar_call_decision_verify(&decision, closure, profile, error,
                                           sizeof(error)));
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

    XrTargetProfile *other_profile = xr_test_target_profile_build(
        true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(other_profile != NULL);
    require_rejected(&decision, closure, other_profile);
    xr_target_profile_free(other_profile);

    XrGenerationClosureId stale = generation;
    stale.bytes[0] ^= UINT8_C(0x01);
    REQUIRE(!xr_scalar_call_decision_build(closure, stale, profile, &mutated,
                                           error, sizeof(error)));
    xr_target_profile_free(profile);
    xr_program_semantic_closure_free(closure);
}

static void test_opaque_psc_fingerprints_are_not_authority(void) {
    const FixtureMutation mutations[] = {
        FIXTURE_OPAQUE_CALLEE_SIGNATURE,
        FIXTURE_OPAQUE_CALL_CONTRACT,
        FIXTURE_CAPABILITY,
    };
    XrTargetProfile *profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        XrProgramSemanticClosure *closure = build_closure(mutations[i]);
        XrScalarCallDecision decision;
        char error[512] = {0};
        REQUIRE(!xr_scalar_call_decision_build(
            closure, xr_program_semantic_closure_generation_id(closure), profile,
            &decision, error, sizeof(error)));
        REQUIRE(strstr(error, "sealed direct i64 call family") != NULL);
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
