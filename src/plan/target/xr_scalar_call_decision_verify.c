/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_scalar_call_decision_verify.c - Independent scalar call decision verifier
 */

#include "xr_scalar_call_decision.h"
#include "../semantic/xr_scalar_call_semantics.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static bool reject(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1003: %s", detail);
    return false;
}

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t size) {
    return memcmp(left, right, size) == 0;
}

static bool same_id(XrStableId left, XrStableId right) {
    return same_bytes(left.bytes, right.bytes, sizeof(left.bytes));
}

static bool same_fingerprint(XrFingerprint left, XrFingerprint right) {
    return same_bytes(left.bytes, right.bytes, sizeof(left.bytes));
}

static bool all_zero(const uint8_t *bytes, size_t size) {
    uint8_t value = 0;
    for (size_t i = 0; i < size; i++)
        value |= bytes[i];
    return value == 0;
}

static void verifier_u8(XrSHA256Context *context, uint8_t value) {
    xr_sha256_update(context, &value, sizeof(value));
}

static void verifier_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t) value, (uint8_t) (value >> 8u), (uint8_t) (value >> 16u),
                        (uint8_t) (value >> 24u)};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_u16(XrSHA256Context *context, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8u)};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    verifier_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void verifier_slot(XrSHA256Context *context, XrScalarCallDecisionSlot slot) {
    verifier_u8(context, slot.machine_rep);
    verifier_u8(context, slot.mode);
    verifier_u8(context, slot.ownership);
    verifier_u8(context, slot.slot_policy);
}

static XrFingerprint verifier_fingerprint(const XrScalarCallDecision *decision) {
    static const uint8_t domain[] = "xray-scalar-call-decision-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    verifier_bytes(&context, domain, sizeof(domain) - 1);
    verifier_u32(&context, decision->schema);
    verifier_u16(&context, decision->native_abi);
    verifier_u8(&context, decision->sealed);
    verifier_u8(&context, decision->calling_convention);
    verifier_u8(&context, decision->target_kind);
    verifier_u8(&context, decision->entry_policy);
    verifier_u8(&context, decision->argument_count);
    verifier_u8(&context, decision->result_count);
    verifier_u8(&context, decision->entry_cell_count);
    verifier_u8(&context, decision->adapter_count);
    verifier_u8(&context, decision->cleanup_count);
    verifier_u8(&context, decision->error_channel_count);
    verifier_u8(&context, decision->suspend_point_count);
    verifier_bytes(&context, decision->reserved, sizeof(decision->reserved));
    verifier_u64(&context, decision->capability_mask);
    verifier_slot(&context, decision->argument);
    verifier_slot(&context, decision->result);
    verifier_bytes(&context, decision->generation_id.bytes, sizeof(decision->generation_id.bytes));
    verifier_bytes(&context, decision->closure_fingerprint.bytes,
                   sizeof(decision->closure_fingerprint.bytes));
    verifier_bytes(&context, decision->target_profile_fingerprint.bytes,
                   sizeof(decision->target_profile_fingerprint.bytes));
    verifier_bytes(&context, decision->call_identity.bytes, sizeof(decision->call_identity.bytes));
    verifier_bytes(&context, decision->callsite_identity.bytes,
                   sizeof(decision->callsite_identity.bytes));
    verifier_bytes(&context, decision->caller_function.bytes,
                   sizeof(decision->caller_function.bytes));
    verifier_bytes(&context, decision->callee_function.bytes,
                   sizeof(decision->callee_function.bytes));
    xr_sha256_final(&context, result.bytes);
    return result;
}

static const XrProgramSemanticFunctionRecord *
verifier_find_function(const XrProgramSemanticClosure *closure, XrStableId identity) {
    const XrProgramSemanticFunctionRecord *answer = NULL;
    size_t count = xr_program_semantic_closure_function_count(closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticFunctionRecord *candidate =
            xr_program_semantic_closure_function(closure, i);
        if (!candidate || !same_id(candidate->id, identity))
            continue;
        if (answer)
            return NULL;
        answer = candidate;
    }
    return answer;
}

static bool verifier_function_exact(const XrProgramSemanticFunctionRecord *function,
                                    XrScalarI64FunctionShape shape, uint8_t required_flags) {
    XrScalarI64FunctionContract semantic;
    return function && xr_scalar_i64_function_contract(shape, &semantic) &&
           same_fingerprint(function->signature_fingerprint, semantic.signature_fingerprint) &&
           same_fingerprint(function->effect_fingerprint, semantic.effect_fingerprint) &&
           function->capability_mask == 0 && function->flags == required_flags;
}

bool xr_scalar_call_decision_verify(const XrScalarCallDecision *decision,
                                    const XrProgramSemanticClosure *closure,
                                    const XrTargetProfile *target_profile, char *error,
                                    size_t error_size) {
    if (!decision || !closure || !target_profile ||
        !xr_program_semantic_closure_verify(closure, NULL, 0) ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL ||
        !xr_target_profile_verify(target_profile, NULL, 0))
        return reject(error, error_size, "scalar call verification requires complete authorities");
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(target_profile);
    if (!machine || decision->schema != XR_SCALAR_CALL_DECISION_SCHEMA_VERSION ||
        decision->native_abi != machine->native_abi || decision->native_abi <= XR_TARGET_ABI_NONE ||
        decision->native_abi >= XR_TARGET_ABI_COUNT || decision->sealed != 1 ||
        decision->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        decision->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        decision->entry_policy != XR_SCALAR_CALL_ENTRY_STATIC_DIRECT ||
        decision->argument_count != 1 || decision->result_count != 1 ||
        decision->entry_cell_count != 0 || decision->adapter_count != 0 ||
        decision->cleanup_count != 0 || decision->error_channel_count != 0 ||
        decision->suspend_point_count != 0 ||
        !all_zero(decision->reserved, sizeof(decision->reserved)) ||
        decision->capability_mask != 0 || decision->argument.machine_rep != XR_MACHINE_REP_I64 ||
        decision->argument.mode != XR_TARGET_CALL_VALUE ||
        decision->argument.ownership != XR_TARGET_CALL_NONE ||
        decision->argument.slot_policy != XR_SCALAR_CALL_SLOT_REGISTER_ONLY ||
        decision->result.machine_rep != XR_MACHINE_REP_I64 ||
        decision->result.mode != XR_TARGET_CALL_VALUE ||
        decision->result.ownership != XR_TARGET_CALL_NONE ||
        decision->result.slot_policy != XR_SCALAR_CALL_SLOT_REGISTER_ONLY)
        return reject(error, error_size, "scalar call decision shape is invalid");

    if (xr_program_semantic_closure_module_count(closure) != 1 ||
        xr_program_semantic_closure_dependency_count(closure) != 0 ||
        xr_program_semantic_closure_type_count(closure) != 0 ||
        xr_program_semantic_closure_function_count(closure) != 2 ||
        xr_program_semantic_closure_call_count(closure) != 1)
        return reject(error, error_size, "program closure is outside the bounded scalar family");

    XrGenerationClosureId actual_generation = xr_program_semantic_closure_generation_id(closure);
    XrFingerprint closure_fingerprint = xr_program_semantic_closure_fingerprint(closure);
    XrFingerprint target_fingerprint = xr_target_profile_fingerprint(target_profile);
    if (!same_bytes(decision->generation_id.bytes, actual_generation.bytes,
                    sizeof(actual_generation.bytes)) ||
        !same_fingerprint(decision->closure_fingerprint, closure_fingerprint) ||
        !same_fingerprint(decision->target_profile_fingerprint, target_fingerprint))
        return reject(error, error_size, "decision authority fingerprints are stale");

    const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(closure, 0);
    const XrProgramSemanticFunctionRecord *caller =
        call ? verifier_find_function(closure, call->caller_function) : NULL;
    const XrProgramSemanticFunctionRecord *callee =
        call ? verifier_find_function(closure, call->callee_function) : NULL;
    XrScalarI64FunctionContract callee_semantic;
    XrFingerprint expected_contract = {{0}};
    if (!call || !caller || !callee || caller == callee ||
        !verifier_function_exact(caller, XR_SCALAR_I64_FUNCTION_NULLARY,
                                 XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) ||
        !verifier_function_exact(callee, XR_SCALAR_I64_FUNCTION_UNARY, 0) ||
        !same_id(caller->module_identity, callee->module_identity) ||
        !xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY, &callee_semantic) ||
        !xr_scalar_i64_call_contract(&callee_semantic, &expected_contract) ||
        !same_fingerprint(call->contract_fingerprint, expected_contract) ||
        !same_id(decision->call_identity, call->id) ||
        !same_id(decision->callsite_identity, call->callsite_identity) ||
        !same_id(decision->caller_function, call->caller_function) ||
        !same_id(decision->callee_function, call->callee_function))
        return reject(error, error_size, "decision does not bind the exact scalar source call");

    XrFingerprint expected_fingerprint = verifier_fingerprint(decision);
    if (!same_fingerprint(decision->fingerprint, expected_fingerprint))
        return reject(error, error_size, "scalar call decision fingerprint is invalid");
    return true;
}
