/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile_verify.c - Runtime-side immutable target verification
 */

#include "xr_target_profile_internal.h"

#include <stdio.h>
#include <string.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static bool is_power_of_two(uint32_t value) {
    return value && (value & (value - 1u)) == 0;
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    uint8_t combined = 0;
    for (uint32_t index = 0; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined == 0;
}

static bool profile_identity_is_consistent(const XrTargetMachineFacts *facts) {
    switch (facts->operating_system) {
        case XR_TARGET_OS_WINDOWS:
            return facts->environment == XR_TARGET_ENV_MSVC &&
                   ((facts->architecture == XR_TARGET_ARCH_X86_64 &&
                     facts->native_abi == XR_TARGET_ABI_WIN64_X86_64) ||
                    (facts->architecture == XR_TARGET_ARCH_AARCH64 &&
                     facts->native_abi == XR_TARGET_ABI_WIN64_AARCH64));
        case XR_TARGET_OS_LINUX:
            if (facts->environment != XR_TARGET_ENV_GNU && facts->environment != XR_TARGET_ENV_MUSL)
                return false;
            if (facts->architecture == XR_TARGET_ARCH_X86_64)
                return facts->native_abi == XR_TARGET_ABI_SYSV_X86_64;
            if (facts->architecture == XR_TARGET_ARCH_AARCH64)
                return facts->native_abi == XR_TARGET_ABI_AAPCS64;
            if (facts->architecture == XR_TARGET_ARCH_POWERPC64)
                return facts->native_abi == XR_TARGET_ABI_PPC64_ELFV2;
            if (facts->architecture == XR_TARGET_ARCH_LOONGARCH64)
                return facts->native_abi == XR_TARGET_ABI_LOONGARCH_LP64D;
            return false;
        case XR_TARGET_OS_MACOS:
            return facts->environment == XR_TARGET_ENV_DARWIN &&
                   ((facts->architecture == XR_TARGET_ARCH_X86_64 &&
                     facts->native_abi == XR_TARGET_ABI_DARWIN_X86_64) ||
                    (facts->architecture == XR_TARGET_ARCH_AARCH64 &&
                     facts->native_abi == XR_TARGET_ABI_DARWIN_AARCH64));
        case XR_TARGET_OS_WASI:
            return facts->architecture == XR_TARGET_ARCH_WASM32 &&
                   facts->environment == XR_TARGET_ENV_WASI &&
                   facts->native_abi == XR_TARGET_ABI_WASM;
        case XR_TARGET_OS_FREESTANDING:
            if (facts->environment != XR_TARGET_ENV_FREESTANDING)
                return false;
            switch (facts->architecture) {
                case XR_TARGET_ARCH_X86_64:
                    return facts->native_abi == XR_TARGET_ABI_SYSV_X86_64;
                case XR_TARGET_ARCH_AARCH64:
                    return facts->native_abi == XR_TARGET_ABI_AAPCS64;
                case XR_TARGET_ARCH_POWERPC64:
                    return facts->native_abi == XR_TARGET_ABI_PPC64_ELFV2;
                case XR_TARGET_ARCH_LOONGARCH64:
                    return facts->native_abi == XR_TARGET_ABI_LOONGARCH_LP64D;
                case XR_TARGET_ARCH_WASM32:
                    return facts->native_abi == XR_TARGET_ABI_WASM;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static bool profile_machine_features_are_consistent(const XrTargetMachineFacts *facts) {
    uint64_t vectors = facts->vector_feature_mask;
    uint64_t allowed = 0;
    switch (facts->architecture) {
        case XR_TARGET_ARCH_X86_64:
            allowed = XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 | XR_TARGET_VECTOR_AVX512;
            break;
        case XR_TARGET_ARCH_AARCH64:
            allowed = XR_TARGET_VECTOR_NEON | XR_TARGET_VECTOR_SVE;
            break;
        case XR_TARGET_ARCH_POWERPC64:
            allowed = XR_TARGET_VECTOR_VSX;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            allowed = XR_TARGET_VECTOR_LSX;
            break;
        case XR_TARGET_ARCH_WASM32:
            allowed = XR_TARGET_VECTOR_WASM128;
            break;
        default:
            return false;
    }
    uint32_t expected_pointer_size = facts->architecture == XR_TARGET_ARCH_WASM32 ? 4u : 8u;
    bool requires_little_endian = facts->architecture == XR_TARGET_ARCH_X86_64 ||
                                  facts->architecture == XR_TARGET_ARCH_LOONGARCH64 ||
                                  facts->architecture == XR_TARGET_ARCH_WASM32 ||
                                  facts->operating_system == XR_TARGET_OS_WINDOWS ||
                                  facts->operating_system == XR_TARGET_OS_MACOS;
    uint16_t exact_vector_bits = 0;
    switch (facts->architecture) {
        case XR_TARGET_ARCH_X86_64:
            if (vectors == XR_TARGET_VECTOR_SSE2)
                exact_vector_bits = 128;
            else if (vectors == (XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2))
                exact_vector_bits = 256;
            else if (vectors ==
                     (XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 | XR_TARGET_VECTOR_AVX512))
                exact_vector_bits = 512;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_AARCH64:
            if (vectors == XR_TARGET_VECTOR_NEON)
                exact_vector_bits = 128;
            else if (vectors == (XR_TARGET_VECTOR_NEON | XR_TARGET_VECTOR_SVE)) {
                if (facts->maximum_vector_bits < 128 || facts->maximum_vector_bits > 2048 ||
                    !is_power_of_two(facts->maximum_vector_bits))
                    return false;
                exact_vector_bits = facts->maximum_vector_bits;
            } else if (vectors != 0) {
                return false;
            }
            break;
        case XR_TARGET_ARCH_POWERPC64:
            if (vectors == XR_TARGET_VECTOR_VSX)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            if (vectors == XR_TARGET_VECTOR_LSX)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_WASM32:
            if (vectors == XR_TARGET_VECTOR_WASM128)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        default:
            return false;
    }
    return (vectors & ~allowed) == 0 && facts->maximum_vector_bits == exact_vector_bits &&
           facts->data_layout.pointer.size == expected_pointer_size &&
           (!requires_little_endian || facts->data_layout.endian == XR_TARGET_ENDIAN_LITTLE);
}

bool xr_target_profile_verify(const XrTargetProfile *profile, char *error, size_t error_size) {
    if (!profile || !profile->frozen)
        return report(error, error_size, "XR_TARGET_1000", "target profile is not frozen");
    const XrTargetProfileDraft *facts = &profile->facts;
    const XrTargetMachineFacts *machine = &facts->machine;
    if (facts->schema_version != XR_TARGET_PROFILE_SCHEMA_VERSION ||
        machine->architecture <= XR_TARGET_ARCH_NONE ||
        machine->architecture >= XR_TARGET_ARCH_COUNT ||
        machine->operating_system <= XR_TARGET_OS_NONE ||
        machine->operating_system >= XR_TARGET_OS_COUNT ||
        machine->environment <= XR_TARGET_ENV_NONE || machine->environment >= XR_TARGET_ENV_COUNT ||
        machine->native_abi <= XR_TARGET_ABI_NONE || machine->native_abi >= XR_TARGET_ABI_COUNT ||
        machine->runtime_profile < XR_TARGET_RUNTIME_PROFILE_HOSTED ||
        machine->runtime_profile > XR_TARGET_RUNTIME_PROFILE_FREESTANDING ||
        machine->reserved8[0] != 0 || machine->reserved8[1] != 0 || machine->reserved8[2] != 0 ||
        machine->reserved16 != 0)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile contains an unsupported exact identity");
    if (!xr_target_data_layout_validate(&machine->data_layout))
        return report(error, error_size, "XR_TARGET_1000", "target profile data layout is invalid");
    const uint64_t atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 | XR_TARGET_ATOMIC_WIDTH_16 |
                                       XR_TARGET_ATOMIC_WIDTH_32 | XR_TARGET_ATOMIC_WIDTH_64 |
                                       XR_TARGET_ATOMIC_WIDTH_128;
    const uint64_t atomic_order_mask = XR_TARGET_ATOMIC_RELAXED | XR_TARGET_ATOMIC_ACQUIRE |
                                       XR_TARGET_ATOMIC_RELEASE | XR_TARGET_ATOMIC_ACQ_REL |
                                       XR_TARGET_ATOMIC_SEQ_CST;
    const uint64_t float_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT |
                                XR_TARGET_FLOAT_FAST | XR_TARGET_FLOAT_FMA;
    const uint64_t vector_mask = XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 |
                                 XR_TARGET_VECTOR_AVX512 | XR_TARGET_VECTOR_NEON |
                                 XR_TARGET_VECTOR_SVE | XR_TARGET_VECTOR_VSX |
                                 XR_TARGET_VECTOR_LSX | XR_TARGET_VECTOR_WASM128;
    const uint64_t provider_mask =
        XR_TARGET_PROVIDER_MASK_ALL | XR_TARGET_PROVIDER_DERIVED_CAPABILITY_MASK;
    if ((machine->atomic_width_mask & ~atomic_width_mask) != 0 ||
        (machine->atomic_order_mask & ~atomic_order_mask) != 0 ||
        (machine->float_feature_mask & ~float_mask) != 0 ||
        (machine->vector_feature_mask & ~vector_mask) != 0 ||
        (facts->provider_mask & ~provider_mask) != 0 ||
        (facts->provider_mask & XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR)) == 0 ||
        (facts->provider_mask & XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC)) == 0 ||
        (machine->float_feature_mask & XR_TARGET_FLOAT_IEEE754) == 0 ||
        ((machine->float_feature_mask & XR_TARGET_FLOAT_STRICT) != 0 &&
         (machine->float_feature_mask & XR_TARGET_FLOAT_FAST) != 0) ||
        (machine->vector_feature_mask == 0 && machine->maximum_vector_bits != 0) ||
        (machine->vector_feature_mask != 0 &&
         (!is_power_of_two(machine->maximum_vector_bits) || machine->maximum_vector_bits < 128u ||
          machine->maximum_vector_bits > 2048u)) ||
        fingerprint_is_zero(facts->provider_set_fingerprint) ||
        fingerprint_is_zero(facts->object_header_fingerprint) ||
        fingerprint_is_zero(facts->runtime_abi_fingerprint) ||
        xr_runtime_string_literal_materialization_contract_verify(&facts->string_literal) !=
            XR_RUNTIME_ABI_OK)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile runtime facts are incomplete");
    if (!profile_identity_is_consistent(machine) ||
        !profile_machine_features_are_consistent(machine))
        return report(error, error_size, "XR_TARGET_1000",
                      "target machine identity or feature facts are inconsistent");
    XrTargetSemanticsId target_semantics_id;
    XrBoundaryAbi boundary_abi;
    XrRuntimeKernelContract runtime_kernel;
    xr_target_profile_compute_partitions(facts, &target_semantics_id, &boundary_abi,
                                         &runtime_kernel);
    if (!xr_fingerprint_equal(target_semantics_id, profile->target_semantics_id) ||
        memcmp(&boundary_abi, &profile->boundary_abi, sizeof(boundary_abi)) != 0 ||
        memcmp(&runtime_kernel, &profile->runtime_kernel, sizeof(runtime_kernel)) != 0)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile partition identity changed after freeze");
    if (profile->provider_contracts_materialized) {
        uint64_t actual_provider_mask = 0;
        XrFingerprint provider_set_id;
        if (!profile->providers || profile->provider_count == 0 ||
            profile->provider_count > XR_RUNTIME_ABI_MAX_PROVIDERS ||
            xr_target_provider_set_fingerprint(profile->providers, profile->provider_count,
                                               &actual_provider_mask,
                                               &provider_set_id) != XR_RUNTIME_ABI_OK ||
            actual_provider_mask != facts->provider_mask ||
            !xr_fingerprint_equal(provider_set_id, facts->provider_set_fingerprint))
            return report(error, error_size, "XR_TARGET_1000",
                          "materialized provider contracts do not match profile identity");
    } else if (profile->providers || profile->provider_count != 0) {
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile has partial provider materialization");
    }
    XrFingerprint actual;
    xr_target_profile_compute_fingerprint(facts, &actual);
    if (!xr_fingerprint_equal(actual, profile->fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile fingerprint changed after freeze");
    return true;
}
