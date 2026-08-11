/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_machine_facts.h - Canonical target machine fact namespace
 *
 * KEY CONCEPT:
 *   Machine facts use one numeric namespace across runtime authority,
 *   TargetProfile, artifacts, and execution. Runtime-owned native facts can
 *   therefore be compared without target strings or compiler identities.
 */

#ifndef XR_TARGET_MACHINE_FACTS_H
#define XR_TARGET_MACHINE_FACTS_H

#include "../../base/xtarget_data_layout.h"
#include "xr_target_runtime_profile.h"
#include <stdint.h>

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

typedef struct XrTargetMachineFacts {
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
} XrTargetMachineFacts;

#endif  // XR_TARGET_MACHINE_FACTS_H
