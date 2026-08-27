/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xr_i64_overflow_decision.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1003: %s", detail);
    return false;
}

static void put_u8(XrSHA256Context *context, uint8_t value) {
    xr_sha256_update(context, &value, sizeof(value));
}

static void put_u16(XrSHA256Context *context, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8u)};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void put_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
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

static void hash_row(XrSHA256Context *context, const XrI64OverflowDecisionRow *row) {
    put_bytes(context, row->program_call.bytes, sizeof(row->program_call.bytes));
    put_bytes(context, row->callsite.bytes, sizeof(row->callsite.bytes));
    put_bytes(context, row->caller_function.bytes, sizeof(row->caller_function.bytes));
    put_bytes(context, row->builtin_identity.bytes, sizeof(row->builtin_identity.bytes));
    put_bytes(context, row->contract.bytes, sizeof(row->contract.bytes));
    put_u32(context, row->program_row);
    put_u32(context, row->method_symbol);
    put_u8(context, row->kind);
    put_u8(context, row->receiver_rep);
    put_u8(context, row->argument_rep);
    put_u8(context, row->result_rep);
    put_bytes(context, row->reserved, sizeof(row->reserved));
}

static XrFingerprint row_fingerprint(const XrI64OverflowDecisionRow *row) {
    static const uint8_t domain[] = "xray-i64-overflow-decision-row-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    put_bytes(&context, domain, sizeof(domain) - 1u);
    hash_row(&context, row);
    xr_sha256_final(&context, result.bytes);
    return result;
}

static XrFingerprint table_fingerprint(const XrI64OverflowDecisionTable *table) {
    static const uint8_t domain[] = "xray-i64-overflow-decision-table-v1";
    XrSHA256Context context;
    XrFingerprint result;
    xr_sha256_init(&context);
    put_bytes(&context, domain, sizeof(domain) - 1u);
    put_u32(&context, table->schema);
    put_u32(&context, table->row_count);
    put_u16(&context, table->native_abi);
    put_u8(&context, table->sealed);
    put_bytes(&context, table->reserved, sizeof(table->reserved));
    put_bytes(&context, table->generation_id.bytes, sizeof(table->generation_id.bytes));
    put_bytes(&context, table->closure_fingerprint.bytes,
              sizeof(table->closure_fingerprint.bytes));
    put_bytes(&context, table->target_profile_fingerprint.bytes,
              sizeof(table->target_profile_fingerprint.bytes));
    for (uint32_t i = 0; i < table->row_count; i++)
        put_bytes(&context, table->rows[i].fingerprint.bytes,
                  sizeof(table->rows[i].fingerprint.bytes));
    xr_sha256_final(&context, result.bytes);
    return result;
}

bool xr_i64_overflow_decision_build(const XrProgramSemanticClosure *closure,
                                    XrGenerationClosureId expected_generation,
                                    const XrTargetProfile *target_profile,
                                    XrI64OverflowDecisionTable *out, char *error,
                                    size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !closure || !target_profile ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        !xr_program_semantic_closure_verify(closure, error, error_size) ||
        !xr_target_profile_verify(target_profile, error, error_size))
        return fail(error, error_size, "overflow decisions require verified authorities");
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(closure);
    if (!xr_generation_closure_id_equal(generation, expected_generation))
        return fail(error, error_size, "overflow decision generation is stale");
    uint32_t count = (uint32_t) xr_program_semantic_closure_call_count(closure);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(target_profile);
    XrI64OverflowDecisionRow *rows = count ? (XrI64OverflowDecisionRow *) xr_calloc(
                                                count, sizeof(*rows))
                                          : NULL;
    if (!machine || machine->native_abi <= XR_TARGET_ABI_NONE ||
        machine->native_abi >= XR_TARGET_ABI_COUNT || count == 0 || !rows)
        return fail(error, error_size, "overflow decision target shape is invalid");
    *out = (XrI64OverflowDecisionTable) {
        .schema = XR_I64_OVERFLOW_DECISION_SCHEMA_VERSION,
        .row_count = count,
        .native_abi = machine->native_abi,
        .sealed = 1,
        .generation_id = generation,
        .closure_fingerprint = xr_program_semantic_closure_fingerprint(closure),
        .target_profile_fingerprint = xr_target_profile_fingerprint(target_profile),
        .rows = rows,
    };
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(closure, i);
        XrI64OverflowPredicateKind kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
        uint32_t symbol = 0;
        if (!call ||
            !xr_i64_overflow_predicate_kind_from_builtin_identity(call->callee_function, &kind) ||
            !xr_i64_overflow_predicate_method_symbol(kind, &symbol)) {
            xr_i64_overflow_decision_dispose(out);
            return fail(error, error_size, "overflow PSC call has no exact builtin identity");
        }
        rows[i] = (XrI64OverflowDecisionRow) {
            .program_call = call->id,
            .callsite = call->callsite_identity,
            .caller_function = call->caller_function,
            .builtin_identity = call->callee_function,
            .contract = call->contract_fingerprint,
            .program_row = i,
            .method_symbol = symbol,
            .kind = (uint8_t) kind,
            .receiver_rep = XR_MACHINE_REP_I64,
            .argument_rep = XR_MACHINE_REP_I64,
            .result_rep = XR_MACHINE_REP_I1,
        };
        rows[i].fingerprint = row_fingerprint(&rows[i]);
    }
    out->fingerprint = table_fingerprint(out);
    if (!xr_i64_overflow_decision_verify(out, closure, target_profile, error, error_size)) {
        xr_i64_overflow_decision_dispose(out);
        return false;
    }
    return true;
}

const XrI64OverflowDecisionRow *xr_i64_overflow_decision_for_program_row(
    const XrI64OverflowDecisionTable *table, uint32_t program_row) {
    return table && table->sealed == 1 && program_row < table->row_count && table->rows &&
                   table->rows[program_row].program_row == program_row
               ? &table->rows[program_row]
               : NULL;
}

void xr_i64_overflow_decision_dispose(XrI64OverflowDecisionTable *table) {
    if (!table)
        return;
    xr_free(table->rows);
    memset(table, 0, sizeof(*table));
}
