/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_target_profile.c - Numeric toolchain TargetProfile authority
 */

#include "xtc_target_profile.h"

#include "../../runtime/abi/xr_runtime_target_authority.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static bool authority_error(char *error, size_t error_size,
                            const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1000: %s", detail);
    return false;
}

static bool map_architecture(XrToolchainTargetArch source, uint16_t *out) {
    switch (source) {
        case XR_TOOLCHAIN_TARGET_ARCH_X86_64:
            *out = XR_TARGET_ARCH_X86_64;
            return true;
        case XR_TOOLCHAIN_TARGET_ARCH_AARCH64:
            *out = XR_TARGET_ARCH_AARCH64;
            return true;
        case XR_TOOLCHAIN_TARGET_ARCH_POWERPC64:
            *out = XR_TARGET_ARCH_POWERPC64;
            return true;
        case XR_TOOLCHAIN_TARGET_ARCH_LOONGARCH64:
            *out = XR_TARGET_ARCH_LOONGARCH64;
            return true;
        default:
            return false;
    }
}

static bool map_operating_system(XrToolchainTargetOs source, uint16_t *out) {
    switch (source) {
        case XR_TOOLCHAIN_TARGET_OS_DARWIN:
            *out = XR_TARGET_OS_MACOS;
            return true;
        case XR_TOOLCHAIN_TARGET_OS_LINUX:
            *out = XR_TARGET_OS_LINUX;
            return true;
        case XR_TOOLCHAIN_TARGET_OS_WINDOWS:
            *out = XR_TARGET_OS_WINDOWS;
            return true;
        default:
            return false;
    }
}

static bool map_environment(XrToolchainTargetAbi source, uint16_t *out) {
    switch (source) {
        case XR_TOOLCHAIN_TARGET_ABI_DARWIN:
            *out = XR_TARGET_ENV_DARWIN;
            return true;
        case XR_TOOLCHAIN_TARGET_ABI_GNU:
            *out = XR_TARGET_ENV_GNU;
            return true;
        case XR_TOOLCHAIN_TARGET_ABI_MUSL:
            *out = XR_TARGET_ENV_MUSL;
            return true;
        case XR_TOOLCHAIN_TARGET_ABI_MSVC:
            *out = XR_TARGET_ENV_MSVC;
            return true;
        default:
            return false;
    }
}

static bool map_native_abi(const XrToolchainTarget *target, uint16_t *out) {
    switch (target->arch) {
        case XR_TOOLCHAIN_TARGET_ARCH_X86_64:
            if (target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS) {
                *out = XR_TARGET_ABI_WIN64_X86_64;
                return target->abi == XR_TOOLCHAIN_TARGET_ABI_MSVC;
            }
            if (target->os == XR_TOOLCHAIN_TARGET_OS_DARWIN) {
                *out = XR_TARGET_ABI_DARWIN_X86_64;
                return target->abi == XR_TOOLCHAIN_TARGET_ABI_DARWIN;
            }
            *out = XR_TARGET_ABI_SYSV_X86_64;
            return target->os == XR_TOOLCHAIN_TARGET_OS_LINUX &&
                   (target->abi == XR_TOOLCHAIN_TARGET_ABI_GNU ||
                    target->abi == XR_TOOLCHAIN_TARGET_ABI_MUSL);
        case XR_TOOLCHAIN_TARGET_ARCH_AARCH64:
            if (target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS) {
                *out = XR_TARGET_ABI_WIN64_AARCH64;
                return target->abi == XR_TOOLCHAIN_TARGET_ABI_MSVC;
            }
            if (target->os == XR_TOOLCHAIN_TARGET_OS_DARWIN) {
                *out = XR_TARGET_ABI_DARWIN_AARCH64;
                return target->abi == XR_TOOLCHAIN_TARGET_ABI_DARWIN;
            }
            *out = XR_TARGET_ABI_AAPCS64;
            return target->os == XR_TOOLCHAIN_TARGET_OS_LINUX &&
                   (target->abi == XR_TOOLCHAIN_TARGET_ABI_GNU ||
                    target->abi == XR_TOOLCHAIN_TARGET_ABI_MUSL);
        case XR_TOOLCHAIN_TARGET_ARCH_POWERPC64:
            *out = XR_TARGET_ABI_PPC64_ELFV2;
            return target->os == XR_TOOLCHAIN_TARGET_OS_LINUX &&
                   target->abi == XR_TOOLCHAIN_TARGET_ABI_MUSL;
        case XR_TOOLCHAIN_TARGET_ARCH_LOONGARCH64:
            *out = XR_TARGET_ABI_LOONGARCH_LP64D;
            return target->os == XR_TOOLCHAIN_TARGET_OS_LINUX &&
                   target->abi == XR_TOOLCHAIN_TARGET_ABI_MUSL;
        default:
            return false;
    }
}

static bool layout_equal(const XrTargetDataLayout *left,
                         const XrTargetDataLayout *right) {
    return left && right && xr_target_data_layout_validate(left) &&
           xr_target_data_layout_validate(right) &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static void fill_native_atomic_facts(XrTargetMachineFacts *machine) {
    _Atomic uint8_t atomic8;
    _Atomic uint16_t atomic16;
    _Atomic uint32_t atomic32;
    _Atomic uint64_t atomic64;
    atomic_init(&atomic8, 0);
    atomic_init(&atomic16, 0);
    atomic_init(&atomic32, 0);
    atomic_init(&atomic64, 0);
    if (atomic_is_lock_free(&atomic8))
        machine->atomic_width_mask |= XR_TARGET_ATOMIC_WIDTH_8;
    if (atomic_is_lock_free(&atomic16))
        machine->atomic_width_mask |= XR_TARGET_ATOMIC_WIDTH_16;
    if (atomic_is_lock_free(&atomic32))
        machine->atomic_width_mask |= XR_TARGET_ATOMIC_WIDTH_32;
    if (atomic_is_lock_free(&atomic64))
        machine->atomic_width_mask |= XR_TARGET_ATOMIC_WIDTH_64;
    machine->atomic_order_mask = XR_TARGET_ATOMIC_RELAXED |
                                 XR_TARGET_ATOMIC_ACQUIRE |
                                 XR_TARGET_ATOMIC_RELEASE |
                                 XR_TARGET_ATOMIC_ACQ_REL |
                                 XR_TARGET_ATOMIC_SEQ_CST;
}

bool xtc_target_profile_build_native_hosted(
    const XrToolchainTarget *target,
    const XrTargetCodegenFacts *codegen,
    XrTargetProfile **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!target || !codegen || !out)
        return authority_error(error, error_size,
                               "numeric target authority is missing");
    if (!target->is_native)
        return authority_error(
            error, error_size,
            "cross-target runtime ABI authority is unavailable");
    if (codegen->reserved16 != 0 || codegen->reserved32 != 0)
        return authority_error(error, error_size,
                               "codegen authority contains reserved data");

    XrTargetMachineFacts machine;
    memset(&machine, 0, sizeof(machine));
    if (!map_architecture(target->arch, &machine.architecture) ||
        !map_operating_system(target->os, &machine.operating_system) ||
        !map_environment(target->abi, &machine.environment) ||
        !map_native_abi(target, &machine.native_abi))
        return authority_error(error, error_size,
                               "numeric native target identity is unsupported");
    if (target->pointer_bits != (int) (sizeof(void *) * 8) ||
        (target->endian == XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE) !=
            (*(const uint8_t *) &(const uint16_t) {1} == 1))
        return authority_error(error, error_size,
                               "numeric target does not match the native process");
    const XrTargetDataLayout *native_layout = xr_target_data_layout_host();
    if (!native_layout)
        return authority_error(error, error_size,
                               "native data-layout authority is unavailable");
    machine.data_layout = *native_layout;
    machine.runtime_profile = XR_TARGET_RUNTIME_PROFILE_HOSTED;
    machine.float_feature_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT;
    machine.vector_feature_mask = codegen->vector_feature_mask;
    machine.maximum_vector_bits = codegen->maximum_vector_bits;
    fill_native_atomic_facts(&machine);

    XrRuntimeTargetAuthority runtime;
    XrRuntimeAbiStatus status =
        xr_runtime_target_authority_native_hosted(&runtime);
    if (status != XR_RUNTIME_ABI_OK)
        return authority_error(error, error_size,
                               "native runtime ABI authority is invalid");
    if (runtime.runtime_abi.pointer_width != machine.data_layout.pointer.size ||
        runtime.runtime_abi.target_endian !=
            (machine.data_layout.endian == XR_TARGET_ENDIAN_LITTLE
                 ? XR_RUNTIME_ENDIAN_LITTLE
                 : XR_RUNTIME_ENDIAN_BIG) ||
        !layout_equal(native_layout, &machine.data_layout))
        return authority_error(error, error_size,
                               "runtime and toolchain machine facts differ");
    XrTargetProfileBuildInput input = {
        .machine = machine,
        .runtime_abi = &runtime.runtime_abi,
        .object_header_materialization =
            &runtime.object_header_materialization,
        .string_contract = &runtime.string_contract,
        .providers = runtime.providers,
        .provider_count = runtime.provider_count,
    };
    return xr_target_profile_build(&input, out, error, error_size);
}

bool xtc_target_profile_build_current_native_hosted(
    const XrTargetCodegenFacts *codegen, XrTargetProfile **out, char *error,
    size_t error_size) {
    XrToolchainTarget target;
    if (!xtc_target_parse("native", &target, error, error_size)) {
        if (out)
            *out = NULL;
        return false;
    }
    return xtc_target_profile_build_native_hosted(
        &target, codegen, out, error, error_size);
}
