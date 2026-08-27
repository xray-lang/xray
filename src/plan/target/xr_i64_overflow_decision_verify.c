/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Independent verifier for sealed i64 overflow predicate decisions.
 */

#include "xr_i64_overflow_decision.h"
#include "../../base/xsha256.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../shared/xr_param_mode.h"
#include <stdio.h>
#include <string.h>

static bool reject(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1003: %s", detail);
    return false;
}

static bool same(const uint8_t *left, const uint8_t *right, size_t size) {
    return memcmp(left, right, size) == 0;
}

static bool zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static void u8(XrSHA256Context *context, uint8_t value) {
    xr_sha256_update(context, &value, sizeof(value));
}

static void u16(XrSHA256Context *context, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8u)};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void bytes(XrSHA256Context *context, const uint8_t *value, size_t size) {
    u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, value, size);
}

static XrStableId independent_builtin(uint32_t symbol) {
    static const uint8_t domain[] = "xray-language-i64-overflow-builtin-v1";
    XrSHA256Context context;
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId result;
    xr_sha256_init(&context);
    bytes(&context, domain, sizeof(domain) - 1u);
    u32(&context, XR_I64_OVERFLOW_PREDICATE_SEMANTICS_SCHEMA_VERSION);
    u32(&context, symbol);
    xr_sha256_final(&context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
}

static XrFingerprint independent_contract(uint32_t symbol) {
    static const uint8_t domain[] = "xray-language-i64-overflow-call-contract-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    bytes(&context, domain, sizeof(domain) - 1u);
    u32(&context, XR_I64_OVERFLOW_PREDICATE_SEMANTICS_SCHEMA_VERSION);
    u32(&context, symbol);
    u32(&context, XR_EXACT_SCALAR_I64);
    u32(&context, XR_EXACT_SCALAR_I64);
    u32(&context, 1u);
    u32(&context, XR_PARAM_READ);
    u32(&context, 0u);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static void hash_row(XrSHA256Context *context, const XrI64OverflowDecisionRow *row) {
    bytes(context, row->program_call.bytes, sizeof(row->program_call.bytes));
    bytes(context, row->callsite.bytes, sizeof(row->callsite.bytes));
    bytes(context, row->caller_function.bytes, sizeof(row->caller_function.bytes));
    bytes(context, row->builtin_identity.bytes, sizeof(row->builtin_identity.bytes));
    bytes(context, row->contract.bytes, sizeof(row->contract.bytes));
    u32(context, row->program_row);
    u32(context, row->method_symbol);
    u8(context, row->kind);
    u8(context, row->receiver_rep);
    u8(context, row->argument_rep);
    u8(context, row->result_rep);
    bytes(context, row->reserved, sizeof(row->reserved));
}

static XrFingerprint independent_row_fingerprint(const XrI64OverflowDecisionRow *row) {
    static const uint8_t domain[] = "xray-i64-overflow-decision-row-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    bytes(&context, domain, sizeof(domain) - 1u);
    hash_row(&context, row);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrFingerprint independent_table_fingerprint(const XrI64OverflowDecisionTable *table) {
    static const uint8_t domain[] = "xray-i64-overflow-decision-table-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    bytes(&context, domain, sizeof(domain) - 1u);
    u32(&context, table->schema);
    u32(&context, table->row_count);
    u16(&context, table->native_abi);
    u8(&context, table->sealed);
    bytes(&context, table->reserved, sizeof(table->reserved));
    bytes(&context, table->generation_id.bytes, sizeof(table->generation_id.bytes));
    bytes(&context, table->closure_fingerprint.bytes,
          sizeof(table->closure_fingerprint.bytes));
    bytes(&context, table->target_profile_fingerprint.bytes,
          sizeof(table->target_profile_fingerprint.bytes));
    for (uint32_t i = 0; i < table->row_count; i++)
        bytes(&context, table->rows[i].fingerprint.bytes,
              sizeof(table->rows[i].fingerprint.bytes));
    xr_sha256_final(&context, result.bytes);
    return result;
}

bool xr_i64_overflow_decision_verify(const XrI64OverflowDecisionTable *table,
                                     const XrProgramSemanticClosure *closure,
                                     const XrTargetProfile *target_profile, char *error,
                                     size_t error_size) {
    if (!table || !closure || !target_profile ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        !xr_program_semantic_closure_verify(closure, NULL, 0) ||
        !xr_target_profile_verify(target_profile, NULL, 0))
        return reject(error, error_size, "overflow decision authority is incomplete");
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(target_profile);
    uint32_t count = (uint32_t) xr_program_semantic_closure_call_count(closure);
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    XrFingerprint closure_fingerprint = xr_program_semantic_closure_fingerprint(closure);
    XrFingerprint profile_fingerprint = xr_target_profile_fingerprint(target_profile);
    if (!machine || table->schema != XR_I64_OVERFLOW_DECISION_SCHEMA_VERSION ||
        table->row_count != count || count == 0 || !table->rows ||
        table->native_abi != machine->native_abi || table->sealed != 1 ||
        !zero(table->reserved, sizeof(table->reserved)) ||
        !same(table->generation_id.bytes, generation.bytes, sizeof(generation.bytes)) ||
        !same(table->closure_fingerprint.bytes, closure_fingerprint.bytes,
              sizeof(closure_fingerprint.bytes)) ||
        !same(table->target_profile_fingerprint.bytes, profile_fingerprint.bytes,
              sizeof(profile_fingerprint.bytes)))
        return reject(error, error_size, "overflow decision table shape is invalid");
    for (uint32_t i = 0; i < count; i++) {
        const XrI64OverflowDecisionRow *row = &table->rows[i];
        const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(closure, i);
        uint32_t expected_symbol = row->kind == XR_I64_OVERFLOW_PREDICATE_ADD
                                       ? XR_I64_OVERFLOW_METHOD_SYMBOL_ADD
                                   : row->kind == XR_I64_OVERFLOW_PREDICATE_SUB
                                       ? XR_I64_OVERFLOW_METHOD_SYMBOL_SUB
                                   : row->kind == XR_I64_OVERFLOW_PREDICATE_MUL
                                       ? XR_I64_OVERFLOW_METHOD_SYMBOL_MUL
                                       : 0;
        XrStableId builtin = independent_builtin(expected_symbol);
        XrFingerprint contract = independent_contract(expected_symbol);
        XrFingerprint fingerprint = independent_row_fingerprint(row);
        if (!call || expected_symbol == 0 || row->program_row != i ||
            row->method_symbol != expected_symbol || row->receiver_rep != XR_MACHINE_REP_I64 ||
            row->argument_rep != XR_MACHINE_REP_I64 || row->result_rep != XR_MACHINE_REP_I1 ||
            !zero(row->reserved, sizeof(row->reserved)) ||
            !same(row->program_call.bytes, call->id.bytes, sizeof(call->id.bytes)) ||
            !same(row->callsite.bytes, call->callsite_identity.bytes,
                  sizeof(call->callsite_identity.bytes)) ||
            !same(row->caller_function.bytes, call->caller_function.bytes,
                  sizeof(call->caller_function.bytes)) ||
            !same(row->builtin_identity.bytes, builtin.bytes, sizeof(builtin.bytes)) ||
            !same(row->builtin_identity.bytes, call->callee_function.bytes,
                  sizeof(call->callee_function.bytes)) ||
            !same(row->contract.bytes, contract.bytes, sizeof(contract.bytes)) ||
            !same(row->contract.bytes, call->contract_fingerprint.bytes,
                  sizeof(call->contract_fingerprint.bytes)) ||
            !same(row->fingerprint.bytes, fingerprint.bytes, sizeof(fingerprint.bytes)))
            return reject(error, error_size, "overflow decision row is not canonical");
    }
    XrFingerprint fingerprint = independent_table_fingerprint(table);
    return same(table->fingerprint.bytes, fingerprint.bytes, sizeof(fingerprint.bytes)) ||
           reject(error, error_size, "overflow decision table fingerprint is invalid");
}
