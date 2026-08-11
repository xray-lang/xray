/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile.h - Immutable backend-neutral target facts
 *
 * KEY CONCEPT:
 *   A target profile is an exact, immutable value. It contains numeric ABI,
 *   runtime, and provider identities but no backend spelling or compiler
 *   object, so every executor can compare the same fingerprint.
 */

#ifndef XR_TARGET_PROFILE_H
#define XR_TARGET_PROFILE_H

#include "../semantic/xr_semantic_ids.h"
#include "../../base/xtarget_data_layout.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_TARGET_PLAN_SCHEMA_VERSION UINT32_C(1)

typedef struct XrTargetProfile XrTargetProfile;

typedef enum XrTargetArchitecture {
    XR_TARGET_ARCH_NONE = 0,
    XR_TARGET_ARCH_X86_64,
    XR_TARGET_ARCH_AARCH64,
    XR_TARGET_ARCH_POWERPC64,
    XR_TARGET_ARCH_LOONGARCH64,
    XR_TARGET_ARCH_WASM32,
    XR_TARGET_ARCH_COUNT,
} XrTargetArchitecture;

typedef enum XrTargetOperatingSystem {
    XR_TARGET_OS_NONE = 0,
    XR_TARGET_OS_WINDOWS,
    XR_TARGET_OS_LINUX,
    XR_TARGET_OS_MACOS,
    XR_TARGET_OS_WASI,
    XR_TARGET_OS_FREESTANDING,
    XR_TARGET_OS_COUNT,
} XrTargetOperatingSystem;

typedef enum XrTargetEnvironment {
    XR_TARGET_ENV_NONE = 0,
    XR_TARGET_ENV_MSVC,
    XR_TARGET_ENV_GNU,
    XR_TARGET_ENV_MUSL,
    XR_TARGET_ENV_DARWIN,
    XR_TARGET_ENV_WASI,
    XR_TARGET_ENV_FREESTANDING,
    XR_TARGET_ENV_COUNT,
} XrTargetEnvironment;

typedef enum XrTargetNativeAbi {
    XR_TARGET_ABI_NONE = 0,
    XR_TARGET_ABI_WIN64_X86_64,
    XR_TARGET_ABI_WIN64_AARCH64,
    XR_TARGET_ABI_SYSV_X86_64,
    XR_TARGET_ABI_AAPCS64,
    XR_TARGET_ABI_DARWIN_X86_64,
    XR_TARGET_ABI_DARWIN_AARCH64,
    XR_TARGET_ABI_PPC64_ELFV2,
    XR_TARGET_ABI_LOONGARCH_LP64D,
    XR_TARGET_ABI_WASM,
    XR_TARGET_ABI_COUNT,
} XrTargetNativeAbi;

typedef enum XrTargetRuntimeProfile {
    XR_TARGET_RUNTIME_NONE = 0,
    XR_TARGET_RUNTIME_HOSTED,
    XR_TARGET_RUNTIME_FREESTANDING,
} XrTargetRuntimeProfile;

typedef enum XrTargetProvider {
    XR_TARGET_PROVIDER_NONE = 0,
    XR_TARGET_PROVIDER_ALLOCATOR = 1,
    XR_TARGET_PROVIDER_PANIC = 2,
    XR_TARGET_PROVIDER_CLOCK = 3,
    XR_TARGET_PROVIDER_SCHEDULER = 4,
    XR_TARGET_PROVIDER_IO = 5,
    XR_TARGET_PROVIDER_TLS = 6,
    XR_TARGET_PROVIDER_FFI = 7,
    XR_TARGET_PROVIDER_COUNT,
} XrTargetProvider;

typedef enum XrTargetAtomicWidth {
    XR_TARGET_ATOMIC_WIDTH_8 = 1u << 0,
    XR_TARGET_ATOMIC_WIDTH_16 = 1u << 1,
    XR_TARGET_ATOMIC_WIDTH_32 = 1u << 2,
    XR_TARGET_ATOMIC_WIDTH_64 = 1u << 3,
    XR_TARGET_ATOMIC_WIDTH_128 = 1u << 4,
} XrTargetAtomicWidth;

typedef enum XrTargetAtomicOrder {
    XR_TARGET_ATOMIC_RELAXED = 1u << 0,
    XR_TARGET_ATOMIC_ACQUIRE = 1u << 1,
    XR_TARGET_ATOMIC_RELEASE = 1u << 2,
    XR_TARGET_ATOMIC_ACQ_REL = 1u << 3,
    XR_TARGET_ATOMIC_SEQ_CST = 1u << 4,
} XrTargetAtomicOrder;

typedef enum XrTargetFloatFeature {
    XR_TARGET_FLOAT_IEEE754 = 1u << 0,
    XR_TARGET_FLOAT_STRICT = 1u << 1,
    XR_TARGET_FLOAT_FAST = 1u << 2,
    XR_TARGET_FLOAT_FMA = 1u << 3,
} XrTargetFloatFeature;

typedef enum XrTargetVectorFeature {
    XR_TARGET_VECTOR_SSE2 = 1u << 0,
    XR_TARGET_VECTOR_AVX2 = 1u << 1,
    XR_TARGET_VECTOR_AVX512 = 1u << 2,
    XR_TARGET_VECTOR_NEON = 1u << 3,
    XR_TARGET_VECTOR_SVE = 1u << 4,
    XR_TARGET_VECTOR_VSX = 1u << 5,
    XR_TARGET_VECTOR_LSX = 1u << 6,
    XR_TARGET_VECTOR_WASM128 = 1u << 7,
} XrTargetVectorFeature;

typedef struct XrTargetProfileDraft {
    uint32_t schema_version;
    uint16_t architecture;
    uint16_t operating_system;
    uint16_t environment;
    uint16_t native_abi;
    uint8_t runtime_profile;
    uint8_t reserved8[3];
    XrTargetDataLayout data_layout;
    uint64_t atomic_width_mask;
    uint64_t atomic_order_mask;
    uint64_t float_feature_mask;
    uint64_t vector_feature_mask;
    uint16_t maximum_vector_bits;
    uint16_t reserved16;
    uint64_t provider_mask;
    XrFingerprint provider_set_fingerprint;
    XrFingerprint object_header_fingerprint;
    XrFingerprint runtime_abi_fingerprint;
} XrTargetProfileDraft;

XR_FUNC bool xr_target_profile_freeze(const XrTargetProfileDraft *draft,
                                      XrTargetProfile **out, char *error,
                                      size_t error_size);
XR_FUNC XrTargetProfile *xr_target_profile_retain(XrTargetProfile *profile);
XR_FUNC void xr_target_profile_free(XrTargetProfile *profile);
XR_FUNC bool xr_target_profile_is_frozen(const XrTargetProfile *profile);
XR_FUNC bool xr_target_profile_verify(const XrTargetProfile *profile, char *error,
                                      size_t error_size);
XR_FUNC XrFingerprint xr_target_profile_fingerprint(const XrTargetProfile *profile);
XR_FUNC const XrTargetProfileDraft *xr_target_profile_facts(const XrTargetProfile *profile);

#endif  // XR_TARGET_PROFILE_H
