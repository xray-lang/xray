/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_toolchain.h - AOT target/toolchain command planning
 */

#ifndef XCLI_TOOLCHAIN_H
#define XCLI_TOOLCHAIN_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum XrCliTargetArch {
    XR_CLI_TARGET_ARCH_NATIVE,
    XR_CLI_TARGET_ARCH_X86_64,
    XR_CLI_TARGET_ARCH_AARCH64
} XrCliTargetArch;

typedef enum XrCliTargetOs {
    XR_CLI_TARGET_OS_NATIVE,
    XR_CLI_TARGET_OS_LINUX,
    XR_CLI_TARGET_OS_WINDOWS
} XrCliTargetOs;

typedef enum XrCliTargetAbi {
    XR_CLI_TARGET_ABI_NATIVE,
    XR_CLI_TARGET_ABI_MUSL,
    XR_CLI_TARGET_ABI_GNU
} XrCliTargetAbi;

typedef struct XrCliBuildTarget {
    const char *name;
    const char *zig_triple;
    const char *exe_suffix;
    XrCliTargetArch arch;
    XrCliTargetOs os;
    XrCliTargetAbi abi;
    bool is_native;
} XrCliBuildTarget;

typedef enum XrCliToolchainKind {
    XR_CLI_TOOLCHAIN_AUTO,
    XR_CLI_TOOLCHAIN_HOST,
    XR_CLI_TOOLCHAIN_ZIG,
    XR_CLI_TOOLCHAIN_CLANG
} XrCliToolchainKind;

typedef struct XrCliToolchainCommand {
    const char *program;
    const char *argv[40];
    char aot_include_flag[600];
    char runtime_include_flag[600];
    char sysroot_flag[600];
    char define_target_flag[96];
} XrCliToolchainCommand;

typedef struct XrCliToolchainPlan {
    XrCliToolchainKind kind;
    const char *program;
    char program_storage[1200];
} XrCliToolchainPlan;

XR_FUNC bool xr_cli_build_target_parse(const char *text, XrCliBuildTarget *out, char *err,
                                       size_t err_size);
XR_FUNC const char *const *xr_cli_build_target_supported_names(size_t *out_count);
XR_FUNC const char *xr_cli_build_target_default_output(const XrCliBuildTarget *target);

XR_FUNC bool xr_cli_toolchain_kind_parse(const char *text, XrCliToolchainKind *out, char *err,
                                         size_t err_size);
XR_FUNC const char *xr_cli_toolchain_kind_name(XrCliToolchainKind kind);
XR_FUNC bool xr_cli_toolchain_resolve(XrCliToolchainKind requested, const XrCliBuildTarget *target,
                                      const char *cc, const char *zig_path, XrCliToolchainPlan *out,
                                      char *err, size_t err_size);
XR_FUNC bool xr_cli_toolchain_resolve_ex(XrCliToolchainKind requested,
                                         const XrCliBuildTarget *target, const char *cc,
                                         const char *zig_path, const char *program_hint,
                                         XrCliToolchainPlan *out, char *err, size_t err_size);
XR_FUNC bool xr_cli_toolchain_find_executable(const char *program, char *out, size_t out_size);
XR_FUNC bool xr_cli_toolchain_find_bundled_zig(const char *program_hint, char *out,
                                               size_t out_size);

XR_FUNC bool xr_cli_toolchain_build_standalone(const XrCliToolchainPlan *plan,
                                               const XrCliBuildTarget *target, const char *opt_flag,
                                               const char *output_file, const char *c_file,
                                               const char *aot_include, const char *runtime_include,
                                               const char *sysroot, bool strip_symbols,
                                               XrCliToolchainCommand *out, char *err,
                                               size_t err_size);
XR_FUNC void xr_cli_toolchain_print_command(const XrCliToolchainCommand *cmd);

#endif /* XCLI_TOOLCHAIN_H */
