/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xr_i64_overflow_predicate_semantics.h"
#include "../../base/xsha256.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../shared/xr_param_mode.h"
#include <string.h>

static void hash_u8(XrSHA256Context *context, uint8_t value) {
    xr_sha256_update(context, &value, sizeof(value));
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    hash_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void hash_begin(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    hash_bytes(context, (const uint8_t *) domain, strlen(domain));
    hash_u32(context, XR_I64_OVERFLOW_PREDICATE_SEMANTICS_SCHEMA_VERSION);
}

static XrFingerprint finish_fingerprint(XrSHA256Context *context) {
    XrFingerprint result;
    xr_sha256_final(context, result.bytes);
    return result;
}

static XrStableId finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId result;
    xr_sha256_final(context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
}

bool xr_i64_overflow_predicate_method_symbol(XrI64OverflowPredicateKind kind,
                                             uint32_t *symbol) {
    if (symbol)
        *symbol = 0;
    if (!symbol)
        return false;
    switch (kind) {
        case XR_I64_OVERFLOW_PREDICATE_ADD:
            *symbol = XR_I64_OVERFLOW_METHOD_SYMBOL_ADD;
            return true;
        case XR_I64_OVERFLOW_PREDICATE_SUB:
            *symbol = XR_I64_OVERFLOW_METHOD_SYMBOL_SUB;
            return true;
        case XR_I64_OVERFLOW_PREDICATE_MUL:
            *symbol = XR_I64_OVERFLOW_METHOD_SYMBOL_MUL;
            return true;
        default: return false;
    }
}

bool xr_i64_overflow_predicate_kind_from_method_symbol(
    uint32_t symbol, XrI64OverflowPredicateKind *kind) {
    if (kind)
        *kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
    if (!kind)
        return false;
    switch (symbol) {
        case XR_I64_OVERFLOW_METHOD_SYMBOL_ADD:
            *kind = XR_I64_OVERFLOW_PREDICATE_ADD;
            return true;
        case XR_I64_OVERFLOW_METHOD_SYMBOL_SUB:
            *kind = XR_I64_OVERFLOW_PREDICATE_SUB;
            return true;
        case XR_I64_OVERFLOW_METHOD_SYMBOL_MUL:
            *kind = XR_I64_OVERFLOW_PREDICATE_MUL;
            return true;
        default: return false;
    }
}

bool xr_i64_overflow_predicate_builtin_identity(XrI64OverflowPredicateKind kind,
                                                XrStableId *identity) {
    if (identity)
        memset(identity, 0, sizeof(*identity));
    uint32_t symbol = 0;
    if (!identity || !xr_i64_overflow_predicate_method_symbol(kind, &symbol))
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-language-i64-overflow-builtin-v1");
    hash_u32(&context, symbol);
    *identity = finish_id(&context);
    return true;
}

bool xr_i64_overflow_predicate_kind_from_builtin_identity(
    XrStableId identity, XrI64OverflowPredicateKind *kind) {
    if (kind)
        *kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
    if (!kind)
        return false;
    for (uint32_t raw = XR_I64_OVERFLOW_PREDICATE_ADD;
         raw < XR_I64_OVERFLOW_PREDICATE_COUNT; raw++) {
        XrStableId expected;
        if (xr_i64_overflow_predicate_builtin_identity((XrI64OverflowPredicateKind) raw,
                                                       &expected) &&
            memcmp(identity.bytes, expected.bytes, sizeof(identity.bytes)) == 0) {
            *kind = (XrI64OverflowPredicateKind) raw;
            return true;
        }
    }
    return false;
}

bool xr_i64_overflow_predicate_policy(XrFingerprint *fingerprint) {
    if (!fingerprint)
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-i64-overflow-program-policy-v1");
    hash_u32(&context, XR_I64_OVERFLOW_PREDICATE_COUNT - 1u);
    *fingerprint = finish_fingerprint(&context);
    return true;
}

bool xr_i64_overflow_predicate_entry_signature(XrFingerprint *fingerprint) {
    if (!fingerprint)
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-language-i64-overflow-entry-signature-v1");
    hash_u32(&context, 2); /* exact arity and min-arity */
    hash_u32(&context, 2);
    hash_u8(&context, 0); /* complete no-throw contract */
    for (uint32_t i = 0; i < 2; i++) {
        hash_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64);
        hash_u8(&context, (uint8_t) XR_PARAM_READ);
    }
    hash_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64);
    *fingerprint = finish_fingerprint(&context);
    return true;
}

bool xr_i64_overflow_predicate_entry_effect(XrFingerprint *fingerprint) {
    if (!fingerprint)
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-language-i64-overflow-entry-effect-v1");
    for (uint32_t i = 0; i < 9; i++)
        hash_u32(&context, 0);
    *fingerprint = finish_fingerprint(&context);
    return true;
}

bool xr_i64_overflow_predicate_call_contract(XrI64OverflowPredicateKind kind,
                                             XrFingerprint *fingerprint) {
    if (!fingerprint)
        return false;
    uint32_t symbol = 0;
    if (!xr_i64_overflow_predicate_method_symbol(kind, &symbol))
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-language-i64-overflow-call-contract-v1");
    hash_u32(&context, symbol);
    hash_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64); /* receiver */
    hash_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64); /* argument */
    hash_u32(&context, 1u); /* bool result */
    hash_u32(&context, XR_PARAM_READ);
    hash_u32(&context, 0u); /* semantic effects */
    *fingerprint = finish_fingerprint(&context);
    return true;
}
