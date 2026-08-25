/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_scalar_program_authority_verify.c - Independent scalar snapshot verifier
 */

#include "xa_scalar_program_authority_internal.h"
#include "xa_alloc_effect.h"
#include "xa_effect_db.h"
#include "../parser/xast_types.h"
#include "../../base/xsha256.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include <stdio.h>
#include <string.h>

static bool verify_fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static bool zero_bytes(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static bool equal_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool equal_fingerprint(XrFingerprint left, XrFingerprint right) {
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

static void put_text(XrSHA256Context *context, const char *text) {
    put_bytes(context, (const uint8_t *) text, text ? strlen(text) : 0);
}

static void put_id(XrSHA256Context *context, XrStableId id) {
    put_bytes(context, id.bytes, sizeof(id.bytes));
}

static void put_fingerprint(XrSHA256Context *context, XrFingerprint value) {
    put_bytes(context, value.bytes, sizeof(value.bytes));
}

static void put_span(XrSHA256Context *context, XaScalarSourceSpan span) {
    put_u32(context, span.kind);
    put_u32(context, span.start_line);
    put_u32(context, span.start_column);
    put_u32(context, span.end_line);
    put_u32(context, span.end_column);
}

static void begin_digest(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    put_text(context, domain);
    put_u32(context, XA_SCALAR_PROGRAM_AUTHORITY_SCHEMA);
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

static bool valid_span(XaScalarSourceSpan span, uint32_t kind) {
    if (span.kind != kind || span.start_line == 0 || span.start_column == 0 ||
        span.end_line == 0 || span.end_column == 0)
        return false;
    return span.end_line > span.start_line ||
           (span.end_line == span.start_line && span.end_column > span.start_column);
}

static bool point_before_or_equal(uint32_t left_line, uint32_t left_column,
                                  uint32_t right_line, uint32_t right_column) {
    return left_line < right_line ||
           (left_line == right_line && left_column <= right_column);
}

static bool span_contains(XaScalarSourceSpan outer, XaScalarSourceSpan inner) {
    return point_before_or_equal(outer.start_line, outer.start_column, inner.start_line,
                                 inner.start_column) &&
           point_before_or_equal(inner.end_line, inner.end_column, outer.end_line,
                                 outer.end_column);
}

static bool equal_span(XaScalarSourceSpan left, XaScalarSourceSpan right) {
    return left.kind == right.kind && left.start_line == right.start_line &&
           left.start_column == right.start_column && left.end_line == right.end_line &&
           left.end_column == right.end_column;
}

static XrFingerprint expected_policy(void) {
    XrSHA256Context context;
    begin_digest(&context, "xray-pre-xi-scalar-authority-policy-v1");
    put_u32(&context, XA_SCALAR_PROGRAM_FUNCTION_COUNT);
    put_u32(&context, 1);
    return finish_fingerprint(&context);
}

static XrStableId expected_module_id(XrFingerprint module_authority) {
    XrSHA256Context context;
    begin_digest(&context, "xray-source-module-identity-v1");
    put_fingerprint(&context, module_authority);
    return finish_id(&context);
}

static XrFingerprint expected_export_fingerprint(XrStableId module) {
    XrSHA256Context context;
    begin_digest(&context, "xray-source-module-empty-exports-v1");
    put_id(&context, module);
    put_u32(&context, 0);
    return finish_fingerprint(&context);
}

static XrFingerprint expected_signature(uint8_t parameter_count) {
    XrSHA256Context context;
    begin_digest(&context, "xray-language-scalar-function-signature-v1");
    put_u32(&context, parameter_count);
    put_u32(&context, parameter_count);
    put_u8(&context, (uint8_t) XR_FN_EFFECT_NO_THROW);
    if (parameter_count == 1) {
        put_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64);
        put_u8(&context, (uint8_t) XR_PARAM_READ);
    }
    put_u32(&context, (uint32_t) XR_EXACT_SCALAR_I64);
    return finish_fingerprint(&context);
}

static XrFingerprint expected_effect(void) {
    XrSHA256Context context;
    begin_digest(&context, "xray-language-scalar-effect-contract-v1");
    put_u32(&context, (uint32_t) XA_EFFECT_COMPLETE);
    put_u32(&context, 0); /* semantic effects */
    put_u32(&context, 0); /* unknown semantic effects */
    put_u32(&context, (uint32_t) XA_EFFECT_COMPLETE);
    put_u32(&context, 0); /* escaping errors */
    put_u32(&context, (uint32_t) XA_EFFECT_COMPLETE);
    put_u32(&context, 0); /* memory roots */
    put_u32(&context, (uint32_t) XA_ALLOC_PROVEN_NONE);
    put_u32(&context, 0); /* allocation reasons */
    return finish_fingerprint(&context);
}

static XrStableId expected_declaration(const XaScalarProgramAuthority *authority,
                                       const XaScalarFunctionAuthority *row) {
    XrSHA256Context context;
    begin_digest(&context, "xray-source-scalar-function-declaration-v1");
    put_id(&context, authority->module.module_identity);
    put_fingerprint(&context, authority->module.source_fingerprint);
    put_span(&context, row->declaration_span);
    put_fingerprint(&context, row->signature_fingerprint);
    return finish_id(&context);
}

static XrStableId expected_instance(const XaScalarProgramAuthority *authority,
                                    const XaScalarFunctionAuthority *row) {
    XrSHA256Context context;
    begin_digest(&context, "xray-source-nongeneric-scalar-instance-v1");
    put_id(&context, authority->module.module_identity);
    put_id(&context, row->declaration_identity);
    put_fingerprint(&context, row->signature_fingerprint);
    return finish_id(&context);
}

static XrStableId expected_callsite(const XaScalarProgramAuthority *authority) {
    XrSHA256Context context;
    begin_digest(&context, "xray-source-scalar-callsite-v1");
    put_fingerprint(&context, authority->module.source_fingerprint);
    put_id(&context, authority->module.module_identity);
    put_id(&context, authority->call.caller_declaration_identity);
    put_span(&context, authority->call.callsite_span);
    return finish_id(&context);
}

static XrFingerprint expected_call_contract(const XaScalarFunctionAuthority *callee) {
    XrSHA256Context context;
    begin_digest(&context, "xray-source-scalar-call-contract-v1");
    put_fingerprint(&context, callee->signature_fingerprint);
    put_fingerprint(&context, callee->effect_fingerprint);
    put_u64(&context, callee->capability_mask);
    return finish_fingerprint(&context);
}

static XrFingerprint expected_authority_fingerprint(
    const XaScalarProgramAuthority *authority) {
    XrSHA256Context context;
    begin_digest(&context, "xray-scalar-program-authority-freeze-v1");
    put_fingerprint(&context, authority->policy_fingerprint);
    put_id(&context, authority->module.module_identity);
    put_fingerprint(&context, authority->module.module_authority_fingerprint);
    put_fingerprint(&context, authority->module.source_fingerprint);
    put_fingerprint(&context, authority->module.export_fingerprint);
    for (uint32_t i = 0; i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row = &authority->functions[i];
        put_id(&context, row->declaration_identity);
        put_id(&context, row->concrete_instance_identity);
        put_fingerprint(&context, row->signature_fingerprint);
        put_fingerprint(&context, row->effect_fingerprint);
        put_span(&context, row->declaration_span);
        put_u64(&context, row->capability_mask);
        put_u8(&context, row->flags);
        put_u8(&context, row->parameter_count);
    }
    put_id(&context, authority->call.callsite_identity);
    put_id(&context, authority->call.caller_declaration_identity);
    put_id(&context, authority->call.caller_instance_identity);
    put_id(&context, authority->call.callee_declaration_identity);
    put_id(&context, authority->call.callee_instance_identity);
    put_fingerprint(&context, authority->call.contract_fingerprint);
    put_span(&context, authority->call.callsite_span);
    return finish_fingerprint(&context);
}

static const XaScalarFunctionAuthority *find_function(
    const XaScalarProgramAuthority *authority, XrStableId declaration, XrStableId instance) {
    const XaScalarFunctionAuthority *found = NULL;
    for (uint32_t i = 0; i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row = &authority->functions[i];
        if (!equal_id(row->declaration_identity, declaration) ||
            !equal_id(row->concrete_instance_identity, instance))
            continue;
        if (found)
            return NULL;
        found = row;
    }
    return found;
}

bool xa_scalar_program_authority_verify(const XaScalarProgramAuthority *authority,
                                        char *error, size_t error_size) {
    if (!authority || authority->schema != XA_SCALAR_PROGRAM_AUTHORITY_SCHEMA ||
        authority->verified != 1 || !zero_bytes(authority->reserved,
                                                sizeof(authority->reserved)) ||
        !equal_fingerprint(authority->policy_fingerprint, expected_policy()))
        return verify_fail(error, error_size, "scalar authority header is invalid");
    if (zero_bytes(authority->module.module_authority_fingerprint.bytes,
                   sizeof(authority->module.module_authority_fingerprint.bytes)) ||
        zero_bytes(authority->module.source_fingerprint.bytes,
                   sizeof(authority->module.source_fingerprint.bytes)) ||
        !equal_id(authority->module.module_identity,
                  expected_module_id(authority->module.module_authority_fingerprint)) ||
        !equal_fingerprint(authority->module.export_fingerprint,
                           expected_export_fingerprint(authority->module.module_identity)))
        return verify_fail(error, error_size, "scalar module authority is invalid");

    uint32_t roots = 0;
    for (uint32_t i = 0; i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row = &authority->functions[i];
        if (!valid_span(row->declaration_span, AST_FUNCTION_DECL) ||
            row->parameter_count > 1 || row->capability_mask != 0 ||
            (row->flags & ~XA_SCALAR_PROGRAM_FUNCTION_ENTRY) != 0 ||
            !zero_bytes(row->reserved, sizeof(row->reserved)) ||
            !equal_fingerprint(row->signature_fingerprint,
                               expected_signature(row->parameter_count)) ||
            !equal_fingerprint(row->effect_fingerprint, expected_effect()) ||
            !equal_id(row->declaration_identity, expected_declaration(authority, row)) ||
            !equal_id(row->concrete_instance_identity,
                      expected_instance(authority, row)))
            return verify_fail(error, error_size, "scalar function authority is invalid");
        roots += (row->flags & XA_SCALAR_PROGRAM_FUNCTION_ENTRY) != 0;
    }
    if (roots != 1 ||
        equal_span(authority->functions[0].declaration_span,
                   authority->functions[1].declaration_span) ||
        memcmp(authority->functions[0].declaration_identity.bytes,
               authority->functions[1].declaration_identity.bytes,
               sizeof(authority->functions[0].declaration_identity.bytes)) >= 0)
        return verify_fail(error, error_size, "scalar function set is not canonical");

    const XaScalarFunctionAuthority *caller =
        find_function(authority, authority->call.caller_declaration_identity,
                      authority->call.caller_instance_identity);
    const XaScalarFunctionAuthority *callee =
        find_function(authority, authority->call.callee_declaration_identity,
                      authority->call.callee_instance_identity);
    if (!caller || !callee || caller == callee || caller->parameter_count != 0 ||
        callee->parameter_count != 1 ||
        (caller->flags & XA_SCALAR_PROGRAM_FUNCTION_ENTRY) == 0 ||
        !valid_span(authority->call.callsite_span, AST_CALL_EXPR) ||
        !span_contains(caller->declaration_span, authority->call.callsite_span) ||
        !zero_bytes(authority->call.reserved, sizeof(authority->call.reserved)) ||
        !equal_id(authority->call.callsite_identity, expected_callsite(authority)) ||
        !equal_fingerprint(authority->call.contract_fingerprint,
                           expected_call_contract(callee)))
        return verify_fail(error, error_size, "scalar direct-call authority is invalid");
    if (!equal_fingerprint(authority->fingerprint,
                           expected_authority_fingerprint(authority)))
        return verify_fail(error, error_size, "scalar authority fingerprint is invalid");
    return true;
}
