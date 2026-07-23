/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_model.c - Stable AOT toolchain provider/target domain model
 */

#include "xtc_model.h"

#include <stdio.h>
#include <string.h>

typedef struct XtcTargetRecord {
    const char *name;
    const char *zig_triple;
    const char *cpu;
    const char *exe_suffix;
    XrToolchainTargetArch arch;
    XrToolchainTargetOs os;
    XrToolchainTargetAbi abi;
    XrToolchainTargetEndian endian;
    int pointer_bits;
} XtcTargetRecord;

static const XtcTargetRecord xtc_targets[] = {
    {"aarch64-apple-darwin", "aarch64-macos", NULL, "", XR_TOOLCHAIN_TARGET_ARCH_AARCH64,
     XR_TOOLCHAIN_TARGET_OS_DARWIN, XR_TOOLCHAIN_TARGET_ABI_DARWIN,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 64},
    {"x86_64-apple-darwin", "x86_64-macos", NULL, "", XR_TOOLCHAIN_TARGET_ARCH_X86_64,
     XR_TOOLCHAIN_TARGET_OS_DARWIN, XR_TOOLCHAIN_TARGET_ABI_DARWIN,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 64},
    {"x86_64-linux-gnu", "x86_64-linux-gnu", NULL, "", XR_TOOLCHAIN_TARGET_ARCH_X86_64,
     XR_TOOLCHAIN_TARGET_OS_LINUX, XR_TOOLCHAIN_TARGET_ABI_GNU, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE,
     64},
    {"aarch64-linux-gnu", "aarch64-linux-gnu", NULL, "", XR_TOOLCHAIN_TARGET_ARCH_AARCH64,
     XR_TOOLCHAIN_TARGET_OS_LINUX, XR_TOOLCHAIN_TARGET_ABI_GNU, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE,
     64},
    {"x86_64-linux-musl", "x86_64-linux-musl", NULL, "", XR_TOOLCHAIN_TARGET_ARCH_X86_64,
     XR_TOOLCHAIN_TARGET_OS_LINUX, XR_TOOLCHAIN_TARGET_ABI_MUSL, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE,
     64},
    {"aarch64-linux-musl", "aarch64-linux-musl", NULL, "", XR_TOOLCHAIN_TARGET_ARCH_AARCH64,
     XR_TOOLCHAIN_TARGET_OS_LINUX, XR_TOOLCHAIN_TARGET_ABI_MUSL, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE,
     64},
    {"x86_64-windows-msvc", "x86_64-windows-msvc", NULL, ".exe", XR_TOOLCHAIN_TARGET_ARCH_X86_64,
     XR_TOOLCHAIN_TARGET_OS_WINDOWS, XR_TOOLCHAIN_TARGET_ABI_MSVC,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 64},
    {"aarch64-windows-msvc", "aarch64-windows-msvc", NULL, ".exe", XR_TOOLCHAIN_TARGET_ARCH_AARCH64,
     XR_TOOLCHAIN_TARGET_OS_WINDOWS, XR_TOOLCHAIN_TARGET_ABI_MSVC,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 64},
    {"x86_64-windows-gnu", "x86_64-windows-gnu", NULL, ".exe", XR_TOOLCHAIN_TARGET_ARCH_X86_64,
     XR_TOOLCHAIN_TARGET_OS_WINDOWS, XR_TOOLCHAIN_TARGET_ABI_GNU, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE,
     64},
    {"aarch64-windows-gnu", "aarch64-windows-gnu", NULL, ".exe", XR_TOOLCHAIN_TARGET_ARCH_AARCH64,
     XR_TOOLCHAIN_TARGET_OS_WINDOWS, XR_TOOLCHAIN_TARGET_ABI_GNU, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE,
     64},
    {"x86_64-freestanding-none", "x86_64-freestanding-none", NULL, "",
     XR_TOOLCHAIN_TARGET_ARCH_X86_64, XR_TOOLCHAIN_TARGET_OS_NONE, XR_TOOLCHAIN_TARGET_ABI_NONE,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 64},
    {"riscv32-freestanding-none", "riscv32-freestanding-none", NULL, "",
     XR_TOOLCHAIN_TARGET_ARCH_RISCV32, XR_TOOLCHAIN_TARGET_OS_NONE, XR_TOOLCHAIN_TARGET_ABI_NONE,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 32},
    {"thumb-freestanding-eabi", "thumb-freestanding-eabi", "cortex_m4", "",
     XR_TOOLCHAIN_TARGET_ARCH_THUMB, XR_TOOLCHAIN_TARGET_OS_NONE, XR_TOOLCHAIN_TARGET_ABI_EABI,
     XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE, 32},
};

static const char *const xtc_target_names[] = {
    "native",
    "aarch64-apple-darwin",
    "x86_64-apple-darwin",
    "x86_64-linux-gnu",
    "aarch64-linux-gnu",
    "x86_64-linux-musl",
    "aarch64-linux-musl",
    "x86_64-windows-msvc",
    "x86_64-windows-gnu",
    "aarch64-windows-gnu",
    "x86_64-freestanding-none",
    "riscv32-freestanding-none",
    "thumb-freestanding-eabi",
};

static void xtc_error(char *err, size_t err_size, const char *format, const char *arg) {
    if (!err || err_size == 0)
        return;
    if (arg)
        snprintf(err, err_size, format, arg);
    else
        snprintf(err, err_size, "%s", format);
}

static const char *xtc_native_target_name(void) {
#if defined(XR_OS_MACOS) && defined(__aarch64__)
    return "aarch64-apple-darwin";
#elif defined(XR_OS_MACOS) && defined(__x86_64__)
    return "x86_64-apple-darwin";
#elif defined(XR_OS_LINUX) && defined(__aarch64__)
    return "aarch64-linux-gnu";
#elif defined(XR_OS_LINUX) && defined(__x86_64__)
    return "x86_64-linux-gnu";
#elif defined(XR_OS_WINDOWS) && defined(_M_ARM64)
    return "aarch64-windows-msvc";
#elif defined(XR_OS_WINDOWS) && defined(_M_X64)
    return "x86_64-windows-msvc";
#else
    return NULL;
#endif
}

static const XtcTargetRecord *xtc_find_target(const char *name) {
    size_t count = sizeof(xtc_targets) / sizeof(xtc_targets[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, xtc_targets[i].name) == 0)
            return &xtc_targets[i];
    }
    return NULL;
}

XR_FUNC bool xtc_target_parse(const char *text, XrToolchainTarget *out, char *err,
                              size_t err_size) {
    const char *requested = (text && text[0]) ? text : "native";
    const char *normalized = requested;
    const char *host = xtc_native_target_name();

    if (!out) {
        xtc_error(err, err_size, "missing output target", NULL);
        return false;
    }
    if (strcmp(requested, "native") == 0) {
        if (!host) {
            xtc_error(err, err_size, "native target is unsupported on this host", NULL);
            return false;
        }
        normalized = host;
    }

    const XtcTargetRecord *record = xtc_find_target(normalized);
    if (!record) {
        xtc_error(err, err_size, "unsupported AOT target '%s'", requested);
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->requested_name = strcmp(requested, "native") == 0 ? "native" : record->name;
    out->name = record->name;
    out->zig_triple = record->zig_triple;
    out->cpu = record->cpu;
    out->exe_suffix = record->exe_suffix;
    out->arch = record->arch;
    out->os = record->os;
    out->abi = record->abi;
    out->endian = record->endian;
    out->pointer_bits = record->pointer_bits;
    out->is_native = host && strcmp(host, record->name) == 0;
    return true;
}

XR_FUNC const char *const *xtc_target_supported_names(size_t *out_count) {
    if (out_count)
        *out_count = sizeof(xtc_target_names) / sizeof(xtc_target_names[0]);
    return xtc_target_names;
}

XR_FUNC const char *xtc_target_default_output(const XrToolchainTarget *target) {
    return target && target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS ? "a.exe" : "a.out";
}

XR_FUNC bool xtc_target_is_hosted(const XrToolchainTarget *target) {
    return target && target->os != XR_TOOLCHAIN_TARGET_OS_NONE;
}

XR_FUNC bool xtc_selector_parse(const char *text, XrToolchainSelector *out, char *err,
                                size_t err_size) {
    const char *selector = (text && text[0]) ? text : "auto";
    if (!out) {
        xtc_error(err, err_size, "missing output toolchain selector", NULL);
        return false;
    }
    if (strcmp(selector, "auto") == 0)
        *out = XR_TOOLCHAIN_SELECTOR_AUTO;
    else if (strcmp(selector, "host") == 0)
        *out = XR_TOOLCHAIN_SELECTOR_HOST;
    else if (strcmp(selector, "clang") == 0)
        *out = XR_TOOLCHAIN_SELECTOR_CLANG;
    else if (strcmp(selector, "gcc") == 0)
        *out = XR_TOOLCHAIN_SELECTOR_GCC;
    else if (strcmp(selector, "msvc") == 0)
        *out = XR_TOOLCHAIN_SELECTOR_MSVC;
    else if (strcmp(selector, "zig") == 0)
        *out = XR_TOOLCHAIN_SELECTOR_ZIG;
    else {
        xtc_error(err, err_size,
                  "unsupported AOT provider '%s' (expected auto, host, clang, gcc, msvc, or zig)",
                  selector);
        return false;
    }
    return true;
}

XR_FUNC const char *xtc_selector_name(XrToolchainSelector selector) {
    switch (selector) {
        case XR_TOOLCHAIN_SELECTOR_AUTO:
            return "auto";
        case XR_TOOLCHAIN_SELECTOR_HOST:
            return "host";
        case XR_TOOLCHAIN_SELECTOR_CLANG:
            return "clang";
        case XR_TOOLCHAIN_SELECTOR_GCC:
            return "gcc";
        case XR_TOOLCHAIN_SELECTOR_MSVC:
            return "msvc";
        case XR_TOOLCHAIN_SELECTOR_ZIG:
            return "zig";
    }
    return "unknown";
}

XR_FUNC const char *xtc_provider_name(XrToolchainProviderId provider) {
    switch (provider) {
        case XR_TOOLCHAIN_PROVIDER_APPLE_CLANG:
            return "apple-clang";
        case XR_TOOLCHAIN_PROVIDER_GCC:
            return "gcc";
        case XR_TOOLCHAIN_PROVIDER_LLVM_CLANG:
            return "llvm-clang";
        case XR_TOOLCHAIN_PROVIDER_MSVC:
            return "msvc";
        case XR_TOOLCHAIN_PROVIDER_ZIG:
            return "zig";
        case XR_TOOLCHAIN_PROVIDER_NONE:
            return "none";
    }
    return "none";
}

XR_FUNC const char *xtc_readiness_name(XrToolchainReadiness readiness) {
    switch (readiness) {
        case XR_TOOLCHAIN_NOT_FOUND:
            return "not-found";
        case XR_TOOLCHAIN_DISCOVERED:
            return "discovered";
        case XR_TOOLCHAIN_RUNNABLE:
            return "runnable";
        case XR_TOOLCHAIN_C_COMPILE_OK:
            return "c-compile-ok";
        case XR_TOOLCHAIN_SDK_COMPILE_OK:
            return "sdk-compile-ok";
        case XR_TOOLCHAIN_RUNTIME_LINK_OK:
            return "runtime-link-ok";
        case XR_TOOLCHAIN_NATIVE_RUN_OK:
            return "native-run-ok";
        case XR_TOOLCHAIN_READY:
            return "ready";
    }
    return "not-found";
}

XR_FUNC const char *xtc_reason_code_name(XrToolchainReasonCode reason) {
    static const char *const names[] = {
        "",
        "TOOLCHAIN_NOT_FOUND",
        "TOOLCHAIN_VERSION_UNSUPPORTED",
        "TOOLCHAIN_ENV_INCOMPLETE",
        "TARGET_UNSUPPORTED",
        "SDK_MISSING",
        "SDK_VERSION_MISMATCH",
        "RUNTIME_ARTIFACT_MISSING",
        "ABI_MISMATCH",
        "COMPILE_PROBE_FAILED",
        "LINK_PROBE_FAILED",
        "RUN_PROBE_FAILED",
        "PROVIDER_EXPLICIT_NO_FALLBACK",
        "MANAGED_ZIG_AVAILABLE",
        "EXTERNAL_INSTALL_REQUIRED",
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    return (size_t) reason < count ? names[reason] : "TOOLCHAIN_ENV_INCOMPLETE";
}

XR_FUNC const char *xtc_ownership_name(XrToolchainOwnership ownership) {
    return ownership == XR_TOOLCHAIN_OWNERSHIP_MANAGED ? "managed" : "external";
}

XR_FUNC bool xtc_provider_uses_gnu_driver(XrToolchainProviderId provider) {
    return provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG || provider == XR_TOOLCHAIN_PROVIDER_GCC ||
           provider == XR_TOOLCHAIN_PROVIDER_LLVM_CLANG || provider == XR_TOOLCHAIN_PROVIDER_ZIG;
}
