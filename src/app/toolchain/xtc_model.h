/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_model.h - Stable AOT toolchain provider/target domain model
 */

#ifndef XTC_MODEL_H
#define XTC_MODEL_H

#include "../../base/xdefs.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum XrToolchainProviderId {
    XR_TOOLCHAIN_PROVIDER_NONE = 0,
    XR_TOOLCHAIN_PROVIDER_APPLE_CLANG,
    XR_TOOLCHAIN_PROVIDER_GCC,
    XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
    XR_TOOLCHAIN_PROVIDER_MSVC,
    XR_TOOLCHAIN_PROVIDER_ZIG,
} XrToolchainProviderId;

typedef enum XrToolchainSelector {
    XR_TOOLCHAIN_SELECTOR_AUTO = 0,
    XR_TOOLCHAIN_SELECTOR_HOST,
    XR_TOOLCHAIN_SELECTOR_CLANG,
    XR_TOOLCHAIN_SELECTOR_GCC,
    XR_TOOLCHAIN_SELECTOR_MSVC,
    XR_TOOLCHAIN_SELECTOR_ZIG,
} XrToolchainSelector;

typedef enum XrToolchainReadiness {
    XR_TOOLCHAIN_NOT_FOUND = 0,
    XR_TOOLCHAIN_DISCOVERED,
    XR_TOOLCHAIN_RUNNABLE,
    XR_TOOLCHAIN_C_COMPILE_OK,
    XR_TOOLCHAIN_SDK_COMPILE_OK,
    XR_TOOLCHAIN_RUNTIME_LINK_OK,
    XR_TOOLCHAIN_NATIVE_RUN_OK,
    XR_TOOLCHAIN_READY,
} XrToolchainReadiness;

typedef enum XrToolchainReasonCode {
    XR_TOOLCHAIN_REASON_NONE = 0,
    XR_TOOLCHAIN_REASON_TOOLCHAIN_NOT_FOUND,
    XR_TOOLCHAIN_REASON_TOOLCHAIN_VERSION_UNSUPPORTED,
    XR_TOOLCHAIN_REASON_TOOLCHAIN_ENV_INCOMPLETE,
    XR_TOOLCHAIN_REASON_TARGET_UNSUPPORTED,
    XR_TOOLCHAIN_REASON_SDK_MISSING,
    XR_TOOLCHAIN_REASON_SDK_VERSION_MISMATCH,
    XR_TOOLCHAIN_REASON_RUNTIME_ARTIFACT_MISSING,
    XR_TOOLCHAIN_REASON_ABI_MISMATCH,
    XR_TOOLCHAIN_REASON_COMPILE_PROBE_FAILED,
    XR_TOOLCHAIN_REASON_LINK_PROBE_FAILED,
    XR_TOOLCHAIN_REASON_RUN_PROBE_FAILED,
    XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK,
    XR_TOOLCHAIN_REASON_MANAGED_ZIG_AVAILABLE,
    XR_TOOLCHAIN_REASON_EXTERNAL_INSTALL_REQUIRED,
    XR_TOOLCHAIN_REASON_CODEGEN_CAPABILITY_UNSUPPORTED,
} XrToolchainReasonCode;

typedef enum XrToolchainOwnership {
    XR_TOOLCHAIN_OWNERSHIP_EXTERNAL = 0,
    XR_TOOLCHAIN_OWNERSHIP_MANAGED,
} XrToolchainOwnership;

typedef enum XrToolchainTargetArch {
    XR_TOOLCHAIN_TARGET_ARCH_X86 = 0,
    XR_TOOLCHAIN_TARGET_ARCH_X86_64,
    XR_TOOLCHAIN_TARGET_ARCH_ARM,
    XR_TOOLCHAIN_TARGET_ARCH_AARCH64,
    XR_TOOLCHAIN_TARGET_ARCH_POWERPC64,
    XR_TOOLCHAIN_TARGET_ARCH_LOONGARCH64,
    XR_TOOLCHAIN_TARGET_ARCH_RISCV32,
    XR_TOOLCHAIN_TARGET_ARCH_RISCV64,
    XR_TOOLCHAIN_TARGET_ARCH_THUMB,
} XrToolchainTargetArch;

typedef enum XrToolchainTargetOs {
    XR_TOOLCHAIN_TARGET_OS_NONE = 0,
    XR_TOOLCHAIN_TARGET_OS_DARWIN,
    XR_TOOLCHAIN_TARGET_OS_LINUX,
    XR_TOOLCHAIN_TARGET_OS_WINDOWS,
} XrToolchainTargetOs;

typedef enum XrToolchainTargetAbi {
    XR_TOOLCHAIN_TARGET_ABI_NONE = 0,
    XR_TOOLCHAIN_TARGET_ABI_DARWIN,
    XR_TOOLCHAIN_TARGET_ABI_GNU,
    XR_TOOLCHAIN_TARGET_ABI_MUSL,
    XR_TOOLCHAIN_TARGET_ABI_MSVC,
    XR_TOOLCHAIN_TARGET_ABI_EABI,
} XrToolchainTargetAbi;

typedef enum XrToolchainTargetEndian {
    XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE = 0,
    XR_TOOLCHAIN_TARGET_ENDIAN_BIG,
} XrToolchainTargetEndian;

typedef struct XrToolchainTarget {
    const char *requested_name;
    const char *name;
    const char *zig_triple;
    const char *cpu;
    const char *exe_suffix;
    XrToolchainTargetArch arch;
    XrToolchainTargetOs os;
    XrToolchainTargetAbi abi;
    XrToolchainTargetEndian endian;
    int pointer_bits;
    bool is_native;
} XrToolchainTarget;

typedef struct XrToolchainSelection {
    XrToolchainProviderId provider;
    XrToolchainOwnership ownership;
    XrToolchainReadiness readiness;
    XrToolchainReasonCode reason;
    XrToolchainTarget target;
    const char *program;
    char program_storage[1200];
    char version[512];
    char compiler_fingerprint[80];
    char system_sdk[1200];
    char sdk_digest[72];
    char public_include[1200];
    char private_aot_include[1200];
    char runtime_ids[8][256];
    char runtime_paths[8][1200];
    size_t runtime_count;
    char system_libraries[16][64];
    size_t system_library_count;
    char runtime_artifact[256];
    char probe_fingerprint[80];
    bool fallback_used;
} XrToolchainSelection;

XR_FUNC bool xtc_target_parse(const char *text, XrToolchainTarget *out, char *err, size_t err_size);
XR_FUNC const char *const *xtc_target_supported_names(size_t *out_count);
XR_FUNC const char *xtc_target_default_output(const XrToolchainTarget *target);
XR_FUNC bool xtc_target_is_hosted(const XrToolchainTarget *target);

XR_FUNC bool xtc_selector_parse(const char *text, XrToolchainSelector *out, char *err,
                                size_t err_size);
XR_FUNC const char *xtc_selector_name(XrToolchainSelector selector);
XR_FUNC const char *xtc_provider_name(XrToolchainProviderId provider);
XR_FUNC const char *xtc_readiness_name(XrToolchainReadiness readiness);
XR_FUNC const char *xtc_reason_code_name(XrToolchainReasonCode reason);
XR_FUNC const char *xtc_ownership_name(XrToolchainOwnership ownership);
XR_FUNC bool xtc_provider_uses_gnu_driver(XrToolchainProviderId provider);

#endif /* XTC_MODEL_H */
