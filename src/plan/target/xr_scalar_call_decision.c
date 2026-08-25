/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_scalar_call_decision.c - Sealed direct scalar call decision builder
 */

#include "xr_scalar_call_decision.h"
#include "../semantic/xr_scalar_call_semantics.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(XrScalarCallDecisionSlot) == 4,
               "scalar call slots must remain pointer-free values");

static bool fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1003: %s", detail);
    return false;
}

static bool id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static void put_u8(XrSHA256Context *context, uint8_t value) {
    xr_sha256_update(context, &value, sizeof(value));
}

static void put_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void put_u16(XrSHA256Context *context, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8u)};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void put_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void put_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    put_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void put_slot(XrSHA256Context *context, XrScalarCallDecisionSlot slot) {
    put_u8(context, slot.machine_rep);
    put_u8(context, slot.mode);
    put_u8(context, slot.ownership);
    put_u8(context, slot.slot_policy);
}

static XrFingerprint decision_fingerprint(const XrScalarCallDecision *decision) {
    static const char domain[] = "xray-scalar-call-decision-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    put_bytes(&context, (const uint8_t *) domain, strlen(domain));
    put_u32(&context, decision->schema);
    put_u16(&context, decision->native_abi);
    put_u8(&context, decision->sealed);
    put_u8(&context, decision->calling_convention);
    put_u8(&context, decision->target_kind);
    put_u8(&context, decision->entry_policy);
    put_u8(&context, decision->argument_count);
    put_u8(&context, decision->result_count);
    put_u8(&context, decision->entry_cell_count);
    put_u8(&context, decision->adapter_count);
    put_u8(&context, decision->cleanup_count);
    put_u8(&context, decision->error_channel_count);
    put_u8(&context, decision->suspend_point_count);
    put_bytes(&context, decision->reserved, sizeof(decision->reserved));
    put_u64(&context, decision->capability_mask);
    put_slot(&context, decision->argument);
    put_slot(&context, decision->result);
    put_bytes(&context, decision->generation_id.bytes,
              sizeof(decision->generation_id.bytes));
    put_bytes(&context, decision->closure_fingerprint.bytes,
              sizeof(decision->closure_fingerprint.bytes));
    put_bytes(&context, decision->target_profile_fingerprint.bytes,
              sizeof(decision->target_profile_fingerprint.bytes));
    put_bytes(&context, decision->call_identity.bytes,
              sizeof(decision->call_identity.bytes));
    put_bytes(&context, decision->callsite_identity.bytes,
              sizeof(decision->callsite_identity.bytes));
    put_bytes(&context, decision->caller_function.bytes,
              sizeof(decision->caller_function.bytes));
    put_bytes(&context, decision->callee_function.bytes,
              sizeof(decision->callee_function.bytes));
    xr_sha256_final(&context, result.bytes);
    return result;
}

static const XrProgramSemanticFunctionRecord *find_function(
    const XrProgramSemanticClosure *closure, XrStableId id) {
    const XrProgramSemanticFunctionRecord *found = NULL;
    for (uint32_t i = 0;
         i < xr_program_semantic_closure_function_count(closure); i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        if (!row || !id_equal(row->id, id))
            continue;
        if (found)
            return NULL;
        found = row;
    }
    return found;
}

static bool exact_function(const XrProgramSemanticFunctionRecord *row,
                           XrScalarI64FunctionShape shape, uint8_t flags) {
    XrScalarI64FunctionContract expected;
    return row && xr_scalar_i64_function_contract(shape, &expected) &&
           fingerprint_equal(row->signature_fingerprint,
                             expected.signature_fingerprint) &&
           fingerprint_equal(row->effect_fingerprint,
                             expected.effect_fingerprint) &&
           row->capability_mask == 0 && row->flags == flags;
}

static bool exact_source_call(const XrProgramSemanticClosure *closure,
                              const XrProgramSemanticCallRecord **out_call) {
    if (out_call)
        *out_call = NULL;
    if (!out_call || xr_program_semantic_closure_module_count(closure) != 1 ||
        xr_program_semantic_closure_dependency_count(closure) != 0 ||
        xr_program_semantic_closure_type_count(closure) != 0 ||
        xr_program_semantic_closure_function_count(closure) != 2 ||
        xr_program_semantic_closure_call_count(closure) != 1)
        return false;
    const XrProgramSemanticCallRecord *call =
        xr_program_semantic_closure_call(closure, 0);
    const XrProgramSemanticFunctionRecord *caller =
        call ? find_function(closure, call->caller_function) : NULL;
    const XrProgramSemanticFunctionRecord *callee =
        call ? find_function(closure, call->callee_function) : NULL;
    XrScalarI64FunctionContract callee_contract;
    XrFingerprint expected_call = {{0}};
    if (!call || !caller || !callee || caller == callee ||
        !exact_function(caller, XR_SCALAR_I64_FUNCTION_NULLARY,
                        XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) ||
        !exact_function(callee, XR_SCALAR_I64_FUNCTION_UNARY, 0) ||
        !id_equal(caller->module_identity, callee->module_identity) ||
        !xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY,
                                         &callee_contract) ||
        !xr_scalar_i64_call_contract(&callee_contract, &expected_call) ||
        !fingerprint_equal(call->contract_fingerprint, expected_call))
        return false;
    *out_call = call;
    return true;
}

bool xr_scalar_call_decision_build(const XrProgramSemanticClosure *closure,
                                   XrGenerationClosureId expected_generation,
                                   const XrTargetProfile *target_profile,
                                   XrScalarCallDecision *out, char *error,
                                   size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !closure || !target_profile ||
        !xr_program_semantic_closure_verify(closure, error, error_size) ||
        !xr_target_profile_verify(target_profile, error, error_size))
        return fail(error, error_size,
                    "scalar call decision requires verified authorities");
    XrGenerationClosureId generation =
        xr_program_semantic_closure_generation_id(closure);
    if (!xr_generation_closure_id_equal(generation, expected_generation))
        return fail(error, error_size, "generation closure identity is stale");
    const XrProgramSemanticCallRecord *call = NULL;
    if (!exact_source_call(closure, &call))
        return fail(error, error_size,
                    "program closure is not the sealed direct i64 call family");

    const XrTargetMachineFacts *machine =
        xr_target_profile_machine_facts(target_profile);
    if (!machine || machine->native_abi <= XR_TARGET_ABI_NONE ||
        machine->native_abi >= XR_TARGET_ABI_COUNT)
        return fail(error, error_size, "target native ABI is invalid");
    *out = (XrScalarCallDecision) {
        .schema = XR_SCALAR_CALL_DECISION_SCHEMA_VERSION,
        .native_abi = machine->native_abi,
        .sealed = 1,
        .calling_convention = XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL,
        .target_kind = XR_TARGET_CALL_TARGET_DIRECT_LOCAL,
        .entry_policy = XR_SCALAR_CALL_ENTRY_STATIC_DIRECT,
        .argument_count = 1,
        .result_count = 1,
        .argument = {XR_MACHINE_REP_I64, XR_TARGET_CALL_VALUE,
                     XR_TARGET_CALL_NONE, XR_SCALAR_CALL_SLOT_REGISTER_ONLY},
        .result = {XR_MACHINE_REP_I64, XR_TARGET_CALL_VALUE,
                   XR_TARGET_CALL_NONE, XR_SCALAR_CALL_SLOT_REGISTER_ONLY},
        .generation_id = generation,
        .closure_fingerprint =
            xr_program_semantic_closure_fingerprint(closure),
        .target_profile_fingerprint =
            xr_target_profile_fingerprint(target_profile),
        .call_identity = call->id,
        .callsite_identity = call->callsite_identity,
        .caller_function = call->caller_function,
        .callee_function = call->callee_function,
    };
    out->fingerprint = decision_fingerprint(out);
    if (!xr_scalar_call_decision_verify(out, closure, target_profile, error,
                                        error_size)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}
