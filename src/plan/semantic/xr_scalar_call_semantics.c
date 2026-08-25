/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_scalar_call_semantics.c - Canonical sealed scalar call semantics
 */

#include "xr_scalar_call_semantics.h"
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
    hash_u32(context, XR_SCALAR_CALL_SEMANTICS_SCHEMA_VERSION);
}

static XrFingerprint hash_finish(XrSHA256Context *context) {
    XrFingerprint result;
    xr_sha256_final(context, result.bytes);
    return result;
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

bool xr_scalar_i64_function_contract(XrScalarI64FunctionShape shape,
                                     XrScalarI64FunctionContract *out) {
    if (!out || (shape != XR_SCALAR_I64_FUNCTION_NULLARY &&
                 shape != XR_SCALAR_I64_FUNCTION_UNARY))
        return false;
    memset(out, 0, sizeof(*out));
    out->schema = XR_SCALAR_CALL_SEMANTICS_SCHEMA_VERSION;
    out->parameter_count = (uint8_t) shape;

    XrSHA256Context context;
    hash_begin(&context, "xray-language-scalar-function-signature-v1");
    hash_u32(&context, out->parameter_count);
    hash_u32(&context, out->parameter_count);
    hash_u8(&context, 0); /* complete no-throw contract */
    if (shape == XR_SCALAR_I64_FUNCTION_UNARY) {
        hash_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64);
        hash_u8(&context, (uint8_t) XR_PARAM_READ);
    }
    hash_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64);
    out->signature_fingerprint = hash_finish(&context);

    hash_begin(&context, "xray-language-scalar-effect-contract-v1");
    hash_u32(&context, 0); /* complete function effects */
    hash_u32(&context, 0); /* semantic effects */
    hash_u32(&context, 0); /* unknown semantic effects */
    hash_u32(&context, 0); /* complete error set */
    hash_u32(&context, 0); /* escaping errors */
    hash_u32(&context, 0); /* complete memory effects */
    hash_u32(&context, 0); /* memory roots */
    hash_u32(&context, 0); /* proven no allocation */
    hash_u32(&context, 0); /* allocation reasons */
    out->effect_fingerprint = hash_finish(&context);
    return true;
}

bool xr_scalar_i64_call_contract(const XrScalarI64FunctionContract *callee,
                                 XrFingerprint *out) {
    if (!out || !xr_scalar_i64_function_contract_is_exact(
                    callee, XR_SCALAR_I64_FUNCTION_UNARY))
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-source-scalar-call-contract-v1");
    hash_bytes(&context, callee->signature_fingerprint.bytes,
               sizeof(callee->signature_fingerprint.bytes));
    hash_bytes(&context, callee->effect_fingerprint.bytes,
               sizeof(callee->effect_fingerprint.bytes));
    hash_u64(&context, callee->capability_mask);
    *out = hash_finish(&context);
    return true;
}

bool xr_scalar_i64_function_contract_is_exact(
    const XrScalarI64FunctionContract *contract, XrScalarI64FunctionShape shape) {
    XrScalarI64FunctionContract expected;
    return contract && xr_scalar_i64_function_contract(shape, &expected) &&
           contract->schema == expected.schema &&
           contract->parameter_count == expected.parameter_count &&
           memcmp(contract->reserved, expected.reserved,
                  sizeof(contract->reserved)) == 0 &&
           contract->capability_mask == 0 &&
           fingerprint_equal(contract->signature_fingerprint,
                             expected.signature_fingerprint) &&
           fingerprint_equal(contract->effect_fingerprint,
                             expected.effect_fingerprint);
}
