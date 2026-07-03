/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build.c - 'xray build' command implementation
 *
 * KEY CONCEPT:
 *   Two build modes:
 *   1. Default: bytecode embedding (full xray features, needs runtime)
 *   2. --native: AOT + bytecode hybrid, links xray runtime (like Go)
 *
 * WHY THIS DESIGN:
 *   - Bytecode mode supports all xray features (coroutines, dynamic types)
 *   - Native mode combines AOT performance with full runtime support
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xcli_toolchain.h"
#include "../../api/xisolate_profile.h"
#include "xray.h"
#include "xray_vm.h"
#include "../../module/xbundle.h"
#include "../../aot/xaot_driver.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../os/os_dir.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"
#ifdef XR_OS_WINDOWS
#include <io.h>
#define XR_CLI_CHMOD _chmod
#else
#include <sys/stat.h>
#define XR_CLI_CHMOD chmod
#endif
#ifdef XR_OS_MACOS
#include <ftw.h>
#endif

typedef enum XrCliBuildProfile {
    XR_CLI_BUILD_PROFILE_HOSTED = 0,
    XR_CLI_BUILD_PROFILE_FREESTANDING,
} XrCliBuildProfile;

static const char *build_profile_name(XrCliBuildProfile profile) {
    switch (profile) {
        case XR_CLI_BUILD_PROFILE_HOSTED:
            return "hosted";
        case XR_CLI_BUILD_PROFILE_FREESTANDING:
            return "freestanding";
        default:
            return "unknown";
    }
}

static bool build_profile_parse(const char *name, XrCliBuildProfile *out, char *err,
                                size_t err_size) {
    if (!out) {
        if (err && err_size > 0)
            snprintf(err, err_size, "internal error: missing build profile output");
        return false;
    }
    if (!name || name[0] == '\0' || strcmp(name, "hosted") == 0) {
        *out = XR_CLI_BUILD_PROFILE_HOSTED;
        return true;
    }
    if (strcmp(name, "freestanding") == 0) {
        *out = XR_CLI_BUILD_PROFILE_FREESTANDING;
        return true;
    }
    if (err && err_size > 0) {
        snprintf(err, err_size, "unknown build profile '%s' (expected 'hosted' or 'freestanding')",
                 name);
    }
    return false;
}

/* ========== Shared Helpers ========== */

// Invoke C compiler to link generated C source (+ optional .o) into executable
static int invoke_cc(const char *cc, const char *opt_flag, const char *output_file,
                     const char *c_file, const char *obj_file, bool strip_symbols,
                     bool debug_symbols, const char *sysroot) {
    const char *xray_include = getenv("XRAY_INCLUDE");
    const char *xray_lib = getenv("XRAY_LIB");

    char include_path[512], lib_path[512];
    if (sysroot) {
        snprintf(include_path, sizeof(include_path), "%s/include/xray", sysroot);
        snprintf(lib_path, sizeof(lib_path), "%s/lib", sysroot);
        xray_include = include_path;
        xray_lib = lib_path;
    } else {
        if (!xray_include) {
#ifdef XRT_SOURCE_INCLUDE_DIR
            xray_include = XRT_SOURCE_INCLUDE_DIR;
#else
            xray_include = "/usr/local/include/xray";
#endif
        }
        if (!xray_lib) {
#ifdef XRT_BUILD_LIB_DIR
            xray_lib = XRT_BUILD_LIB_DIR;
#else
            xray_lib = "/usr/local/lib";
#endif
        }
    }

    char include_flag[600], lib_flag[600];
    snprintf(include_flag, sizeof(include_flag), "-I%s", xray_include);
    snprintf(lib_flag, sizeof(lib_flag), "-L%s", xray_lib);

    const char *spawn_argv[40];
    int ai = 0;
    spawn_argv[ai++] = cc;
    spawn_argv[ai++] = opt_flag;
    /* Match the interpreter's per-op rounding: never fuse a*b+c into a single
     * FMA rounding, or AOT output would diverge from the VM for the same .xr. */
    spawn_argv[ai++] = "-ffp-contract=off";
    if (debug_symbols) {
        spawn_argv[ai++] = "-g";
        spawn_argv[ai++] = "-fno-omit-frame-pointer";
    }
    /* Propagate sanitizer flags to this combined compile+link invocation when the
     * linked libxray_core was built with sanitizers. Without this, a sanitizer
     * build links the instrumented runtime archive but omits the sanitizer
     * runtime, leaving the sanitizer interceptor symbols undefined at link time.
     * The native (-N) path already does this via
     * xaot_cli_add_build_sanitizer_flags; this is the matching fix for the
     * default bytecode-embedding path. */
#if defined(XR_BUILD_ASAN) && XR_BUILD_ASAN
    spawn_argv[ai++] = "-fsanitize=address";
    spawn_argv[ai++] = "-fno-omit-frame-pointer";
#endif
#if defined(XR_BUILD_UBSAN) && XR_BUILD_UBSAN
    spawn_argv[ai++] = "-fsanitize=undefined";
    spawn_argv[ai++] = "-fno-sanitize=function";
#endif
#if defined(XR_BUILD_TSAN) && XR_BUILD_TSAN
    spawn_argv[ai++] = "-fsanitize=thread";
    spawn_argv[ai++] = "-fno-omit-frame-pointer";
#endif
#if defined(XR_BUILD_MSAN) && XR_BUILD_MSAN
    spawn_argv[ai++] = "-fsanitize=memory";
    spawn_argv[ai++] = "-fno-omit-frame-pointer";
#endif
    spawn_argv[ai++] = "-o";
    spawn_argv[ai++] = output_file;
    spawn_argv[ai++] = c_file;
    if (obj_file)
        spawn_argv[ai++] = obj_file;
    spawn_argv[ai++] = include_flag;
#ifdef XRT_AOT_INCLUDE_DIR
    spawn_argv[ai++] = "-I" XRT_AOT_INCLUDE_DIR;
#endif
    spawn_argv[ai++] = lib_flag;
    spawn_argv[ai++] = "-lxray_core";
#ifdef XRAY_HAVE_LIBFFI
    /* xray_core embeds the VM's libffi-based @extern invoker (xvm_ffi.c), so a
     * program that links the runtime must resolve libffi too. Must follow
     * -lxray_core: the archive pulls in ffi_* only on demand. */
    spawn_argv[ai++] = "-lffi";
#endif
#ifdef XR_HAS_IO_URING
    /* xray_core embeds the per-worker io_uring completion rings (netpoll_iouring),
     * so the AOT-linked program must resolve liburing too. Must follow
     * -lxray_core: the archive pulls in io_uring_* only on demand. */
    spawn_argv[ai++] = "-luring";
#endif
#ifdef XR_OS_MACOS
    /* Override for Intel Homebrew (/usr/local) or custom openssl prefixes;
     * default stays Apple-Silicon Homebrew. ssl_flag outlives the spawn. */
    const char *ssl_libdir = getenv("XRAY_OPENSSL_LIBDIR");
    char ssl_flag[512];
    snprintf(ssl_flag, sizeof(ssl_flag), "-L%s",
             ssl_libdir && ssl_libdir[0] ? ssl_libdir : "/opt/homebrew/opt/openssl@3/lib");
    spawn_argv[ai++] = ssl_flag;
#endif
#ifdef XR_ENABLE_TLS
    spawn_argv[ai++] = "-lssl";
    spawn_argv[ai++] = "-lcrypto";
#endif
    spawn_argv[ai++] = "-lz";
    spawn_argv[ai++] = "-lpthread";
    spawn_argv[ai++] = "-lm";
#ifdef XR_OS_MACOS
    spawn_argv[ai++] = "-Wl,-dead_strip";
#else
    spawn_argv[ai++] = "-ffunction-sections";
    spawn_argv[ai++] = "-fdata-sections";
    spawn_argv[ai++] = "-Wl,--gc-sections";
#endif
    if (strip_symbols) {
        spawn_argv[ai++] = "-Wl,-S";
        spawn_argv[ai++] = "-Wl,-x";
    }
    spawn_argv[ai] = NULL;

    printf("Linking:");
    for (int i = 0; spawn_argv[i]; i++)
        printf(" %s", spawn_argv[i]);
    printf("\n");

    XrProcId pid = xr_proc_spawn(cc, spawn_argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start compiler '%s'\n", cc);
        return 1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: linking failed\n");
        fprintf(stderr, "Tip: set XRAY_INCLUDE and XRAY_LIB environment variables\n");
        fprintf(stderr, "  export XRAY_INCLUDE=/path/to/xray/include\n");
        fprintf(stderr, "  export XRAY_LIB=/path/to/xray/build\n");
        return 1;
    }
    return 0;
}

static void resolve_aot_include_paths(const char *sysroot, char *aot_include, size_t aot_include_sz,
                                      char *runtime_include, size_t runtime_include_sz) {
    if (aot_include && aot_include_sz > 0)
        aot_include[0] = '\0';
    if (runtime_include && runtime_include_sz > 0)
        runtime_include[0] = '\0';

#ifdef XRT_AOT_INCLUDE_DIR
    (void) sysroot;
    if (aot_include && aot_include_sz > 0)
        snprintf(aot_include, aot_include_sz, "%s", XRT_AOT_INCLUDE_DIR);
#ifdef XRT_SOURCE_INCLUDE_DIR
    if (runtime_include && runtime_include_sz > 0)
        snprintf(runtime_include, runtime_include_sz, "%s", XRT_SOURCE_INCLUDE_DIR);
#endif
#else
    const char *xray_include = getenv("XRAY_INCLUDE");
    if (sysroot) {
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s/include/xray", sysroot);
    } else if (xray_include) {
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s", xray_include);
    } else if (aot_include && aot_include_sz > 0) {
        snprintf(aot_include, aot_include_sz, "%s", "/usr/local/include/xray");
    }
#endif
}

#define XAOT_CLI_LINK_MAX_ARGS 160
#define XAOT_CLI_LINK_MAX_OWNED 96

typedef struct XaotCliLinkCommand {
    const char *program;
    const char *argv[XAOT_CLI_LINK_MAX_ARGS];
    char owned[XAOT_CLI_LINK_MAX_OWNED][600];
    int argc;
    int nowned;
} XaotCliLinkCommand;

static bool xaot_cli_link_add_arg(XaotCliLinkCommand *cmd, const char *arg, char *err,
                                  size_t err_size) {
    if (!arg || !arg[0])
        return true;
    if (cmd->argc >= XAOT_CLI_LINK_MAX_ARGS - 1) {
        snprintf(err, err_size, "AOT link command has too many arguments");
        return false;
    }
    cmd->argv[cmd->argc++] = arg;
    cmd->argv[cmd->argc] = NULL;
    return true;
}

static bool xaot_cli_link_add_prefixed(XaotCliLinkCommand *cmd, const char *prefix,
                                       const char *value, char *err, size_t err_size) {
    int written;

    if (!value || !value[0])
        return true;
    if (cmd->nowned >= XAOT_CLI_LINK_MAX_OWNED) {
        snprintf(err, err_size, "AOT link command has too many generated arguments");
        return false;
    }
    written = snprintf(cmd->owned[cmd->nowned], sizeof(cmd->owned[cmd->nowned]), "%s%s",
                       prefix ? prefix : "", value);
    if (written < 0 || (size_t) written >= sizeof(cmd->owned[cmd->nowned])) {
        snprintf(err, err_size, "AOT link argument is too long: %s%s", prefix ? prefix : "", value);
        return false;
    }
    return xaot_cli_link_add_arg(cmd, cmd->owned[cmd->nowned++], err, err_size);
}

static bool xaot_cli_link_add_runtime_object(XaotCliLinkCommand *cmd, const char *value, char *err,
                                             size_t err_size) {
    size_t len;

    if (!value || !value[0])
        return true;
    len = strlen(value);
    if (strchr(value, '/') || (len > 2 && strcmp(value + len - 2, ".o") == 0) ||
        (len > 2 && strcmp(value + len - 2, ".a") == 0))
        return xaot_cli_link_add_arg(cmd, value, err, err_size);
    return xaot_cli_link_add_prefixed(cmd, "-l", value, err, err_size);
}

static bool xaot_cli_stdlib_object_needs_aot_core(const char *value) {
    return value &&
           (strncmp(value, "crypto.", 7) == 0 || strncmp(value, "regex.", 6) == 0 ||
            strcmp(value, "compress.deflate") == 0 || strcmp(value, "compress.gunzip") == 0 ||
            strcmp(value, "compress.gzip") == 0 || strcmp(value, "compress.inflate") == 0 ||
            strcmp(value, "compress.isGzip") == 0 || strcmp(value, "compress.isZlib") == 0 ||
            strcmp(value, "compress.zlibCompress") == 0 ||
            strcmp(value, "compress.zlibDecompress") == 0 || strcmp(value, "math.random") == 0 ||
            strcmp(value, "math.randomInt") == 0 || strcmp(value, "time.now") == 0 ||
            strcmp(value, "time.monotonic") == 0 || strcmp(value, "time.nanos") == 0 ||
            strcmp(value, "time.micros") == 0 || strcmp(value, "time.clock") == 0);
}

static bool xaot_cli_link_add_stdlib_object(XaotCliLinkCommand *cmd, const char *value, char *err,
                                            size_t err_size) {
    size_t len;

    if (!value || !value[0] || xaot_cli_stdlib_object_needs_aot_core(value))
        return true;
    len = strlen(value);
    if (strchr(value, '/') || (len > 2 && strcmp(value + len - 2, ".o") == 0) ||
        (len > 2 && strcmp(value + len - 2, ".a") == 0) ||
        (len > 2 && strcmp(value + len - 2, ".c") == 0))
        return xaot_cli_link_add_arg(cmd, value, err, err_size);
    return true;
}

static bool xaot_cli_manifest_needs_aot_core(const XaotLinkManifest *manifest) {
    if (!manifest)
        return false;
    for (uint32_t i = 0; i < manifest->n_stdlib_objects; i++) {
        if (xaot_cli_stdlib_object_needs_aot_core(manifest->stdlib_objects[i]))
            return true;
    }
    for (uint32_t i = 0; i < manifest->n_stdlib_symbols; i++) {
        if (xaot_cli_stdlib_object_needs_aot_core(manifest->stdlib_symbols[i]))
            return true;
    }
    return false;
}

static bool xaot_cli_link_ld_flag_supported(const XrCliBuildTarget *target, const char *flag) {
    if (!target || !flag)
        return true;
    if (!target->is_native && strcmp(flag, "-Wl,-dead_strip") == 0)
        return false;
    return true;
}

static void xaot_cli_manifest_remove_string(char ***items, uint32_t *count, const char *value) {
    uint32_t i;

    if (!items || !*items || !count || !value)
        return;
    i = 0;
    while (i < *count) {
        if ((*items)[i] && strcmp((*items)[i], value) == 0) {
            uint32_t j;
            xr_free((*items)[i]);
            for (j = i + 1; j < *count; j++)
                (*items)[j - 1] = (*items)[j];
            *count = *count - 1;
            continue;
        }
        i++;
    }
}

static void xaot_cli_manifest_clear_string_list(char ***items, uint32_t *count) {
    if (!items || !*items || !count)
        return;
    for (uint32_t i = 0; i < *count; i++)
        xr_free((*items)[i]);
    xr_free(*items);
    *items = NULL;
    *count = 0;
}

static bool xaot_cli_normalize_manifest_for_target(XaotLinkManifest *manifest,
                                                   const XrCliBuildTarget *target, char *err,
                                                   size_t err_size) {
    if (!manifest || !target)
        return true;
    if (target->is_native)
        return true;

    xaot_cli_manifest_remove_string(&manifest->ld_flags, &manifest->n_ld_flags, "-Wl,-dead_strip");
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-ffunction-sections") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fdata-sections") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-Wl,--gc-sections")) {
        snprintf(err, err_size, "failed to normalize AOT link manifest for target '%s'",
                 target->name);
        return false;
    }
    return true;
}

static bool xaot_cli_apply_freestanding_profile(XaotLinkManifest *manifest, char *err,
                                                size_t err_size) {
    if (!manifest)
        return true;

    if (xaot_link_manifest_needs_runtime(manifest) || manifest->n_runtime_objects > 0) {
        snprintf(err, err_size,
                 "freestanding profile rejects runtime-backed features "
                 "(runtime_caps=%u runtime_objects=%u)",
                 manifest->n_runtime_caps, manifest->n_runtime_objects);
        return false;
    }
    if (manifest->n_stdlib_objects > 0 || manifest->n_stdlib_symbols > 0) {
        snprintf(err, err_size,
                 "freestanding profile rejects hosted stdlib objects "
                 "(stdlib_objects=%u stdlib_symbols=%u)",
                 manifest->n_stdlib_objects, manifest->n_stdlib_symbols);
        return false;
    }

    xaot_cli_manifest_clear_string_list(&manifest->system_libs, &manifest->n_system_libs);
    xaot_cli_manifest_remove_string(&manifest->ld_flags, &manifest->n_ld_flags, "-Wl,-dead_strip");
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRAY_PROFILE_FREESTANDING=1") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-ffreestanding") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-stack-protector") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-nostdlib")) {
        snprintf(err, err_size, "failed to apply freestanding build profile");
        return false;
    }
    return true;
}

static bool xaot_cli_add_build_sanitizer_flags(XaotLinkManifest *manifest,
                                               const XrCliBuildTarget *target, char *err,
                                               size_t err_size) {
    if (!manifest)
        return true;
    if (target && !target->is_native)
        return true;
#if defined(XR_BUILD_ASAN) && XR_BUILD_ASAN
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fsanitize=address") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fsanitize=address")) {
        snprintf(err, err_size, "failed to add ASan flags to AOT link manifest");
        return false;
    }
#endif
#if defined(XR_BUILD_UBSAN) && XR_BUILD_UBSAN
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fsanitize=undefined") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-sanitize=function") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fsanitize=undefined") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fno-sanitize=function")) {
        snprintf(err, err_size, "failed to add UBSan flags to AOT link manifest");
        return false;
    }
#endif
#if defined(XR_BUILD_TSAN) && XR_BUILD_TSAN
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fsanitize=thread") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fsanitize=thread")) {
        snprintf(err, err_size, "failed to add TSan flags to AOT link manifest");
        return false;
    }
#endif
#if defined(XR_BUILD_MSAN) && XR_BUILD_MSAN
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fsanitize=memory") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG,
                                       "-fsanitize-memory-track-origins=2") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG,
                                       "-fno-optimize-sibling-calls") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fsanitize=memory")) {
        snprintf(err, err_size, "failed to add MSan flags to AOT link manifest");
        return false;
    }
#endif
    (void) err;
    (void) err_size;
    return true;
}

static bool xaot_cli_add_build_debug_flags(XaotLinkManifest *manifest, char *err, size_t err_size) {
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-g") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRAY_AOT_DEBUG_LOCALS=1")) {
        snprintf(err, err_size, "failed to add debug flags to AOT link manifest");
        return false;
    }
    return true;
}

static void xaot_cli_print_command(const char *label, const XaotCliLinkCommand *cmd) {
    int i;

    if (!cmd || !cmd->argv[0])
        return;
    printf("%s:", label ? label : "Command");
    for (i = 0; cmd->argv[i]; i++)
        printf(" %s", cmd->argv[i]);
    printf("\n");
}

static void xaot_cli_link_print_command(const XaotCliLinkCommand *cmd) {
    xaot_cli_print_command("Link command", cmd);
}

static bool xaot_cli_build_compile_command(const XrCliToolchainPlan *plan,
                                           const XrCliBuildTarget *target,
                                           const XaotLinkManifest *manifest, const char *opt_flag,
                                           const char *c_file, const char *obj_file,
                                           const char *sysroot, XaotCliLinkCommand *cmd, char *err,
                                           size_t err_size) {
    char aot_include[600];
    char runtime_include[600];
    uint32_t i;

    if (!plan || !target || !manifest || !c_file || !obj_file || !cmd) {
        snprintf(err, err_size, "missing AOT compile command input");
        return false;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->program = plan->program;
    if (!xaot_cli_link_add_arg(cmd, plan->program, err, err_size))
        return false;
    if (plan->kind == XR_CLI_TOOLCHAIN_ZIG) {
        if (!xaot_cli_link_add_arg(cmd, "cc", err, err_size))
            return false;
        if (!target->is_native) {
            if (!xaot_cli_link_add_arg(cmd, "-target", err, err_size) ||
                !xaot_cli_link_add_arg(cmd, target->zig_triple, err, err_size))
                return false;
        }
    }
    if (!xaot_cli_link_add_arg(cmd, opt_flag, err, err_size) ||
        !xaot_cli_link_add_arg(cmd, "-ffp-contract=off", err, err_size) ||
        !xaot_cli_link_add_arg(cmd, "-c", err, err_size) ||
        !xaot_cli_link_add_arg(cmd, c_file, err, err_size) ||
        !xaot_cli_link_add_arg(cmd, "-o", err, err_size) ||
        !xaot_cli_link_add_arg(cmd, obj_file, err, err_size))
        return false;

    resolve_aot_include_paths(sysroot, aot_include, sizeof(aot_include), runtime_include,
                              sizeof(runtime_include));
    if (!xaot_cli_link_add_prefixed(cmd, "-I", aot_include, err, err_size) ||
        !xaot_cli_link_add_prefixed(cmd, "-I", runtime_include, err, err_size))
        return false;

    if (sysroot && sysroot[0] &&
        !xaot_cli_link_add_prefixed(cmd, "--sysroot=", sysroot, err, err_size))
        return false;

    if (!target->is_native && !xaot_cli_link_add_arg(cmd, "-DXR_AOT_CROSS_TARGET=1", err, err_size))
        return false;

    for (i = 0; i < manifest->n_defines; i++) {
        if (!xaot_cli_link_add_prefixed(cmd, "-D", manifest->defines[i], err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_cc_flags; i++) {
        if (!xaot_cli_link_add_arg(cmd, manifest->cc_flags[i], err, err_size))
            return false;
    }
    if (!target->is_native) {
        if (!xaot_link_manifest_contains(manifest, XAOT_LINK_CC_FLAG, "-ffunction-sections") &&
            !xaot_cli_link_add_arg(cmd, "-ffunction-sections", err, err_size))
            return false;
        if (!xaot_link_manifest_contains(manifest, XAOT_LINK_CC_FLAG, "-fdata-sections") &&
            !xaot_cli_link_add_arg(cmd, "-fdata-sections", err, err_size))
            return false;
    }

    return true;
}

static int invoke_aot_manifest_compile(const XrCliToolchainPlan *plan,
                                       const XrCliBuildTarget *target,
                                       const XaotLinkManifest *manifest, const char *opt_flag,
                                       const char *c_file, const char *obj_file,
                                       const char *sysroot, bool dump_command) {
    char err[512];
    XaotCliLinkCommand cmd;

    if (!xaot_cli_build_compile_command(plan, target, manifest, opt_flag, c_file, obj_file, sysroot,
                                        &cmd, err, sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    if (dump_command)
        xaot_cli_print_command("Compile command", &cmd);

    XrProcId pid = xr_proc_spawn(cmd.program, cmd.argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start toolchain '%s'\n", cmd.program);
        if (plan && plan->kind == XR_CLI_TOOLCHAIN_ZIG)
            fprintf(stderr, "Tip: install Zig, bundle Zig with xray, or set XRAY_ZIG/--zig\n");
        return 1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: AOT manifest compilation failed for target '%s'\n",
                target ? target->name : "native");
        return 1;
    }
    return 0;
}

static const char *resolve_xray_lib_path(const char *sysroot, char *lib_path, size_t lib_path_sz) {
    const char *xray_lib = getenv("XRAY_LIB");

    if (sysroot) {
        snprintf(lib_path, lib_path_sz, "%s/lib", sysroot);
        return lib_path;
    }
    if (xray_lib)
        return xray_lib;
#ifdef XRT_BUILD_LIB_DIR
    return XRT_BUILD_LIB_DIR;
#else
    return "/usr/local/lib";
#endif
}

static bool xaot_cli_build_link_command(const XrCliToolchainPlan *plan,
                                        const XrCliBuildTarget *target,
                                        const XaotLinkManifest *manifest, const char *opt_flag,
                                        const char *output_file, const char *const *inputs,
                                        int n_inputs, bool strip_symbols, bool shared_library,
                                        const char *sysroot, XaotCliLinkCommand *cmd, char *err,
                                        size_t err_size) {
    char aot_include[600];
    char runtime_include[600];
    char lib_path[512];
    bool needs_runtime;
    bool needs_aot_core;
    bool freestanding_profile;
    uint32_t i;
    int in;

    if (!plan || !target || !manifest || !output_file || !inputs || n_inputs <= 0 || !cmd) {
        snprintf(err, err_size, "missing AOT link command input");
        return false;
    }
    if (manifest->n_generated_c_files == 0) {
        snprintf(err, err_size, "AOT link manifest has no generated C input");
        return false;
    }

    needs_runtime = xaot_link_manifest_needs_runtime(manifest);
    needs_aot_core = xaot_cli_manifest_needs_aot_core(manifest);
    freestanding_profile =
        xaot_link_manifest_contains(manifest, XAOT_LINK_DEFINE, "XRAY_PROFILE_FREESTANDING=1");
    if (needs_runtime && !target->is_native) {
        snprintf(err, err_size,
                 "cross target '%s' cannot consume runtime objects from AOT link manifest yet",
                 target->name);
        return false;
    }
    if (needs_runtime && plan->kind != XR_CLI_TOOLCHAIN_HOST) {
        snprintf(err, err_size,
                 "toolchain '%s' cannot consume runtime objects from AOT link manifest yet",
                 xr_cli_toolchain_kind_name(plan->kind));
        return false;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->program = plan->program;
    if (!xaot_cli_link_add_arg(cmd, plan->program, err, err_size))
        return false;
    if (plan->kind == XR_CLI_TOOLCHAIN_ZIG) {
        if (!xaot_cli_link_add_arg(cmd, "cc", err, err_size))
            return false;
        if (!target->is_native) {
            if (!xaot_cli_link_add_arg(cmd, "-target", err, err_size) ||
                !xaot_cli_link_add_arg(cmd, target->zig_triple, err, err_size))
                return false;
        }
    }
    if (!xaot_cli_link_add_arg(cmd, opt_flag, err, err_size) ||
        !xaot_cli_link_add_arg(cmd, "-ffp-contract=off", err, err_size))
        return false;
    if (shared_library && freestanding_profile) {
        if (!xaot_cli_link_add_arg(cmd, "-r", err, err_size))
            return false;
    } else if (shared_library) {
#ifdef XR_OS_MACOS
        if (!xaot_cli_link_add_arg(cmd, "-dynamiclib", err, err_size))
            return false;
#else
        if (!xaot_cli_link_add_arg(cmd, "-shared", err, err_size))
            return false;
#endif
    }
    if (!xaot_cli_link_add_arg(cmd, "-o", err, err_size) ||
        !xaot_cli_link_add_arg(cmd, output_file, err, err_size))
        return false;
    for (in = 0; in < n_inputs; in++) {
        if (!xaot_cli_link_add_arg(cmd, inputs[in], err, err_size))
            return false;
    }

    resolve_aot_include_paths(sysroot, aot_include, sizeof(aot_include), runtime_include,
                              sizeof(runtime_include));
    if (!xaot_cli_link_add_prefixed(cmd, "-I", aot_include, err, err_size) ||
        !xaot_cli_link_add_prefixed(cmd, "-I", runtime_include, err, err_size))
        return false;

    if (sysroot && sysroot[0] &&
        !xaot_cli_link_add_prefixed(cmd, "--sysroot=", sysroot, err, err_size))
        return false;

    if (!target->is_native && !xaot_cli_link_add_arg(cmd, "-DXR_AOT_CROSS_TARGET=1", err, err_size))
        return false;

    for (i = 0; i < manifest->n_defines; i++) {
        if (!xaot_cli_link_add_prefixed(cmd, "-D", manifest->defines[i], err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_cc_flags; i++) {
        if (!xaot_cli_link_add_arg(cmd, manifest->cc_flags[i], err, err_size))
            return false;
    }
    if (!target->is_native) {
        if (!xaot_link_manifest_contains(manifest, XAOT_LINK_CC_FLAG, "-ffunction-sections") &&
            !xaot_cli_link_add_arg(cmd, "-ffunction-sections", err, err_size))
            return false;
        if (!xaot_link_manifest_contains(manifest, XAOT_LINK_CC_FLAG, "-fdata-sections") &&
            !xaot_cli_link_add_arg(cmd, "-fdata-sections", err, err_size))
            return false;
    }
    if ((needs_runtime || needs_aot_core) &&
        !xaot_cli_link_add_prefixed(
            cmd, "-L", resolve_xray_lib_path(sysroot, lib_path, sizeof(lib_path)), err, err_size))
        return false;
    for (i = 0; i < manifest->n_runtime_objects; i++) {
        if (!xaot_cli_link_add_runtime_object(cmd, manifest->runtime_objects[i], err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_stdlib_objects; i++) {
        if (!xaot_cli_link_add_stdlib_object(cmd, manifest->stdlib_objects[i], err, err_size))
            return false;
    }
    if (needs_aot_core && !xaot_cli_link_add_runtime_object(cmd, "xray_aot_core", err, err_size))
        return false;
    for (i = 0; i < manifest->n_system_libs; i++) {
        if (!xaot_cli_link_add_prefixed(cmd, "-l", manifest->system_libs[i], err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_ld_flags; i++) {
        if (!xaot_cli_link_ld_flag_supported(target, manifest->ld_flags[i]))
            continue;
        if (!xaot_cli_link_add_arg(cmd, manifest->ld_flags[i], err, err_size))
            return false;
    }
    if (!target->is_native &&
        !xaot_link_manifest_contains(manifest, XAOT_LINK_LD_FLAG, "-Wl,--gc-sections")) {
        if (!xaot_cli_link_add_arg(cmd, "-Wl,--gc-sections", err, err_size))
            return false;
    }
    if (strip_symbols && !xaot_link_manifest_contains(manifest, XAOT_LINK_LD_FLAG, "-Wl,-S")) {
        if (!xaot_cli_link_add_arg(cmd, "-Wl,-S", err, err_size))
            return false;
    }
    if (strip_symbols && !xaot_link_manifest_contains(manifest, XAOT_LINK_LD_FLAG, "-Wl,-x")) {
        if (!xaot_cli_link_add_arg(cmd, "-Wl,-x", err, err_size))
            return false;
    }

    return true;
}

static int invoke_aot_manifest_link(const XrCliToolchainPlan *plan, const XrCliBuildTarget *target,
                                    const XaotLinkManifest *manifest, const char *opt_flag,
                                    const char *output_file, const char *const *inputs,
                                    int n_inputs, bool strip_symbols, bool shared_library,
                                    const char *sysroot, bool dump_link_command,
                                    bool dry_run_link) {
    char err[512];
    XaotCliLinkCommand cmd;

    if (!xaot_cli_build_link_command(plan, target, manifest, opt_flag, output_file, inputs,
                                     n_inputs, strip_symbols, shared_library, sysroot, &cmd, err,
                                     sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    if (dump_link_command)
        xaot_cli_link_print_command(&cmd);

    if (dry_run_link)
        return 0;

    XrProcId pid = xr_proc_spawn(cmd.program, cmd.argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start toolchain '%s'\n", cmd.program);
        if (plan && plan->kind == XR_CLI_TOOLCHAIN_ZIG)
            fprintf(stderr, "Tip: install Zig, bundle Zig with xray, or set XRAY_ZIG/--zig\n");
        return 1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: AOT manifest linking failed for target '%s'\n",
                target ? target->name : "native");
        return 1;
    }
    return 0;
}

#ifdef XR_OS_MACOS
static int remove_tree_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void) st;
    (void) type;
    (void) ftw;
    return remove(path);
}

static void remove_dsym_bundle(const char *output_file) {
    char dsym_path[XR_PATH_MAX];
    int written;

    if (!output_file || !output_file[0])
        return;
    written = snprintf(dsym_path, sizeof(dsym_path), "%s.dSYM", output_file);
    if (written < 0 || (size_t) written >= sizeof(dsym_path))
        return;
    if (xr_fs_exists(dsym_path))
        (void) nftw(dsym_path, remove_tree_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int invoke_dsymutil(const char *output_file, bool dump_command) {
    const char *spawn_argv[3];
    XrProcId pid;
    int code = -1;

    spawn_argv[0] = "dsymutil";
    spawn_argv[1] = output_file;
    spawn_argv[2] = NULL;

    if (dump_command)
        printf("Debug info command: dsymutil %s\n", output_file);

    pid = xr_proc_spawn("dsymutil", spawn_argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start dsymutil for native debug build\n");
        return 1;
    }
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: dsymutil failed for '%s'\n", output_file);
        return 1;
    }
    return 0;
}
#endif

// Write the main() function for bytecode execution into a C file
// bundle_source is the output of xr_bundle_to_c_source()
static void write_bytecode_main(FILE *f, const char *bundle_source) {
    // Headers (bundle source uses strcmp but doesn't include string.h)
    fprintf(f, "#include <stdio.h>\n"
               "#include <stdlib.h>\n"
               "#include <string.h>\n"
               "#include <stdint.h>\n"
               "#include <stddef.h>\n\n");

    // Bundle-generated data (module bytecode arrays, module table, lookup function)
    fprintf(f, "%s\n\n", bundle_source);

    // Main: default bytecode bundles run with full runtime support so imports
    // and runtime exception objects behave like `xray run`.
    fprintf(f, "#include \"xray_vm.h\"\n"
               "extern int xr_eval_bytecode(XrVMRuntime*, const uint8_t*, size_t);\n"
               "\n"
               "int main(int argc, char **argv) {\n"
               "    XrVMConfig params;\n"
               "    xray_vm_config_init(&params);\n"
               "    params.script_argc = argc > 1 ? argc - 1 : 0;\n"
               "    params.script_argv = argc > 1 ? argv + 1 : NULL;\n"
               "    XrVMRuntime *X = xray_vm_new_full(&params);\n"
               "    if (!X) { fprintf(stderr, \"Failed to create runtime\\n\"); return 1; }\n"
               "    xray_vm_multicore_init(X, 0);\n"
               "    const XrEmbeddedModule *entry = &xr_app_modules[xr_app_entry_index];\n"
               "    xray_vm_set_script_info(X, entry->path, params.script_argc, "
               "params.script_argv);\n"
               "    int result = xr_eval_bytecode(X, entry->bc, entry->size);\n"
               "    xray_vm_multicore_destroy(X);\n"
               "    xray_vm_delete(X);\n"
               "    return result;\n"
               "}\n");
}

/* ========== Optimization Flag ========== */

/* Default is -O3: AOT output is a release artifact and the C compiler is the
 * final optimizer of the pipeline. Use -O <level> to lower it explicitly. */
static const char *make_opt_flag(const char *level, bool debug_symbols) {
    if (!level)
        return debug_symbols ? "-O0" : "-O3";
    if (strcmp(level, "0") == 0)
        return "-O0";
    if (strcmp(level, "1") == 0)
        return "-O1";
    if (strcmp(level, "2") == 0)
        return "-O2";
    if (strcmp(level, "3") == 0)
        return "-O3";
    if (strcmp(level, "s") == 0)
        return "-Os";
    /* Xray's fast tier is a whole-program build strategy, not unsafe fast-math:
     * keep NaN/float semantics intact and let native AOT add LTO/CPU tuning. */
    if (strcmp(level, "fast") == 0)
        return "-O3";
    fprintf(stderr, "Warning: unknown optimization level '%s', using -O3\n", level);
    return "-O3";
}

static bool build_opt_level_is_fast(const char *level) {
    return level && strcmp(level, "fast") == 0;
}

static const char *default_shared_library_output(void) {
#ifdef XR_OS_MACOS
    return "libxray_exports.dylib";
#else
    return "libxray_exports.so";
#endif
}

/* ========== Build Sub-Modes (forward declarations) ========== */

static int cmd_build_bytecode(const char *input, const char *output, const char *cc,
                              const char *opt_flag, bool c_only, bool strip, bool debug_symbols,
                              const char *sysroot);
static int cmd_build_native(const char *input, const char *output, const char *cc,
                            const char *opt_flag, const char *cpu, bool c_only, bool strip,
                            bool debug_symbols, bool shared_library, XrCliBuildProfile profile,
                            const char *sysroot, bool verbose, bool dump_xaot_plan,
                            bool dump_link_manifest, bool dump_link_command, bool dry_run_link,
                            const char *c_header, bool keep_c, const char *cache_dir_arg,
                            bool rebuild, bool lto, const XrCliBuildTarget *target,
                            const XrCliToolchainPlan *toolchain_plan);

/* ========== CLI Entry Point ========== */

XR_FUNC int cmd_build(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "build expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);
    const char *cc = xr_cli_opt_string(&inv->options, "cc", "cc");
    const char *opt_level = xr_cli_opt_string(&inv->options, "opt", NULL);
    const char *sysroot = xr_cli_opt_string(&inv->options, "sysroot", NULL);
    const char *target_arg = xr_cli_opt_string(&inv->options, "target", "native");
    const char *toolchain_arg = xr_cli_opt_string(&inv->options, "toolchain", "auto");
    const char *zig_path = xr_cli_opt_string(&inv->options, "zig", NULL);
    const char *cpu = xr_cli_opt_string(&inv->options, "cpu", NULL);
    const char *profile_arg = xr_cli_opt_string(&inv->options, "profile", "hosted");
    bool c_only = xr_cli_opt_bool(&inv->options, "c-only");
    bool strip_symbols = xr_cli_opt_bool(&inv->options, "strip");
    bool debug_symbols = xr_cli_opt_bool(&inv->options, "debug");
    bool native_mode = xr_cli_opt_bool(&inv->options, "native");
    bool shared_library = xr_cli_opt_bool(&inv->options, "shared");
    bool dump_xaot_plan = xr_cli_opt_bool(&inv->options, "dump-xaot-plan");
    bool dump_link_manifest = xr_cli_opt_bool(&inv->options, "dump-link-manifest");
    bool dump_link_command = xr_cli_opt_bool(&inv->options, "dump-link-command");
    bool dry_run_link = xr_cli_opt_bool(&inv->options, "dry-run-link");
    const char *c_header = xr_cli_opt_string(&inv->options, "c-header", NULL);
    bool keep_c = xr_cli_opt_bool(&inv->options, "keep-c");
    const char *cache_dir_arg = xr_cli_opt_string(&inv->options, "cache-dir", NULL);
    bool rebuild = xr_cli_opt_bool(&inv->options, "rebuild");
    bool lto = xr_cli_opt_bool(&inv->options, "lto");
    bool verbose = xr_cli_opt_bool(&inv->options, "verbose") || (inv->ctx && inv->ctx->verbose);
    bool opt_fast = build_opt_level_is_fast(opt_level);
    XrCliBuildTarget target;
    XrCliBuildProfile profile;
    XrCliToolchainKind toolchain_kind;
    XrCliToolchainPlan toolchain_plan;
    char parse_err[512];

    if (!xr_cli_build_target_parse(target_arg, &target, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        return 2;
    }
    if (!build_profile_parse(profile_arg, &profile, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        return 2;
    }
    if (!xr_cli_toolchain_kind_parse(toolchain_arg, &toolchain_kind, parse_err,
                                     sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        return 2;
    }
    if (profile != XR_CLI_BUILD_PROFILE_HOSTED && !native_mode) {
        fprintf(stderr, "Error: --profile %s requires --native\n", build_profile_name(profile));
        return 2;
    }

    if (dump_xaot_plan && !native_mode) {
        fprintf(stderr, "Error: --dump-xaot-plan requires --native\n");
        return 2;
    }
    if (dump_link_manifest && !native_mode) {
        fprintf(stderr, "Error: --dump-link-manifest requires --native\n");
        return 2;
    }
    if (dump_link_command && !native_mode) {
        fprintf(stderr, "Error: --dump-link-command requires --native\n");
        return 2;
    }
    if (dry_run_link && !native_mode) {
        fprintf(stderr, "Error: --dry-run-link requires --native\n");
        return 2;
    }
    if (!target.is_native && !native_mode) {
        fprintf(stderr, "Error: --target %s requires --native\n", target.name);
        return 2;
    }
    if ((xr_cli_opt_present(&inv->options, "toolchain") ||
         xr_cli_opt_present(&inv->options, "zig") || keep_c) &&
        !native_mode) {
        fprintf(stderr, "Error: --toolchain/--zig/--keep-c require --native\n");
        return 2;
    }
    if (c_header && !native_mode) {
        fprintf(stderr, "Error: --c-header requires --native\n");
        return 2;
    }
    if (shared_library && !native_mode) {
        fprintf(stderr, "Error: --shared requires --native\n");
        return 2;
    }
    if (shared_library && c_only) {
        fprintf(stderr, "Error: --shared cannot be combined with --c-only\n");
        return 2;
    }
    if (dry_run_link && c_only) {
        fprintf(stderr, "Error: --dry-run-link cannot be combined with --c-only\n");
        return 2;
    }
    if (shared_library && !target.is_native) {
        fprintf(stderr, "Error: --shared currently requires native target\n");
        return 2;
    }
#ifdef XR_OS_WINDOWS
    if (shared_library) {
        fprintf(stderr, "Error: --shared is not implemented for Windows hosts yet\n");
        return 2;
    }
#endif
    if ((xr_cli_opt_present(&inv->options, "cache-dir") || rebuild) && !native_mode) {
        fprintf(stderr, "Error: --cache-dir/--rebuild require --native\n");
        return 2;
    }
    if (cpu && !native_mode) {
        fprintf(stderr, "Error: --cpu requires --native\n");
        return 2;
    }
    if (debug_symbols && !native_mode) {
        fprintf(stderr, "Error: --debug requires --native\n");
        return 2;
    }
    if (cpu && !target.is_native) {
        fprintf(stderr,
                "Error: --cpu is host-only; cross target '%s' selects its CPU via the "
                "target triple\n",
                target.name);
        return 2;
    }
    if (!xr_cli_toolchain_resolve_ex(toolchain_kind, &target, cc, zig_path,
                                     inv->ctx ? inv->ctx->program : NULL, &toolchain_plan,
                                     parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        return 2;
    }
    if (shared_library && toolchain_plan.kind != XR_CLI_TOOLCHAIN_HOST) {
        fprintf(stderr, "Error: --shared currently requires the host toolchain\n");
        return 2;
    }

    if (!output_file)
        output_file = c_only ? "app.c"
                             : (shared_library ? default_shared_library_output()
                                               : xr_cli_build_target_default_output(&target));

    const char *opt_flag = make_opt_flag(opt_level, debug_symbols);
    const char *effective_cpu = cpu;
    bool effective_lto = lto;
    if (native_mode && target.is_native && opt_fast) {
        effective_lto = true;
        if (!effective_cpu || !effective_cpu[0])
            effective_cpu = "native";
    }

    if (native_mode) {
        return cmd_build_native(input_file, output_file, cc, opt_flag, effective_cpu, c_only,
                                strip_symbols, debug_symbols, shared_library, profile, sysroot,
                                verbose, dump_xaot_plan, dump_link_manifest, dump_link_command,
                                dry_run_link, c_header, keep_c, cache_dir_arg, rebuild,
                                effective_lto, &target, &toolchain_plan);
    }
    return cmd_build_bytecode(input_file, output_file, cc, opt_flag, c_only, strip_symbols,
                              debug_symbols, sysroot);
}

/* ========== Bytecode Bundling (default mode) ========== */

static int cmd_build_bytecode(const char *input, const char *output, const char *cc,
                              const char *opt_flag, bool c_only, bool strip, bool debug_symbols,
                              const char *sysroot) {
    printf("[bytecode] Building: %s\n", input);

    XrVMRuntime *X = xr_isolate_profile_new(XR_ISOLATE_PROFILE_RUN);
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        return 1;
    }

    XrBundle *bundle = xr_bundle_create_ex(X, input, XR_BUNDLE_DEFAULT);
    if (!bundle) {
        fprintf(stderr, "Error: bytecode bundling failed\n");
        xray_vm_delete(X);
        return 1;
    }
    xray_vm_delete(X);

    printf("Modules: %d\n", bundle->count);
    for (int i = 0; i < bundle->count; i++)
        printf("  %s (%zu bytes)\n", bundle->entries[i].path, bundle->entries[i].bc_size);

    char *bc_source = xr_bundle_to_c_source(bundle, "xr_app");
    xr_bundle_free(bundle);
    if (!bc_source) {
        fprintf(stderr, "Error: C source generation failed\n");
        return 1;
    }

    char c_file[512];
    if (c_only)
        snprintf(c_file, sizeof(c_file), "%s", output);
    else
        snprintf(c_file, sizeof(c_file), "/tmp/xray_bc_%d.c", (int) xr_proc_self_pid());

    FILE *f = fopen(c_file, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", c_file);
        xr_free(bc_source);
        return 1;
    }
    write_bytecode_main(f, bc_source);
    fclose(f);
    xr_free(bc_source);

    if (c_only) {
        printf("Generated: %s\n", output);
        return 0;
    }

    int ret = invoke_cc(cc, opt_flag, output, c_file, NULL, strip, debug_symbols, sysroot);
    xr_fs_remove(c_file);
    if (ret == 0)
        printf("Generated: %s\n", output);
    return ret;
}

/* ========== Native Build (--native, Xi IR AOT pipeline) ========== */

static void print_aot_coro_frame_stats(const XaotBuildResult *result) {
    XR_DCHECK(result != NULL, "AOT result is NULL");
    const XiCgenCoroFrameStats *stats = &result->coro_frame_stats;
    if (stats->coroutine_count == 0) {
        printf("[xi-native] Coroutine frames: none\n");
        return;
    }
    double avg_frame = (double) stats->total_frame_bytes / (double) stats->coroutine_count;
    printf("[xi-native] Coroutine frames: count=%u total=%zuB avg=%.1fB max=%zuB "
           "roots=%u releases=%u max_roots=%u max_releases=%u\n",
           stats->coroutine_count, stats->total_frame_bytes, avg_frame, stats->max_frame_bytes,
           stats->total_roots, stats->total_releases, stats->max_roots, stats->max_releases);
}

static void print_aot_codegen_stats(const XaotBuildResult *result) {
    XR_DCHECK(result != NULL, "AOT result is NULL");
    const XiCgenStats *stats = &result->cgen_stats;
    printf("[xi-native] Codegen stats: functions=%u native=%u tagged=%u coro=%u "
           "boxed_adapters=%u sync_go_wrappers=%u xi_box=%u xi_unbox=%u\n",
           stats->functions_total, stats->functions_native_abi, stats->functions_tagged_abi,
           stats->functions_coro_abi, stats->boxed_adapters, stats->sync_go_wrappers,
           stats->xi_box_ops, stats->xi_unbox_ops);
}

static void print_aot_prepare_stats(const XaotBuildResult *result) {
    XR_DCHECK(result != NULL, "AOT result is NULL");
    const XaotPrepareStats *stats = &result->prepare_stats;
    printf("[xi-native] AOT prepare: functions=%u native=%u tagged=%u coro=%u values=%u "
           "boundaries=%u value_scalar=%u value_tagged=%u value_ptr=%u value_aggregate=%u "
           "value_view=%u value_void=%u\n",
           stats->functions_total, stats->functions_native_abi, stats->functions_tagged_abi,
           stats->functions_coro_abi, stats->values_total, stats->boundary_count,
           stats->values_scalar, stats->values_tagged, stats->values_ptr, stats->values_aggregate,
           stats->values_view, stats->values_void);
}

/* ========== Per-module object cache (114: separate compilation) ==========
 *
 * Each generated module C translation unit is compiled independently to an
 * object file keyed by a content hash of (generated C + the full compile
 * environment).  A second build only re-runs the C compiler for modules whose
 * generated C or compile flags changed; unchanged modules reuse the cached
 * object.  Because the generated C already encodes every cross-module
 * observable (callee symbol names, ABI signatures), an interface change to a
 * dependency alters its dependents' generated C (cache miss) while a private
 * implementation change leaves them byte-identical (cache hit) — dirty
 * propagation falls out of content addressing with no separate fingerprint. */

static uint64_t xaot_hash_fold(uint64_t h, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *) data;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= XR_FNV64_PRIME;
    }
    return h;
}

static uint64_t xaot_hash_fold_str(uint64_t h, const char *s) {
    return s ? xaot_hash_fold(h, s, strlen(s) + 1) : xaot_hash_fold(h, "", 1);
}

static uint64_t xaot_hash_fold_bool(uint64_t h, bool v) {
    uint8_t b = v ? 1 : 0;
    return xaot_hash_fold(h, &b, sizeof(b));
}

static uint64_t xaot_hash_fold_string_list(uint64_t h, char **items, uint32_t count) {
    h = xaot_hash_fold(h, &count, sizeof(count));
    for (uint32_t i = 0; i < count; i++)
        h = xaot_hash_fold_str(h, items[i]);
    return h;
}

static uint64_t xaot_hash_fold_file_stat(uint64_t h, const char *path);

typedef struct XaotSourcePathList {
    char **items;
    size_t count;
    size_t cap;
} XaotSourcePathList;

static void xaot_source_path_list_free(XaotSourcePathList *list) {
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        xr_free(list->items[i]);
    xr_free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool xaot_source_path_list_add(XaotSourcePathList *list, const char *path) {
    char **next;
    size_t cap;

    if (!list || !path)
        return false;
    if (list->count == list->cap) {
        cap = list->cap ? list->cap * 2 : 64;
        next = (char **) xr_realloc(list->items, cap * sizeof(char *));
        if (!next)
            return false;
        list->items = next;
        list->cap = cap;
    }
    list->items[list->count] = xr_strdup(path);
    if (!list->items[list->count])
        return false;
    list->count++;
    return true;
}

static int xaot_source_path_cmp(const void *a, const void *b) {
    const char *pa = *(const char *const *) a;
    const char *pb = *(const char *const *) b;
    return strcmp(pa ? pa : "", pb ? pb : "");
}

static bool xaot_source_path_has_cache_suffix(const char *path) {
    size_t len;

    if (!path)
        return false;
    len = strlen(path);
    return (len > 2 && strcmp(path + len - 2, ".c") == 0) ||
           (len > 2 && strcmp(path + len - 2, ".h") == 0);
}

static void xaot_collect_source_tree_files(const char *dir, XaotSourcePathList *list, int depth) {
    XrDirIter *it;
    XrDirEntry entry;

    if (!dir || !list || depth > 16)
        return;
    it = xr_dir_open(dir);
    if (!it)
        return;
    while (xr_dir_next(it, &entry)) {
        char path[XR_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry.name);
        if (n < 0 || (size_t) n >= sizeof(path))
            continue;
        if (entry.is_dir) {
            xaot_collect_source_tree_files(path, list, depth + 1);
        } else if (xaot_source_path_has_cache_suffix(path)) {
            (void) xaot_source_path_list_add(list, path);
        }
    }
    xr_dir_close(it);
}

static uint64_t xaot_hash_fold_source_path_list(uint64_t h, const char *label, const char *root) {
    XaotSourcePathList list = {0};
    int exists;

    h = xaot_hash_fold_str(h, label);
    h = xaot_hash_fold_str(h, root);
    exists = (root && root[0] && xr_fs_is_dir(root)) ? 1 : 0;
    h = xaot_hash_fold(h, &exists, sizeof(exists));
    if (!exists)
        return h;

    xaot_collect_source_tree_files(root, &list, 0);
    if (list.count > 1)
        qsort(list.items, list.count, sizeof(char *), xaot_source_path_cmp);
    h = xaot_hash_fold(h, &list.count, sizeof(list.count));
    for (size_t i = 0; i < list.count; i++)
        h = xaot_hash_fold_file_stat(h, list.items[i]);
    xaot_source_path_list_free(&list);
    return h;
}

static uint64_t xaot_aot_runtime_source_key(void) {
    static bool cached = false;
    static uint64_t cached_key = 0;
    uint64_t h;
    char shared_dir[XR_PATH_MAX];

    if (cached)
        return cached_key;
    h = XR_FNV64_OFFSET_BASIS;
    h = xaot_hash_fold_str(h, "xaot-aot-runtime-source-key-v1");
#ifdef XRT_AOT_INCLUDE_DIR
    h = xaot_hash_fold_source_path_list(h, "aot", XRT_AOT_INCLUDE_DIR);
    snprintf(shared_dir, sizeof(shared_dir), "%s/../shared", XRT_AOT_INCLUDE_DIR);
    h = xaot_hash_fold_source_path_list(h, "shared", shared_dir);
#else
    h = xaot_hash_fold_source_path_list(h, "aot", NULL);
    h = xaot_hash_fold_source_path_list(h, "shared", NULL);
#endif
#ifdef XRT_SOURCE_INCLUDE_DIR
    h = xaot_hash_fold_source_path_list(h, "include", XRT_SOURCE_INCLUDE_DIR);
#else
    h = xaot_hash_fold_source_path_list(h, "include", NULL);
#endif
    cached_key = h;
    cached = true;
    return cached_key;
}

static uint64_t xaot_hash_fold_file_stat(uint64_t h, const char *path) {
    XrFsStat st;
    int ok;

    h = xaot_hash_fold_str(h, path);
    ok = (path && path[0] && xr_fs_stat(path, &st) == 0) ? 1 : 0;
    h = xaot_hash_fold(h, &ok, sizeof(ok));
    if (ok) {
        h = xaot_hash_fold(h, &st.kind, sizeof(st.kind));
        h = xaot_hash_fold(h, &st.size, sizeof(st.size));
        h = xaot_hash_fold(h, &st.mtime_ns, sizeof(st.mtime_ns));
    }
    return h;
}

static bool xaot_link_value_is_path(const char *value) {
    size_t len;

    if (!value)
        return false;
    len = strlen(value);
    return strchr(value, '/') || (len > 2 && strcmp(value + len - 2, ".o") == 0) ||
           (len > 2 && strcmp(value + len - 2, ".a") == 0) ||
           (len > 4 && strcmp(value + len - 4, ".lib") == 0);
}

static void xaot_runtime_archive_path(const char *lib_dir, const char *name, char *out,
                                      size_t out_sz) {
#ifdef XR_OS_WINDOWS
    snprintf(out, out_sz, "%s/lib%s.a", lib_dir ? lib_dir : "", name ? name : "");
#else
    snprintf(out, out_sz, "%s/lib%s.a", lib_dir ? lib_dir : "", name ? name : "");
#endif
}

static uint64_t xaot_hash_fold_link_dependency_stats(uint64_t h, const XaotLinkManifest *manifest,
                                                     const char *sysroot) {
    char lib_dir[XR_PATH_MAX];
    char dep[XR_PATH_MAX];
    const char *resolved_lib_dir = resolve_xray_lib_path(sysroot, lib_dir, sizeof(lib_dir));

    if (!manifest)
        return h;

    h = xaot_hash_fold_str(h, resolved_lib_dir);
    for (uint32_t i = 0; i < manifest->n_runtime_objects; i++) {
        const char *value = manifest->runtime_objects[i];
        if (xaot_link_value_is_path(value)) {
            h = xaot_hash_fold_file_stat(h, value);
        } else {
            xaot_runtime_archive_path(resolved_lib_dir, value, dep, sizeof(dep));
            h = xaot_hash_fold_file_stat(h, dep);
        }
    }
    for (uint32_t i = 0; i < manifest->n_stdlib_objects; i++) {
        const char *value = manifest->stdlib_objects[i];
        if (xaot_link_value_is_path(value))
            h = xaot_hash_fold_file_stat(h, value);
    }
    if (xaot_cli_manifest_needs_aot_core(manifest)) {
        xaot_runtime_archive_path(resolved_lib_dir, "xray_aot_core", dep, sizeof(dep));
        h = xaot_hash_fold_file_stat(h, dep);
    }
    return h;
}

/* Cache key = content hash of the generated C plus everything that changes the
 * resulting object: optimization level, target, toolchain, sysroot, and every
 * preprocessor define / cc flag carried by the link manifest (which already
 * folds in sanitizer, cpu, and debug flags). */
static uint64_t xaot_object_cache_key(const char *c_source, const char *opt_flag,
                                      const XrCliBuildTarget *target,
                                      const XrCliToolchainPlan *plan,
                                      const XaotLinkManifest *manifest, const char *sysroot) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = xaot_hash_fold_str(h, "xaot-obj-cache-v2");
    h = xaot_hash_fold(h, &(uint64_t) {xaot_aot_runtime_source_key()}, sizeof(uint64_t));
    h = xaot_hash_fold_str(h, opt_flag);
    if (target) {
        h = xaot_hash_fold_str(h, target->name);
        h = xaot_hash_fold_str(h, target->zig_triple);
        h = xaot_hash_fold(h, &target->is_native, sizeof(target->is_native));
    }
    if (plan) {
        h = xaot_hash_fold_str(h, plan->program);
        h = xaot_hash_fold(h, &plan->kind, sizeof(plan->kind));
    }
    h = xaot_hash_fold_str(h, sysroot);
    if (manifest) {
        for (uint32_t i = 0; i < manifest->n_defines; i++)
            h = xaot_hash_fold_str(h, manifest->defines[i]);
        for (uint32_t i = 0; i < manifest->n_cc_flags; i++)
            h = xaot_hash_fold_str(h, manifest->cc_flags[i]);
    }
    h = xaot_hash_fold_str(h, c_source);
    return h;
}

static uint64_t xaot_link_output_cache_key(const XaotBuildResult *result, const char *opt_flag,
                                           const XrCliBuildTarget *target,
                                           const XrCliToolchainPlan *plan, const char *sysroot,
                                           bool strip_symbols, bool shared_library) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;

    h = xaot_hash_fold_str(h, "xaot-link-output-cache-v2");
    h = xaot_hash_fold(h, &(uint64_t) {xaot_aot_runtime_source_key()}, sizeof(uint64_t));
    h = xaot_hash_fold_str(h, opt_flag);
    h = xaot_hash_fold_bool(h, strip_symbols);
    h = xaot_hash_fold_bool(h, shared_library);
    h = xaot_hash_fold_str(h, getenv("XRAY_INCLUDE"));
    h = xaot_hash_fold_str(h, getenv("XRAY_LIB"));
    h = xaot_hash_fold_str(h, getenv("XRAY_OPENSSL_LIBDIR"));
    if (target) {
        h = xaot_hash_fold_str(h, target->name);
        h = xaot_hash_fold_str(h, target->zig_triple);
        h = xaot_hash_fold_bool(h, target->is_native);
    }
    if (plan) {
        h = xaot_hash_fold_str(h, plan->program);
        h = xaot_hash_fold(h, &plan->kind, sizeof(plan->kind));
    }
    h = xaot_hash_fold_str(h, sysroot);
    if (result) {
        const XaotLinkManifest *manifest = &result->link_manifest;
        h = xaot_hash_fold_string_list(h, manifest->runtime_caps, manifest->n_runtime_caps);
        h = xaot_hash_fold_string_list(h, manifest->runtime_objects, manifest->n_runtime_objects);
        h = xaot_hash_fold_string_list(h, manifest->stdlib_objects, manifest->n_stdlib_objects);
        h = xaot_hash_fold_string_list(h, manifest->generated_c_files,
                                       manifest->n_generated_c_files);
        h = xaot_hash_fold_string_list(h, manifest->system_libs, manifest->n_system_libs);
        h = xaot_hash_fold_string_list(h, manifest->defines, manifest->n_defines);
        h = xaot_hash_fold_string_list(h, manifest->cc_flags, manifest->n_cc_flags);
        h = xaot_hash_fold_string_list(h, manifest->ld_flags, manifest->n_ld_flags);
        h = xaot_hash_fold_link_dependency_stats(h, manifest, sysroot);
        h = xaot_hash_fold(h, &result->n_sources, sizeof(result->n_sources));
        for (int i = 0; i < result->n_sources; i++) {
            h = xaot_hash_fold_str(h, result->sources[i].name);
            h = xaot_hash_fold_str(h, result->sources[i].c_source);
        }
    }
    return h;
}

/* Create directory `path` and all missing parents (like `mkdir -p`). */
static int xaot_mkdir_p(const char *path) {
    char buf[XR_PATH_MAX];
    size_t len;

    if (!path || !path[0])
        return 0;
    len = strlen(path);
    if (len >= sizeof(buf))
        return -1;
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (xr_fs_mkdir(buf, 0755) != 0 && !xr_fs_is_dir(buf))
            return -1;
        *p = '/';
    }
    if (xr_fs_mkdir(buf, 0755) != 0 && !xr_fs_is_dir(buf))
        return -1;
    return 0;
}

static int xaot_copy_file(const char *src, const char *dst, unsigned int mode) {
    FILE *in;
    FILE *out;
    char buf[64 * 1024];
    size_t n;
    int rc = 0;

    in = fopen(src, "rb");
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in))
        rc = -1;
    if (fclose(out) != 0)
        rc = -1;
    fclose(in);
    if (rc != 0) {
        xr_fs_remove(dst);
        return -1;
    }
    (void) XR_CLI_CHMOD(dst, (int) mode);
    return 0;
}

static int xaot_link_output_cache_path(const char *cache_dir, uint64_t key, char *out,
                                       size_t out_sz) {
    char bin_dir[XR_PATH_MAX];
    int n;

    n = snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cache_dir ? cache_dir : "");
    if (n < 0 || (size_t) n >= sizeof(bin_dir))
        return -1;
    if (xaot_mkdir_p(bin_dir) != 0)
        return -1;
    n = snprintf(out, out_sz, "%s/%016llx.bin", bin_dir, (unsigned long long) key);
    return (n >= 0 && (size_t) n < out_sz) ? 0 : -1;
}

static int xaot_restore_link_output_cache(const char *cache_dir, uint64_t key, const char *output,
                                          bool verbose) {
    char cached[XR_PATH_MAX];

    if (xaot_link_output_cache_path(cache_dir, key, cached, sizeof(cached)) != 0)
        return 0;
    if (!xr_fs_is_file(cached))
        return 0;
    if (xaot_copy_file(cached, output, 0755) != 0)
        return -1;
    if (verbose)
        printf("[xi-native] output cache hit: %016llx\n", (unsigned long long) key);
    return 1;
}

static void xaot_store_link_output_cache(const char *cache_dir, uint64_t key, const char *output) {
    char cached[XR_PATH_MAX];
    char tmp[XR_PATH_MAX];
    int n;

    if (xaot_link_output_cache_path(cache_dir, key, cached, sizeof(cached)) != 0)
        return;
    n = snprintf(tmp, sizeof(tmp), "%s.%d.tmp", cached, (int) xr_proc_self_pid());
    if (n < 0 || (size_t) n >= sizeof(tmp))
        return;
    if (xaot_copy_file(output, tmp, 0755) != 0)
        return;
    if (xr_fs_rename(tmp, cached) != 0)
        xr_fs_remove(tmp);
}

static void xaot_dirname(const char *path, char *out, size_t out_sz) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    size_t len;
    if (!slash) {
        snprintf(out, out_sz, "%s", ".");
        return;
    }
    if (slash == path) {
        snprintf(out, out_sz, "%s", "/");
        return;
    }
    len = (size_t) (slash - path);
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

/* Resolve the object cache directory, then /aot/<target>.  Precedence:
 * --cache-dir flag, then $XRAY_CACHE_DIR, then <output dir>/.xray-cache.
 * Created on demand.  Returns 0 on success. */
static int xaot_resolve_cache_dir(const char *output_file, const XrCliBuildTarget *target,
                                  const char *cache_dir_arg, char *out, size_t out_sz) {
    const char *env_cache = getenv("XRAY_CACHE_DIR");
    char base[XR_PATH_MAX];

    if (cache_dir_arg && cache_dir_arg[0]) {
        snprintf(base, sizeof(base), "%s", cache_dir_arg);
    } else if (env_cache && env_cache[0]) {
        snprintf(base, sizeof(base), "%s", env_cache);
    } else {
        char outdir[XR_PATH_MAX];
        xaot_dirname(output_file, outdir, sizeof(outdir));
        snprintf(base, sizeof(base), "%s/.xray-cache", outdir);
    }
    snprintf(out, out_sz, "%s/aot/%s", base, (target && target->name) ? target->name : "native");
    return xaot_mkdir_p(out);
}

/* Compile one module's generated C to an object file, reusing a cached object
 * when the content hash already exists.  Fills `obj_out` with the object path
 * (which the caller then links).  Returns 0 on success. */
static int xaot_compile_source_cached(const XrCliToolchainPlan *plan,
                                      const XrCliBuildTarget *target,
                                      const XaotLinkManifest *manifest, const char *opt_flag,
                                      const XaotModuleSource *src, const char *cache_dir,
                                      const char *sysroot, bool dump_command, bool keep_c,
                                      bool verbose, bool force_rebuild, bool dry_run, char *obj_out,
                                      size_t obj_out_sz) {
    uint64_t key = xaot_object_cache_key(src->c_source, opt_flag, target, plan, manifest, sysroot);
    char tmp_c[XR_PATH_MAX];
    char tmp_o[XR_PATH_MAX];
    int ret;

    snprintf(obj_out, obj_out_sz, "%s/%016llx.o", cache_dir, (unsigned long long) key);
    if (dry_run) {
        if (verbose)
            printf("[xi-native] link-plan object: %s (%016llx)\n", src->name ? src->name : "?",
                   (unsigned long long) key);
        return 0;
    }
    if (!force_rebuild && xr_fs_is_file(obj_out)) {
        if (verbose)
            printf("[xi-native] cache hit: %s (%016llx)\n", src->name ? src->name : "?",
                   (unsigned long long) key);
        return 0;
    }

    snprintf(tmp_c, sizeof(tmp_c), "%s/%016llx.%d.c", cache_dir, (unsigned long long) key,
             (int) xr_proc_self_pid());
    snprintf(tmp_o, sizeof(tmp_o), "%s/%016llx.%d.o.tmp", cache_dir, (unsigned long long) key,
             (int) xr_proc_self_pid());

    FILE *f = fopen(tmp_c, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", tmp_c);
        return 1;
    }
    fputs(src->c_source, f);
    fclose(f);

    if (verbose)
        printf("[xi-native] compiling: %s (%016llx)\n", src->name ? src->name : "?",
               (unsigned long long) key);
    ret = invoke_aot_manifest_compile(plan, target, manifest, opt_flag, tmp_c, tmp_o, sysroot,
                                      dump_command);
    if (ret == 0 && xr_fs_rename(tmp_o, obj_out) != 0) {
        fprintf(stderr, "Error: cannot install cached object '%s'\n", obj_out);
        xr_fs_remove(tmp_o);
        ret = 1;
    }
    if (keep_c)
        printf("Kept C source: %s\n", tmp_c);
    else
        xr_fs_remove(tmp_c);
    return ret;
}

static int xaot_write_c_export_header(const XaotBuildResult *result, const char *path) {
    FILE *f;

    if (!path || !path[0])
        return 0;
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return 1;
    }
    fputs(result && result->c_export_header ? result->c_export_header
                                            : "/* No @c_export symbols. */\n",
          f);
    if (fclose(f) != 0) {
        fprintf(stderr, "Error: failed to write '%s'\n", path);
        return 1;
    }
    printf("Generated header: %s\n", path);
    return 0;
}

static bool xaot_cli_fast_test_build_enabled(void) {
    const char *flag = getenv("XRAY_AOT_FAST_TEST_BUILD");
    return flag && flag[0] && strcmp(flag, "0") != 0;
}

static int xaot_write_temp_c_source(const char *cache_dir, uint64_t key,
                                    const XaotModuleSource *src, char *out, size_t out_sz) {
    int n;
    FILE *f;

    n = snprintf(out, out_sz, "%s/%016llx.%d.fast-test.c", cache_dir ? cache_dir : "",
                 (unsigned long long) key, (int) xr_proc_self_pid());
    if (n < 0 || (size_t) n >= out_sz)
        return 1;
    f = fopen(out, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", out);
        return 1;
    }
    if (fputs(src && src->c_source ? src->c_source : "", f) < 0 || fclose(f) != 0) {
        fprintf(stderr, "Error: failed to write '%s'\n", out);
        xr_fs_remove(out);
        return 1;
    }
    return 0;
}

static bool xaot_cli_fast_test_direct_link_allowed(const XaotLinkManifest *manifest) {
    return manifest && !xaot_link_manifest_needs_runtime(manifest) &&
           !xaot_cli_manifest_needs_aot_core(manifest) && manifest->n_runtime_objects == 0 &&
           manifest->n_stdlib_objects == 0;
}

static int cmd_build_native(const char *input, const char *output, const char *cc,
                            const char *opt_flag, const char *cpu, bool c_only, bool strip,
                            bool debug_symbols, bool shared_library, XrCliBuildProfile profile,
                            const char *sysroot, bool verbose, bool dump_xaot_plan,
                            bool dump_link_manifest, bool dump_link_command, bool dry_run_link,
                            const char *c_header, bool keep_c, const char *cache_dir_arg,
                            bool rebuild, bool lto, const XrCliBuildTarget *target,
                            const XrCliToolchainPlan *toolchain_plan) {
    XaotBuildResult aot_result;
    XaotBuildProfile aot_profile = profile == XR_CLI_BUILD_PROFILE_FREESTANDING
                                       ? XAOT_BUILD_PROFILE_FREESTANDING
                                       : XAOT_BUILD_PROFILE_HOSTED;
    int rc = xaot_build_ex(input, dump_xaot_plan, !shared_library, aot_profile, &aot_result);
    if (rc != 0)
        return rc;
    if (target && target->name) {
        XaotTarget link_target;
        if (!xaot_target_init(&link_target, target->name) ||
            !xaot_link_manifest_set_target(&aot_result.link_manifest, &link_target)) {
            fprintf(stderr, "Error: failed to set AOT link target '%s'\n", target->name);
            xaot_target_free(&link_target);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        xaot_target_free(&link_target);
    }
    {
        char normalize_err[512];
        if (!xaot_cli_normalize_manifest_for_target(&aot_result.link_manifest, target,
                                                    normalize_err, sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        if (!xaot_cli_add_build_sanitizer_flags(&aot_result.link_manifest, target, normalize_err,
                                                sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        if (profile == XR_CLI_BUILD_PROFILE_FREESTANDING &&
            !xaot_cli_apply_freestanding_profile(&aot_result.link_manifest, normalize_err,
                                                 sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        /* Whole-program LTO: compile each module to bitcode and let the linker
         * inline across modules.  The per-module bitcode stays content-addressed
         * (the cache key folds in cc flags), so only changed modules recompile
         * while every link re-runs cross-module optimization. */
        if (lto && (!xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_CC_FLAG,
                                                   "-flto") ||
                    !xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_LD_FLAG,
                                                   "-flto"))) {
            fprintf(stderr, "Error: failed to add LTO flags to AOT link manifest\n");
            xaot_build_result_free(&aot_result);
            return 1;
        }
        if (shared_library &&
            !xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_CC_FLAG, "-fPIC")) {
            fprintf(stderr, "Error: failed to add shared-library PIC flag to AOT link manifest\n");
            xaot_build_result_free(&aot_result);
            return 1;
        }
        if (debug_symbols && !xaot_cli_add_build_debug_flags(
                                 &aot_result.link_manifest, normalize_err, sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
    }
    if (cpu && cpu[0]) {
        char cpu_flag[128];
        /* ARM toolchains reject -march=native; -mcpu is the tuning knob there. */
#if defined(XR_ARCH_ARM64) || defined(XR_ARCH_ARM)
        snprintf(cpu_flag, sizeof(cpu_flag), "-mcpu=%s", cpu);
#else
        snprintf(cpu_flag, sizeof(cpu_flag), "-march=%s", cpu);
#endif
        if (!xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_CC_FLAG,
                                           cpu_flag)) {
            fprintf(stderr, "Error: failed to record --cpu flag in AOT link manifest\n");
            xaot_build_result_free(&aot_result);
            return 1;
        }
    }
    if (shared_library && xaot_link_manifest_needs_runtime(&aot_result.link_manifest)) {
        fprintf(stderr, "Error: --shared does not support runtime-backed features yet; export "
                        "scalar/raw-pointer functions or build an executable\n");
        xaot_build_result_free(&aot_result);
        return 1;
    }
    if (dump_xaot_plan && aot_result.plan_dump) {
        printf("%s", aot_result.plan_dump);
        if (aot_result.plan_dump[0] &&
            aot_result.plan_dump[strlen(aot_result.plan_dump) - 1] != '\n')
            printf("\n");
    }
    if (dump_link_manifest) {
        char *manifest_json = xaot_link_manifest_dump_json(&aot_result.link_manifest);
        if (!manifest_json) {
            fprintf(stderr, "Error: failed to dump AOT link manifest\n");
            xaot_build_result_free(&aot_result);
            return 1;
        }
        printf("%s", manifest_json);
        if (manifest_json[0] && manifest_json[strlen(manifest_json) - 1] != '\n')
            printf("\n");
        xr_free(manifest_json);
    }
    if (verbose) {
        print_aot_prepare_stats(&aot_result);
        print_aot_codegen_stats(&aot_result);
        print_aot_coro_frame_stats(&aot_result);
    }

    int n_sources = aot_result.n_sources;
    if (n_sources <= 0 || !aot_result.sources) {
        fprintf(stderr, "Error: AOT build produced no C sources\n");
        xaot_build_result_free(&aot_result);
        return 1;
    }

    if (xaot_write_c_export_header(&aot_result, c_header) != 0) {
        xaot_build_result_free(&aot_result);
        return 1;
    }

    /* --c-only: emit the generated C for inspection.  A single-module program
     * is one translation unit; a multi-module program is written as the
     * concatenation of its per-module units, each labeled. */
    if (c_only) {
        FILE *f = fopen(output, "w");
        if (!f) {
            fprintf(stderr, "Error: cannot create '%s'\n", output);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        for (int i = 0; i < n_sources; i++) {
            if (n_sources > 1)
                fprintf(f, "/* ==== module: %s ==== */\n",
                        aot_result.sources[i].name ? aot_result.sources[i].name : "?");
            fputs(aot_result.sources[i].c_source, f);
            fputc('\n', f);
        }
        fclose(f);
        printf("Generated: %s\n", output);
        xaot_build_result_free(&aot_result);
        return 0;
    }

    (void) cc;
#ifdef XR_OS_MACOS
    bool build_dsym = debug_symbols && !strip && target && target->is_native;
#else
    bool build_dsym = false;
#endif

    /* Resolve the per-module object cache, then compile each generated C unit
     * to an object (reusing cached objects on content-hash hit) and link the
     * whole set together. */
    char cache_dir[XR_PATH_MAX];
    if (xaot_resolve_cache_dir(output, target, cache_dir_arg, cache_dir, sizeof(cache_dir)) != 0) {
        fprintf(stderr, "Error: cannot create object cache directory '%s'\n", cache_dir);
        xaot_build_result_free(&aot_result);
        return 1;
    }

    bool use_link_output_cache = !rebuild && !dry_run_link && !dump_link_command && !verbose &&
                                 !debug_symbols && !shared_library && !keep_c;
    uint64_t link_output_cache_key = 0;
    if (use_link_output_cache) {
        link_output_cache_key = xaot_link_output_cache_key(
            &aot_result, opt_flag, target, toolchain_plan, sysroot, strip, shared_library);
        int cache_hit =
            xaot_restore_link_output_cache(cache_dir, link_output_cache_key, output, verbose);
        if (cache_hit > 0) {
            printf("Generated: %s\n", output);
            xaot_build_result_free(&aot_result);
            return 0;
        }
    }

    if (xaot_cli_fast_test_build_enabled() && n_sources == 1 &&
        xaot_cli_fast_test_direct_link_allowed(&aot_result.link_manifest) && !rebuild &&
        !dry_run_link && !debug_symbols && !shared_library) {
        char c_file[XR_PATH_MAX];
        const char *inputs[1];
        int ret;
        uint64_t key =
            link_output_cache_key
                ? link_output_cache_key
                : xaot_link_output_cache_key(&aot_result, opt_flag, target, toolchain_plan, sysroot,
                                             strip, shared_library);
        if (xaot_write_temp_c_source(cache_dir, key, &aot_result.sources[0], c_file,
                                     sizeof(c_file)) != 0) {
            xaot_build_result_free(&aot_result);
            return 1;
        }
        inputs[0] = c_file;
        ret = invoke_aot_manifest_link(toolchain_plan, target, &aot_result.link_manifest, opt_flag,
                                       output, inputs, 1, strip, shared_library, sysroot,
                                       dump_link_command || verbose, false);
        if (keep_c)
            printf("Kept C source: %s\n", c_file);
        else
            xr_fs_remove(c_file);
#ifdef XR_OS_MACOS
        if (ret == 0 && strip)
            remove_dsym_bundle(output);
#endif
        if (ret == 0 && use_link_output_cache)
            xaot_store_link_output_cache(cache_dir, link_output_cache_key, output);
        if (ret == 0)
            printf("Generated: %s\n", output);
        xaot_build_result_free(&aot_result);
        return ret;
    }

    char (*obj_bufs)[XR_PATH_MAX] =
        (char (*)[XR_PATH_MAX]) xr_calloc((size_t) n_sources, XR_PATH_MAX);
    const char **obj_ptrs = (const char **) xr_calloc((size_t) n_sources, sizeof(char *));
    if (!obj_bufs || !obj_ptrs) {
        xr_free(obj_bufs);
        xr_free(obj_ptrs);
        xaot_build_result_free(&aot_result);
        return 1;
    }

    int ret = 0;
    for (int i = 0; i < n_sources; i++) {
        ret = xaot_compile_source_cached(toolchain_plan, target, &aot_result.link_manifest,
                                         opt_flag, &aot_result.sources[i], cache_dir, sysroot,
                                         dump_link_command || verbose, keep_c, verbose, rebuild,
                                         dry_run_link, obj_bufs[i], XR_PATH_MAX);
        if (ret != 0)
            break;
        obj_ptrs[i] = obj_bufs[i];
    }

    if (ret == 0)
        ret = invoke_aot_manifest_link(toolchain_plan, target, &aot_result.link_manifest, opt_flag,
                                       output, obj_ptrs, n_sources, strip, shared_library, sysroot,
                                       dump_link_command || verbose || dry_run_link, dry_run_link);
#ifdef XR_OS_MACOS
    if (ret == 0 && build_dsym && !dry_run_link)
        ret = invoke_dsymutil(output, dump_link_command || verbose);
#else
    (void) build_dsym;
#endif

    xr_free(obj_bufs);
    xr_free(obj_ptrs);
#ifdef XR_OS_MACOS
    if (ret == 0 && strip)
        remove_dsym_bundle(output);
#endif
    if (ret == 0 && use_link_output_cache)
        xaot_store_link_output_cache(cache_dir, link_output_cache_key, output);
    if (ret == 0 && !dry_run_link)
        printf("Generated: %s\n", output);
    xaot_build_result_free(&aot_result);
    return ret;
}
