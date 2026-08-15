/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_entry_abi.c - Canonical dynamic-entry ABI identity
 */

#include "xr_target_entry_abi.h"
#include "../../base/xsha256.h"

static void hash_u16(XrSHA256Context *context, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) (value >> 8), (uint8_t) value};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t) (value >> 24), (uint8_t) (value >> 16),
                        (uint8_t) (value >> 8), (uint8_t) value};
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> ((sizeof(bytes) - 1u - i) * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

bool xr_target_entry_abi_fingerprint(const XrTargetEntryAbiFacts *facts,
                                     XrFingerprint *fingerprint) {
    if (!facts || !fingerprint ||
        facts->schema_version != XR_TARGET_ENTRY_ABI_SCHEMA_VERSION ||
        facts->value_kind != XR_TARGET_ENTRY_VALUE_EXACT_I64 ||
        facts->reserved8[0] || facts->reserved8[1] || facts->reserved8[2])
        return false;
    static const uint8_t domain[] = "xray-entry-abi-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, facts->schema_version);
    hash_u16(&context, facts->parameter_count);
    hash_u16(&context, facts->value_kind);
    hash_u16(&context, facts->native_abi);
    hash_u64(&context, facts->target_data_layout);
    xr_sha256_update(&context, facts->target_profile_fingerprint.bytes,
                     sizeof(facts->target_profile_fingerprint.bytes));
    xr_sha256_final(&context, fingerprint->bytes);
    return true;
}

bool xr_target_entry_identity_adapter_fingerprint(
    const XrFingerprint *entry_abi_fingerprint, XrFingerprint *fingerprint) {
    if (!entry_abi_fingerprint || !fingerprint)
        return false;
    static const uint8_t domain[] = "xray-entry-adapter-identity-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_TARGET_ENTRY_ADAPTER_IDENTITY);
    xr_sha256_update(&context, entry_abi_fingerprint->bytes,
                     sizeof(entry_abi_fingerprint->bytes));
    xr_sha256_final(&context, fingerprint->bytes);
    return true;
}
