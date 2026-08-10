/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_ids.c - Stable semantic identities and fingerprints
 */

#include "xr_semantic_ids.h"
#include "../../base/xsha256.h"
#include <string.h>

static const uint8_t xr_entity_domain[] = "xray-entity-id-v1\0";
static const uint8_t xr_semantic_domain[] = "xray-semantic-plan-v1\0";

static void hash_framed(const uint8_t *domain, size_t domain_size, const uint8_t *bytes,
                        size_t size, uint8_t digest[XR_FINGERPRINT_BYTES]) {
    XrSHA256Context ctx;
    uint8_t length[8];
    uint64_t width = (uint64_t) size;
    for (unsigned i = 0; i < sizeof(length); i++)
        length[i] = (uint8_t) (width >> (i * 8));
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, domain_size);
    xr_sha256_update(&ctx, length, sizeof(length));
    xr_sha256_update(&ctx, bytes, size);
    xr_sha256_final(&ctx, digest);
}

bool xr_stable_id_from_key(const char *canonical_key, XrStableId *id, XrFingerprint *key_digest) {
    if (!canonical_key || !id || !key_digest)
        return false;
    uint8_t digest[XR_FINGERPRINT_BYTES];
    hash_framed(xr_entity_domain, sizeof(xr_entity_domain) - 1, (const uint8_t *) canonical_key,
                strlen(canonical_key), digest);
    memcpy(id->bytes, digest, sizeof(id->bytes));
    memcpy(key_digest->bytes, digest, sizeof(key_digest->bytes));
    return true;
}

void xr_semantic_fingerprint(const uint8_t *bytes, size_t size, XrFingerprint *out) {
    if (!out || (!bytes && size != 0))
        return;
    hash_framed(xr_semantic_domain, sizeof(xr_semantic_domain) - 1, bytes, size, out->bytes);
}

bool xr_stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

bool xr_fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

int xr_stable_id_compare(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes));
}

static void bytes_hex(const uint8_t *bytes, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[size * 2] = '\0';
}

void xr_stable_id_hex(XrStableId id, char out[XR_STABLE_ID_BYTES * 2 + 1]) {
    if (out)
        bytes_hex(id.bytes, sizeof(id.bytes), out);
}

void xr_fingerprint_hex(XrFingerprint fingerprint, char out[XR_FINGERPRINT_BYTES * 2 + 1]) {
    if (out)
        bytes_hex(fingerprint.bytes, sizeof(fingerprint.bytes), out);
}
