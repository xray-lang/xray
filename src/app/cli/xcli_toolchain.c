/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_toolchain.c - AOT target/toolchain command planning
 */

#include "xcli_toolchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

static const char *const xr_cli_tc_supported_targets[] = {
    "native",
    "x86_64-linux-musl",
    "aarch64-linux-musl",
    "x86_64-windows-gnu",
    "aarch64-windows-gnu",
};

static void xr_cli_tc_error(char *err, size_t err_size, const char *msg, const char *arg) {
    if (!err || err_size == 0)
        return;
    if (arg)
        snprintf(err, err_size, msg, arg);
    else
        snprintf(err, err_size, "%s", msg);
}

static void xr_cli_tc_set_target(XrCliBuildTarget *out, const char *name, const char *zig_triple,
                                 const char *exe_suffix, XrCliTargetArch arch, XrCliTargetOs os,
                                 XrCliTargetAbi abi, bool is_native) {
    out->name = name;
    out->zig_triple = zig_triple;
    out->exe_suffix = exe_suffix;
    out->arch = arch;
    out->os = os;
    out->abi = abi;
    out->is_native = is_native;
}

XR_FUNC bool xr_cli_build_target_parse(const char *text, XrCliBuildTarget *out, char *err,
                                       size_t err_size) {
    const char *name = text;

    if (!out) {
        xr_cli_tc_error(err, err_size, "missing output target", NULL);
        return false;
    }

    if (!name || name[0] == '\0' || strcmp(name, "native") == 0 ||
        strcmp(name, "native-c90") == 0) {
        xr_cli_tc_set_target(out, "native", NULL, "", XR_CLI_TARGET_ARCH_NATIVE,
                             XR_CLI_TARGET_OS_NATIVE, XR_CLI_TARGET_ABI_NATIVE, true);
        return true;
    }

    if (strcmp(name, "x86_64-linux-musl") == 0) {
        xr_cli_tc_set_target(out, "x86_64-linux-musl", "x86_64-linux-musl", "",
                             XR_CLI_TARGET_ARCH_X86_64, XR_CLI_TARGET_OS_LINUX,
                             XR_CLI_TARGET_ABI_MUSL, false);
        return true;
    }

    if (strcmp(name, "aarch64-linux-musl") == 0) {
        xr_cli_tc_set_target(out, "aarch64-linux-musl", "aarch64-linux-musl", "",
                             XR_CLI_TARGET_ARCH_AARCH64, XR_CLI_TARGET_OS_LINUX,
                             XR_CLI_TARGET_ABI_MUSL, false);
        return true;
    }

    if (strcmp(name, "x86_64-windows-gnu") == 0) {
        xr_cli_tc_set_target(out, "x86_64-windows-gnu", "x86_64-windows-gnu", ".exe",
                             XR_CLI_TARGET_ARCH_X86_64, XR_CLI_TARGET_OS_WINDOWS,
                             XR_CLI_TARGET_ABI_GNU, false);
        return true;
    }

    if (strcmp(name, "aarch64-windows-gnu") == 0) {
        xr_cli_tc_set_target(out, "aarch64-windows-gnu", "aarch64-windows-gnu", ".exe",
                             XR_CLI_TARGET_ARCH_AARCH64, XR_CLI_TARGET_OS_WINDOWS,
                             XR_CLI_TARGET_ABI_GNU, false);
        return true;
    }

    xr_cli_tc_error(err, err_size,
                    "unsupported AOT target '%s' (supported: native, x86_64-linux-musl, "
                    "aarch64-linux-musl, x86_64-windows-gnu, aarch64-windows-gnu)",
                    name);
    return false;
}

XR_FUNC const char *const *xr_cli_build_target_supported_names(size_t *out_count) {
    if (out_count)
        *out_count = sizeof(xr_cli_tc_supported_targets) / sizeof(xr_cli_tc_supported_targets[0]);
    return xr_cli_tc_supported_targets;
}

XR_FUNC const char *xr_cli_build_target_default_output(const XrCliBuildTarget *target) {
    if (target && target->os == XR_CLI_TARGET_OS_WINDOWS)
        return "a.exe";
    return "a.out";
}

XR_FUNC bool xr_cli_toolchain_kind_parse(const char *text, XrCliToolchainKind *out, char *err,
                                         size_t err_size) {
    const char *kind = text;

    if (!out) {
        xr_cli_tc_error(err, err_size, "missing output toolchain kind", NULL);
        return false;
    }

    if (!kind || kind[0] == '\0' || strcmp(kind, "auto") == 0) {
        *out = XR_CLI_TOOLCHAIN_AUTO;
        return true;
    }
    if (strcmp(kind, "host") == 0 || strcmp(kind, "cc") == 0) {
        *out = XR_CLI_TOOLCHAIN_HOST;
        return true;
    }
    if (strcmp(kind, "zig") == 0 || strcmp(kind, "zig-cc") == 0) {
        *out = XR_CLI_TOOLCHAIN_ZIG;
        return true;
    }
    if (strcmp(kind, "clang") == 0 || strcmp(kind, "clang-lld") == 0) {
        *out = XR_CLI_TOOLCHAIN_CLANG;
        return true;
    }

    xr_cli_tc_error(err, err_size,
                    "unsupported AOT toolchain '%s' (supported: auto, host, zig, clang)", kind);
    return false;
}

XR_FUNC const char *xr_cli_toolchain_kind_name(XrCliToolchainKind kind) {
    switch (kind) {
        case XR_CLI_TOOLCHAIN_AUTO:
            return "auto";
        case XR_CLI_TOOLCHAIN_HOST:
            return "host";
        case XR_CLI_TOOLCHAIN_ZIG:
            return "zig";
        case XR_CLI_TOOLCHAIN_CLANG:
            return "clang";
        default:
            return "unknown";
    }
}

XR_FUNC bool xr_cli_toolchain_resolve_ex(XrCliToolchainKind requested,
                                         const XrCliBuildTarget *target, const char *cc,
                                         const char *zig_path, const char *program_hint,
                                         XrCliToolchainPlan *out, char *err, size_t err_size) {
    XrCliToolchainKind resolved = requested;
    const char *program = NULL;

    if (!target || !out) {
        xr_cli_tc_error(err, err_size, "missing target or toolchain plan", NULL);
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (resolved == XR_CLI_TOOLCHAIN_AUTO)
        resolved = target->is_native ? XR_CLI_TOOLCHAIN_HOST : XR_CLI_TOOLCHAIN_ZIG;

    if (!target->is_native && resolved != XR_CLI_TOOLCHAIN_ZIG) {
        xr_cli_tc_error(err, err_size, "cross target '%s' currently requires --toolchain zig",
                        target->name);
        return false;
    }

    switch (resolved) {
        case XR_CLI_TOOLCHAIN_HOST:
            program = (cc && cc[0]) ? cc : "cc";
            break;
        case XR_CLI_TOOLCHAIN_ZIG:
            if (zig_path && zig_path[0]) {
                program = zig_path;
            } else {
                const char *env_zig = getenv("XRAY_ZIG");
                if (env_zig && env_zig[0]) {
                    program = env_zig;
                } else if (xr_cli_toolchain_find_bundled_zig(program_hint, out->program_storage,
                                                             sizeof(out->program_storage))) {
                    program = out->program_storage;
                } else {
                    program = "zig";
                }
            }
            break;
        case XR_CLI_TOOLCHAIN_CLANG:
            xr_cli_tc_error(err, err_size,
                            "clang/lld toolchain is reserved for a later phase; use zig or host",
                            NULL);
            return false;
        default:
            xr_cli_tc_error(err, err_size, "unsupported resolved toolchain", NULL);
            return false;
    }

    out->kind = resolved;
    out->program = program;
    return true;
}

XR_FUNC bool xr_cli_toolchain_resolve(XrCliToolchainKind requested, const XrCliBuildTarget *target,
                                      const char *cc, const char *zig_path, XrCliToolchainPlan *out,
                                      char *err, size_t err_size) {
    return xr_cli_toolchain_resolve_ex(requested, target, cc, zig_path, NULL, out, err, err_size);
}

static bool xr_cli_tc_is_path_like(const char *program) {
    return program && (strchr(program, '/') || strchr(program, '\\'));
}

static bool xr_cli_tc_is_executable(const char *path) {
    if (!path || !path[0])
        return false;
#ifdef XR_OS_WINDOWS
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

static bool xr_cli_tc_copy_path(const char *path, char *out, size_t out_size) {
    int written;

    if (!path || !out || out_size == 0)
        return false;
    written = snprintf(out, out_size, "%s", path);
    return written >= 0 && (size_t) written < out_size;
}

static const char *xr_cli_tc_zig_exe_name(void) {
#ifdef XR_OS_WINDOWS
    return "zig.exe";
#else
    return "zig";
#endif
}

static bool xr_cli_tc_path_dir(const char *path, size_t *out_dir_len) {
    const char *slash;
    const char *backslash;
    const char *sep;

    if (!path || !out_dir_len)
        return false;
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    sep = slash;
    if (backslash && (!sep || backslash > sep))
        sep = backslash;
    if (!sep)
        return false;
    *out_dir_len = (size_t) (sep - path);
    return *out_dir_len > 0;
}

static bool xr_cli_tc_bundled_candidate(const char *dir, size_t dir_len, const char *suffix,
                                        char *out, size_t out_size) {
    char candidate[1200];
    int written;

    written = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int) dir_len, dir, suffix);
    if (written < 0 || (size_t) written >= sizeof(candidate))
        return false;
    if (!xr_cli_tc_is_executable(candidate))
        return false;
    return xr_cli_tc_copy_path(candidate, out, out_size);
}

XR_FUNC bool xr_cli_toolchain_find_bundled_zig(const char *program_hint, char *out,
                                               size_t out_size) {
    char program_path[1200];
    char suffixes[6][96];
    size_t dir_len;
    int i;

    if (!program_hint || !program_hint[0] || !out || out_size == 0)
        return false;

    if (xr_cli_tc_is_path_like(program_hint)) {
        if (!xr_cli_tc_copy_path(program_hint, program_path, sizeof(program_path)))
            return false;
    } else if (!xr_cli_toolchain_find_executable(program_hint, program_path,
                                                 sizeof(program_path))) {
        return false;
    }
    if (!xr_cli_tc_path_dir(program_path, &dir_len))
        return false;

    snprintf(suffixes[0], sizeof(suffixes[0]), "%s", xr_cli_tc_zig_exe_name());
    snprintf(suffixes[1], sizeof(suffixes[1]), "tools/zig/%s", xr_cli_tc_zig_exe_name());
    snprintf(suffixes[2], sizeof(suffixes[2]), "tools/zig/bin/%s", xr_cli_tc_zig_exe_name());
    snprintf(suffixes[3], sizeof(suffixes[3]), "../lib/xray/zig/%s", xr_cli_tc_zig_exe_name());
    snprintf(suffixes[4], sizeof(suffixes[4]), "../libexec/xray/zig/%s", xr_cli_tc_zig_exe_name());
    suffixes[5][0] = '\0';

    for (i = 0; suffixes[i][0]; i++) {
        if (xr_cli_tc_bundled_candidate(program_path, dir_len, suffixes[i], out, out_size))
            return true;
    }
    return false;
}

static bool xr_cli_tc_candidate_is_executable(const char *dir, size_t dir_len, const char *program,
                                              char *out, size_t out_size) {
    char candidate[1200];
    int written;

    if (!dir || dir_len == 0 || !program || !program[0])
        return false;
    written = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int) dir_len, dir, program);
    if (written < 0 || (size_t) written >= sizeof(candidate))
        return false;
    if (!xr_cli_tc_is_executable(candidate))
        return false;
    return xr_cli_tc_copy_path(candidate, out, out_size);
}

XR_FUNC bool xr_cli_toolchain_build_standalone(const XrCliToolchainPlan *plan,
                                               const XrCliBuildTarget *target, const char *opt_flag,
                                               const char *output_file, const char *c_file,
                                               const char *aot_include, const char *runtime_include,
                                               const char *sysroot, bool strip_symbols,
                                               XrCliToolchainCommand *out, char *err,
                                               size_t err_size) {
    int ai;

    if (!plan || !target || !output_file || !c_file || !out) {
        xr_cli_tc_error(err, err_size, "missing toolchain command input", NULL);
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->program = plan->program;
    ai = 0;

    out->argv[ai++] = plan->program;
    if (plan->kind == XR_CLI_TOOLCHAIN_ZIG) {
        out->argv[ai++] = "cc";
        if (!target->is_native) {
            out->argv[ai++] = "-target";
            out->argv[ai++] = target->zig_triple;
        }
    }

    if (opt_flag)
        out->argv[ai++] = opt_flag;
    out->argv[ai++] = "-o";
    out->argv[ai++] = output_file;
    out->argv[ai++] = c_file;

    if (aot_include && aot_include[0]) {
        snprintf(out->aot_include_flag, sizeof(out->aot_include_flag), "-I%s", aot_include);
        out->argv[ai++] = out->aot_include_flag;
    }
    if (runtime_include && runtime_include[0]) {
        snprintf(out->runtime_include_flag, sizeof(out->runtime_include_flag), "-I%s",
                 runtime_include);
        out->argv[ai++] = out->runtime_include_flag;
    }
    if (sysroot && sysroot[0]) {
        if (plan->kind == XR_CLI_TOOLCHAIN_ZIG)
            snprintf(out->sysroot_flag, sizeof(out->sysroot_flag), "--sysroot=%s", sysroot);
        else
            snprintf(out->sysroot_flag, sizeof(out->sysroot_flag), "--sysroot=%s", sysroot);
        out->argv[ai++] = out->sysroot_flag;
    }

    if (!target->is_native) {
        snprintf(out->define_target_flag, sizeof(out->define_target_flag),
                 "-DXR_AOT_CROSS_TARGET=1");
        out->argv[ai++] = out->define_target_flag;
    }

    out->argv[ai++] = "-lm";
    if (target->is_native) {
#ifdef XR_OS_MACOS
        out->argv[ai++] = "-Wl,-dead_strip";
#else
        out->argv[ai++] = "-ffunction-sections";
        out->argv[ai++] = "-fdata-sections";
        out->argv[ai++] = "-Wl,--gc-sections";
#endif
    } else {
        out->argv[ai++] = "-ffunction-sections";
        out->argv[ai++] = "-fdata-sections";
        out->argv[ai++] = "-Wl,--gc-sections";
    }
    if (strip_symbols)
        out->argv[ai++] = "-Wl,-x";

    out->argv[ai] = NULL;
    (void) err;
    (void) err_size;
    return true;
}

XR_FUNC bool xr_cli_toolchain_find_executable(const char *program, char *out, size_t out_size) {
    const char *path_env;
    const char *cur;

    if (!program || !program[0] || !out || out_size == 0)
        return false;

    if (xr_cli_tc_is_path_like(program)) {
        if (!xr_cli_tc_is_executable(program))
            return false;
        return xr_cli_tc_copy_path(program, out, out_size);
    }

    path_env = getenv("PATH");
    if (!path_env || !path_env[0])
        return false;

    cur = path_env;
    while (*cur) {
        const char *sep = strchr(cur,
#ifdef XR_OS_WINDOWS
                                 ';'
#else
                                 ':'
#endif
        );
        size_t len = sep ? (size_t) (sep - cur) : strlen(cur);
        if (xr_cli_tc_candidate_is_executable(cur, len, program, out, out_size))
            return true;
#ifdef XR_OS_WINDOWS
        {
            char exe_name[300];
            int written = snprintf(exe_name, sizeof(exe_name), "%s.exe", program);
            if (written >= 0 && (size_t) written < sizeof(exe_name) &&
                xr_cli_tc_candidate_is_executable(cur, len, exe_name, out, out_size))
                return true;
        }
#endif
        if (!sep)
            break;
        cur = sep + 1;
    }

    return false;
}

XR_FUNC void xr_cli_toolchain_print_command(const XrCliToolchainCommand *cmd) {
    int i;

    if (!cmd || !cmd->argv[0])
        return;
    printf("Link command:");
    for (i = 0; cmd->argv[i]; i++)
        printf(" %s", cmd->argv[i]);
    printf("\n");
}
