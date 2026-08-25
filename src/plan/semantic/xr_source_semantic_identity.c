/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_source_semantic_identity.c - Canonical source identity authority
 */

#include "xr_source_semantic_identity.h"
#include "../../base/xsha256.h"
#include <string.h>

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

static void begin(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    put_bytes(context, (const uint8_t *) domain, strlen(domain));
    put_u32(context, XR_SOURCE_SEMANTIC_IDENTITY_VERSION);
}

static void put_id(XrSHA256Context *context, XrStableId id) {
    put_bytes(context, id.bytes, sizeof(id.bytes));
}

static void put_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    put_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static bool bytes_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
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

XR_FUNC bool xr_source_semantic_module_authority(const char *canonical_module,
                                                 XrFingerprint source_fingerprint,
                                                 XrProgramSemanticModuleInput *out,
                                                 XrFingerprint *authority_fingerprint) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (authority_fingerprint)
        memset(authority_fingerprint, 0, sizeof(*authority_fingerprint));
    if (!out || !canonical_module || canonical_module[0] == '\0' ||
        bytes_zero(source_fingerprint.bytes, sizeof(source_fingerprint.bytes)))
        return false;
    XrSHA256Context context;
    begin(&context, "xray-source-module-authority-v1");
    put_bytes(&context, (const uint8_t *) canonical_module, strlen(canonical_module));
    XrFingerprint authority = finish_fingerprint(&context);
    begin(&context, "xray-source-module-identity-v1");
    put_fingerprint(&context, authority);
    XrStableId identity = finish_id(&context);
    begin(&context, "xray-source-module-empty-exports-v1");
    put_id(&context, identity);
    put_u32(&context, 0);
    *out = (XrProgramSemanticModuleInput) {
        .module_identity = identity,
        .module_authority_fingerprint = authority,
        .source_fingerprint = source_fingerprint,
        .export_fingerprint = finish_fingerprint(&context),
    };
    if (authority_fingerprint)
        *authority_fingerprint = authority;
    return true;
}

XR_FUNC bool xr_source_semantic_callsite_identity(XrFingerprint source_fingerprint,
                                                  XrStableId module_identity,
                                                  XrStableId caller_declaration,
                                                  XrProgramSemanticSourceLocator locator,
                                                  XrStableId *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || bytes_zero(source_fingerprint.bytes, sizeof(source_fingerprint.bytes)) ||
        bytes_zero(module_identity.bytes, sizeof(module_identity.bytes)) ||
        bytes_zero(caller_declaration.bytes, sizeof(caller_declaration.bytes)) ||
        locator.kind == 0 || locator.start_line == 0 || locator.start_column == 0 ||
        locator.end_line == 0 || locator.end_column == 0 ||
        (locator.end_line < locator.start_line) ||
        (locator.end_line == locator.start_line && locator.end_column <= locator.start_column))
        return false;
    XrSHA256Context context;
    begin(&context, "xray-source-program-callsite-v2");
    put_fingerprint(&context, source_fingerprint);
    put_id(&context, module_identity);
    put_id(&context, caller_declaration);
    put_u32(&context, locator.kind);
    put_u32(&context, locator.start_line);
    put_u32(&context, locator.start_column);
    put_u32(&context, locator.end_line);
    put_u32(&context, locator.end_column);
    *out = finish_id(&context);
    return true;
}
