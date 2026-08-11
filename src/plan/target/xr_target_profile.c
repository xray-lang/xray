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

static void set_runtime_error(char *error, size_t size, const char *detail,
                              XrRuntimeAbiStatus status) {
    if (error && size)
        snprintf(error, size, "XR_TARGET_1000: %s: %s", detail,
                 xr_runtime_abi_status_name(status));
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
    const XrTargetMachineFacts *machine = &facts->machine;
    hash_u64(&ctx, machine->architecture);
    hash_u64(&ctx, machine->operating_system);
    hash_u64(&ctx, machine->environment);
    hash_u64(&ctx, machine->native_abi);
    hash_u64(&ctx, machine->runtime_profile);
    for (uint32_t i = 0; i < sizeof(machine->reserved8); i++)
        hash_u64(&ctx, machine->reserved8[i]);
    hash_type_layout(&ctx, machine->data_layout.i8);
    hash_type_layout(&ctx, machine->data_layout.u8);
    hash_type_layout(&ctx, machine->data_layout.i16);
    hash_type_layout(&ctx, machine->data_layout.u16);
    hash_type_layout(&ctx, machine->data_layout.i32);
    hash_type_layout(&ctx, machine->data_layout.u32);
    hash_type_layout(&ctx, machine->data_layout.i64);
    hash_type_layout(&ctx, machine->data_layout.u64);
    hash_type_layout(&ctx, machine->data_layout.f32);
    hash_type_layout(&ctx, machine->data_layout.f64);
    hash_type_layout(&ctx, machine->data_layout.boolean);
    hash_type_layout(&ctx, machine->data_layout.pointer);
    hash_type_layout(&ctx, machine->data_layout.isize);
    hash_type_layout(&ctx, machine->data_layout.usize);
    hash_type_layout(&ctx, machine->data_layout.xr_value);
    hash_u64(&ctx, machine->data_layout.endian);
    hash_u64(&ctx, machine->data_layout.abi_id);
    hash_u64(&ctx, machine->data_layout.stable_hash);
    hash_u64(&ctx, machine->atomic_width_mask);
    hash_u64(&ctx, machine->atomic_order_mask);
    hash_u64(&ctx, machine->float_feature_mask);
    hash_u64(&ctx, machine->vector_feature_mask);
    hash_u64(&ctx, machine->maximum_vector_bits);
    hash_u64(&ctx, machine->reserved16);
    hash_u64(&ctx, facts->provider_mask);
    hash_fingerprint(&ctx, facts->provider_set_fingerprint);
    hash_fingerprint(&ctx, facts->object_header_fingerprint);
    hash_fingerprint(&ctx, facts->runtime_abi_fingerprint);
    xr_sha256_final(&ctx, out->bytes);
}

bool xr_target_profile_build(const XrTargetProfileBuildInput *input,
                             XrTargetProfile **out, char *error,
                             size_t error_size) {
    if (out)
        *out = NULL;
    if (!input || !out || !input->runtime_abi ||
        !input->object_header_materialization || !input->providers) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "structured target profile input is missing");
        return false;
    }
    if (input->provider_count == 0 ||
        input->provider_count > XR_RUNTIME_ABI_MAX_PROVIDERS) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "target provider contract count is invalid");
        return false;
    }
    for (size_t i = 0; i < input->provider_count; i++) {
        if (input->providers[i].runtime_profile !=
            input->machine.runtime_profile) {
            set_error(error, error_size, "XR_TARGET_1000",
                      "provider runtime profile does not match machine facts");
            return false;
        }
    }
    if (input->runtime_abi->target_endian != input->machine.data_layout.endian ||
        input->runtime_abi->pointer_width != input->machine.data_layout.pointer.size) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "runtime ABI does not match exact machine layout facts");
        return false;
    }

    XrRuntimeObjectHeaderAbi materialized_header;
    XrRuntimeAbiStatus status = xr_runtime_object_header_abi_materialize(
        input->object_header_materialization, &materialized_header);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size,
                          "object-header materialization facts are invalid", status);
        return false;
    }
    XrFingerprint object_header_fingerprint;
    status = xr_runtime_object_header_abi_fingerprint(
        &materialized_header, &object_header_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size,
                          "materialized object-header ABI is invalid", status);
        return false;
    }
    XrFingerprint contract_header_fingerprint;
    status = xr_runtime_object_header_abi_fingerprint(
        &input->runtime_abi->object_header, &contract_header_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size,
                          "runtime object-header ABI is invalid", status);
        return false;
    }
    if (!xr_fingerprint_equal(object_header_fingerprint,
                              contract_header_fingerprint)) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "runtime and materialized object-header ABIs differ");
        return false;
    }

    XrFingerprint runtime_abi_fingerprint;
    status = xr_runtime_abi_contract_fingerprint(input->runtime_abi,
                                                 &runtime_abi_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size,
                          "runtime ABI contract is invalid", status);
        return false;
    }
    uint64_t provider_mask = 0;
    XrFingerprint provider_set_fingerprint;
    status = xr_target_provider_set_fingerprint(
        input->providers, input->provider_count, &provider_mask,
        &provider_set_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size,
                          "target provider contracts are invalid", status);
        return false;
    }
    XrTargetProfileDraft draft = {
        .schema_version = XR_TARGET_PROFILE_SCHEMA_VERSION,
        .machine = input->machine,
        .provider_mask = provider_mask,
        .provider_set_fingerprint = provider_set_fingerprint,
        .object_header_fingerprint = object_header_fingerprint,
        .runtime_abi_fingerprint = runtime_abi_fingerprint,
    };
    return xr_target_profile_freeze(&draft, out, error, error_size);
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

bool xr_target_profile_require_exact(const XrTargetProfile *expected,
                                     const XrTargetProfile *actual, char *error,
                                     size_t error_size) {
    if (!expected || !actual ||
        !xr_target_profile_verify(expected, NULL, 0) ||
        !xr_target_profile_verify(actual, NULL, 0)) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "exact target profile comparison requires verified profiles");
        return false;
    }
    if (!xr_fingerprint_equal(expected->fingerprint, actual->fingerprint)) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "exact target profile fingerprint mismatch");
        return false;
    }
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

const XrTargetMachineFacts *xr_target_profile_machine_facts(
    const XrTargetProfile *profile) {
    return profile ? &profile->facts.machine : NULL;
}
