/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile.c - Immutable target profile storage and hashing
 */

#include "xr_target_profile_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_fingerprint(XrSHA256Context *ctx, XrFingerprint fingerprint) {
    xr_sha256_update(ctx, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void hash_type_layout(XrSHA256Context *ctx, XrTargetTypeLayout layout) {
    hash_u64(ctx, layout.size);
    hash_u64(ctx, layout.align);
}

void xr_target_profile_compute_fingerprint(const XrTargetProfileDraft *facts,
                                           XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-profile-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_u64(&ctx, facts->schema_version);
    hash_u64(&ctx, facts->architecture);
    hash_u64(&ctx, facts->operating_system);
    hash_u64(&ctx, facts->environment);
    hash_u64(&ctx, facts->native_abi);
    hash_u64(&ctx, facts->runtime_profile);
    for (uint32_t i = 0; i < sizeof(facts->reserved8); i++)
        hash_u64(&ctx, facts->reserved8[i]);
    hash_type_layout(&ctx, facts->data_layout.i8);
    hash_type_layout(&ctx, facts->data_layout.u8);
    hash_type_layout(&ctx, facts->data_layout.i16);
    hash_type_layout(&ctx, facts->data_layout.u16);
    hash_type_layout(&ctx, facts->data_layout.i32);
    hash_type_layout(&ctx, facts->data_layout.u32);
    hash_type_layout(&ctx, facts->data_layout.i64);
    hash_type_layout(&ctx, facts->data_layout.u64);
    hash_type_layout(&ctx, facts->data_layout.f32);
    hash_type_layout(&ctx, facts->data_layout.f64);
    hash_type_layout(&ctx, facts->data_layout.boolean);
    hash_type_layout(&ctx, facts->data_layout.pointer);
    hash_type_layout(&ctx, facts->data_layout.isize);
    hash_type_layout(&ctx, facts->data_layout.usize);
    hash_type_layout(&ctx, facts->data_layout.xr_value);
    hash_u64(&ctx, facts->data_layout.endian);
    hash_u64(&ctx, facts->data_layout.abi_id);
    hash_u64(&ctx, facts->data_layout.stable_hash);
    hash_u64(&ctx, facts->atomic_width_mask);
    hash_u64(&ctx, facts->atomic_order_mask);
    hash_u64(&ctx, facts->float_feature_mask);
    hash_u64(&ctx, facts->vector_feature_mask);
    hash_u64(&ctx, facts->maximum_vector_bits);
    hash_u64(&ctx, facts->reserved16);
    hash_u64(&ctx, facts->provider_mask);
    hash_fingerprint(&ctx, facts->provider_set_fingerprint);
    hash_fingerprint(&ctx, facts->object_header_fingerprint);
    hash_fingerprint(&ctx, facts->runtime_abi_fingerprint);
    xr_sha256_final(&ctx, out->bytes);
}

bool xr_target_profile_freeze(const XrTargetProfileDraft *draft, XrTargetProfile **out,
                              char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!draft || !out) {
        set_error(error, error_size, "XR_TARGET_1000", "target profile input is missing");
        return false;
    }
    XrTargetProfile *profile = (XrTargetProfile *) xr_calloc(1, sizeof(*profile));
    if (!profile) {
        set_error(error, error_size, "XR_EXEC_5003", "target profile allocation failed");
        return false;
    }
    atomic_init(&profile->references, 1);
    memcpy(&profile->facts, draft, sizeof(*draft));
    xr_target_profile_compute_fingerprint(&profile->facts, &profile->fingerprint);
    profile->frozen = true;
    if (!xr_target_profile_verify(profile, error, error_size)) {
        xr_target_profile_free(profile);
        return false;
    }
    *out = profile;
    return true;
}

XrTargetProfile *xr_target_profile_retain(XrTargetProfile *profile) {
    if (profile)
        atomic_fetch_add_explicit(&profile->references, 1, memory_order_relaxed);
    return profile;
}

void xr_target_profile_free(XrTargetProfile *profile) {
    if (!profile)
        return;
    if (atomic_fetch_sub_explicit(&profile->references, 1, memory_order_acq_rel) != 1)
        return;
    xr_free(profile);
}

bool xr_target_profile_is_frozen(const XrTargetProfile *profile) {
    return profile && profile->frozen;
}

XrFingerprint xr_target_profile_fingerprint(const XrTargetProfile *profile) {
    XrFingerprint zero = {{0}};
    return profile ? profile->fingerprint : zero;
}

const XrTargetProfileDraft *xr_target_profile_facts(const XrTargetProfile *profile) {
    return profile ? &profile->facts : NULL;
}
