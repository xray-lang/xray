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
#include "../toolchain/xtc_config.h"
#include "../toolchain/xtc_command.h"
#include "../toolchain/xtc_discovery.h"
#include "../toolchain/xtc_model.h"
#include "../toolchain/xtc_probe.h"
#include "../toolchain/xtc_target_profile.h"
#include "../../api/xisolate_profile.h"
#include "xray.h"
#include "xray_vm.h"
#include "../../module/xbundle.h"
#include "../../module/xproject.h"
#include "../../module/xlockfile.h"
#include "../../aot/xaot_driver.h"
#include "../../ir/xi_arc_verify.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../os/os_dir.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"
#ifdef XR_OS_WINDOWS
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
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

static bool build_artifact_kind_parse(const char *name, XaotArtifactKind *out, char *err,
                                      size_t err_size) {
    if (!out) {
        if (err && err_size > 0)
            snprintf(err, err_size, "internal error: missing artifact kind output");
        return false;
    }
    if (!name || name[0] == '\0' || strcmp(name, "executable") == 0) {
        *out = XAOT_ARTIFACT_EXECUTABLE;
        return true;
    }
    if (strcmp(name, "shared-library") == 0) {
        *out = XAOT_ARTIFACT_SHARED_LIBRARY;
        return true;
    }
    if (strcmp(name, "hosted-fragment") == 0) {
        *out = XAOT_ARTIFACT_HOSTED_FRAGMENT;
        return true;
    }
    if (err && err_size > 0) {
        snprintf(err, err_size,
                 "unknown artifact kind '%s' (expected 'executable', 'shared-library', or "
                 "'hosted-fragment')",
                 name);
    }
    return false;
}

static bool build_c_dialect_parse(const char *name, XiCgenCDialect *out, char *err,
                                  size_t err_size) {
    if (!out) {
        if (err && err_size > 0)
            snprintf(err, err_size, "internal error: missing C dialect output");
        return false;
    }
    if (!name || name[0] == '\0' || strcmp(name, "c11") == 0) {
        *out = XI_CGEN_C_DIALECT_C11;
        return true;
    }
    if (strcmp(name, "c90") == 0) {
        *out = XI_CGEN_C_DIALECT_C90;
        return true;
    }
    if (err && err_size > 0)
        snprintf(err, err_size, "unknown C dialect '%s' (expected 'c11' or 'c90')", name);
    return false;
}

static bool build_c90_target_supported(const XrToolchainTarget *target) {
    if (!target || target->pointer_bits != 64)
        return false;
    return target->os == XR_TOOLCHAIN_TARGET_OS_LINUX ||
           target->os == XR_TOOLCHAIN_TARGET_OS_DARWIN;
}

static bool build_type_name_profile_parse(const char *name, XrCliBuildProfile build_profile,
                                          XiCgenTypeNameProfile *out, char *err, size_t err_size) {
    if (!out) {
        if (err && err_size > 0)
            snprintf(err, err_size, "internal error: missing type-name profile output");
        return false;
    }
    if (!name || name[0] == '\0') {
        *out = build_profile == XR_CLI_BUILD_PROFILE_FREESTANDING ? XI_CGEN_TYPE_NAMES_NONE
                                                                  : XI_CGEN_TYPE_NAMES_ALL;
        return true;
    }
    if (strcmp(name, "none") == 0) {
        *out = XI_CGEN_TYPE_NAMES_NONE;
        return true;
    }
    if (strcmp(name, "public") == 0) {
        *out = XI_CGEN_TYPE_NAMES_PUBLIC;
        return true;
    }
    if (strcmp(name, "all") == 0) {
        *out = XI_CGEN_TYPE_NAMES_ALL;
        return true;
    }
    if (err && err_size > 0) {
        snprintf(err, err_size,
                 "unknown type-name profile '%s' (expected 'none', 'public', or 'all')", name);
    }
    return false;
}

static bool build_path_is_absolute(const char *path) {
    if (!path || !path[0])
        return false;
    if (path[0] == '/' || path[0] == '\\')
        return true;
    return strlen(path) > 2 && path[1] == ':';
}

static bool build_dirname_in_place(char *path) {
    char *slash;
    char *backslash;
    char *sep;

    if (!path || !path[0])
        return false;
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    sep = slash;
    if (backslash && (!sep || backslash > sep))
        sep = backslash;
    if (!sep)
        return false;
    if (sep == path)
        sep[1] = '\0';
    else
        *sep = '\0';
    return true;
}

/* Resolve <prefix> from <prefix>/bin/xray only when the private AOT SDK is
 * present.  Development-tree binaries therefore keep using their configured
 * source/build paths, while copied release binaries become fully relocatable. */
static bool build_resolve_install_prefix(char *out, size_t out_size) {
    char exe[XR_PATH_MAX];
    char sdk[XR_PATH_MAX];
    int written;

    if (!out || out_size == 0 || xr_proc_self_exe_path(exe, sizeof(exe)) != 0)
        return false;
    if (!build_dirname_in_place(exe) || !build_dirname_in_place(exe))
        return false;
    written = snprintf(sdk, sizeof(sdk), "%s/lib/xray/sdk/src/aot", exe);
    if (written < 0 || (size_t) written >= sizeof(sdk) || !xr_fs_is_dir(sdk))
        return false;
    written = snprintf(out, out_size, "%s", exe);
    return written >= 0 && (size_t) written < out_size;
}

static bool build_join_project_path(const char *root, const char *path, char *out,
                                    size_t out_size) {
    int written;

    if (!path || !path[0] || !out || out_size == 0)
        return false;
    if (build_path_is_absolute(path) || !root || !root[0]) {
        written = snprintf(out, out_size, "%s", path);
    } else {
        written = snprintf(out, out_size, "%s/%s", root, path);
    }
    return written >= 0 && (size_t) written < out_size;
}

static const char *build_config_string(const XrCliOptionMap *opts, const char *opt_name,
                                       const char *configured, const char *fallback) {
    if (opts && xr_cli_opt_present(opts, opt_name))
        return xr_cli_opt_string(opts, opt_name, fallback);
    return (configured && configured[0]) ? configured : fallback;
}

/* ========== Shared Helpers ========== */

// Invoke C compiler to link generated C source (+ optional .o) into executable
static int invoke_cc(const char *cc, const char *opt_flag, const char *output_file,
                     const char *c_file, const char *obj_file, bool strip_symbols,
                     bool debug_symbols, const char *sysroot) {
    const char *xray_include = getenv("XRAY_INCLUDE");
    const char *xray_lib = getenv("XRAY_LIB");

    char include_path[512], lib_path[512], install_prefix[XR_PATH_MAX];
    if (sysroot) {
        snprintf(include_path, sizeof(include_path), "%s/include/xray", sysroot);
        snprintf(lib_path, sizeof(lib_path), "%s/lib", sysroot);
        xray_include = include_path;
        xray_lib = lib_path;
    } else {
        if ((!xray_include || !xray_lib) &&
            build_resolve_install_prefix(install_prefix, sizeof(install_prefix))) {
            if (!xray_include) {
                snprintf(include_path, sizeof(include_path), "%s/include/xray", install_prefix);
                xray_include = include_path;
            }
            if (!xray_lib) {
                snprintf(lib_path, sizeof(lib_path), "%s/lib", install_prefix);
                xray_lib = lib_path;
            }
        }
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
    /* xray_core embeds the VM's libffi-based extern invoker (xvm_ffi.c), so a
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

static void resolve_aot_include_paths(const XrToolchainSelection *plan, const char *sysroot,
                                      char *aot_include, size_t aot_include_sz,
                                      char *runtime_include, size_t runtime_include_sz) {
    const char *xray_include = getenv("XRAY_INCLUDE");
    char install_prefix[XR_PATH_MAX];

    if (aot_include && aot_include_sz > 0)
        aot_include[0] = '\0';
    if (runtime_include && runtime_include_sz > 0)
        runtime_include[0] = '\0';

    if (!sysroot && plan && plan->private_aot_include[0] && plan->public_include[0]) {
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s", plan->private_aot_include);
        if (runtime_include && runtime_include_sz > 0)
            snprintf(runtime_include, runtime_include_sz, "%s", plan->public_include);
    } else if (sysroot) {
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s/include/xray", sysroot);
        if (runtime_include && runtime_include_sz > 0)
            snprintf(runtime_include, runtime_include_sz, "%s/include/xray", sysroot);
    } else if (xray_include) {
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s", xray_include);
        if (runtime_include && runtime_include_sz > 0)
            snprintf(runtime_include, runtime_include_sz, "%s", xray_include);
    } else if (build_resolve_install_prefix(install_prefix, sizeof(install_prefix))) {
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s/lib/xray/sdk/src/aot", install_prefix);
        if (runtime_include && runtime_include_sz > 0)
            snprintf(runtime_include, runtime_include_sz, "%s/include/xray", install_prefix);
    } else {
#ifdef XRT_AOT_INCLUDE_DIR
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s", XRT_AOT_INCLUDE_DIR);
#else
        if (aot_include && aot_include_sz > 0)
            snprintf(aot_include, aot_include_sz, "%s", "/usr/local/include/xray");
#endif
#ifdef XRT_SOURCE_INCLUDE_DIR
        if (runtime_include && runtime_include_sz > 0)
            snprintf(runtime_include, runtime_include_sz, "%s", XRT_SOURCE_INCLUDE_DIR);
#endif
    }
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

static bool xaot_cli_command_sink_add(void *context, const char *arg, char *err, size_t err_size) {
    return xaot_cli_link_add_arg((XaotCliLinkCommand *) context, arg, err, err_size);
}

static bool xaot_cli_command_sink_add_joined(void *context, const char *prefix, const char *value,
                                             char *err, size_t err_size) {
    return xaot_cli_link_add_prefixed((XaotCliLinkCommand *) context, prefix, value, err, err_size);
}

static XrToolchainArgSink xaot_cli_command_sink(XaotCliLinkCommand *cmd) {
    XrToolchainArgSink sink;
    sink.context = cmd;
    sink.add = xaot_cli_command_sink_add;
    sink.add_joined = xaot_cli_command_sink_add_joined;
    return sink;
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

static const char *xaot_verified_runtime_path(const XrToolchainSelection *plan,
                                              const char *logical_name) {
    const char *id_prefix = NULL;
    if (!plan || !logical_name)
        return NULL;
    if (strcmp(logical_name, "xray_rt_coro") == 0)
        id_prefix = "xray-rt-coro-";
    else if (strcmp(logical_name, "xray_aot_core") == 0)
        id_prefix = "xray-aot-core-";
    else
        return NULL;
    size_t prefix_len = strlen(id_prefix);
    for (size_t i = 0; i < plan->runtime_count; i++) {
        if (strncmp(plan->runtime_ids[i], id_prefix, prefix_len) == 0)
            return plan->runtime_paths[i];
    }
    return NULL;
}

static bool xaot_cli_link_add_verified_runtime(XaotCliLinkCommand *cmd,
                                               const XrToolchainSelection *plan, const char *value,
                                               char *err, size_t err_size) {
    if (!value || !value[0])
        return true;
    if (strchr(value, '/') || strchr(value, '\\'))
        return xaot_cli_link_add_runtime_object(cmd, value, err, err_size);
    const char *path = xaot_verified_runtime_path(plan, value);
    if (!path) {
        snprintf(err, err_size,
                 "verified runtime manifest has no exact artifact for logical object '%s'", value);
        return false;
    }
    return xaot_cli_link_add_arg(cmd, path, err, err_size);
}

/* Whether a stdlib symbol is implemented inside the AOT core archive. This
 * decides which system libraries the link needs -- the archive's crypto and
 * compress members pull OpenSSL and zlib -- and it does NOT decide whether the
 * archive itself goes on the link line. That is unconditional: see
 * xaot_cli_link_add_aot_core. */
static bool xaot_cli_stdlib_object_uses_aot_core(const char *value) {
    return value &&
           (strncmp(value, "crypto.", 7) == 0 || strncmp(value, "cluster.", 8) == 0 ||
            strncmp(value, "regex.", 6) == 0 || strcmp(value, "compress.deflate") == 0 ||
            strcmp(value, "compress.gunzip") == 0 || strcmp(value, "compress.gzip") == 0 ||
            strcmp(value, "compress.inflate") == 0 || strcmp(value, "compress.isGzip") == 0 ||
            strcmp(value, "compress.isZlib") == 0 || strcmp(value, "compress.zlibCompress") == 0 ||
            strcmp(value, "compress.zlibDecompress") == 0 || strcmp(value, "math.random") == 0 ||
            strcmp(value, "math.randomInt") == 0 || strcmp(value, "time.now") == 0 ||
            strcmp(value, "time.monotonic") == 0 || strcmp(value, "time.nanos") == 0 ||
            strcmp(value, "time.micros") == 0 || strcmp(value, "time.clock") == 0);
}

static bool xaot_cli_stdlib_object_uses_aot_core_for_target(const char *value,
                                                            const XrToolchainTarget *target) {
    if (target && !target->is_native && value && strncmp(value, "time.", 5) == 0)
        return false;
    return xaot_cli_stdlib_object_uses_aot_core(value);
}

static bool xaot_cli_link_add_stdlib_object(XaotCliLinkCommand *cmd, const char *value, char *err,
                                            size_t err_size) {
    size_t len;

    if (!value || !value[0] || xaot_cli_stdlib_object_uses_aot_core(value))
        return true;
    len = strlen(value);
    if (strchr(value, '/') || (len > 2 && strcmp(value + len - 2, ".o") == 0) ||
        (len > 2 && strcmp(value + len - 2, ".a") == 0) ||
        (len > 2 && strcmp(value + len - 2, ".c") == 0))
        return xaot_cli_link_add_arg(cmd, value, err, err_size);
    return true;
}

static bool xaot_cli_manifest_uses_aot_core(const XaotLinkManifest *manifest) {
    if (!manifest)
        return false;
    for (uint32_t i = 0; i < manifest->n_stdlib_objects; i++) {
        if (xaot_cli_stdlib_object_uses_aot_core(manifest->stdlib_objects[i]))
            return true;
    }
    for (uint32_t i = 0; i < manifest->n_stdlib_symbols; i++) {
        if (xaot_cli_stdlib_object_uses_aot_core(manifest->stdlib_symbols[i]))
            return true;
    }
    return false;
}

static bool xaot_cli_manifest_uses_aot_core_for_target(const XaotLinkManifest *manifest,
                                                       const XrToolchainTarget *target) {
    if (!manifest)
        return false;
    for (uint32_t i = 0; i < manifest->n_stdlib_objects; i++) {
        if (xaot_cli_stdlib_object_uses_aot_core_for_target(manifest->stdlib_objects[i], target))
            return true;
    }
    for (uint32_t i = 0; i < manifest->n_stdlib_symbols; i++) {
        if (xaot_cli_stdlib_object_uses_aot_core_for_target(manifest->stdlib_symbols[i], target))
            return true;
    }
    return false;
}

static bool xaot_cli_link_ld_flag_supported(const XrToolchainTarget *target, const char *flag) {
    if (!target || !flag)
        return true;
    if (!target->is_native && strcmp(flag, "-Wl,-dead_strip") == 0)
        return false;
    return true;
}

static bool xaot_cli_add_provider_driver_prefix(XaotCliLinkCommand *cmd,
                                                const XrToolchainSelection *plan,
                                                const XrToolchainTarget *target, char *err,
                                                size_t err_size) {
    if (!cmd || !plan || !plan->program) {
        snprintf(err, err_size, "missing verified provider command input");
        return false;
    }
    cmd->program = plan->program;
    XrToolchainArgSink sink = xaot_cli_command_sink(cmd);
    return xtc_command_emit_driver(plan, target, &sink, err, err_size);
}

static XrOptimizationLevel xaot_cli_optimization(XaotOptimizationLevel optimization) {
    switch (optimization) {
        case XAOT_OPTIMIZATION_NONE:
            return XR_OPTIMIZATION_NONE;
        case XAOT_OPTIMIZATION_BASIC:
            return XR_OPTIMIZATION_BASIC;
        case XAOT_OPTIMIZATION_SIZE:
            return XR_OPTIMIZATION_SIZE;
        case XAOT_OPTIMIZATION_RELEASE:
            return XR_OPTIMIZATION_RELEASE;
        case XAOT_OPTIMIZATION_SPEED:
            return XR_OPTIMIZATION_SPEED;
    }
    return XR_OPTIMIZATION_NONE;
}

static XrNativeSimdMode xaot_cli_native_simd(const XaotTarget *target) {
    if (!target || target->simd_mode == XAOT_SIMD_DISPATCH)
        return XR_NATIVE_SIMD_DEFAULT;
    if ((target->simd_features & XAOT_SIMD_FEATURE_AVX512) != 0)
        return XR_NATIVE_SIMD_AVX512;
    if ((target->simd_features & XAOT_SIMD_FEATURE_AVX2) != 0)
        return XR_NATIVE_SIMD_AVX2;
    if ((target->simd_features & XAOT_SIMD_FEATURE_SSE2) != 0)
        return XR_NATIVE_SIMD_SSE2;
    if ((target->simd_features & XAOT_SIMD_FEATURE_SVE) != 0)
        return XR_NATIVE_SIMD_SVE;
    if ((target->simd_features & XAOT_SIMD_FEATURE_NEON) != 0)
        return XR_NATIVE_SIMD_NEON;
    if ((target->simd_features & XAOT_SIMD_FEATURE_VSX) != 0)
        return XR_NATIVE_SIMD_VSX;
    if ((target->simd_features & XAOT_SIMD_FEATURE_LSX) != 0)
        return XR_NATIVE_SIMD_LSX;
    return XR_NATIVE_SIMD_DEFAULT;
}

static bool xaot_cli_provider_supports_x86_vector_islands(const XrToolchainSelection *plan) {
    if (!plan)
        return false;
    return plan->provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG ||
           plan->provider == XR_TOOLCHAIN_PROVIDER_LLVM_CLANG ||
           plan->provider == XR_TOOLCHAIN_PROVIDER_GCC ||
           plan->provider == XR_TOOLCHAIN_PROVIDER_ZIG;
}

static XrNativeSimdMode xaot_cli_generated_source_simd(const char *path) {
    if (!path || !path[0])
        return XR_NATIVE_SIMD_DEFAULT;
    FILE *f = fopen(path, "rb");
    if (!f)
        return XR_NATIVE_SIMD_DEFAULT;
    char prefix[4097];
    size_t n = fread(prefix, 1, sizeof(prefix) - 1, f);
    fclose(f);
    prefix[n] = '\0';
    if (strstr(prefix, "#define XR_AOT_COMPILE_SIMD_AVX512 1"))
        return XR_NATIVE_SIMD_AVX512;
    if (strstr(prefix, "#define XR_AOT_COMPILE_SIMD_AVX2 1"))
        return XR_NATIVE_SIMD_AVX2;
    return XR_NATIVE_SIMD_DEFAULT;
}

static void xaot_cli_semantic_specs(const XaotLinkManifest *manifest,
                                    const XrToolchainSelection *plan, XrNativeCompileSpec *compile,
                                    XrNativeLinkSpec *link) {
    memset(compile, 0, sizeof(*compile));
    memset(link, 0, sizeof(*link));
    compile->optimization = xaot_cli_optimization(manifest->compile.optimization);
    compile->fp_contract =
        manifest->compile.fp_contract_off ? XR_FP_CONTRACT_OFF : XR_FP_CONTRACT_DEFAULT;
    compile->debug_info = manifest->compile.debug_info ? XR_DEBUG_INFO_FULL : XR_DEBUG_INFO_NONE;
    compile->frame_pointer = manifest->compile.frame_pointer;
    compile->lto = manifest->compile.lto;
    compile->pic = manifest->compile.pic;
    compile->function_sections = manifest->compile.function_sections;
    compile->data_sections = manifest->compile.data_sections;
    compile->warnings = manifest->compile.suppress_warnings ? XR_WARNING_POLICY_SUPPRESS
                                                            : XR_WARNING_POLICY_DEFAULT;
    compile->freestanding = manifest->compile.freestanding;
    compile->disable_stack_protector = !manifest->compile.stack_protector;
    compile->disable_unwind_tables = !manifest->compile.unwind_tables;
    compile->disable_machine_outliner = manifest->compile.disable_machine_outliner;
    compile->disable_vectorization = manifest->target.simd_mode == XAOT_SIMD_SVE;
    compile->disable_slp_vectorization = manifest->target.simd_mode == XAOT_SIMD_SVE;
    compile->cpu = manifest->compile.cpu;
    /* Xray-generated translation units include the C11 runtime ABI (including
     * stdatomic.h), so every provider must compile them as C11. */
    compile->language_standard = "c11";
    compile->simd = xaot_cli_native_simd(&manifest->target);
    /* GCC-family providers can compile Xray's attributed AVX2/AVX-512
     * function islands from a baseline translation unit.  Keep MSVC's
     * /arch-based whole-unit path because cl.exe has no equivalent target
     * attribute. */
    if ((manifest->target.simd_features & (XAOT_SIMD_FEATURE_AVX2 | XAOT_SIMD_FEATURE_AVX512)) !=
            0 &&
        xaot_cli_provider_supports_x86_vector_islands(plan))
        compile->simd = XR_NATIVE_SIMD_DEFAULT;
    if (!compile->cpu[0] && compile->simd == XR_NATIVE_SIMD_SVE)
        compile->cpu = "generic+sve";
    if (!compile->cpu[0] && compile->simd == XR_NATIVE_SIMD_VSX)
        compile->cpu = "pwr8";

    link->shared = manifest->link.shared;
    link->relocatable = manifest->link.relocatable;
    link->resolve_from_host = manifest->link.resolve_from_host;
    link->strip = manifest->link.strip;
    link->dead_strip = manifest->link.dead_strip;
    link->lto = manifest->link.lto;
    link->no_standard_libraries = !manifest->link.standard_libraries;
    link->entry = manifest->link.entry[0] ? manifest->link.entry : NULL;
    link->linker_script = manifest->link.linker_script[0] ? manifest->link.linker_script : NULL;
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

static void xaot_cli_manifest_remove_string(char ***items, uint32_t *count, const char *value) {
    if (!items || !*items || !count || !value)
        return;
    uint32_t i = 0;
    while (i < *count) {
        if ((*items)[i] && strcmp((*items)[i], value) == 0) {
            xr_free((*items)[i]);
            for (uint32_t j = i + 1; j < *count; j++)
                (*items)[j - 1] = (*items)[j];
            *count = *count - 1;
            continue;
        }
        i++;
    }
}

static bool xaot_cli_normalize_manifest_for_target(XaotLinkManifest *manifest,
                                                   const XrToolchainTarget *target, char *err,
                                                   size_t err_size) {
    if (!manifest || !target)
        return true;
    manifest->compile.function_sections = true;
    manifest->compile.data_sections = true;
    /* A relocatable link preserves sections for a later final link. Apple ld
     * rejects -r with -dead_strip, and GNU linkers must not discard that
     * intermediate object's closure either. */
    manifest->link.dead_strip = !manifest->link.relocatable;
    (void) err;
    (void) err_size;
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
    manifest->compile.freestanding = true;
    manifest->compile.stack_protector = false;
    manifest->compile.unwind_tables = false;
    manifest->link.standard_libraries = false;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRAY_PROFILE_FREESTANDING=1")) {
        snprintf(err, err_size, "failed to apply freestanding build profile");
        return false;
    }
    return true;
}

static bool xaot_cli_add_linker_script(XaotLinkManifest *manifest, const char *script, char *err,
                                       size_t err_size) {
    int written;

    if (!script || !script[0])
        return true;

    written =
        snprintf(manifest->link.linker_script, sizeof(manifest->link.linker_script), "%s", script);
    if (written < 0 || (size_t) written >= sizeof(manifest->link.linker_script)) {
        snprintf(err, err_size, "linker script path is too long: %s", script);
        return false;
    }
    return true;
}

static bool xaot_cli_add_build_sanitizer_flags(XaotLinkManifest *manifest,
                                               const XrToolchainTarget *target,
                                               const XrToolchainSelection *selection, char *err,
                                               size_t err_size) {
    if (!manifest)
        return true;
    (void) selection;
    if (target && !target->is_native)
        return true;
#if (defined(XR_BUILD_ASAN) && XR_BUILD_ASAN) || (defined(XR_BUILD_UBSAN) && XR_BUILD_UBSAN) ||    \
    (defined(XR_BUILD_TSAN) && XR_BUILD_TSAN) || (defined(XR_BUILD_MSAN) && XR_BUILD_MSAN)
    if (!selection || !xtc_provider_uses_gnu_driver(selection->provider)) {
        snprintf(err, err_size, "this instrumented Xray build requires a GNU-driver provider");
        return false;
    }
    snprintf(manifest->raw_flag_provider, sizeof(manifest->raw_flag_provider), "%s",
             xtc_provider_name(selection->provider));
#endif
#if defined(XR_BUILD_ASAN) && XR_BUILD_ASAN
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fsanitize=address") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fsanitize=address")) {
        snprintf(err, err_size, "failed to add ASan flags to AOT link manifest");
        return false;
    }
#if defined(XR_OS_WINDOWS) && defined(XRT_BUILD_ASAN_WINDOWS_IMPORT) &&                            \
    defined(XRT_BUILD_ASAN_WINDOWS_THUNK)
    if (selection && selection->provider == XR_TOOLCHAIN_PROVIDER_ZIG &&
        (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-Wl,--whole-archive") ||
         !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG,
                                        XRT_BUILD_ASAN_WINDOWS_THUNK) ||
         !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-Wl,--no-whole-archive") ||
         !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG,
                                        XRT_BUILD_ASAN_WINDOWS_IMPORT))) {
        snprintf(err, err_size, "failed to add the MSVC ASan runtime to the Zig link plan");
        return false;
    }
#endif
#endif
#if defined(XR_BUILD_UBSAN) && XR_BUILD_UBSAN
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fsanitize=undefined") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fno-omit-frame-pointer") ||
        !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-fsanitize=undefined")) {
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
    manifest->compile.debug_info = true;
    manifest->compile.frame_pointer = true;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRAY_AOT_DEBUG_LOCALS=1")) {
        snprintf(err, err_size, "failed to add debug flags to AOT link manifest");
        return false;
    }
    return true;
}

static bool xaot_cli_add_target_config_flags(XaotLinkManifest *manifest,
                                             const XrTargetConfig *config,
                                             const XrToolchainSelection *selection, char *err,
                                             size_t err_size) {
    if (!manifest || !config)
        return true;
    if (config->n_cc_flags == 0 && config->n_ld_flags == 0)
        return true;
    if (!selection || !config->toolchain || !config->toolchain[0] ||
        strcmp(config->toolchain, "auto") == 0 || strcmp(config->toolchain, "host") == 0) {
        snprintf(err, err_size,
                 "raw target cc_flags/ld_flags require an explicit provider selector");
        return false;
    }
    const char *provider = xtc_provider_name(selection->provider);
    bool matches = strcmp(config->toolchain, provider) == 0 ||
                   (strcmp(config->toolchain, "clang") == 0 &&
                    (selection->provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG ||
                     selection->provider == XR_TOOLCHAIN_PROVIDER_LLVM_CLANG));
    if (!matches) {
        snprintf(err, err_size, "raw target flags are scoped to provider '%s', selected '%s'",
                 config->toolchain, provider);
        return false;
    }
    snprintf(manifest->raw_flag_provider, sizeof(manifest->raw_flag_provider), "%s", provider);
    for (int i = 0; i < config->n_cc_flags; i++) {
        const char *flag = config->cc_flags ? config->cc_flags[i] : NULL;
        if (flag && flag[0] && !xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, flag)) {
            snprintf(err, err_size, "failed to add xray.toml target cc flag '%s'", flag);
            return false;
        }
    }
    for (int i = 0; i < config->n_ld_flags; i++) {
        const char *flag = config->ld_flags ? config->ld_flags[i] : NULL;
        if (flag && flag[0] && !xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, flag)) {
            snprintf(err, err_size, "failed to add xray.toml target ld flag '%s'", flag);
            return false;
        }
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

static void xaot_cli_objcopy_print_command(const XaotCliLinkCommand *cmd) {
    xaot_cli_print_command("Objcopy command", cmd);
}

static bool xaot_cli_target_config_has_objcopy(const XrTargetConfig *config) {
    return config &&
           ((config->objcopy && config->objcopy[0]) ||
            (config->objcopy_output && config->objcopy_output[0]) || config->n_objcopy_flags > 0);
}

static bool xaot_cli_link_add_target_metadata_flags(XaotCliLinkCommand *cmd,
                                                    const XrToolchainTarget *target, char *err,
                                                    size_t err_size) {
    char value[32];

    if (!cmd || !target || target->is_native)
        return true;

    snprintf(value, sizeof(value), "%d", target->pointer_bits);
    if (!xaot_cli_link_add_prefixed(cmd, "-DXR_AOT_TARGET_PTR_BITS=", value, err, err_size))
        return false;

    if (target->endian == XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE)
        return xaot_cli_link_add_arg(cmd, "-DXR_AOT_TARGET_LITTLE_ENDIAN=1", err, err_size);
    if (target->endian == XR_TOOLCHAIN_TARGET_ENDIAN_BIG)
        return xaot_cli_link_add_arg(cmd, "-DXR_AOT_TARGET_LITTLE_ENDIAN=0", err, err_size);
    return true;
}

static bool xaot_cli_build_objcopy_command(const XrTargetConfig *config, const char *input_file,
                                           const char *output_file, XaotCliLinkCommand *cmd,
                                           char *err, size_t err_size) {
    const char *program;

    if (!config || !output_file || !output_file[0] || !cmd) {
        snprintf(err, err_size, "missing objcopy command input");
        return false;
    }
    if (!input_file || !input_file[0]) {
        snprintf(err, err_size, "missing objcopy input file");
        return false;
    }

    memset(cmd, 0, sizeof(*cmd));
    program = (config->objcopy && config->objcopy[0]) ? config->objcopy : "objcopy";
    cmd->program = program;
    if (!xaot_cli_link_add_arg(cmd, program, err, err_size))
        return false;
    for (int i = 0; i < config->n_objcopy_flags; i++) {
        const char *flag = config->objcopy_flags ? config->objcopy_flags[i] : NULL;
        if (flag && flag[0] && !xaot_cli_link_add_arg(cmd, flag, err, err_size))
            return false;
    }
    return xaot_cli_link_add_arg(cmd, input_file, err, err_size) &&
           xaot_cli_link_add_arg(cmd, output_file, err, err_size);
}

static int invoke_target_objcopy(const XrTargetConfig *config, const char *input_file,
                                 const char *output_file, bool dump_objcopy_command,
                                 bool dry_run_objcopy) {
    char err[512];
    XaotCliLinkCommand cmd;

    if (!config || !output_file || !output_file[0])
        return 0;

    if (!xaot_cli_build_objcopy_command(config, input_file, output_file, &cmd, err, sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    if (dump_objcopy_command)
        xaot_cli_objcopy_print_command(&cmd);
    if (dry_run_objcopy)
        return 0;

    XrProcId pid = xr_proc_spawn(cmd.program, cmd.argv);
    if (pid == XR_PROC_INVALID) {
        fprintf(stderr, "Error: failed to start objcopy tool '%s'\n", cmd.program);
        return 1;
    }
    int code = -1;
    if (xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: post-link objcopy failed for '%s'\n", input_file);
        return 1;
    }
    return 0;
}

static bool xaot_cli_build_compile_command(const XrToolchainSelection *plan,
                                           const XrToolchainTarget *target,
                                           const XaotLinkManifest *manifest, const char *opt_flag,
                                           const char *c_file, const char *obj_file,
                                           const char *sysroot, XaotCliLinkCommand *cmd, char *err,
                                           size_t err_size) {
    char aot_include[600];
    char runtime_include[600];
    XrNativeCompileSpec compile_spec;
    XrNativeLinkSpec unused_link_spec;
    XrToolchainArgSink sink;
    uint32_t i;

    if (!plan || !target || !manifest || !c_file || !obj_file || !cmd) {
        snprintf(err, err_size, "missing AOT compile command input");
        return false;
    }
    if ((manifest->n_cc_flags > 0 || manifest->n_ld_flags > 0) &&
        (!manifest->raw_flag_provider[0] ||
         strcmp(manifest->raw_flag_provider, xtc_provider_name(plan->provider)) != 0)) {
        snprintf(err, err_size, "raw native flags are not scoped to selected provider '%s'",
                 xtc_provider_name(plan->provider));
        return false;
    }

    memset(cmd, 0, sizeof(*cmd));
    if (!xaot_cli_add_provider_driver_prefix(cmd, plan, target, err, err_size))
        return false;
    (void) opt_flag;
    sink = xaot_cli_command_sink(cmd);
    xaot_cli_semantic_specs(manifest, plan, &compile_spec, &unused_link_spec);
    /* Static x86 SIMD is compiled one generated unit at a time.  Only units
     * whose source-backed marker proves they define native vector values get
     * the provider's AVX flag; scalar and cross-module caller units remain at
     * the baseline ISA.  Dispatch never emits these markers. */
    if (xaot_cli_provider_supports_x86_vector_islands(plan)) {
        XrNativeSimdMode unit_simd = xaot_cli_generated_source_simd(c_file);
        if (unit_simd != XR_NATIVE_SIMD_DEFAULT)
            compile_spec.simd = unit_simd;
    }
    if (!xtc_command_emit_compile(plan, &compile_spec, &sink, err, err_size) ||
        !xtc_command_emit_compile_io(plan->provider, c_file, obj_file, &sink, err, err_size))
        return false;

    resolve_aot_include_paths(plan, sysroot, aot_include, sizeof(aot_include), runtime_include,
                              sizeof(runtime_include));
    if (!xtc_command_emit_include(plan->provider, aot_include, &sink, err, err_size) ||
        !xtc_command_emit_include(plan->provider, runtime_include, &sink, err, err_size))
        return false;

    if (!xtc_command_emit_sysroot(plan->provider, sysroot, &sink, err, err_size))
        return false;

    if (!target->is_native) {
        if (!xaot_cli_link_add_arg(cmd, "-DXR_AOT_CROSS_TARGET=1", err, err_size) ||
            !xaot_cli_link_add_target_metadata_flags(cmd, target, err, err_size))
            return false;
    }

    for (i = 0; i < manifest->n_defines; i++) {
        if (!xtc_command_emit_define(plan->provider, manifest->defines[i], &sink, err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_cc_flags; i++) {
        if (!xaot_cli_link_add_arg(cmd, manifest->cc_flags[i], err, err_size))
            return false;
    }
    return true;
}

static int invoke_aot_manifest_compile(const XrToolchainSelection *plan,
                                       const XrToolchainTarget *target,
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
        if (plan && plan->provider == XR_TOOLCHAIN_PROVIDER_ZIG)
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
    char install_prefix[XR_PATH_MAX];

    if (sysroot) {
        snprintf(lib_path, lib_path_sz, "%s/lib", sysroot);
        return lib_path;
    }
    if (xray_lib)
        return xray_lib;
    if (build_resolve_install_prefix(install_prefix, sizeof(install_prefix))) {
        snprintf(lib_path, lib_path_sz, "%s/lib", install_prefix);
        return lib_path;
    }
#ifdef XRT_BUILD_LIB_DIR
    return XRT_BUILD_LIB_DIR;
#else
    return "/usr/local/lib";
#endif
}

static bool xaot_cli_build_link_command(const XrToolchainSelection *plan,
                                        const XrToolchainTarget *target,
                                        const XaotLinkManifest *manifest, const char *opt_flag,
                                        const char *output_file, const char *const *inputs,
                                        int n_inputs, bool strip_symbols, bool shared_library,
                                        const char *sysroot, const char *intermediate_object,
                                        XaotCliLinkCommand *cmd, char *err, size_t err_size) {
    char aot_include[600];
    char runtime_include[600];
    bool needs_runtime;
    bool uses_aot_core;
    bool freestanding_profile;
    XrNativeCompileSpec compile_spec;
    XrNativeLinkSpec link_spec;
    XrToolchainArgSink sink;
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
    if ((manifest->n_cc_flags > 0 || manifest->n_ld_flags > 0) &&
        (!manifest->raw_flag_provider[0] ||
         strcmp(manifest->raw_flag_provider, xtc_provider_name(plan->provider)) != 0)) {
        snprintf(err, err_size, "raw native flags are not scoped to selected provider '%s'",
                 xtc_provider_name(plan->provider));
        return false;
    }

    needs_runtime = xaot_link_manifest_needs_runtime(manifest);
    uses_aot_core = xaot_cli_manifest_uses_aot_core_for_target(manifest, target);
    freestanding_profile =
        xaot_link_manifest_contains(manifest, XAOT_LINK_DEFINE, "XRAY_PROFILE_FREESTANDING=1");
    if (needs_runtime && !target->is_native) {
        snprintf(err, err_size,
                 "cross target '%s' cannot consume runtime objects from AOT link manifest yet",
                 target->name);
        return false;
    }
    memset(cmd, 0, sizeof(*cmd));
    if (!xaot_cli_add_provider_driver_prefix(cmd, plan, target, err, err_size))
        return false;
    (void) opt_flag;
    (void) strip_symbols;
    (void) shared_library;
    sink = xaot_cli_command_sink(cmd);
    xaot_cli_semantic_specs(manifest, plan, &compile_spec, &link_spec);
    if (!xtc_command_emit_compile(plan, &compile_spec, &sink, err, err_size) ||
        !xtc_command_emit_link_output(plan->provider, output_file, &sink, err, err_size))
        return false;
    if (intermediate_object && !xtc_command_emit_intermediate_object_output(
                                   plan->provider, intermediate_object, &sink, err, err_size))
        return false;
    for (in = 0; in < n_inputs; in++) {
        if (!xaot_cli_link_add_arg(cmd, inputs[in], err, err_size))
            return false;
    }

    resolve_aot_include_paths(plan, sysroot, aot_include, sizeof(aot_include), runtime_include,
                              sizeof(runtime_include));
    if (!xtc_command_emit_include(plan->provider, aot_include, &sink, err, err_size) ||
        !xtc_command_emit_include(plan->provider, runtime_include, &sink, err, err_size))
        return false;

    if (!xtc_command_emit_sysroot(plan->provider, sysroot, &sink, err, err_size))
        return false;

    if (!target->is_native) {
        if (!xaot_cli_link_add_arg(cmd, "-DXR_AOT_CROSS_TARGET=1", err, err_size) ||
            !xaot_cli_link_add_target_metadata_flags(cmd, target, err, err_size))
            return false;
    }

    for (i = 0; i < manifest->n_defines; i++) {
        if (!xtc_command_emit_define(plan->provider, manifest->defines[i], &sink, err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_cc_flags; i++) {
        if (!xaot_cli_link_add_arg(cmd, manifest->cc_flags[i], err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_runtime_objects; i++) {
        if (!xaot_cli_link_add_verified_runtime(cmd, plan, manifest->runtime_objects[i], err,
                                                err_size))
            return false;
    }
    for (i = 0; i < manifest->n_stdlib_objects; i++) {
        if (!xaot_cli_link_add_stdlib_object(cmd, manifest->stdlib_objects[i], err, err_size))
            return false;
    }
    for (i = 0; i < manifest->n_native_inputs; i++) {
        if (!xaot_cli_link_add_arg(cmd, manifest->native_inputs[i], err, err_size))
            return false;
    }
    /* The AOT core archive goes on every link line, whatever the program does.
     * Every generated translation unit includes xrt_method.h, whose rune
     * classification helpers call into the Unicode property tables this archive
     * carries. At -O2 the helpers inline away and the archive contributes
     * nothing, but at -O0 the compiler emits them out of line and the link needs
     * xr_unicode_is_property -- so a program as small as `print(len(s))` failed
     * to link while the same program at -O2 succeeded. That dependency belongs
     * to the header, not to any feature a manifest can observe, so no feature
     * predicate can decide it. A static archive contributes only the members the
     * link actually pulls, so naming it unconditionally costs nothing for the
     * programs that need none of it. */
    if (!xaot_cli_link_add_verified_runtime(cmd, plan, "xray_aot_core", err, err_size))
        return false;
    if (needs_runtime || uses_aot_core) {
        for (i = 0; i < plan->system_library_count; i++) {
            if (!xtc_command_emit_system_library(plan->provider, target, plan->system_libraries[i],
                                                 &sink, err, err_size))
                return false;
        }
    }
    for (i = 0; i < manifest->n_system_libs; i++) {
        if (!xtc_command_emit_system_library(plan->provider, target, manifest->system_libs[i],
                                             &sink, err, err_size))
            return false;
    }
    if (!xtc_command_emit_link(plan, target, &link_spec, &sink, err, err_size))
        return false;
    for (i = 0; i < manifest->n_ld_flags; i++) {
        if (!xaot_cli_link_ld_flag_supported(target, manifest->ld_flags[i]))
            continue;
        if (shared_library && freestanding_profile &&
            strcmp(manifest->ld_flags[i], "-Wl,--gc-sections") == 0)
            continue;
        if (!xaot_cli_link_add_arg(cmd, manifest->ld_flags[i], err, err_size))
            return false;
    }
    return true;
}

static int invoke_aot_manifest_link(const XrToolchainSelection *plan,
                                    const XrToolchainTarget *target,
                                    const XaotLinkManifest *manifest, const char *opt_flag,
                                    const char *output_file, const char *const *inputs,
                                    int n_inputs, bool strip_symbols, bool shared_library,
                                    const char *sysroot, bool dump_link_command, bool dry_run_link,
                                    const char *intermediate_object) {
    char err[512];
    XaotCliLinkCommand cmd;

    if (!xaot_cli_build_link_command(plan, target, manifest, opt_flag, output_file, inputs,
                                     n_inputs, strip_symbols, shared_library, sysroot,
                                     intermediate_object, &cmd, err, sizeof(err))) {
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
        if (plan && plan->provider == XR_TOOLCHAIN_PROVIDER_ZIG)
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

    fprintf(f, "#include \"xray_vm.h\"\n\n");

    // Bundle-generated data (module bytecode arrays and module table)
    fprintf(f, "%s\n\n", bundle_source);

    // Main: default bytecode bundles run with full runtime support so imports
    // and runtime exception objects behave like `xray run`.
    fprintf(f, "\n"
               "int main(int argc, char **argv) {\n"
               "    setvbuf(stdout, NULL, _IONBF, 0);\n"
               "    setvbuf(stderr, NULL, _IONBF, 0);\n"
               "    XrVMConfig params = {0};\n"
               "    const XrBytecodeModule *entry = &xr_app_modules[xr_app_entry_index];\n"
               "    params.script_file = entry->path;\n"
               "    params.script_argc = argc > 1 ? argc - 1 : 0;\n"
               "    params.script_argv = argc > 1 ? argv + 1 : NULL;\n"
               "    XrVMRuntime *X = xray_vm_new_full(&params);\n"
               "    if (!X) { fprintf(stderr, \"Failed to create runtime\\n\"); return 1; }\n"
               "    int result = xray_vm_eval_bundle(X, &xr_app_bundle);\n"
               "    xray_vm_delete(X);\n"
               "    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;\n"
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

static XaotOptimizationLevel make_semantic_opt_level(const char *level, bool debug_symbols) {
    if (!level)
        return debug_symbols ? XAOT_OPTIMIZATION_NONE : XAOT_OPTIMIZATION_SPEED;
    if (strcmp(level, "0") == 0)
        return XAOT_OPTIMIZATION_NONE;
    if (strcmp(level, "1") == 0)
        return XAOT_OPTIMIZATION_BASIC;
    if (strcmp(level, "2") == 0)
        return XAOT_OPTIMIZATION_RELEASE;
    if (strcmp(level, "s") == 0)
        return XAOT_OPTIMIZATION_SIZE;
    return XAOT_OPTIMIZATION_SPEED;
}

static bool build_opt_level_is_fast(const char *level) {
    return level && strcmp(level, "fast") == 0;
}

static const char *default_shared_library_output(const XrToolchainTarget *target) {
    if (target && target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS)
        return "xray_exports.dll";
#ifdef XR_OS_MACOS
    if (!target || target->is_native)
        return "libxray_exports.dylib";
#else
    (void) target;
#endif
    return "libxray_exports.so";
}

static const char *default_hosted_fragment_output(const XrToolchainTarget *target) {
    if (target && target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS)
        return "xray_fragment.dll";
#ifdef XR_OS_MACOS
    if (!target || target->is_native)
        return "libxray_fragment.dylib";
#else
    (void) target;
#endif
    return "libxray_fragment.so";
}

/* ========== Build Sub-Modes (forward declarations) ========== */

static int cmd_build_bytecode(const char *input, const char *output, const char *cc,
                              const char *opt_flag, bool c_only, bool strip, bool debug_symbols,
                              const char *sysroot);
static int cmd_build_native(
    const char *input, const char *output, const char *cc, const char *opt_flag,
    XaotOptimizationLevel optimization, const char *cpu, XaotSimdMode simd_mode, bool c_only,
    bool strip, bool debug_symbols, XaotArtifactKind artifact_kind, XrCliBuildProfile profile,
    XiCgenCDialect c_dialect, XiCgenTypeNameProfile type_name_profile, const char *sysroot,
    const char *linker_script, bool verbose, bool dump_xaot_plan, bool dump_global_evidence,
    bool dump_xi_evidence, bool dump_link_manifest, bool dump_residue, bool dump_link_command,
    bool dry_run_link, const char *c_header, bool keep_c, const char *cache_dir_arg, bool rebuild,
    bool lto, bool rc_guard, const XrToolchainTarget *target,
    const XrToolchainSelection *toolchain_plan, const XrTargetConfig *target_config,
    const XrProject *project, const XrNativePackagePlan *native_package_plan,
    const char *objcopy_output);

/* ========== CLI Entry Point ========== */

XR_FUNC int cmd_build(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "build expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);
    const char *opt_level = xr_cli_opt_string(&inv->options, "opt", NULL);
    const char *target_arg = xr_cli_opt_string(&inv->options, "target", "native");
    const char *cpu = xr_cli_opt_string(&inv->options, "cpu", NULL);
    const char *simd_arg = xr_cli_opt_string(&inv->options, "simd", "auto");
    XaotSimdMode simd_mode = XAOT_SIMD_AUTO;
    const char *type_names_arg = xr_cli_opt_string(&inv->options, "type-names", NULL);
    const char *c_dialect_arg = xr_cli_opt_string(&inv->options, "c-dialect", "c11");
    bool c_only = xr_cli_opt_bool(&inv->options, "c-only");
    bool strip_symbols = xr_cli_opt_bool(&inv->options, "strip");
    bool debug_symbols = xr_cli_opt_bool(&inv->options, "debug");
    bool native_mode = xr_cli_opt_bool(&inv->options, "native");
    const char *artifact_arg = xr_cli_opt_string(&inv->options, "artifact", "executable");
    XaotArtifactKind artifact_kind = XAOT_ARTIFACT_EXECUTABLE;
    bool dump_xaot_plan = xr_cli_opt_bool(&inv->options, "dump-xaot-plan");
    bool dump_global_evidence = xr_cli_opt_bool(&inv->options, "dump-global-evidence");
    bool dump_xi_evidence = xr_cli_opt_bool(&inv->options, "dump-xi-evidence");
    bool dump_link_manifest = xr_cli_opt_bool(&inv->options, "dump-link-manifest");
    bool dump_residue = xr_cli_opt_bool(&inv->options, "dump-residue");
    bool dump_link_command = xr_cli_opt_bool(&inv->options, "dump-link-command");
    bool dump_toolchain_plan = xr_cli_opt_bool(&inv->options, "dump-toolchain-plan");
    bool dry_run_link = xr_cli_opt_bool(&inv->options, "dry-run-link");
    const char *c_header = xr_cli_opt_string(&inv->options, "c-header", NULL);
    const char *c_export_prefix = xr_cli_opt_string(&inv->options, "c-export-prefix", NULL);
    const char *c_export_exclude = xr_cli_opt_string(&inv->options, "c-export-exclude", NULL);
    bool keep_c = xr_cli_opt_bool(&inv->options, "keep-c");
    const char *cache_dir_arg = xr_cli_opt_string(&inv->options, "cache-dir", NULL);
    bool rebuild = xr_cli_opt_bool(&inv->options, "rebuild");
    bool lto = xr_cli_opt_bool(&inv->options, "lto");
    bool rc_guard = xr_cli_opt_bool(&inv->options, "rc-guard");
    /* Task 219: --verify-arc forces the RC/ownership verifier on after every
     * lifetime/CFG-invalidating optimization pass (post-ARC single run stays
     * always-on regardless). Accepted both here and as a global flag. */
    if (xr_cli_opt_bool(&inv->options, "verify-arc"))
        xi_arc_verify_set_per_pass(true);
    /* Settled before anything compiles: the optimizer policy is fixed for the
     * whole session, so every module of this build -- including a body inlined
     * across a module boundary -- is optimized under one configuration. */
    if (!xr_cli_apply_xi_opt(inv, "build"))
        return XR_CLI_EXIT_USAGE;
    bool verbose = xr_cli_opt_bool(&inv->options, "verbose") || (inv->ctx && inv->ctx->verbose);
    bool opt_fast = build_opt_level_is_fast(opt_level);
    XrProject *project = NULL;
    const XrTargetConfig *target_config = NULL;
    char project_root[XR_PATH_MAX];
    char linker_script_from_config[XR_PATH_MAX];
    char objcopy_output_from_config[XR_PATH_MAX];
    const char *cc;
    const char *sysroot;
    const char *toolchain_arg;
    const char *zig_path;
    const char *profile_arg;
    const char *linker_script;
    const char *objcopy_output;
    XrToolchainTarget target;
    XrCliBuildProfile profile;
    XiCgenCDialect c_dialect;
    XiCgenTypeNameProfile type_name_profile;
    XrToolchainSelector toolchain_selector;
    XrToolchainSelection toolchain_plan;
    XrToolchainProbeResult toolchain_probe;
    XrToolchainConfig user_toolchain_config;
    const XrToolchainPreference *user_toolchain_preference = NULL;
    char user_toolchain_config_path[XR_PATH_MAX];
    char parse_err[512];
    int rc;

#define CMD_BUILD_RETURN(code)                                                                     \
    do {                                                                                           \
        xr_project_free(project);                                                                  \
        return (code);                                                                             \
    } while (0)

    project_root[0] = '\0';
    linker_script_from_config[0] = '\0';
    objcopy_output_from_config[0] = '\0';
    if (native_mode && xr_cli_find_project_root(input_file, project_root, sizeof(project_root))) {
        project = xr_project_load(NULL, project_root);
        if (project && !project->initialized) {
            fprintf(stderr, "Error: %s\n",
                    project->native_plan && project->native_plan->error
                        ? project->native_plan->error
                        : "invalid xray.toml project configuration");
            CMD_BUILD_RETURN(2);
        }
        if (project)
            target_config = xr_project_find_target_config(project, target_arg);
    }

    if ((c_export_prefix || c_export_exclude) && !native_mode) {
        fprintf(stderr, "Error: --c-export-prefix/--c-export-exclude require --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (c_export_prefix || c_export_exclude) {
        if (!project || !project->native_plan) {
            fprintf(stderr,
                    "Error: --c-export-prefix/--c-export-exclude require manifest C exports\n");
            CMD_BUILD_RETURN(2);
        }
        if (!xr_native_package_configure_c_exports(project->native_plan, c_export_prefix,
                                                   c_export_exclude, parse_err,
                                                   sizeof(parse_err))) {
            fprintf(stderr, "Error: %s\n", parse_err);
            CMD_BUILD_RETURN(2);
        }
    }

    cc = build_config_string(&inv->options, "cc", target_config ? target_config->cc : NULL, NULL);
    sysroot = build_config_string(&inv->options, "sysroot",
                                  target_config ? target_config->sysroot : NULL, NULL);
    toolchain_arg = build_config_string(&inv->options, "toolchain",
                                        target_config ? target_config->toolchain : NULL, NULL);
    zig_path =
        build_config_string(&inv->options, "zig", target_config ? target_config->zig : NULL, NULL);
    profile_arg = build_config_string(&inv->options, "profile",
                                      target_config ? target_config->profile : NULL, "hosted");
    linker_script = build_config_string(&inv->options, "linker-script",
                                        target_config ? target_config->linker_script : NULL, NULL);
    if (target_config && target_config->linker_script &&
        !xr_cli_opt_present(&inv->options, "linker-script") &&
        build_join_project_path(project ? project->root : project_root,
                                target_config->linker_script, linker_script_from_config,
                                sizeof(linker_script_from_config))) {
        linker_script = linker_script_from_config;
    }
    objcopy_output = NULL;
    if (target_config && target_config->objcopy_output && target_config->objcopy_output[0] &&
        build_join_project_path(project ? project->root : project_root,
                                target_config->objcopy_output, objcopy_output_from_config,
                                sizeof(objcopy_output_from_config))) {
        objcopy_output = objcopy_output_from_config;
    }

    if (!xtc_target_parse(target_arg, &target, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        CMD_BUILD_RETURN(2);
    }
    if (native_mode) {
        bool config_exists = false;
        if (!xtc_config_path(user_toolchain_config_path, sizeof(user_toolchain_config_path),
                             parse_err, sizeof(parse_err)) ||
            !xtc_config_load(user_toolchain_config_path, &user_toolchain_config, &config_exists,
                             parse_err, sizeof(parse_err))) {
            fprintf(stderr, "Error: %s\n", parse_err);
            CMD_BUILD_RETURN(1);
        }
        user_toolchain_preference = xtc_config_find(&user_toolchain_config, target_arg);
        if (!toolchain_arg && user_toolchain_preference)
            toolchain_arg = xtc_selector_name(user_toolchain_preference->selector);
    }
    if (!toolchain_arg)
        toolchain_arg = "auto";
    if (!build_profile_parse(profile_arg, &profile, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        CMD_BUILD_RETURN(2);
    }
    if (!build_artifact_kind_parse(artifact_arg, &artifact_kind, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        CMD_BUILD_RETURN(2);
    }
    bool library_artifact = artifact_kind != XAOT_ARTIFACT_EXECUTABLE;
    bool shared_library = artifact_kind == XAOT_ARTIFACT_SHARED_LIBRARY;
    if (!build_c_dialect_parse(c_dialect_arg, &c_dialect, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        CMD_BUILD_RETURN(2);
    }
    if (!build_type_name_profile_parse(type_names_arg, profile, &type_name_profile, parse_err,
                                       sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        CMD_BUILD_RETURN(2);
    }
    if (!xtc_selector_parse(toolchain_arg, &toolchain_selector, parse_err, sizeof(parse_err))) {
        fprintf(stderr, "Error: %s\n", parse_err);
        CMD_BUILD_RETURN(2);
    }
    xtc_config_apply_provider_paths(user_toolchain_preference, toolchain_selector, cc, getenv("CC"),
                                    zig_path, getenv("XRAY_ZIG"), &cc, &zig_path);
    if (profile != XR_CLI_BUILD_PROFILE_HOSTED && !native_mode) {
        fprintf(stderr, "Error: --profile %s requires --native\n", build_profile_name(profile));
        CMD_BUILD_RETURN(2);
    }
    if (project && project->native_plan && profile != XR_CLI_BUILD_PROFILE_FREESTANDING &&
        project->native_plan->entry_count > 0) {
        fprintf(stderr,
                "Error: freestanding.entry is only supported by the freestanding AOT profile\n");
        CMD_BUILD_RETURN(2);
    }
    if (project && project->native_plan && profile == XR_CLI_BUILD_PROFILE_HOSTED) {
        for (uint32_t i = 0; i < project->native_plan->link_symbol_count; i++) {
            if (!project->native_plan->link_symbols[i].section)
                continue;
            fprintf(stderr,
                    "Error: link.symbol section policy is only supported by the freestanding "
                    "AOT profile\n");
            CMD_BUILD_RETURN(2);
        }
    }
    if (xr_cli_opt_present(&inv->options, "type-names") && !native_mode) {
        fprintf(stderr, "Error: --type-names requires --native\n");
        CMD_BUILD_RETURN(2);
    }

    if (dump_xaot_plan && !native_mode) {
        fprintf(stderr, "Error: --dump-xaot-plan requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_global_evidence && !native_mode) {
        fprintf(stderr, "Error: --dump-global-evidence requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_xi_evidence && !native_mode) {
        fprintf(stderr, "Error: --dump-xi-evidence requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_link_manifest && !native_mode) {
        fprintf(stderr, "Error: --dump-link-manifest requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_residue && !native_mode) {
        fprintf(stderr, "Error: --dump-residue requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_link_command && !native_mode) {
        fprintf(stderr, "Error: --dump-link-command requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_toolchain_plan && !native_mode) {
        fprintf(stderr, "Error: --dump-toolchain-plan requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (dry_run_link && !native_mode) {
        fprintf(stderr, "Error: --dry-run-link requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (xr_cli_opt_present(&inv->options, "linker-script") && !native_mode) {
        fprintf(stderr, "Error: --linker-script requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if ((xr_cli_opt_present(&inv->options, "linker-script") || target_config) && linker_script &&
        !linker_script[0]) {
        fprintf(stderr, "Error: --linker-script requires a non-empty path\n");
        CMD_BUILD_RETURN(2);
    }
    if (!target.is_native && !native_mode) {
        fprintf(stderr, "Error: --target %s requires --native\n", target.name);
        CMD_BUILD_RETURN(2);
    }
    if ((xr_cli_opt_present(&inv->options, "toolchain") ||
         xr_cli_opt_present(&inv->options, "zig") || keep_c) &&
        !native_mode) {
        fprintf(stderr, "Error: --toolchain/--zig/--keep-c require --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (c_header && !native_mode) {
        fprintf(stderr, "Error: --c-header requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (library_artifact && !native_mode) {
        fprintf(stderr, "Error: --artifact %s requires --native\n", artifact_arg);
        CMD_BUILD_RETURN(2);
    }
    if (artifact_kind == XAOT_ARTIFACT_HOSTED_FRAGMENT && profile != XR_CLI_BUILD_PROFILE_HOSTED) {
        fprintf(stderr, "Error: --artifact hosted-fragment requires --profile hosted\n");
        CMD_BUILD_RETURN(2);
    }
    if (dry_run_link && c_only) {
        fprintf(stderr, "Error: --dry-run-link cannot be combined with --c-only\n");
        CMD_BUILD_RETURN(2);
    }
    if (dump_toolchain_plan && c_only) {
        fprintf(stderr, "Error: --dump-toolchain-plan cannot be combined with --c-only\n");
        CMD_BUILD_RETURN(2);
    }
    if (linker_script && c_only && xr_cli_opt_present(&inv->options, "linker-script")) {
        fprintf(stderr, "Error: --linker-script cannot be combined with --c-only\n");
        CMD_BUILD_RETURN(2);
    }
    if (c_only && !xr_cli_opt_present(&inv->options, "linker-script"))
        linker_script = NULL;
    if (c_only) {
        objcopy_output = NULL;
    } else if (target_config && target_config->objcopy_output &&
               !target_config->objcopy_output[0]) {
        fprintf(stderr, "Error: xray.toml target objcopy requires non-empty objcopy_output\n");
        CMD_BUILD_RETURN(2);
    } else if (target_config && xaot_cli_target_config_has_objcopy(target_config) &&
               (!target_config->objcopy_output || !target_config->objcopy_output[0])) {
        fprintf(stderr, "Error: xray.toml target objcopy requires non-empty objcopy_output\n");
        CMD_BUILD_RETURN(2);
    } else if (target_config && target_config->objcopy_output && target_config->objcopy_output[0] &&
               (!objcopy_output || !objcopy_output[0])) {
        fprintf(stderr, "Error: xray.toml target objcopy_output path is too long: %s\n",
                target_config->objcopy_output);
        CMD_BUILD_RETURN(2);
    }
    if ((xr_cli_opt_present(&inv->options, "cache-dir") || rebuild) && !native_mode) {
        fprintf(stderr, "Error: --cache-dir/--rebuild require --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (cpu && !native_mode) {
        fprintf(stderr, "Error: --cpu requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (xr_cli_opt_present(&inv->options, "simd") && !native_mode) {
        fprintf(stderr, "Error: --simd requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (!xaot_simd_mode_parse(simd_arg, &simd_mode)) {
        fprintf(stderr,
                "Error: invalid --simd mode '%s' (expected auto, scalar, native, neon, sse2, "
                "avx2, avx512, vsx, lsx, sve, or dispatch)\n",
                simd_arg ? simd_arg : "");
        CMD_BUILD_RETURN(2);
    }
    if (c_dialect == XI_CGEN_C_DIALECT_C90 &&
        (!native_mode || !c_only || !shared_library ||
         profile != XR_CLI_BUILD_PROFILE_FREESTANDING || simd_mode != XAOT_SIMD_SCALAR)) {
        fprintf(stderr,
                "Error: --c-dialect c90 requires --native --profile freestanding --artifact "
                "shared-library --c-only --simd scalar\n");
        CMD_BUILD_RETURN(2);
    }
    if (c_dialect == XI_CGEN_C_DIALECT_C90 && !build_c90_target_supported(&target)) {
        fprintf(stderr,
                "Error: --c-dialect c90 currently requires an LP64 Linux or Darwin target; "
                "target '%s' is unsupported\n",
                target.name ? target.name : "?");
        CMD_BUILD_RETURN(2);
    }
    if (debug_symbols && !native_mode) {
        fprintf(stderr, "Error: --debug requires --native\n");
        CMD_BUILD_RETURN(2);
    }
    if (cpu && !target.is_native) {
        fprintf(stderr,
                "Error: --cpu is host-only; cross target '%s' selects its CPU via the "
                "target triple\n",
                target.name);
        CMD_BUILD_RETURN(2);
    }
    if (cc && cc[0] && (toolchain_selector == XR_TOOLCHAIN_SELECTOR_ZIG)) {
        fprintf(stderr, "Error: --cc cannot be used with --toolchain zig\n");
        CMD_BUILD_RETURN(2);
    }
    if (zig_path && zig_path[0] && toolchain_selector != XR_TOOLCHAIN_SELECTOR_AUTO &&
        toolchain_selector != XR_TOOLCHAIN_SELECTOR_ZIG) {
        fprintf(stderr, "Error: --zig can only be used with --toolchain auto or zig\n");
        CMD_BUILD_RETURN(2);
    }
    memset(&toolchain_plan, 0, sizeof(toolchain_plan));
    toolchain_plan.target = target;
    if (!c_only) {
        XrToolchainProbeOptions probe_options = {0};
        probe_options.request.selector = toolchain_selector;
        probe_options.request.target = target;
        probe_options.request.cc = cc && cc[0] ? cc : getenv("CC");
        probe_options.request.zig = zig_path;
        probe_options.request.program_hint = inv->ctx ? inv->ctx->program : NULL;
        probe_options.profile = profile == XR_CLI_BUILD_PROFILE_FREESTANDING
                                    ? XR_TOOLCHAIN_PROFILE_FREESTANDING
                                    : XR_TOOLCHAIN_PROFILE_HOSTED;
        probe_options.required_codegen_capabilities = XR_TOOLCHAIN_CODEGEN_ALL;
        memset(&toolchain_probe, 0, sizeof(toolchain_probe));
        if (!xtc_probe(&probe_options, &toolchain_probe, parse_err, sizeof(parse_err))) {
            fprintf(stderr, "Error: %s\n", parse_err);
            for (size_t i = 0; i < toolchain_probe.diagnostic_count; i++)
                fprintf(stderr, "  [%s] %s: %s\n",
                        xtc_reason_code_name(toolchain_probe.diagnostics[i].code),
                        toolchain_probe.diagnostics[i].stage,
                        toolchain_probe.diagnostics[i].message);
            CMD_BUILD_RETURN(3);
        }
        toolchain_plan = toolchain_probe.selection;
        toolchain_plan.program = toolchain_plan.program_storage;
    }
    if (!output_file) {
        if (c_only)
            output_file = "app.c";
        else if (artifact_kind == XAOT_ARTIFACT_SHARED_LIBRARY)
            output_file = default_shared_library_output(&target);
        else if (artifact_kind == XAOT_ARTIFACT_HOSTED_FRAGMENT)
            output_file = default_hosted_fragment_output(&target);
        else
            output_file = xtc_target_default_output(&target);
    }

    const char *opt_flag = make_opt_flag(opt_level, debug_symbols);
    XaotOptimizationLevel semantic_optimization = make_semantic_opt_level(opt_level, debug_symbols);
    const char *effective_cpu = cpu;
    bool effective_lto = lto;
    if (native_mode && target.is_native && opt_fast) {
        effective_lto = !c_only && toolchain_probe.lto == XR_TOOLCHAIN_CAPABILITY_OK;
        if (!effective_cpu || !effective_cpu[0])
            effective_cpu = "native";
    }
    if (lto && !c_only && toolchain_probe.lto != XR_TOOLCHAIN_CAPABILITY_OK) {
        fprintf(stderr, "Error: selected provider does not satisfy the requested LTO capability\n");
        CMD_BUILD_RETURN(3);
    }
    if (dump_toolchain_plan) {
        printf("Toolchain plan: provider=%s ownership=%s target=%s compiler=%s "
               "runtime=%s sdk=%s probe=%s cache=%s\n",
               xtc_provider_name(toolchain_plan.provider),
               xtc_ownership_name(toolchain_plan.ownership), target.name,
               toolchain_plan.program ? toolchain_plan.program : "",
               toolchain_plan.runtime_artifact, toolchain_plan.sdk_digest,
               toolchain_plan.probe_fingerprint, toolchain_probe.cache);
    }

    if (native_mode) {
        rc = cmd_build_native(input_file, output_file, cc, opt_flag, semantic_optimization,
                              effective_cpu, simd_mode, c_only, strip_symbols, debug_symbols,
                              artifact_kind, profile, c_dialect, type_name_profile, sysroot,
                              linker_script, verbose, dump_xaot_plan, dump_global_evidence,
                              dump_xi_evidence, dump_link_manifest, dump_residue, dump_link_command,
                              dry_run_link, c_header, keep_c, cache_dir_arg, rebuild, effective_lto,
                              rc_guard, &target, &toolchain_plan, target_config,
                              project, project ? project->native_plan : NULL, objcopy_output);
        CMD_BUILD_RETURN(rc);
    }
    rc = cmd_build_bytecode(input_file, output_file, cc && cc[0] ? cc : "cc", opt_flag, c_only,
                            strip_symbols, debug_symbols, sysroot);
    CMD_BUILD_RETURN(rc);
#undef CMD_BUILD_RETURN
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

    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    XrBundle *bundle =
        xr_module_identity_script_authority_from_source(input, &authority, &authority_root)
            ? xr_bundle_create_ex(X, input, &authority, XR_BUNDLE_DEFAULT)
            : NULL;
    xr_free(authority_root);
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
           "value_vector=%u value_view=%u value_void=%u\n",
           stats->functions_total, stats->functions_native_abi, stats->functions_tagged_abi,
           stats->functions_coro_abi, stats->values_total, stats->boundary_count,
           stats->values_scalar, stats->values_tagged, stats->values_ptr, stats->values_aggregate,
           stats->values_vector, stats->values_view, stats->values_void);
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
    char aot_dir[XR_PATH_MAX];
    char include_dir[XR_PATH_MAX];
    char shared_dir[XR_PATH_MAX];

    if (cached)
        return cached_key;
    h = XR_FNV64_OFFSET_BASIS;
    h = xaot_hash_fold_str(h, "xaot-aot-runtime-source-key-v1");
    resolve_aot_include_paths(NULL, NULL, aot_dir, sizeof(aot_dir), include_dir,
                              sizeof(include_dir));
    h = xaot_hash_fold_source_path_list(h, "aot", aot_dir);
    snprintf(shared_dir, sizeof(shared_dir), "%s/../shared", aot_dir);
    h = xaot_hash_fold_source_path_list(h, "shared", shared_dir);
    h = xaot_hash_fold_source_path_list(h, "include", include_dir);
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
           (len > 4 && strcmp(value + len - 4, ".obj") == 0) ||
           (len > 4 && strcmp(value + len - 4, ".lib") == 0);
}

static const char *xaot_cli_object_suffix(const XrToolchainSelection *plan) {
    return plan && plan->provider == XR_TOOLCHAIN_PROVIDER_MSVC ? ".obj" : ".o";
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
                                                     const XrToolchainSelection *plan,
                                                     const char *sysroot) {
    char lib_dir[XR_PATH_MAX];
    char dep[XR_PATH_MAX];
    const char *resolved_lib_dir = resolve_xray_lib_path(sysroot, lib_dir, sizeof(lib_dir));

    if (!manifest)
        return h;

    if (plan && plan->runtime_count > 0) {
        h = xaot_hash_fold_str(h, plan->sdk_digest);
        for (size_t i = 0; i < plan->runtime_count; i++) {
            h = xaot_hash_fold_str(h, plan->runtime_ids[i]);
            h = xaot_hash_fold_file_stat(h, plan->runtime_paths[i]);
        }
        for (uint32_t i = 0; i < manifest->n_stdlib_objects; i++) {
            const char *value = manifest->stdlib_objects[i];
            if (xaot_link_value_is_path(value))
                h = xaot_hash_fold_file_stat(h, value);
        }
        return h;
    }

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
    /* Unconditional, because the archive is on every link line. Folding it in
     * only when a manifest names one of its stdlib symbols would let a rebuilt
     * archive be answered from a stale cached link. */
    xaot_runtime_archive_path(resolved_lib_dir, "xray_aot_core", dep, sizeof(dep));
    h = xaot_hash_fold_file_stat(h, dep);
    return h;
}

/* Cache key = content hash of the generated C plus everything that changes the
 * resulting object: optimization level, target, toolchain, sysroot, and every
 * semantic compile requirements plus every provider-scoped escape flag carried
 * by the link manifest. */
static uint64_t xaot_object_cache_key(const char *c_source, const char *opt_flag,
                                      const XrToolchainTarget *target,
                                      const XrToolchainSelection *plan,
                                      const XaotLinkManifest *manifest, const char *sysroot,
                                      XiCgenCDialect c_dialect) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = xaot_hash_fold_str(h, "xaot-obj-cache-v4");
    h = xaot_hash_fold(h, &c_dialect, sizeof(c_dialect));
    h = xaot_hash_fold(h, &(uint64_t) {xaot_aot_runtime_source_key()}, sizeof(uint64_t));
    h = xaot_hash_fold_str(h, opt_flag);
    if (target) {
        h = xaot_hash_fold_str(h, target->name);
        h = xaot_hash_fold_str(h, target->zig_triple);
        h = xaot_hash_fold_str(h, target->cpu);
        h = xaot_hash_fold(h, &target->is_native, sizeof(target->is_native));
    }
    if (plan) {
        h = xaot_hash_fold_str(h, plan->program);
        h = xaot_hash_fold(h, &plan->provider, sizeof(plan->provider));
        h = xaot_hash_fold_str(h, plan->compiler_fingerprint);
        h = xaot_hash_fold_str(h, plan->sdk_digest);
        h = xaot_hash_fold_str(h, plan->probe_fingerprint);
    }
    h = xaot_hash_fold_str(h, sysroot);
    if (manifest) {
        h = xaot_hash_fold(h, &manifest->target.simd_mode, sizeof(manifest->target.simd_mode));
        h = xaot_hash_fold(h, &manifest->target.simd_features,
                           sizeof(manifest->target.simd_features));
        h = xaot_hash_fold(h, &manifest->compile, sizeof(manifest->compile));
        for (uint32_t i = 0; i < manifest->n_defines; i++)
            h = xaot_hash_fold_str(h, manifest->defines[i]);
        for (uint32_t i = 0; i < manifest->n_cc_flags; i++)
            h = xaot_hash_fold_str(h, manifest->cc_flags[i]);
    }
    h = xaot_hash_fold_str(h, c_source);
    return h;
}

static uint64_t xaot_link_output_cache_key(const XaotBuildResult *result, const char *opt_flag,
                                           const XrToolchainTarget *target,
                                           const XrToolchainSelection *plan, const char *sysroot,
                                           bool strip_symbols, bool shared_library,
                                           XiCgenCDialect c_dialect) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;

    h = xaot_hash_fold_str(h, "xaot-link-output-cache-v4");
    h = xaot_hash_fold(h, &c_dialect, sizeof(c_dialect));
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
        h = xaot_hash_fold_str(h, target->cpu);
        h = xaot_hash_fold_bool(h, target->is_native);
    }
    if (plan) {
        h = xaot_hash_fold_str(h, plan->program);
        h = xaot_hash_fold(h, &plan->provider, sizeof(plan->provider));
        h = xaot_hash_fold_str(h, plan->compiler_fingerprint);
        h = xaot_hash_fold_str(h, plan->sdk_digest);
        h = xaot_hash_fold_str(h, plan->probe_fingerprint);
    }
    h = xaot_hash_fold_str(h, sysroot);
    if (result) {
        const XaotLinkManifest *manifest = &result->link_manifest;
        h = xaot_hash_fold(h, &manifest->target.simd_mode, sizeof(manifest->target.simd_mode));
        h = xaot_hash_fold(h, &manifest->target.simd_features,
                           sizeof(manifest->target.simd_features));
        h = xaot_hash_fold(h, &manifest->compile, sizeof(manifest->compile));
        h = xaot_hash_fold(h, &manifest->link, sizeof(manifest->link));
        h = xaot_hash_fold_str(h, manifest->raw_flag_provider);
        h = xaot_hash_fold_string_list(h, manifest->runtime_caps, manifest->n_runtime_caps);
        h = xaot_hash_fold_string_list(h, manifest->runtime_objects, manifest->n_runtime_objects);
        h = xaot_hash_fold_string_list(h, manifest->stdlib_objects, manifest->n_stdlib_objects);
        h = xaot_hash_fold_string_list(h, manifest->native_inputs, manifest->n_native_inputs);
        h = xaot_hash_fold_string_list(h, manifest->generated_c_files,
                                       manifest->n_generated_c_files);
        h = xaot_hash_fold_string_list(h, manifest->system_libs, manifest->n_system_libs);
        h = xaot_hash_fold_string_list(h, manifest->defines, manifest->n_defines);
        h = xaot_hash_fold_string_list(h, manifest->cc_flags, manifest->n_cc_flags);
        h = xaot_hash_fold_string_list(h, manifest->ld_flags, manifest->n_ld_flags);
        h = xaot_hash_fold_link_dependency_stats(h, manifest, plan, sysroot);
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
    while (len > 1 && (buf[len - 1] == '/' || buf[len - 1] == '\\'))
        buf[--len] = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char separator = *p;
        *p = '\0';
        if (xr_fs_mkdir(buf, 0755) != 0 && !xr_fs_is_dir(buf))
            return -1;
        *p = separator;
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

static XrOptimizationLevel xaot_native_unit_optimization(const XrNativeUnit *unit) {
    if (unit && unit->optimization && strcmp(unit->optimization, "size") == 0)
        return XR_OPTIMIZATION_SIZE;
    if (unit && unit->optimization && strcmp(unit->optimization, "release") == 0)
        return XR_OPTIMIZATION_RELEASE;
    return XR_OPTIMIZATION_NONE;
}

static bool xaot_cli_build_native_unit_compile_command(const XrToolchainSelection *plan,
                                                       const XrToolchainTarget *target,
                                                       const XrNativeUnit *unit, const char *source,
                                                       const char *object, const char *sysroot,
                                                       XaotCliLinkCommand *cmd, char *err,
                                                       size_t err_size) {
    if (!plan || !target || !unit || !source || !object || !cmd) {
        snprintf(err, err_size, "missing native unit compile command input");
        return false;
    }
    memset(cmd, 0, sizeof(*cmd));
    if (!xaot_cli_add_provider_driver_prefix(cmd, plan, target, err, err_size))
        return false;
    XrToolchainArgSink sink = xaot_cli_command_sink(cmd);
    XrNativeCompileSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.optimization = xaot_native_unit_optimization(unit);
    spec.fp_contract = XR_FP_CONTRACT_OFF;
    spec.language_standard = unit->kind == XR_NATIVE_UNIT_C ? unit->language_standard : NULL;
    spec.visibility = unit->visibility && strcmp(unit->visibility, "hidden") == 0
                          ? XR_VISIBILITY_HIDDEN
                          : XR_VISIBILITY_DEFAULT;
    spec.warnings = unit->warning_policy && strcmp(unit->warning_policy, "strict") == 0
                        ? XR_WARNING_POLICY_STRICT
                        : XR_WARNING_POLICY_DEFAULT;
    if (!xtc_command_emit_compile(plan, &spec, &sink, err, err_size))
        return false;
    for (uint32_t i = 0; i < unit->include_dir_count; i++) {
        if (!xtc_command_emit_include(plan->provider, unit->include_dirs[i], &sink, err, err_size))
            return false;
    }
    for (uint32_t i = 0; i < unit->define_count; i++) {
        if (!xtc_command_emit_define(plan->provider, unit->defines[i], &sink, err, err_size))
            return false;
    }
    if (!xtc_command_emit_sysroot(plan->provider, sysroot, &sink, err, err_size))
        return false;
    if (!target->is_native && !xaot_cli_link_add_target_metadata_flags(cmd, target, err, err_size))
        return false;
    return xtc_command_emit_compile_io(plan->provider, source, object, &sink, err, err_size);
}

static int xaot_invoke_native_unit_compile(const XrToolchainSelection *plan,
                                           const XrToolchainTarget *target,
                                           const XrNativeUnit *unit, const char *source,
                                           const char *object, const char *sysroot,
                                           bool dump_command, bool dry_run) {
    char err[512];
    XaotCliLinkCommand cmd;
    if (!xaot_cli_build_native_unit_compile_command(plan, target, unit, source, object, sysroot,
                                                    &cmd, err, sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    if (dump_command)
        xaot_cli_print_command("Native compile command", &cmd);
    if (dry_run)
        return 0;
    XrProcId pid = xr_proc_spawn(cmd.program, cmd.argv);
    int code = -1;
    if (pid == XR_PROC_INVALID || xr_proc_wait(pid, &code) != 0 || code != 0) {
        fprintf(stderr, "Error: native unit '%s' compilation failed for '%s'\n",
                unit->name ? unit->name : "?", source);
        return 1;
    }
    return 0;
}

static uint64_t xaot_native_object_cache_key(const XrNativeUnit *unit, uint32_t source_index,
                                             const XrToolchainTarget *target,
                                             const XrToolchainSelection *plan,
                                             const char *sysroot) {
    uint64_t h = XR_FNV64_OFFSET_BASIS;
    h = xaot_hash_fold_str(h, "xray-native-unit-object-v1");
    h = xaot_hash_fold(h, &unit->fingerprint, sizeof(unit->fingerprint));
    h = xaot_hash_fold(h, &source_index, sizeof(source_index));
    h = xaot_hash_fold_str(h, target ? target->name : NULL);
    h = xaot_hash_fold_str(h, target ? target->zig_triple : NULL);
    h = xaot_hash_fold_str(h, target ? target->cpu : NULL);
    h = xaot_hash_fold_str(h, plan ? plan->program : NULL);
    if (plan)
        h = xaot_hash_fold(h, &plan->provider, sizeof(plan->provider));
    return xaot_hash_fold_str(h, sysroot);
}

static bool xaot_native_unit_owns_stub(const XrNativeUnit *unit, const char *stub) {
    if (!unit || !unit->sources || !stub)
        return false;
    for (uint32_t si = 0; si < unit->source_count; si++) {
        if (unit->sources[si] && strcmp(unit->sources[si], stub) == 0)
            return true;
    }
    return false;
}

static bool xaot_native_unit_owns_entry_stub(const XrNativePackagePlan *plan,
                                             const XrNativeUnit *unit) {
    if (!plan)
        return false;
    for (uint32_t ei = 0; ei < plan->entry_count; ei++) {
        if (xaot_native_unit_owns_stub(unit, plan->entries[ei].stub))
            return true;
    }
    return false;
}

static int xaot_compile_native_package(const XrNativePackagePlan *native_plan,
                                       XaotLinkManifest *link_manifest,
                                       const XrToolchainSelection *toolchain_plan,
                                       const XrToolchainTarget *target, const char *cache_dir,
                                       const char *sysroot, bool verbose, bool rebuild,
                                       bool dump_command, bool dry_run) {
    char native_cache[XR_PATH_MAX];
    if (!native_plan)
        return 0;
    if (snprintf(native_cache, sizeof(native_cache), "%s/native", cache_dir) < 0 ||
        xaot_mkdir_p(native_cache) != 0) {
        fprintf(stderr, "Error: cannot create native unit cache in '%s'\n", cache_dir);
        return 1;
    }
    for (uint32_t ui = 0; ui < native_plan->unit_count; ui++) {
        const XrNativeUnit *unit = &native_plan->units[ui];
        /* Platform libraries are selected through reachable native.symbol
         * entries and already appear in the XAOT feature/link manifest.
         * Adding every declared platform unit here would retain libraries for
         * unused foreign declarations. */
        for (uint32_t li = 0; unit->kind != XR_NATIVE_UNIT_PLATFORM && li < unit->system_link_count;
             li++) {
            if (!xaot_link_manifest_add_unique(link_manifest, XAOT_LINK_SYSTEM_LIB,
                                               unit->system_links[li])) {
                fprintf(stderr, "Error: cannot record native system link '%s'\n",
                        unit->system_links[li]);
                return 1;
            }
        }
        if (unit->kind != XR_NATIVE_UNIT_C && unit->kind != XR_NATIVE_UNIT_ASM)
            continue;
        const char **objects =
            (const char **) xr_calloc((size_t) unit->source_count, sizeof(char *));
        char (*object_paths)[XR_PATH_MAX] =
            (char (*)[XR_PATH_MAX]) xr_calloc((size_t) unit->source_count, XR_PATH_MAX);
        if (!objects || !object_paths) {
            xr_free(objects);
            xr_free(object_paths);
            return 1;
        }
        int ret = 0;
        for (uint32_t si = 0; si < unit->source_count; si++) {
            uint64_t key = xaot_native_object_cache_key(unit, si, target, toolchain_plan, sysroot);
            char tmp[XR_PATH_MAX];
            snprintf(object_paths[si], XR_PATH_MAX, "%s/%016llx%s", native_cache,
                     (unsigned long long) key, xaot_cli_object_suffix(toolchain_plan));
            objects[si] = object_paths[si];
            if (!rebuild && xr_fs_is_file(object_paths[si])) {
                if (verbose)
                    printf("[xi-native] native cache hit: %s[%u] (%016llx)\n", unit->name, si,
                           (unsigned long long) key);
                continue;
            }
            snprintf(tmp, sizeof(tmp), "%s/%016llx.%d%s.tmp", native_cache,
                     (unsigned long long) key, (int) xr_proc_self_pid(),
                     xaot_cli_object_suffix(toolchain_plan));
            if (verbose)
                printf("[xi-native] compiling native unit: %s[%u] (%016llx)\n", unit->name, si,
                       (unsigned long long) key);
            ret = xaot_invoke_native_unit_compile(toolchain_plan, target, unit, unit->sources[si],
                                                  tmp, sysroot, dump_command, dry_run);
            if (ret == 0 && !dry_run && xr_fs_rename(tmp, object_paths[si]) != 0) {
                fprintf(stderr, "Error: cannot install cached native object '%s'\n",
                        object_paths[si]);
                xr_fs_remove(tmp);
                ret = 1;
            }
            if (ret != 0)
                break;
        }
        bool retain_unit =
            xaot_link_manifest_contains(link_manifest, XAOT_LINK_NATIVE_INPUT, unit->output) ||
            xaot_link_manifest_contains(link_manifest, XAOT_LINK_LD_FLAG, unit->output) ||
            xaot_native_unit_owns_entry_stub(native_plan, unit);
        xaot_cli_manifest_remove_string(&link_manifest->native_inputs,
                                        &link_manifest->n_native_inputs, unit->output);
        xaot_cli_manifest_remove_string(&link_manifest->ld_flags, &link_manifest->n_ld_flags,
                                        unit->output);
        for (uint32_t si = 0; ret == 0 && retain_unit && si < unit->source_count; si++) {
            if (!xaot_link_manifest_add_unique(link_manifest, XAOT_LINK_NATIVE_INPUT,
                                               objects[si])) {
                fprintf(stderr, "Error: cannot isolate native unit link object '%s[%u]'\n",
                        unit->name ? unit->name : "?", si);
                ret = 1;
            }
        }
        xr_free(objects);
        xr_free(object_paths);
        if (ret != 0)
            return ret;
        /* The manifest spelling is a package-level identity.  Final linking
         * uses each target/toolchain-keyed cache object directly.  This keeps
         * multi-source units portable across Mach-O/ELF/COFF and prevents
         * concurrent cross-target builds from sharing a package-tree object.
         * Entry stubs are linker roots, so their audited unit is retained even
         * when ordinary extern-symbol reachability does not mention it. */
    }
    for (uint32_t ei = 0; ei < native_plan->entry_count; ei++) {
        const XrFreestandingEntryPlan *entry = &native_plan->entries[ei];
        bool found = entry->stub == NULL;
        for (uint32_t ui = 0; !found && ui < native_plan->unit_count; ui++)
            found = xaot_native_unit_owns_stub(&native_plan->units[ui], entry->stub);
        if (!found) {
            fprintf(stderr,
                    "Error: E-ENTRY-STUB-UNIT: freestanding entry '%s' stub must be a source of "
                    "an audited native unit\n",
                    entry->xray_name ? entry->xray_name : "?");
            return 1;
        }
    }
    for (uint32_t ti = 0; ti < native_plan->target_count; ti++) {
        const XrNativeTargetPlan *native_target = &native_plan->targets[ti];
        const char *actual =
            target && target->zig_triple ? target->zig_triple : (target ? target->name : NULL);
        if (actual && strcmp(native_target->triple, actual) != 0)
            continue;
        for (uint32_t li = 0; li < native_target->system_link_count; li++) {
            if (!xaot_link_manifest_add_unique(link_manifest, XAOT_LINK_SYSTEM_LIB,
                                               native_target->system_links[li]))
                return 1;
        }
    }
    return 0;
}

static void xaot_write_c_string_literal(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *) (text ? text : ""); *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', out);
        fputc((int) *p, out);
    }
    fputc('"', out);
}

static int xaot_verify_native_layouts(const XrNativePackagePlan *native_plan,
                                      const XrToolchainSelection *toolchain_plan,
                                      const XrToolchainTarget *target, const char *cache_dir,
                                      const char *sysroot, bool dump_command, bool dry_run) {
    if (!native_plan || native_plan->layout_count == 0)
        return 0;
    for (uint32_t i = 0; i < native_plan->layout_count; i++) {
        const XrNativeLayoutAssertion *layout = &native_plan->layouts[i];
        char source[XR_PATH_MAX];
        char object[XR_PATH_MAX];
        XrNativeUnit probe_unit;
        if (!layout->resolved) {
            if (!layout->declared) {
                /* No compiled module in this build declares the type, so there is
                 * no layout to disagree with.  Say so rather than failing: a
                 * package may have several entry points and only some of them
                 * pull in the C-layout modules.  A name that no entry point ever
                 * declares is a manifest error, but that is a package-wide lint,
                 * not a fact this single build can establish. */
                fprintf(stderr,
                        "Note: E-NATIVE-LAYOUT: skipping layout proof for '%s': no module in this "
                        "build declares it\n",
                        layout->xray_type ? layout->xray_type : "?");
                continue;
            }
            fprintf(stderr,
                    "Error: E-NATIVE-LAYOUT: Xray type '%s' is declared but has no fixed layout to "
                    "prove\n",
                    layout->xray_type ? layout->xray_type : "?");
            return 1;
        }
        snprintf(source, sizeof(source), "%s/layout-%u.%d.c", cache_dir, i,
                 (int) xr_proc_self_pid());
        snprintf(object, sizeof(object), "%s/layout-%u.%d%s", cache_dir, i,
                 (int) xr_proc_self_pid(), xaot_cli_object_suffix(toolchain_plan));
        if (!dry_run) {
            FILE *f = fopen(source, "w");
            if (!f) {
                fprintf(stderr, "Error: cannot create native layout probe '%s'\n", source);
                return 1;
            }
            fputs("#include <stddef.h>\n#include ", f);
            xaot_write_c_string_literal(f, layout->header);
            fputs("\n", f);
            if (layout->assert_size)
                fprintf(f, "_Static_assert(sizeof(%s) == %u, \"Xray/C size mismatch for %s\");\n",
                        layout->c_type, layout->expected_size, layout->c_type);
            if (layout->assert_align)
                fprintf(f,
                        "_Static_assert(_Alignof(%s) == %u, \"Xray/C alignment mismatch for "
                        "%s\");\n",
                        layout->c_type, layout->expected_align, layout->c_type);
            if (layout->assert_fields) {
                for (uint32_t fi = 0; fi < layout->field_count; fi++)
                    fprintf(f,
                            "_Static_assert(offsetof(%s, %s) == %u, \"Xray/C field offset "
                            "mismatch for %s.%s\");\n",
                            layout->c_type, layout->field_names[fi], layout->field_offsets[fi],
                            layout->c_type, layout->field_names[fi]);
            }
            if (fclose(f) != 0) {
                xr_fs_remove(source);
                return 1;
            }
        }
        memset(&probe_unit, 0, sizeof(probe_unit));
        probe_unit.name = layout->xray_type;
        probe_unit.kind = XR_NATIVE_UNIT_C;
        probe_unit.language_standard = "c11";
        probe_unit.optimization = "none";
        probe_unit.visibility = "hidden";
        probe_unit.warning_policy = "strict";
        int ret = xaot_invoke_native_unit_compile(toolchain_plan, target, &probe_unit, source,
                                                  object, sysroot, dump_command, dry_run);
        if (!dry_run) {
            xr_fs_remove(source);
            xr_fs_remove(object);
        }
        if (ret != 0) {
            fprintf(stderr, "Error: E-NATIVE-LAYOUT: header proof failed for '%s' and '%s'\n",
                    layout->xray_type, layout->c_type);
            return ret;
        }
    }
    return 0;
}

/* Resolve the object cache directory, then /aot/<target>.  Precedence:
 * --cache-dir flag, then $XRAY_CACHE_DIR, then <output dir>/.xray-cache.
 * Created on demand.  Returns 0 on success. */
static int xaot_resolve_cache_dir(const char *output_file, const XrToolchainTarget *target,
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

static int xaot_evidence_cache_manifest_path(const char *cache_dir,
                                             const XgEvidenceCacheManifest *manifest,
                                             uint32_t phase, char *out, size_t out_sz) {
    const XgEvidenceCacheKey *key = xg_evidence_cache_manifest_find(manifest, phase);
    char phase_dir[XR_PATH_MAX];
    int n;

    if (!cache_dir || !manifest || !key || !out || out_sz == 0)
        return -1;
    n = snprintf(phase_dir, sizeof(phase_dir), "%s/evidence/%s", cache_dir,
                 xg_evidence_cache_phase_name(phase));
    if (n < 0 || (size_t) n >= sizeof(phase_dir))
        return -1;
    if (xaot_mkdir_p(phase_dir) != 0)
        return -1;
    n = snprintf(out, out_sz, "%s/%016llx.xgcache", phase_dir,
                 (unsigned long long) xg_evidence_cache_key_hash(key));
    return (n >= 0 && (size_t) n < out_sz) ? 0 : -1;
}

static int xaot_evidence_cache_payload_path(const char *cache_dir,
                                            const XgEvidenceCacheManifest *manifest, uint32_t phase,
                                            char *out, size_t out_sz) {
    const XgEvidenceCacheKey *key = xg_evidence_cache_manifest_find(manifest, phase);
    char phase_dir[XR_PATH_MAX];
    int n;

    if (!cache_dir || !manifest || !key || !out || out_sz == 0)
        return -1;
    n = snprintf(phase_dir, sizeof(phase_dir), "%s/evidence/%s", cache_dir,
                 xg_evidence_cache_phase_name(phase));
    if (n < 0 || (size_t) n >= sizeof(phase_dir))
        return -1;
    if (xaot_mkdir_p(phase_dir) != 0)
        return -1;
    n = snprintf(out, out_sz, "%s/%016llx.xgpayload", phase_dir,
                 (unsigned long long) xg_evidence_cache_key_hash(key));
    return (n >= 0 && (size_t) n < out_sz) ? 0 : -1;
}

static bool xaot_read_evidence_cache_manifest(const char *path,
                                              XgEvidenceCacheManifest *out_manifest) {
    char text[2048];
    FILE *f;
    size_t n;
    bool ok;

    if (!path || !out_manifest)
        return false;
    f = fopen(path, "rb");
    if (!f)
        return false;
    n = fread(text, 1, sizeof(text) - 1, f);
    ok = !ferror(f) && feof(f);
    fclose(f);
    if (!ok)
        return false;
    text[n] = '\0';
    return xg_evidence_cache_manifest_parse(text, out_manifest);
}

static void xaot_write_evidence_cache_text(const char *path, const char *text) {
    char tmp[XR_PATH_MAX];
    FILE *f;
    int n;

    if (!path || !text)
        return;
    n = snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int) xr_proc_self_pid());
    if (n < 0 || (size_t) n >= sizeof(tmp))
        return;
    /* Cache payload headers hash the exact LF-delimited bytes.  Binary mode
     * prevents the Windows CRT from rewriting them to CRLF on disk. */
    f = fopen(tmp, "wb");
    if (!f)
        return;
    bool write_ok = fputs(text, f) >= 0;
    if (fclose(f) != 0)
        write_ok = false;
    if (!write_ok) {
        xr_fs_remove(tmp);
        return;
    }
    if (xr_fs_rename(tmp, path) != 0)
        xr_fs_remove(tmp);
}

static void xaot_write_evidence_cache_manifest(const char *path,
                                               const XgEvidenceCacheManifest *manifest) {
    char text[1400];
    if (!path || !manifest || !xg_evidence_cache_manifest_format(manifest, text, sizeof(text)))
        return;
    xaot_write_evidence_cache_text(path, text);
}

static bool xaot_read_evidence_cache_payload(const char *path, const XgEvidenceCacheKey *expected) {
    size_t size = 0;
    char *text;
    bool ok;
    if (!path || !expected)
        return false;
    text = xr_file_read_all(path, "rb", &size);
    if (!text)
        return false;
    (void) size;
    ok = xg_evidence_cache_payload_matches(text, expected);
    if (ok) {
        XgGlobalEvidence materialized;
        XgEvidenceCacheKey materialized_key;
        memset(&materialized, 0, sizeof(materialized));
        ok = xg_evidence_cache_payload_materialize(text, &materialized);
        if (ok) {
            materialized_key = xg_global_evidence_cache_key(&materialized, expected->phase);
            ok = xg_evidence_cache_key_matches(&materialized_key, expected);
        }
        xg_global_evidence_free(&materialized);
    }
    xr_free(text);
    return ok;
}

static void xaot_probe_evidence_cache_manifest(const char *cache_dir,
                                               const XgEvidenceCacheManifest *manifest,
                                               char *const payloads[XG_EVIDENCE_CACHE_PHASE_COUNT],
                                               bool verbose, bool force_rebuild, bool dry_run) {
    static const uint32_t phases[] = {
        XG_EVIDENCE_CACHE_DECLARATIONS,
        XG_EVIDENCE_CACHE_SEMANTIC_GRAPH,
        XG_EVIDENCE_CACHE_BODY_SUMMARY,
        XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE,
    };
    uint32_t hits = 0;
    uint32_t misses = 0;

    if (!cache_dir || !manifest)
        return;
    for (uint32_t i = 0; i < XG_EVIDENCE_CACHE_PHASE_COUNT; i++) {
        uint32_t phase = phases[i];
        const XgEvidenceCacheKey *expected = xg_evidence_cache_manifest_find(manifest, phase);
        XgEvidenceCacheManifest cached;
        char path[XR_PATH_MAX];
        char payload_path[XR_PATH_MAX];
        bool manifest_hit = false;
        bool payload_hit = false;
        bool hit;

        if (!expected ||
            xaot_evidence_cache_manifest_path(cache_dir, manifest, phase, path, sizeof(path)) != 0)
            continue;
        if (xaot_evidence_cache_payload_path(cache_dir, manifest, phase, payload_path,
                                             sizeof(payload_path)) != 0)
            continue;
        if (!force_rebuild && xaot_read_evidence_cache_manifest(path, &cached))
            manifest_hit = xg_evidence_cache_manifest_phase_matches(&cached, expected);
        if (!force_rebuild)
            payload_hit = xaot_read_evidence_cache_payload(payload_path, expected);
        hit = manifest_hit && payload_hit;
        if (hit)
            hits++;
        else
            misses++;
        if (verbose) {
            printf("[xi-native] evidence cache %s: %s (%016llx manifest=%s payload=%s)%s\n",
                   xg_evidence_cache_phase_name(phase), hit ? "hit" : "miss",
                   (unsigned long long) xg_evidence_cache_key_hash(expected),
                   manifest_hit ? "hit" : "miss", payload_hit ? "hit" : "miss",
                   force_rebuild ? " rebuild" : "");
        }
        if (!dry_run) {
            xaot_write_evidence_cache_manifest(path, manifest);
            if (payloads && payloads[i])
                xaot_write_evidence_cache_text(payload_path, payloads[i]);
        }
    }
    if (verbose)
        printf("[xi-native] evidence cache summary: hits=%u misses=%u%s\n", hits, misses,
               force_rebuild ? " rebuild" : "");
}

/* Compile one module's generated C to an object file, reusing a cached object
 * when the content hash already exists.  Fills `obj_out` with the object path
 * (which the caller then links).  Returns 0 on success. */
static int xaot_compile_source_cached(const XrToolchainSelection *plan,
                                      const XrToolchainTarget *target,
                                      const XaotLinkManifest *manifest, const char *opt_flag,
                                      const XaotModuleSource *src, const char *cache_dir,
                                      const char *sysroot, bool dump_command, bool keep_c,
                                      bool verbose, bool force_rebuild, bool dry_run, char *obj_out,
                                      size_t obj_out_sz, XiCgenCDialect c_dialect) {
    uint64_t key =
        xaot_object_cache_key(src->c_source, opt_flag, target, plan, manifest, sysroot, c_dialect);
    char tmp_c[XR_PATH_MAX];
    char tmp_o[XR_PATH_MAX];
    int ret;

    snprintf(obj_out, obj_out_sz, "%s/%016llx%s", cache_dir, (unsigned long long) key,
             xaot_cli_object_suffix(plan));
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
    snprintf(tmp_o, sizeof(tmp_o), "%s/%016llx.%d%s.tmp", cache_dir, (unsigned long long) key,
             (int) xr_proc_self_pid(), xaot_cli_object_suffix(plan));

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
                                            : "/* No manifest export symbols. */\n",
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
           !xaot_cli_manifest_uses_aot_core(manifest) && manifest->n_runtime_objects == 0 &&
           manifest->n_stdlib_objects == 0;
}

static uint32_t xaot_cli_provider_hook_by_name(const char *name) {
    if (!name)
        return 0;
    if (strcmp(name, "task_alloc") == 0)
        return XAOT_PROVIDER_HOOK_TASK_ALLOC;
    if (strcmp(name, "submit") == 0)
        return XAOT_PROVIDER_HOOK_SUBMIT;
    if (strcmp(name, "park_wake") == 0)
        return XAOT_PROVIDER_HOOK_PARK_WAKE;
    if (strcmp(name, "timer") == 0)
        return XAOT_PROVIDER_HOOK_TIMER;
    if (strcmp(name, "executor_pump") == 0)
        return XAOT_PROVIDER_HOOK_EXECUTOR_PUMP;
    if (strcmp(name, "interrupt_complete") == 0)
        return XAOT_PROVIDER_HOOK_INTERRUPT_COMPLETE;
    if (strcmp(name, "assertion_report") == 0)
        return XAOT_PROVIDER_HOOK_ASSERTION_REPORT;
    if (strcmp(name, "alloc") == 0)
        return XAOT_PROVIDER_HOOK_ALLOC;
    if (strcmp(name, "free") == 0)
        return XAOT_PROVIDER_HOOK_FREE;
    if (strcmp(name, "panic") == 0)
        return XAOT_PROVIDER_HOOK_PANIC;
    return 0;
}

static uint32_t xaot_cli_provider_capability_by_name(const char *name) {
    uint32_t count = 0;
    const uint32_t *catalog = xg_capability_catalog(&count);
    if (!name)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(name, xg_capability_name(catalog[i])) == 0)
            return catalog[i];
    }
    return 0;
}

static bool xaot_cli_provider_from_target_config(const XrTargetConfig *config,
                                                 XrCliBuildProfile profile,
                                                 const XrToolchainTarget *target,
                                                 XaotTargetCapabilityProvider *out,
                                                 bool *out_present,
                                                 uint64_t *out_runtime_provider_mask,
                                                 char *err, size_t err_size) {
    bool present = config && ((config->runtime_provider && config->runtime_provider[0]) ||
                              config->n_runtime_capabilities > 0 || config->n_runtime_hooks > 0);
    if (out_present)
        *out_present = present;
    if (out_runtime_provider_mask)
        *out_runtime_provider_mask = 0;
    if (!present)
        return true;
    if (!out || !config->runtime_provider || !config->runtime_provider[0]) {
        snprintf(err, err_size,
                 "target runtime_capabilities/runtime_hooks require runtime_provider identity");
        return false;
    }
    if (profile != XR_CLI_BUILD_PROFILE_FREESTANDING) {
        snprintf(err, err_size, "runtime_provider is only valid for freestanding targets");
        return false;
    }
    if (strcmp(config->runtime_provider,
               XAOT_FREESTANDING_HOOK_PROVIDER_ID) != 0) {
        snprintf(err, err_size,
                 "unsupported freestanding runtime_provider '%s'; expected exact built-in identity '%s' or a validated provider manifest",
                 config->runtime_provider,
                 XAOT_FREESTANDING_HOOK_PROVIDER_ID);
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->abi_version = XAOT_PROVIDER_ABI_VERSION;
    bool assertion_report_capability = false;
    bool assertion_report_hook = false;
    bool allocator_capability = false;
    bool panic_capability = false;
    bool alloc_hook = false;
    bool free_hook = false;
    bool panic_hook = false;
    for (int i = 0; i < config->n_runtime_capabilities; i++) {
        const char *name = config->runtime_capabilities[i];
        if (name && strcmp(name, "assertion-report") == 0) {
            assertion_report_capability = true;
            continue;
        }
        if (name && strcmp(name, "allocator") == 0) {
            allocator_capability = true;
            continue;
        }
        if (name && strcmp(name, "panic") == 0) {
            panic_capability = true;
            continue;
        }
        uint32_t capability = xaot_cli_provider_capability_by_name(name);
        if (capability == 0) {
            snprintf(err, err_size, "unknown target runtime capability '%s'", name ? name : "");
            return false;
        }
        out->provided_capability_bits |= capability;
    }
    for (int i = 0; i < config->n_runtime_hooks; i++) {
        const char *name = config->runtime_hooks[i];
        uint32_t hook = xaot_cli_provider_hook_by_name(name);
        if (hook == 0) {
            snprintf(err, err_size, "unknown target runtime hook '%s'", name ? name : "");
            return false;
        }
        if (hook == XAOT_PROVIDER_HOOK_ASSERTION_REPORT)
            assertion_report_hook = true;
        else if (hook == XAOT_PROVIDER_HOOK_ALLOC)
            alloc_hook = true;
        else if (hook == XAOT_PROVIDER_HOOK_FREE)
            free_hook = true;
        else if (hook == XAOT_PROVIDER_HOOK_PANIC)
            panic_hook = true;
        out->hook_bits |= hook;
    }
    if (assertion_report_capability != assertion_report_hook) {
        snprintf(err, err_size,
                 "freestanding assertion-report capability and exact hook must be declared together");
        return false;
    }
    if (allocator_capability != (alloc_hook && free_hook) || alloc_hook != free_hook) {
        snprintf(err, err_size,
                 "freestanding allocator capability requires exact alloc and free hooks");
        return false;
    }
    if (panic_capability != panic_hook) {
        snprintf(err, err_size,
                 "freestanding panic capability requires the exact panic hook");
        return false;
    }
    if (!allocator_capability || !panic_capability) {
        snprintf(err, err_size,
                 "freestanding provider is missing allocator or panic foundation hooks");
        return false;
    }
    if (out_runtime_provider_mask) {
        *out_runtime_provider_mask |=
            XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR) |
            XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC);
    }
    if (assertion_report_capability && out_runtime_provider_mask)
        *out_runtime_provider_mask |= XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_IO);
    uint64_t hash = XR_FNV64_OFFSET_BASIS;
    hash = xaot_hash_fold_str(hash, "xray-target-runtime-provider-v1");
    hash = xaot_hash_fold_str(hash, config->runtime_provider);
    hash = xaot_hash_fold_str(hash, target ? target->name : NULL);
    hash = xaot_hash_fold_string_list(hash, config->runtime_capabilities,
                                      (uint32_t) config->n_runtime_capabilities);
    hash =
        xaot_hash_fold_string_list(hash, config->runtime_hooks, (uint32_t) config->n_runtime_hooks);
    out->target_metadata_hash = hash ? hash : 1;
    return true;
}

static int cmd_build_native(
    const char *input, const char *output, const char *cc, const char *opt_flag,
    XaotOptimizationLevel optimization, const char *cpu, XaotSimdMode simd_mode, bool c_only,
    bool strip, bool debug_symbols, XaotArtifactKind artifact_kind, XrCliBuildProfile profile,
    XiCgenCDialect c_dialect, XiCgenTypeNameProfile type_name_profile, const char *sysroot,
    const char *linker_script, bool verbose, bool dump_xaot_plan, bool dump_global_evidence,
    bool dump_xi_evidence, bool dump_link_manifest, bool dump_residue, bool dump_link_command,
    bool dry_run_link, const char *c_header, bool keep_c, const char *cache_dir_arg, bool rebuild,
    bool lto, bool rc_guard, const XrToolchainTarget *target,
    const XrToolchainSelection *toolchain_plan, const XrTargetConfig *target_config,
    const XrProject *project, const XrNativePackagePlan *native_package_plan,
    const char *objcopy_output) {
    XaotBuildResult aot_result;
    XaotBuildOptions build_options;
    XrModuleIdentityAuthority entry_authority = {0};
    char *entry_authority_namespace = NULL;
    char *entry_authority_root = NULL;
    XrTargetProfile *target_profile = NULL;
    XaotTargetCapabilityProvider capability_provider;
    bool has_capability_provider = false;
    uint64_t runtime_provider_mask = 0;
    XaotTarget build_target;
    XaotBuildProfile aot_profile = profile == XR_CLI_BUILD_PROFILE_FREESTANDING
                                       ? XAOT_BUILD_PROFILE_FREESTANDING
                                       : XAOT_BUILD_PROFILE_HOSTED;
    bool library_artifact = artifact_kind != XAOT_ARTIFACT_EXECUTABLE;
    bool shared_library = artifact_kind == XAOT_ARTIFACT_SHARED_LIBRARY;
    char cache_dir[XR_PATH_MAX];
    bool cache_dir_ready = false;
    if (xaot_resolve_cache_dir(output, target, cache_dir_arg, cache_dir, sizeof(cache_dir)) != 0) {
        fprintf(stderr, "Error: cannot create AOT cache directory '%s'\n", cache_dir);
        return 1;
    }
    cache_dir_ready = true;
    if (!xaot_target_init(&build_target, target && target->name ? target->name : "native-c90")) {
        fprintf(stderr, "Error: failed to initialize AOT build target\n");
        return 1;
    }
    {
        char simd_err[192];
        if (!xaot_target_configure_simd(&build_target, simd_mode, cpu, simd_err,
                                        sizeof(simd_err))) {
            fprintf(stderr, "Error: %s\n", simd_err);
            xaot_target_free(&build_target);
            return 2;
        }
    }
    memset(&build_options, 0, sizeof(build_options));
    {
        char provider_err[256];
        if (!xaot_cli_provider_from_target_config(target_config, profile, target,
                                                  &capability_provider, &has_capability_provider,
                                                  &runtime_provider_mask,
                                                  provider_err, sizeof(provider_err))) {
            fprintf(stderr, "Error: %s\n", provider_err);
            xr_target_profile_free(target_profile);
            xaot_target_free(&build_target);
            return 1;
        }
    }
    {
        XrTargetCodegenFacts codegen;
        char profile_err[256] = "invalid numeric target codegen facts";
        bool profile_ok = xaot_target_profile_codegen_facts(&build_target, &codegen) &&
                          (profile == XR_CLI_BUILD_PROFILE_HOSTED
                               ? xtc_target_profile_build_native_hosted(
                                     target, &codegen, &target_profile, profile_err,
                                     sizeof(profile_err))
                               : xtc_target_profile_build_native_freestanding(
                                     target, &codegen, runtime_provider_mask, &target_profile,
                                     profile_err, sizeof(profile_err)));
        if (!profile_ok) {
            fprintf(stderr, "Error: %s\n", profile_err);
            xaot_target_free(&build_target);
            return 1;
        }
    }
    build_options.target = &build_target;
    build_options.target_profile = target_profile;
    build_options.native_package_plan = native_package_plan;
    bool have_entry_authority =
        project ? xr_project_module_identity_authority(
                      project, &entry_authority, &entry_authority_namespace,
                      &entry_authority_root)
                : xr_module_identity_script_authority_from_source(
                      input, &entry_authority, &entry_authority_root);
    if (!have_entry_authority) {
        fprintf(stderr, "Error: failed to establish exact entry module identity authority\n");
        xr_free(entry_authority_namespace);
        xr_free(entry_authority_root);
        xr_target_profile_free(target_profile);
        xaot_target_free(&build_target);
        return 1;
    }
    build_options.entry_module_authority = entry_authority;
    XrLockfile *build_lockfile = NULL;
    char lockfile_path[XR_PATH_MAX];
    if (project && project->root) {
        snprintf(lockfile_path, sizeof(lockfile_path), "%s/xray.lock", project->root);
        build_lockfile = xr_lockfile_load(lockfile_path);
    }
    build_options.lockfile = build_lockfile;
    build_options.capability_provider = has_capability_provider ? &capability_provider : NULL;
    build_options.profile = aot_profile;
    build_options.c_dialect = c_dialect;
    build_options.type_name_profile = type_name_profile;
    build_options.emit_plan_dump = dump_xaot_plan;
    build_options.artifact_kind = artifact_kind;
    build_options.emit_global_evidence_dump = dump_global_evidence;
    build_options.emit_local_evidence_dump = dump_xi_evidence;
    build_options.emit_residue_dump = dump_residue;
    build_options.incremental_cache_dir = cache_dir_ready ? cache_dir : NULL;
    build_options.incremental_cache_rebuild = rebuild;
    build_options.incremental_cache_verbose = verbose;
    int rc = xaot_build(input, &build_options, &aot_result);
    xr_lockfile_free(build_lockfile);
    xr_free(entry_authority_namespace);
    xr_free(entry_authority_root);
    xr_target_profile_free(target_profile);
    xaot_target_free(&build_target);
    if (rc != 0)
        return rc;
    {
        char normalize_err[512];
        aot_result.link_manifest.compile.optimization = optimization;
        aot_result.link_manifest.link.strip = strip;
        aot_result.link_manifest.link.shared =
            library_artifact && profile != XR_CLI_BUILD_PROFILE_FREESTANDING;
        aot_result.link_manifest.link.relocatable =
            shared_library && profile == XR_CLI_BUILD_PROFILE_FREESTANDING;
        aot_result.link_manifest.link.resolve_from_host =
            artifact_kind == XAOT_ARTIFACT_HOSTED_FRAGMENT;
        if (!xaot_cli_normalize_manifest_for_target(&aot_result.link_manifest, target,
                                                    normalize_err, sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        /* --emit-c does not invoke a native provider, so it neither needs nor
         * can scope provider-specific sanitizer argv. */
        if (!c_only &&
            !xaot_cli_add_build_sanitizer_flags(&aot_result.link_manifest, target, toolchain_plan,
                                                normalize_err, sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        /* Task 219 --rc-guard: compile the generated C with the RC guard macro
         * so xrt_arc.h poisons released objects and aborts on use-after-release. */
        if (rc_guard && !xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_DEFINE,
                                                       "XR_RC_GUARD")) {
            fprintf(stderr, "Error: failed to add --rc-guard compile definition\n");
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
        if (!xaot_cli_add_linker_script(&aot_result.link_manifest, linker_script, normalize_err,
                                        sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        if (!xaot_cli_add_target_config_flags(&aot_result.link_manifest, target_config,
                                              toolchain_plan, normalize_err,
                                              sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        for (uint32_t ui = 0; native_package_plan && ui < native_package_plan->unit_count; ui++) {
            const XrNativeUnit *unit = &native_package_plan->units[ui];
            if (unit->kind == XR_NATIVE_UNIT_PLATFORM)
                continue;
            for (uint32_t li = 0; li < unit->system_link_count; li++) {
                if (!xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_SYSTEM_LIB,
                                                   unit->system_links[li])) {
                    fprintf(stderr, "Error: failed to add native system link '%s'\n",
                            unit->system_links[li]);
                    xaot_build_result_free(&aot_result);
                    return 1;
                }
            }
        }
        for (uint32_t ti = 0; native_package_plan && ti < native_package_plan->target_count; ti++) {
            const XrNativeTargetPlan *native_target = &native_package_plan->targets[ti];
            const char *actual =
                target && target->zig_triple ? target->zig_triple : (target ? target->name : NULL);
            if (actual && strcmp(native_target->triple, actual) != 0)
                continue;
            for (uint32_t li = 0; li < native_target->system_link_count; li++) {
                if (!xaot_link_manifest_add_unique(&aot_result.link_manifest, XAOT_LINK_SYSTEM_LIB,
                                                   native_target->system_links[li])) {
                    fprintf(stderr, "Error: failed to add target system link '%s'\n",
                            native_target->system_links[li]);
                    xaot_build_result_free(&aot_result);
                    return 1;
                }
            }
        }
        /* Whole-program LTO: compile each module to bitcode and let the linker
         * inline across modules.  The per-module bitcode stays content-addressed
         * (the cache key folds in cc flags), so only changed modules recompile
         * while every link re-runs cross-module optimization. */
        aot_result.link_manifest.compile.lto = lto;
        aot_result.link_manifest.link.lto = lto;
        /* LLVM's AArch64 machine outliner can replace repeated constant setup
         * and hot scalar/vector kernels with local calls. That is a useful
         * size trade-off for -Os, but it adds a call/return to leaf routines
         * in speed profiles. Keep the release policy target/toolchain based;
         * no source function or package receives a special flag. */
        bool clang_family =
            toolchain_plan && (toolchain_plan->provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG ||
                               toolchain_plan->provider == XR_TOOLCHAIN_PROVIDER_LLVM_CLANG ||
                               toolchain_plan->provider == XR_TOOLCHAIN_PROVIDER_ZIG);
        const char *target_arch = aot_result.link_manifest.target.arch;
        bool aarch64_target = target_arch && (strcmp(target_arch, "aarch64") == 0 ||
                                              strcmp(target_arch, "arm64") == 0);
#if defined(__aarch64__) || defined(__arm64__)
        if (!aarch64_target && target && target->is_native)
            aarch64_target = true;
#endif
        aot_result.link_manifest.compile.disable_machine_outliner =
            clang_family && aarch64_target && optimization != XAOT_OPTIMIZATION_SIZE;
        aot_result.link_manifest.compile.pic =
            library_artifact && (!target || target->os != XR_TOOLCHAIN_TARGET_OS_WINDOWS);
        if (debug_symbols && !xaot_cli_add_build_debug_flags(
                                 &aot_result.link_manifest, normalize_err, sizeof(normalize_err))) {
            fprintf(stderr, "Error: %s\n", normalize_err);
            xaot_build_result_free(&aot_result);
            return 1;
        }
    }
    if (cpu && cpu[0]) {
        if (snprintf(aot_result.link_manifest.compile.cpu,
                     sizeof(aot_result.link_manifest.compile.cpu), "%s", cpu) < 0 ||
            strlen(cpu) >= sizeof(aot_result.link_manifest.compile.cpu)) {
            fprintf(stderr, "Error: --cpu value is too long\n");
            xaot_build_result_free(&aot_result);
            return 1;
        }
    }
    if (shared_library && xaot_link_manifest_needs_runtime(&aot_result.link_manifest)) {
        fprintf(stderr,
                "Error: --artifact shared-library does not support runtime-backed features yet; "
                "export scalar/raw-pointer functions or build an executable\n");
        xaot_build_result_free(&aot_result);
        return 1;
    }
    if (dump_xaot_plan && aot_result.plan_dump) {
        printf("%s", aot_result.plan_dump);
        if (aot_result.plan_dump[0] &&
            aot_result.plan_dump[strlen(aot_result.plan_dump) - 1] != '\n')
            printf("\n");
    }
    if (dump_global_evidence && aot_result.global_evidence_dump) {
        printf("%s", aot_result.global_evidence_dump);
        if (aot_result.global_evidence_dump[0] &&
            aot_result.global_evidence_dump[strlen(aot_result.global_evidence_dump) - 1] != '\n')
            printf("\n");
    }
    if (dump_xi_evidence && aot_result.local_evidence_dump) {
        printf("%s", aot_result.local_evidence_dump);
        if (aot_result.local_evidence_dump[0] &&
            aot_result.local_evidence_dump[strlen(aot_result.local_evidence_dump) - 1] != '\n')
            printf("\n");
    }
    if (dump_residue && aot_result.residue_dump) {
        printf("%s", aot_result.residue_dump);
        if (aot_result.residue_dump[0] &&
            aot_result.residue_dump[strlen(aot_result.residue_dump) - 1] != '\n')
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

    /* --c-only emits one compilable amalgamated translation unit.  Put the
     * entry unit first so its XRT_IMPL definitions are seen before the shared
     * runtime headers' include guards; all remaining units then contribute
     * declarations and module-local code only. */
    if (c_only) {
        size_t amalgamated_size = 0;
        char *amalgamated = xaot_build_result_amalgamate(&aot_result, &amalgamated_size);
        FILE *f = amalgamated ? fopen(output, "wb") : NULL;
        bool write_ok = f && fwrite(amalgamated, 1, amalgamated_size, f) == amalgamated_size;
        if (f && fclose(f) != 0)
            write_ok = false;
        if (!write_ok) {
            fprintf(stderr, "Error: cannot create '%s'\n", output);
            xr_free(amalgamated);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        xr_free(amalgamated);
        printf("Generated: %s\n", output);
        xaot_build_result_free(&aot_result);
        return 0;
    }

    if (xaot_verify_native_layouts(native_package_plan, toolchain_plan, target, cache_dir, sysroot,
                                   dump_link_command || verbose, dry_run_link) != 0) {
        xaot_build_result_free(&aot_result);
        return 1;
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
    if (aot_result.has_evidence_cache_manifest) {
        xaot_probe_evidence_cache_manifest(cache_dir, &aot_result.evidence_cache_manifest,
                                           aot_result.evidence_cache_payloads, verbose, rebuild,
                                           dry_run_link);
    }

    if (xaot_compile_native_package(native_package_plan, &aot_result.link_manifest, toolchain_plan,
                                    target, cache_dir, sysroot, verbose, rebuild,
                                    dump_link_command || verbose, dry_run_link) != 0) {
        xaot_build_result_free(&aot_result);
        return 1;
    }

    /* COFF has no equivalent of the ELF/Mach-O `ld -r` operation for merging
     * several objects into one relocatable object.  The generated-C contract
     * already provides a verified, compilable amalgamation for --c-only; use
     * that same representation as the provider lowering for a multi-module
     * freestanding object on Windows.  This preserves the requested artifact
     * kind (one relocatable .o), instead of silently changing it to an archive
     * or shared library. */
    bool has_objcopy = target_config && objcopy_output && objcopy_output[0];
    bool coff_relocatable_amalgamation = aot_result.link_manifest.link.relocatable && target &&
                                         target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS &&
                                         n_sources > 1;
    if (coff_relocatable_amalgamation) {
        if (aot_result.link_manifest.n_runtime_objects != 0 ||
            aot_result.link_manifest.n_stdlib_objects != 0 ||
            aot_result.link_manifest.n_native_inputs != 0 ||
            aot_result.link_manifest.n_system_libs != 0 ||
            aot_result.link_manifest.n_ld_flags != 0 ||
            aot_result.link_manifest.link.linker_script[0] != '\0') {
            fprintf(stderr,
                    "Error: COFF relocatable amalgamation cannot honor link-stage inputs\n");
            xaot_build_result_free(&aot_result);
            return 1;
        }

        size_t amalgamated_size = 0;
        char *amalgamated = xaot_build_result_amalgamate(&aot_result, &amalgamated_size);
        XaotModuleSource unity_source = {.name = "coff-relocatable-amalgamation",
                                         .c_source = amalgamated};
        char cached_object[XR_PATH_MAX];
        int ret = amalgamated ? xaot_compile_source_cached(
                                    toolchain_plan, target, &aot_result.link_manifest, opt_flag,
                                    &unity_source, cache_dir, sysroot, dump_link_command || verbose,
                                    keep_c, verbose, rebuild, dry_run_link, cached_object,
                                    sizeof(cached_object), c_dialect)
                              : 1;
        if (!amalgamated)
            fprintf(stderr, "Error: cannot amalgamate COFF relocatable translation units\n");
        if (ret == 0 && !dry_run_link && xaot_copy_file(cached_object, output, 0644) != 0) {
            fprintf(stderr, "Error: cannot create COFF relocatable object '%s'\n", output);
            ret = 1;
        }
        if (ret == 0 && has_objcopy)
            ret = invoke_target_objcopy(target_config, output, objcopy_output,
                                        dump_link_command || verbose || dry_run_link, dry_run_link);
        if (ret == 0 && (dump_link_command || verbose || dry_run_link))
            printf("COFF relocatable: one amalgamated translation unit; no link stage\n");
        if (ret == 0 && !dry_run_link)
            printf("Generated: %s\n", output);
        xr_free(amalgamated);
        xaot_build_result_free(&aot_result);
        return ret;
    }

    bool use_link_output_cache = !has_objcopy && !rebuild && !dry_run_link && !dump_link_command &&
                                 !verbose && !debug_symbols && !library_artifact && !keep_c;
    uint64_t link_output_cache_key = 0;
    if (use_link_output_cache) {
        link_output_cache_key =
            xaot_link_output_cache_key(&aot_result, opt_flag, target, toolchain_plan, sysroot,
                                       strip, library_artifact, c_dialect);
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
        !dry_run_link && !debug_symbols && !library_artifact) {
        char c_file[XR_PATH_MAX];
        char object_file[XR_PATH_MAX];
        const char *inputs[1];
        int object_n;
        int ret;
        uint64_t key =
            link_output_cache_key
                ? link_output_cache_key
                : xaot_link_output_cache_key(&aot_result, opt_flag, target, toolchain_plan, sysroot,
                                             strip, library_artifact, c_dialect);
        if (xaot_write_temp_c_source(cache_dir, key, &aot_result.sources[0], c_file,
                                     sizeof(c_file)) != 0) {
            xaot_build_result_free(&aot_result);
            return 1;
        }
        object_n = snprintf(object_file, sizeof(object_file), "%s/%016llx.%d.fast-test%s",
                            cache_dir, (unsigned long long) key, (int) xr_proc_self_pid(),
                            xaot_cli_object_suffix(toolchain_plan));
        if (object_n < 0 || (size_t) object_n >= sizeof(object_file)) {
            fprintf(stderr, "Error: fast-test object cache path is too long\n");
            xr_fs_remove(c_file);
            xaot_build_result_free(&aot_result);
            return 1;
        }
        inputs[0] = c_file;
        ret = invoke_aot_manifest_link(toolchain_plan, target, &aot_result.link_manifest, opt_flag,
                                       output, inputs, 1, strip, library_artifact, sysroot,
                                       dump_link_command || verbose, false, object_file);
        xr_fs_remove(object_file);
        if (keep_c)
            printf("Kept C source: %s\n", c_file);
        else
            xr_fs_remove(c_file);
#ifdef XR_OS_MACOS
        if (ret == 0 && strip)
            remove_dsym_bundle(output);
#endif
        if (ret == 0 && has_objcopy)
            ret = invoke_target_objcopy(target_config, output, objcopy_output,
                                        dump_link_command || verbose, false);
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
                                         dry_run_link, obj_bufs[i], XR_PATH_MAX, c_dialect);
        if (ret != 0)
            break;
        obj_ptrs[i] = obj_bufs[i];
    }

    if (ret == 0)
        ret = invoke_aot_manifest_link(toolchain_plan, target, &aot_result.link_manifest, opt_flag,
                                       output, obj_ptrs, n_sources, strip, library_artifact,
                                       sysroot, dump_link_command || verbose || dry_run_link,
                                       dry_run_link, NULL);
#ifdef XR_OS_MACOS
    if (ret == 0 && build_dsym && !dry_run_link)
        ret = invoke_dsymutil(output, dump_link_command || verbose);
#else
    (void) build_dsym;
#endif

    if (ret == 0 && has_objcopy)
        ret = invoke_target_objcopy(target_config, output, objcopy_output,
                                    dump_link_command || verbose || dry_run_link, dry_run_link);

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
