/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_key.c - Domain-separated incremental artifact cache keys
 */

#include "xr_cache_key.h"

#include "../base/xsha256.h"
#include <string.h>

#define XR_SEMANTIC_CACHE_FIELD_COUNT 9u
#define XR_TARGET_CACHE_FIELD_COUNT 8u

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t encoded[4];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, encoded, sizeof(encoded));
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t encoded[8];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, encoded, sizeof(encoded));
}

static void hash_domain(XrSHA256Context *ctx, const char *domain) {
    size_t size = strlen(domain);
    hash_u64(ctx, (uint64_t) size);
    xr_sha256_update(ctx, (const uint8_t *) domain, size);
}

static void build_key(const char *domain, const XrCacheFingerprint *fields, uint32_t field_count,
                      XrCacheKey *out) {
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    hash_domain(&ctx, domain);
    hash_u32(&ctx, field_count);
    for (uint32_t i = 0; i < field_count; i++) {
        hash_u32(&ctx, i);
        hash_u32(&ctx, XR_CACHE_KEY_BYTES);
        xr_sha256_update(&ctx, fields[i].bytes, XR_CACHE_KEY_BYTES);
    }
    xr_sha256_final(&ctx, out->bytes);
}

void xr_cache_fingerprint_bytes(const uint8_t *bytes, size_t size, XrCacheFingerprint *out) {
    if (!out || (!bytes && size != 0))
        return;
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    hash_domain(&ctx, "xray-cache-content-v1");
    hash_u64(&ctx, (uint64_t) size);
    if (size != 0)
        xr_sha256_update(&ctx, bytes, size);
    xr_sha256_final(&ctx, out->bytes);
}

void xr_cache_key_semantic(const XrSemanticCacheKeyInput *input, XrCacheKey *out) {
    if (!input || !out)
        return;
    XrCacheFingerprint fields[XR_SEMANTIC_CACHE_FIELD_COUNT] = {
        input->normalized_source,
        input->program_semantic_closure,
        input->generation_closure,
        input->compiler,
        input->semantic_schema,
        input->contract,
        input->language_configuration,
        input->semantic_dependencies,
        input->declaration_identities,
    };
    build_key("xray-semantic-cache-key-v2", fields, XR_SEMANTIC_CACHE_FIELD_COUNT, out);
}

void xr_cache_key_target(const XrTargetCacheKeyInput *input, XrCacheKey *out) {
    if (!input || !out)
        return;
    XrCacheFingerprint fields[XR_TARGET_CACHE_FIELD_COUNT] = {
        input->program_semantic_closure, input->generation_closure,
        input->semantic_plan,       input->target_profile,
        input->provider_capabilities, input->runtime_abi,
        input->planner_schema,     input->optimization_budget,
    };
    build_key("xray-target-cache-key-v2", fields, XR_TARGET_CACHE_FIELD_COUNT, out);
}

bool xr_cache_key_equal(XrCacheKey left, XrCacheKey right) {
    return memcmp(left.bytes, right.bytes, XR_CACHE_KEY_BYTES) == 0;
}

int xr_cache_key_compare(XrCacheKey left, XrCacheKey right) {
    return memcmp(left.bytes, right.bytes, XR_CACHE_KEY_BYTES);
}

void xr_cache_key_hex(XrCacheKey key, char out[XR_CACHE_KEY_HEX_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    if (!out)
        return;
    for (size_t i = 0; i < XR_CACHE_KEY_BYTES; i++) {
        out[i * 2] = digits[key.bytes[i] >> 4u];
        out[i * 2 + 1] = digits[key.bytes[i] & 0x0fu];
    }
    out[XR_CACHE_KEY_BYTES * 2u] = '\0';
}

static int hex_value(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

bool xr_cache_key_from_hex(const char *hex, XrCacheKey *out) {
    if (!hex || !out || strlen(hex) != XR_CACHE_KEY_BYTES * 2u)
        return false;
    XrCacheKey parsed = {{0}};
    for (size_t i = 0; i < XR_CACHE_KEY_BYTES; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        parsed.bytes[i] = (uint8_t) ((high << 4) | low);
    }
    *out = parsed;
    return true;
}
