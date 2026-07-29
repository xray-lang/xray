/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_discovery.c - Provider candidate discovery and concrete selection
 */

#include "xtc_discovery.h"

#include "xtc_process.h"
#include "../../os/os_fs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

static void xtc_discovery_error(char *err, size_t err_size, const char *format, const char *arg);

XR_FUNC bool xtc_active_apple_sdk(char *out, size_t out_size, char *err, size_t err_size) {
#ifdef XR_OS_MACOS
    XrProcessSpec spec;
    XrProcessResult result;
    xtc_process_spec_init(&spec, "xcrun", 5000);
    spec.argv[1] = "--show-sdk-path";
    spec.argv[2] = NULL;
    if (!xtc_process_run(&spec, &result, err, err_size))
        return false;
    const char *source = result.stdout_data;
    size_t len = source ? strcspn(source, "\r\n") : 0;
    bool ok = !result.timed_out && result.exit_code == 0 && len > 0 && len < out_size;
    if (ok) {
        memcpy(out, source, len);
        out[len] = '\0';
        ok = xr_fs_is_dir(out);
    }
    xtc_process_result_free(&result);
    if (!ok)
        xtc_discovery_error(err, err_size, "active Apple SDK could not be resolved", NULL);
    return ok;
#else
    (void) out;
    (void) out_size;
    xtc_discovery_error(err, err_size, "Apple SDK discovery is unavailable on this host", NULL);
    return false;
#endif
}

static void xtc_discovery_error(char *err, size_t err_size, const char *format, const char *arg) {
    if (!err || err_size == 0)
        return;
    if (arg)
        snprintf(err, err_size, format, arg);
    else
        snprintf(err, err_size, "%s", format);
}

static bool xtc_is_path_like(const char *program) {
    return program && (strchr(program, '/') || strchr(program, '\\'));
}

static bool xtc_is_executable(const char *path) {
    if (!path || !path[0])
        return false;
#ifdef XR_OS_WINDOWS
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

static bool xtc_copy_canonical_path(const char *path, char *out, size_t out_size) {
    if (xr_fs_realpath(path, out, out_size))
        return true;
    int written = snprintf(out, out_size, "%s", path);
    return written >= 0 && (size_t) written < out_size;
}

static bool xtc_candidate_in_dir(const char *dir, size_t dir_len, const char *program, char *out,
                                 size_t out_size) {
    char candidate[1200];
    int written;
    if (!dir || !program || dir_len == 0)
        return false;
    written = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int) dir_len, dir, program);
    if (written < 0 || (size_t) written >= sizeof(candidate) || !xtc_is_executable(candidate))
        return false;
    return xtc_copy_canonical_path(candidate, out, out_size);
}

XR_FUNC bool xtc_find_executable(const char *program, char *out, size_t out_size) {
    if (!program || !program[0] || !out || out_size == 0)
        return false;
    if (xtc_is_path_like(program))
        return xtc_is_executable(program) && xtc_copy_canonical_path(program, out, out_size);

    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0])
        return false;
    const char *cur = path_env;
    while (*cur) {
        const char *sep = strchr(cur,
#ifdef XR_OS_WINDOWS
                                 ';'
#else
                                 ':'
#endif
        );
        size_t len = sep ? (size_t) (sep - cur) : strlen(cur);
        if (len > 0 && xtc_candidate_in_dir(cur, len, program, out, out_size))
            return true;
#ifdef XR_OS_WINDOWS
        if (len > 0) {
            char exe_name[300];
            int written = snprintf(exe_name, sizeof(exe_name), "%s.exe", program);
            if (written >= 0 && (size_t) written < sizeof(exe_name) &&
                xtc_candidate_in_dir(cur, len, exe_name, out, out_size))
                return true;
        }
#endif
        if (!sep)
            break;
        cur = sep + 1;
    }
    return false;
}

static bool xtc_path_dir(const char *path, size_t *out_dir_len) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *backslash = path ? strrchr(path, '\\') : NULL;
    const char *sep = slash;
    if (backslash && (!sep || backslash > sep))
        sep = backslash;
    if (!sep || sep == path)
        return false;
    *out_dir_len = (size_t) (sep - path);
    return true;
}

XR_FUNC bool xtc_find_bundled_zig(const char *program_hint, char *out, size_t out_size) {
    char program_path[1200];
    size_t dir_len;
    if (!program_hint || !program_hint[0] || !out || out_size == 0)
        return false;
    if (xtc_is_path_like(program_hint)) {
        if (!xtc_copy_canonical_path(program_hint, program_path, sizeof(program_path)))
            return false;
    } else if (!xtc_find_executable(program_hint, program_path, sizeof(program_path))) {
        return false;
    }
    if (!xtc_path_dir(program_path, &dir_len))
        return false;

#ifdef XR_OS_WINDOWS
    static const char *const suffixes[] = {"zig.exe",
                                           "tools/zig/zig.exe",
                                           "tools/zig/bin/zig.exe",
                                           "../lib/xray/zig/zig.exe",
                                           "../libexec/xray/zig/zig.exe",
                                           NULL};
#else
    static const char *const suffixes[] = {"zig",
                                           "tools/zig/zig",
                                           "tools/zig/bin/zig",
                                           "../lib/xray/zig/zig",
                                           "../libexec/xray/zig/zig",
                                           NULL};
#endif
    for (size_t i = 0; suffixes[i]; i++) {
        if (xtc_candidate_in_dir(program_path, dir_len, suffixes[i], out, out_size))
            return true;
    }
    return false;
}

static bool xtc_candidates_add(XrToolchainCandidates *candidates, XrToolchainProviderId provider,
                               XrToolchainOwnership ownership, const char *program) {
    char path[1200];
    if (!program || !program[0] || !xtc_find_executable(program, path, sizeof(path)))
        return false;
    for (size_t i = 0; i < candidates->count; i++) {
        if (strcmp(candidates->items[i].executable, path) == 0)
            return true;
    }
    if (candidates->count >= XTC_MAX_CANDIDATES)
        return false;
    XrToolchainCandidate *candidate = &candidates->items[candidates->count++];
    memset(candidate, 0, sizeof(*candidate));
    candidate->provider = provider;
    candidate->ownership = ownership;
    snprintf(candidate->executable, sizeof(candidate->executable), "%s", path);
    return true;
}

#ifdef XR_OS_MACOS
static bool xtc_xcrun_find_clang(char *out, size_t out_size) {
    char xcrun[1200];
    char line[1200];
    if (!xtc_find_executable("xcrun", xcrun, sizeof(xcrun)))
        return false;
    /* `xcrun --find` requires the tool as a second argument, so use an explicit spec. */
    XrProcessSpec spec;
    XrProcessResult result;
    char err[256];
    xtc_process_spec_init(&spec, xcrun, 5000);
    spec.argv[1] = "--find";
    spec.argv[2] = "clang";
    spec.argv[3] = NULL;
    spec.output_limit = 4096;
    if (!xtc_process_run(&spec, &result, err, sizeof(err)))
        return false;
    const char *source = result.stdout_data;
    bool ok = result.exit_code == 0 && source && source[0];
    if (ok) {
        size_t len = strcspn(source, "\r\n");
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, source, len);
        line[len] = '\0';
        ok = xtc_find_executable(line, out, out_size);
    }
    xtc_process_result_free(&result);
    return ok;
}
#endif

#ifdef XR_OS_WINDOWS
static bool xtc_windows_find_vswhere(char *out, size_t out_size) {
    if (xtc_find_executable("vswhere", out, out_size))
        return true;
    const char *program_files = getenv("ProgramFiles(x86)");
    if (!program_files || !program_files[0])
        return false;
    char candidate[1200];
    int written = snprintf(candidate, sizeof(candidate),
                           "%s/Microsoft Visual Studio/Installer/vswhere.exe", program_files);
    return written >= 0 && (size_t) written < sizeof(candidate) &&
           xtc_is_executable(candidate) && xtc_copy_canonical_path(candidate, out, out_size);
}

static bool xtc_windows_safe_batch_path(const char *path) {
    return path && path[0] && !strpbrk(path, "\"&|<>^\r\n");
}

static bool xtc_windows_apply_msvc_environment(char *environment) {
    bool path_set = false;
    bool include_set = false;
    bool lib_set = false;
    for (char *line = environment; line && *line;) {
        char *next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
            while (*next == '\r' || *next == '\n')
                next++;
        }
        char *equals = strchr(line, '=');
        if (equals) {
            *equals = '\0';
            const char *value = equals + 1;
            bool needed = _stricmp(line, "PATH") == 0 || _stricmp(line, "INCLUDE") == 0 ||
                          _stricmp(line, "LIB") == 0 || _stricmp(line, "LIBPATH") == 0;
            if (needed && _putenv_s(line, value) != 0)
                return false;
            path_set = path_set || _stricmp(line, "PATH") == 0;
            include_set = include_set || _stricmp(line, "INCLUDE") == 0;
            lib_set = lib_set || _stricmp(line, "LIB") == 0;
        }
        line = next;
    }
    return path_set && include_set && lib_set;
}

static bool xtc_windows_activate_latest_msvc(void) {
    char vswhere[1200];
    char cmd[1200];
    char installation[1200];
    char script[1400];
    char err[256];
    XrProcessSpec spec;
    XrProcessResult result;
    if (!xtc_windows_find_vswhere(vswhere, sizeof(vswhere)) ||
        !xtc_find_executable("cmd.exe", cmd, sizeof(cmd)))
        return false;

    xtc_process_spec_init(&spec, vswhere, 10000);
    spec.argv[1] = "-products";
    spec.argv[2] = "*";
    spec.argv[3] = "-requires";
    spec.argv[4] = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
    spec.argv[5] = "-property";
    spec.argv[6] = "installationPath";
    spec.argv[7] = "-format";
    spec.argv[8] = "value";
    spec.argv[9] = "-latest";
    spec.argv[10] = NULL;
    spec.output_limit = 16384;
    if (!xtc_process_run(&spec, &result, err, sizeof(err)))
        return false;
    const char *source = result.stdout_data;
    size_t len = source ? strcspn(source, "\r\n") : 0;
    bool ok = !result.timed_out && result.exit_code == 0 && len > 0 && len < sizeof(installation);
    if (ok) {
        memcpy(installation, source, len);
        installation[len] = '\0';
    }
    xtc_process_result_free(&result);
    if (!ok ||
        snprintf(script, sizeof(script), "%s/Common7/Tools/VsDevCmd.bat", installation) < 0 ||
        !xtc_is_executable(script) || !xtc_windows_safe_batch_path(script))
        return false;
    xtc_process_spec_init(&spec, cmd, 15000);
    spec.argv[1] = "/d";
    spec.argv[2] = "/s";
    spec.argv[3] = "/c";
    spec.argv[4] = "call";
    spec.argv[5] = script;
    spec.argv[6] = "-no_logo";
    spec.argv[7] = "-arch=x64";
    spec.argv[8] = "-host_arch=x64";
    spec.argv[9] = "&&";
    spec.argv[10] = "set";
    spec.argv[11] = NULL;
    spec.output_limit = 256 * 1024;
    if (!xtc_process_run(&spec, &result, err, sizeof(err)))
        return false;
    ok = !result.timed_out && result.exit_code == 0 && result.stdout_data &&
         xtc_windows_apply_msvc_environment(result.stdout_data);
    xtc_process_result_free(&result);
    return ok;
}

static void xtc_windows_add_msvc(const char *requested, XrToolchainCandidates *out) {
    if (requested && requested[0]) {
        (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_MSVC,
                                  XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, requested);
        return;
    }

    /*
     * A program may put cl.exe on PATH only to make the ASan runtime DLL
     * discoverable.  That is not a usable MSVC environment: compilation also
     * needs INCLUDE and LIB.  Complete an incomplete implicit environment via
     * VsDevCmd before recording the native candidate.  If activation is not
     * available, still record cl so the probe can report the precise failure
     * and continue to a managed Zig candidate.
     */
    const char *include = getenv("INCLUDE");
    const char *lib = getenv("LIB");
    if (!include || !include[0] || !lib || !lib[0])
        (void) xtc_windows_activate_latest_msvc();
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_MSVC,
                              XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, "cl");
}
#endif

static void xtc_add_zig(const XrToolchainRequest *request, XrToolchainCandidates *out) {
    char bundled[1200];
    const char *env_zig = getenv("XRAY_ZIG");
    if (request->zig && request->zig[0]) {
        (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_ZIG, XR_TOOLCHAIN_OWNERSHIP_EXTERNAL,
                                  request->zig);
        return;
    }
    if (env_zig && env_zig[0])
        (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_ZIG, XR_TOOLCHAIN_OWNERSHIP_EXTERNAL,
                                  env_zig);
    if (xtc_find_bundled_zig(request->program_hint, bundled, sizeof(bundled)))
        (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_ZIG, XR_TOOLCHAIN_OWNERSHIP_MANAGED,
                                  bundled);
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_ZIG, XR_TOOLCHAIN_OWNERSHIP_EXTERNAL,
                              "zig");
}

XR_FUNC bool xtc_discover_candidates(const XrToolchainRequest *request, XrToolchainCandidates *out,
                                     char *err, size_t err_size) {
    if (!request || !out) {
        xtc_discovery_error(err, err_size, "missing toolchain request", NULL);
        return false;
    }
    memset(out, 0, sizeof(*out));
    bool cross = !request->target.is_native;
    if (cross && request->selector != XR_TOOLCHAIN_SELECTOR_AUTO &&
        request->selector != XR_TOOLCHAIN_SELECTOR_ZIG) {
        xtc_discovery_error(err, err_size,
                            "target '%s' only supports the Zig provider in this release",
                            request->target.name);
        return false;
    }
    if (cross || request->selector == XR_TOOLCHAIN_SELECTOR_ZIG) {
        xtc_add_zig(request, out);
        return true;
    }

    if (request->selector == XR_TOOLCHAIN_SELECTOR_MSVC) {
#ifdef XR_OS_WINDOWS
        xtc_windows_add_msvc(request->cc, out);
#else
        xtc_discovery_error(err, err_size, "MSVC provider is only available on Windows", NULL);
        return false;
#endif
        return true;
    }

    if (request->selector == XR_TOOLCHAIN_SELECTOR_GCC) {
        (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_GCC, XR_TOOLCHAIN_OWNERSHIP_EXTERNAL,
                                  request->cc && request->cc[0] ? request->cc : "gcc");
        return true;
    }
    if (request->selector == XR_TOOLCHAIN_SELECTOR_CLANG) {
        (void) xtc_candidates_add(out,
#ifdef XR_OS_MACOS
                                  XR_TOOLCHAIN_PROVIDER_APPLE_CLANG,
#else
                                  XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
#endif
                                  XR_TOOLCHAIN_OWNERSHIP_EXTERNAL,
                                  request->cc && request->cc[0] ? request->cc : "clang");
        return true;
    }

    if (request->cc && request->cc[0]) {
        (void) xtc_candidates_add(out,
#ifdef XR_OS_MACOS
                                  XR_TOOLCHAIN_PROVIDER_APPLE_CLANG,
#else
                                  XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
#endif
                                  XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, request->cc);
    }

#ifdef XR_OS_MACOS
    char apple_clang[1200];
    if (xtc_xcrun_find_clang(apple_clang, sizeof(apple_clang)))
        (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_APPLE_CLANG,
                                  XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, apple_clang);
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
                              XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, "clang");
#elif defined(XR_OS_LINUX)
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
                              XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, "clang");
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_GCC, XR_TOOLCHAIN_OWNERSHIP_EXTERNAL,
                              "gcc");
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
                              XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, "cc");
#elif defined(XR_OS_WINDOWS)
    (void) xtc_candidates_add(out, XR_TOOLCHAIN_PROVIDER_LLVM_CLANG,
                              XR_TOOLCHAIN_OWNERSHIP_EXTERNAL, "clang");
    xtc_windows_add_msvc(NULL, out);
#endif

    if (request->selector == XR_TOOLCHAIN_SELECTOR_AUTO)
        xtc_add_zig(request, out);
    return true;
}

XR_FUNC bool xtc_msvc_version_from_banner(const char *source, char *version,
                                          size_t version_size) {
    if (!source || !version || version_size == 0)
        return false;
    for (const char *start = source; *start; start++) {
        if (!isdigit((unsigned char) *start))
            continue;
        const char *cursor = start;
        size_t components = 0;
        for (;;) {
            if (!isdigit((unsigned char) *cursor))
                break;
            while (isdigit((unsigned char) *cursor))
                cursor++;
            components++;
            if (*cursor != '.' || components >= 4)
                break;
            cursor++;
        }
        if (components < 3)
            continue;
        size_t len = (size_t) (cursor - start);
        if (len >= version_size)
            len = version_size - 1;
        memcpy(version, start, len);
        version[len] = '\0';
        return true;
    }
    return false;
}

XR_FUNC bool xtc_candidate_read_version(XrToolchainCandidate *candidate, char *err,
                                        size_t err_size) {
    if (!candidate || !candidate->executable[0]) {
        xtc_discovery_error(err, err_size, "invalid toolchain candidate", NULL);
        return false;
    }
    XrProcessSpec spec;
    XrProcessResult result;
    xtc_process_spec_init(&spec, candidate->executable, 5000);
    spec.argv[1] =
        candidate->provider == XR_TOOLCHAIN_PROVIDER_MSVC
            ? "/?"
            : (candidate->provider == XR_TOOLCHAIN_PROVIDER_ZIG ? "version" : "--version");
    spec.argv[2] = NULL;
    spec.output_limit = 16384;
    if (!xtc_process_run(&spec, &result, err, err_size))
        return false;
    const char *source =
        result.stdout_data && result.stdout_data[0] ? result.stdout_data : result.stderr_data;
    bool ok = !result.timed_out &&
              (result.exit_code == 0 || candidate->provider == XR_TOOLCHAIN_PROVIDER_MSVC);
    if (ok) {
        if (candidate->provider == XR_TOOLCHAIN_PROVIDER_MSVC) {
            /* cl.exe writes its version banner to stderr and its /? help body to stdout. */
            ok = xtc_msvc_version_from_banner(result.stderr_data, candidate->version,
                                              sizeof(candidate->version)) ||
                 xtc_msvc_version_from_banner(result.stdout_data, candidate->version,
                                              sizeof(candidate->version));
        } else if (source && source[0]) {
            size_t len = strcspn(source, "\r\n");
            if (len >= sizeof(candidate->version))
                len = sizeof(candidate->version) - 1;
            memcpy(candidate->version, source, len);
            candidate->version[len] = '\0';
        } else {
            ok = false;
        }
    }
    if (ok) {
        candidate->runnable = true;
        if (strstr(source, "Apple clang"))
            candidate->provider = XR_TOOLCHAIN_PROVIDER_APPLE_CLANG;
        else if (strstr(source, "clang"))
            candidate->provider = XR_TOOLCHAIN_PROVIDER_LLVM_CLANG;
        else if (strstr(source, "gcc") || strstr(source, "GCC"))
            candidate->provider = XR_TOOLCHAIN_PROVIDER_GCC;
    }
    xtc_process_result_free(&result);
    if (!ok)
        xtc_discovery_error(err, err_size, "toolchain version command failed", NULL);
    return ok;
}

XR_FUNC bool xtc_selector_accepts_provider(XrToolchainSelector selector,
                                           XrToolchainProviderId provider) {
    switch (selector) {
        case XR_TOOLCHAIN_SELECTOR_AUTO:
            return true;
        case XR_TOOLCHAIN_SELECTOR_HOST:
            return provider != XR_TOOLCHAIN_PROVIDER_ZIG && provider != XR_TOOLCHAIN_PROVIDER_NONE;
        case XR_TOOLCHAIN_SELECTOR_CLANG:
            return provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG ||
                   provider == XR_TOOLCHAIN_PROVIDER_LLVM_CLANG;
        case XR_TOOLCHAIN_SELECTOR_GCC:
            return provider == XR_TOOLCHAIN_PROVIDER_GCC;
        case XR_TOOLCHAIN_SELECTOR_MSVC:
            return provider == XR_TOOLCHAIN_PROVIDER_MSVC;
        case XR_TOOLCHAIN_SELECTOR_ZIG:
            return provider == XR_TOOLCHAIN_PROVIDER_ZIG;
    }
    return false;
}

XR_FUNC bool xtc_select_discovered(const XrToolchainRequest *request, XrToolchainSelection *out,
                                   char *err, size_t err_size) {
    XrToolchainCandidates candidates;
    if (!request || !out || !xtc_discover_candidates(request, &candidates, err, err_size))
        return false;
    memset(out, 0, sizeof(*out));
    out->target = request->target;
    out->reason = XR_TOOLCHAIN_REASON_TOOLCHAIN_NOT_FOUND;
    for (size_t i = 0; i < candidates.count; i++) {
        char version_err[256];
        if (!xtc_candidate_read_version(&candidates.items[i], version_err, sizeof(version_err)))
            continue;
        if (!xtc_selector_accepts_provider(request->selector, candidates.items[i].provider))
            continue;
        out->provider = candidates.items[i].provider;
        out->ownership = candidates.items[i].ownership;
        out->readiness = XR_TOOLCHAIN_RUNNABLE;
        out->reason = XR_TOOLCHAIN_REASON_NONE;
        snprintf(out->program_storage, sizeof(out->program_storage), "%s",
                 candidates.items[i].executable);
        out->program = out->program_storage;
        snprintf(out->version, sizeof(out->version), "%s", candidates.items[i].version);
        out->fallback_used = request->selector == XR_TOOLCHAIN_SELECTOR_AUTO && i > 0;
        return true;
    }
    if (request->selector != XR_TOOLCHAIN_SELECTOR_AUTO)
        out->reason = XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK;
    xtc_discovery_error(err, err_size, "no runnable provider found for target '%s'",
                        request->target.name);
    return false;
}
