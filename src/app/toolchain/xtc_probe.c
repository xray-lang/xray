/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_probe.c - Capability-based provider selection and compile/link/run probe
 */

#include "xtc_probe.h"

#include "xtc_command.h"
#include "xtc_process.h"
#include "xtc_probe_cache.h"
#include "../../os/os_fs.h"
#include "../../os/os_random.h"
#include "../../os/os_temp.h"
#include "../../os/os_time.h"
#include "../../shared/xr_crypto_core.h"

#include "xray_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef XRAY_BUILD_COMMIT
#define XRAY_BUILD_COMMIT "unknown"
#endif

static const char xtc_probe_minimal_c[] = "int main(void) { return 0; }\n";
static const char xtc_probe_sdk_c[] =
    "#include \"xray.h\"\n"
    "#include \"xrt_core_freestanding.h\"\n"
    "typedef struct XtcDialectPair { int first; int second; } XtcDialectPair;\n"
    "static XtcDialectPair xtc_pair(int value) {\n"
    "  return (XtcDialectPair){ value, value + 1 };\n"
    "}\n"
    "int xray_cgen_dialect_probe(void) {\n"
    "  XtcDialectPair pair = xtc_pair(0);\n"
    "  int expression_value = ({ int local = pair.second; local; });\n"
    "  goto done;\n"
    "done:\n"
    "  return pair.first + expression_value - 1;\n"
    "}\n"
    "int xray_sdk_probe(void) { return XRAY_VERSION_MAJOR >= 0 && sizeof(XrValue) > 0 "
    "? xray_cgen_dialect_probe() : 1; }\n";
static const char xtc_probe_freestanding_sdk_c[] =
    "#include \"xrt_core_freestanding.h\"\n"
    "typedef struct XtcDialectPair { int first; int second; } XtcDialectPair;\n"
    "static XtcDialectPair xtc_pair(int value) {\n"
    "  return (XtcDialectPair){ value, value + 1 };\n"
    "}\n"
    "int xray_cgen_dialect_probe(void) {\n"
    "  XtcDialectPair pair = xtc_pair(0);\n"
    "  int expression_value = ({ int local = pair.second; local; });\n"
    "  goto done;\n"
    "done:\n"
    "  return pair.first + expression_value - 1;\n"
    "}\n"
    "int xray_sdk_probe(void) { return sizeof(XrValue) > 0 ? xray_cgen_dialect_probe() : 1; }\n";
static const char xtc_probe_runtime_c[] =
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "extern void xr_sha256(const uint8_t *, size_t, uint8_t[32]);\n"
    "extern void xr_config_init(void *);\n"
    "int main(void) { uint8_t out[32]; xr_config_init((void *)0); "
    "xr_sha256((const uint8_t *)\"a\", 1, out); return out[0] == 0xca ? 0 : 1; }\n";
static const char xtc_probe_force_inline_c[] =
    "#if defined(_MSC_VER)\n"
    "#define XTC_FORCE_INLINE __forceinline\n"
    "#else\n"
    "#define XTC_FORCE_INLINE __attribute__((always_inline)) inline\n"
    "#endif\n"
    "static XTC_FORCE_INLINE int plus_one(int value) { return value + 1; }\n"
    "int main(void) { return plus_one(0) != 1; }\n";
static const char xtc_probe_preserve_call_c[] =
    "#if defined(_MSC_VER)\n"
    "#define XTC_PRESERVE_CALL __declspec(noinline)\n"
    "#else\n"
    "#define XTC_PRESERVE_CALL __attribute__((noinline))\n"
    "#endif\n"
    "static XTC_PRESERVE_CALL int plus_one(int value) { return value + 1; }\n"
    "int main(void) { return plus_one(0) != 1; }\n";
static const char xtc_probe_value_opaque_c[] = "#include <stdint.h>\n"
                                               "#if defined(_MSC_VER)\n"
                                               "#include <intrin.h>\n"
                                               "#pragma intrinsic(_ReadWriteBarrier)\n"
                                               "#endif\n"
                                               "static int64_t opaque(int64_t value) {\n"
                                               "#if defined(_MSC_VER)\n"
                                               "  volatile int64_t hidden = value;\n"
                                               "  _ReadWriteBarrier();\n"
                                               "  return hidden;\n"
                                               "#elif defined(__GNUC__) || defined(__clang__)\n"
                                               "  __asm__ __volatile__(\"\" : \"+r\"(value));\n"
                                               "#else\n"
                                               "#error provider_has_no_register_opacity_construct\n"
                                               "#endif\n"
                                               "  return value;\n"
                                               "}\n"
                                               "int main(void) { return opaque(7) != 7; }\n";
static const char xtc_probe_compiler_fence_c[] =
    "#if defined(_MSC_VER)\n"
    "#include <intrin.h>\n"
    "#pragma intrinsic(_ReadWriteBarrier)\n"
    "static void fence(void) { _ReadWriteBarrier(); }\n"
    "#elif defined(__GNUC__) || defined(__clang__)\n"
    "static void fence(void) { __asm__ __volatile__(\"\" ::: \"memory\"); }\n"
    "#else\n"
    "#error provider_has_no_compiler_fence_construct\n"
    "#endif\n"
    "int main(void) { fence(); return 0; }\n";

#define XTC_PROBE_COMPILE_TIMEOUT_MS 30000u
#define XTC_PROBE_CROSS_ZIG_TIMEOUT_MS 60000u
#define XTC_PROBE_DUMPMACHINE_TIMEOUT_MS 5000u
#define XTC_PROBE_RUN_TIMEOUT_MS 10000u

/* Probe budgets are wall-clock, so they are really "is this machine idle?"
 * tests. On a saturated build host — CI, or a laptop running several builds —
 * spawning and running the freshly linked native probe can exceed the bound for
 * reasons that have nothing to do with the toolchain: scheduler queueing, and
 * on macOS the first-run Gatekeeper scan of an unsigned binary. The whole AOT
 * surface then reports UNAVAILABLE and every AOT test fails for an unrelated
 * reason.
 *
 * XRAY_TOOLCHAIN_PROBE_SCALE multiplies every probe budget so a loaded host can
 * be given proportionally more room without changing what is being probed.
 * Values below 1 are ignored; the product is capped to keep a genuinely broken
 * toolchain from hanging the build. */
static uint32_t xtc_probe_timeout(uint32_t base_ms) {
    const char *scale_env = getenv("XRAY_TOOLCHAIN_PROBE_SCALE");
    if (!scale_env || !*scale_env)
        return base_ms;

    char *end = NULL;
    unsigned long scale = strtoul(scale_env, &end, 10);
    if (end == scale_env || scale < 1)
        return base_ms;

    const unsigned long kMaxTimeoutMs = 600000ul; /* 10 minutes, hard ceiling */
    unsigned long scaled = (unsigned long) base_ms * scale;
    if (scaled > kMaxTimeoutMs)
        scaled = kMaxTimeoutMs;
    return (uint32_t) scaled;
}

static void xtc_probe_error(char *err, size_t err_size, const char *format, const char *arg) {
    if (!err || err_size == 0)
        return;
    if (arg)
        snprintf(err, err_size, format, arg);
    else
        snprintf(err, err_size, "%s", format);
}

static void xtc_probe_add_diagnostic(XrToolchainProbeResult *result, XrToolchainReasonCode code,
                                     const char *stage, const char *message) {
    if (!result || result->diagnostic_count >= XTC_MAX_DIAGNOSTICS)
        return;
    XrToolchainDiagnostic *diagnostic = &result->diagnostics[result->diagnostic_count++];
    diagnostic->code = code;
    snprintf(diagnostic->stage, sizeof(diagnostic->stage), "%s", stage ? stage : "unknown");
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
             message && message[0] ? message : xtc_reason_code_name(code));
}

XR_FUNC bool xtc_profile_parse(const char *text, XrToolchainProfile *out, char *err,
                               size_t err_size) {
    const char *profile = text && text[0] ? text : "hosted";
    if (!out) {
        xtc_probe_error(err, err_size, "missing toolchain profile output", NULL);
        return false;
    }
    if (strcmp(profile, "hosted") == 0)
        *out = XR_TOOLCHAIN_PROFILE_HOSTED;
    else if (strcmp(profile, "freestanding") == 0)
        *out = XR_TOOLCHAIN_PROFILE_FREESTANDING;
    else {
        xtc_probe_error(err, err_size, "unsupported toolchain profile '%s'", profile);
        return false;
    }
    return true;
}

XR_FUNC const char *xtc_profile_name(XrToolchainProfile profile) {
    return profile == XR_TOOLCHAIN_PROFILE_FREESTANDING ? "freestanding" : "hosted";
}

XR_FUNC const char *xtc_capability_state_name(XrToolchainCapabilityState state) {
    switch (state) {
        case XR_TOOLCHAIN_CAPABILITY_OK:
            return "ok";
        case XR_TOOLCHAIN_CAPABILITY_FAILED:
            return "failed";
        case XR_TOOLCHAIN_CAPABILITY_SKIPPED:
            return "skipped";
        case XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED:
            return "unsupported";
    }
    return "unsupported";
}

static bool xtc_probe_write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t len = strlen(content);
    bool ok = fwrite(content, 1, len, file) == len;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static bool xtc_probe_join(char *out, size_t out_size, const char *dir, const char *name) {
    int written = snprintf(out, out_size, "%s/%s", dir, name);
    return written >= 0 && (size_t) written < out_size;
}

static void xtc_probe_cleanup(const char *dir, const char *const *files, size_t file_count,
                              bool keep) {
    if (!dir || keep)
        return;
    for (size_t i = 0; i < file_count; i++) {
        if (files[i] && files[i][0])
            (void) xr_fs_remove(files[i]);
    }
#ifdef XR_OS_WINDOWS
    (void) RemoveDirectoryA(dir);
#else
    (void) rmdir(dir);
#endif
}

typedef struct XtcProbeCommand {
    XrProcessSpec spec;
    char owned[32][1400];
    size_t index;
    size_t owned_count;
} XtcProbeCommand;

static bool xtc_probe_command_add(void *context, const char *arg, char *err, size_t err_size) {
    XtcProbeCommand *command = (XtcProbeCommand *) context;
    if (!arg || !arg[0])
        return true;
    if (!command || command->index >= XTC_PROCESS_MAX_ARGS - 1) {
        snprintf(err, err_size, "toolchain probe command has too many arguments");
        return false;
    }
    command->spec.argv[command->index++] = arg;
    command->spec.argv[command->index] = NULL;
    return true;
}

static bool xtc_probe_command_add_joined(void *context, const char *prefix, const char *value,
                                         char *err, size_t err_size) {
    XtcProbeCommand *command = (XtcProbeCommand *) context;
    if (!value || !value[0])
        return true;
    if (!command || command->owned_count >= 32) {
        snprintf(err, err_size, "toolchain probe command has too many generated arguments");
        return false;
    }
    int written =
        snprintf(command->owned[command->owned_count], sizeof(command->owned[command->owned_count]),
                 "%s%s", prefix ? prefix : "", value);
    if (written < 0 || (size_t) written >= sizeof(command->owned[command->owned_count])) {
        snprintf(err, err_size, "toolchain probe argument is too long");
        return false;
    }
    return xtc_probe_command_add(command, command->owned[command->owned_count++], err, err_size);
}

static XrToolchainArgSink xtc_probe_command_sink(XtcProbeCommand *command) {
    XrToolchainArgSink sink = {command, xtc_probe_command_add, xtc_probe_command_add_joined};
    return sink;
}

static bool xtc_probe_command_init(const XrToolchainSelection *selection, XtcProbeCommand *command,
                                   char *detail, size_t detail_size) {
    if (!selection || !command || !selection->program)
        return false;
    memset(command, 0, sizeof(*command));
    // The first Zig invocation for a cold cross target may populate compiler
    // and sysroot caches before it can link the probe. Keep the usual native
    // bound tight, but give that bounded cold-start path one extra interval.
    uint32_t timeout_ms = xtc_probe_timeout(selection->provider == XR_TOOLCHAIN_PROVIDER_ZIG &&
                                                    !selection->target.is_native
                                                ? XTC_PROBE_CROSS_ZIG_TIMEOUT_MS
                                                : XTC_PROBE_COMPILE_TIMEOUT_MS);
    xtc_process_spec_init(&command->spec, selection->program, timeout_ms);
    command->index = 0;
    XrToolchainArgSink sink = xtc_probe_command_sink(command);
    return xtc_command_emit_driver(selection, &selection->target, &sink, detail, detail_size);
}

static bool xtc_probe_line_has_token(const uint8_t *line, size_t line_len, const char *token) {
    size_t token_len = strlen(token);
    if (!line || token_len == 0 || line_len < token_len)
        return false;
    for (size_t i = 0; i + token_len <= line_len; i++)
        if (memcmp(line + i, token, token_len) == 0)
            return true;
    return false;
}

static void xtc_probe_select_diagnostic_line(const XrProcessByteBuffer *output,
                                             const uint8_t **selected, size_t *selected_len) {
    *selected = output ? output->data : NULL;
    *selected_len = 0;
    if (!output || !output->data)
        return;
    size_t offset = 0;
    while (*selected_len < output->length && output->data[*selected_len] != '\r' &&
           output->data[*selected_len] != '\n')
        (*selected_len)++;
    for (unsigned inspected = 0; offset < output->length && inspected < 12; inspected++) {
        const uint8_t *line = output->data + offset;
        size_t len = 0;
        while (offset + len < output->length && line[len] != '\r' && line[len] != '\n')
            len++;
        if (xtc_probe_line_has_token(line, len, "error") ||
            xtc_probe_line_has_token(line, len, "fatal")) {
            const uint8_t *stable = line;
            for (size_t i = 0; i < len; i++)
                if (line[i] == '/' || line[i] == '\\')
                    stable = line + i + 1;
            *selected = stable;
            *selected_len = len - (size_t) (stable - line);
            return;
        }
        offset += len;
        while (offset < output->length &&
               (output->data[offset] == '\r' || output->data[offset] == '\n'))
            offset++;
    }
}

static bool xtc_probe_run_process(XrProcessSpec *spec, XrProcessResult *process, char *detail,
                                  size_t detail_size) {
    char process_err[256];
    if (!xtc_process_run(spec, process, process_err, sizeof(process_err))) {
        snprintf(detail, detail_size, "%s", process_err);
        return false;
    }
    if (process->timed_out) {
        snprintf(detail, detail_size, "probe stage timed out after %u ms", spec->timeout_ms);
        return false;
    }
    if (process->exit_code != 0) {
        const XrProcessByteBuffer *output = process->stderr_bytes.length > 0
                                                ? &process->stderr_bytes
                                                : &process->stdout_bytes;
        const uint8_t *diagnostic = NULL;
        size_t len = 0;
        xtc_probe_select_diagnostic_line(output, &diagnostic, &len);
        if (len > detail_size - 1)
            len = detail_size - 1;
        if (len > 0) {
            xtc_process_redact_bytes(diagnostic, len, detail, detail_size);
        } else {
            snprintf(detail, detail_size, "process exited with status %d", process->exit_code);
        }
        return false;
    }
    return true;
}

static bool xtc_probe_compile(const XrToolchainSelection *selection, const char *source,
                              const char *object, const XrRuntimeArtifactSet *sdk, bool include_sdk,
                              bool lto, char *detail, size_t detail_size) {
    XrProcessResult process;
    XtcProbeCommand command;
    if (!xtc_probe_command_init(selection, &command, detail, detail_size))
        return false;
    XrToolchainArgSink sink = xtc_probe_command_sink(&command);
    XrNativeCompileSpec compile = {0};
    compile.optimization = XR_OPTIMIZATION_NONE;
    compile.fp_contract = XR_FP_CONTRACT_OFF;
    compile.lto = lto;
    compile.language_standard = "c11";
    if (!xtc_command_emit_compile(selection, &compile, &sink, detail, detail_size))
        return false;
    if (include_sdk) {
        if (!xtc_command_emit_include(selection->provider, sdk->public_include, &sink, detail,
                                      detail_size) ||
            !xtc_command_emit_include(selection->provider, sdk->private_aot_include, &sink, detail,
                                      detail_size))
            return false;
    }
    if (!xtc_command_emit_compile_io(selection->provider, source, object, &sink, detail,
                                     detail_size))
        return false;
    bool ok = xtc_probe_run_process(&command.spec, &process, detail, detail_size);
    xtc_process_result_free(&process);
    return ok;
}

static bool xtc_probe_add_build_sanitizer_link_flags(const XrToolchainSelection *selection,
                                                     XtcProbeCommand *command, char *detail,
                                                     size_t detail_size) {
#if (defined(XR_BUILD_ASAN) && XR_BUILD_ASAN) || (defined(XR_BUILD_UBSAN) && XR_BUILD_UBSAN) ||    \
    (defined(XR_BUILD_TSAN) && XR_BUILD_TSAN) || (defined(XR_BUILD_MSAN) && XR_BUILD_MSAN)
    if (!selection || !xtc_provider_uses_gnu_driver(selection->provider)) {
        snprintf(detail, detail_size,
                 "this instrumented Xray build requires a GNU-driver provider");
        return false;
    }
#endif
#if defined(XR_BUILD_ASAN) && XR_BUILD_ASAN
    if (!xtc_probe_command_add(command, "-fsanitize=address", detail, detail_size))
        return false;
#if defined(XR_OS_WINDOWS) && defined(XRT_BUILD_ASAN_WINDOWS_IMPORT) &&                            \
    defined(XRT_BUILD_ASAN_WINDOWS_THUNK)
    if (selection && selection->provider == XR_TOOLCHAIN_PROVIDER_ZIG &&
        (!xtc_probe_command_add(command, "-Wl,--whole-archive", detail, detail_size) ||
         !xtc_probe_command_add(command, XRT_BUILD_ASAN_WINDOWS_THUNK, detail, detail_size) ||
         !xtc_probe_command_add(command, "-Wl,--no-whole-archive", detail, detail_size) ||
         !xtc_probe_command_add(command, XRT_BUILD_ASAN_WINDOWS_IMPORT, detail, detail_size)))
        return false;
#endif
#endif
#if defined(XR_BUILD_UBSAN) && XR_BUILD_UBSAN
    if (!xtc_probe_command_add(command, "-fsanitize=undefined", detail, detail_size))
        return false;
#endif
#if defined(XR_BUILD_TSAN) && XR_BUILD_TSAN
    if (!xtc_probe_command_add(command, "-fsanitize=thread", detail, detail_size))
        return false;
#endif
#if defined(XR_BUILD_MSAN) && XR_BUILD_MSAN
    if (!xtc_probe_command_add(command, "-fsanitize=memory", detail, detail_size))
        return false;
#endif
    (void) selection;
    (void) command;
    (void) detail;
    (void) detail_size;
    return true;
}

static bool xtc_probe_link_runtime(const XrToolchainSelection *selection,
                                   const XrRuntimeArtifactSet *runtime, const char *object,
                                   const char *executable, char *detail, size_t detail_size) {
    XrProcessResult process;
    XtcProbeCommand command;
    if (!xtc_probe_command_init(selection, &command, detail, detail_size))
        return false;
    XrToolchainArgSink sink = xtc_probe_command_sink(&command);
    XrNativeLinkSpec link = {0};
    if (!xtc_probe_command_add(&command, object, detail, detail_size) ||
        !xtc_command_emit_link_output(selection->provider, executable, &sink, detail, detail_size))
        return false;
    for (size_t i = 0; i < runtime->artifact_count; i++)
        if (!xtc_probe_command_add(&command, runtime->artifacts[i].path, detail, detail_size))
            return false;
    for (size_t i = 0; i < runtime->system_library_count; i++) {
        if (!xtc_command_emit_system_library(selection->provider, &selection->target,
                                             runtime->system_libraries[i], &sink, detail,
                                             detail_size))
            return false;
    }
    if (!xtc_probe_add_build_sanitizer_link_flags(selection, &command, detail, detail_size))
        return false;
    if (!xtc_command_emit_link(selection, &selection->target, &link, &sink, detail, detail_size))
        return false;
    bool ok = xtc_probe_run_process(&command.spec, &process, detail, detail_size);
    xtc_process_result_free(&process);
    return ok;
}

static bool xtc_probe_link_minimal(const XrToolchainSelection *selection, const char *source,
                                   const char *executable, char *detail, size_t detail_size) {
    XrProcessResult process;
    XtcProbeCommand command;
    if (!xtc_probe_command_init(selection, &command, detail, detail_size))
        return false;
    XrToolchainArgSink sink = xtc_probe_command_sink(&command);
    XrNativeCompileSpec compile = {0};
    XrNativeLinkSpec link = {0};
    compile.optimization = XR_OPTIMIZATION_NONE;
    compile.fp_contract = XR_FP_CONTRACT_OFF;
    compile.language_standard = "c11";
    if (!xtc_command_emit_compile(selection, &compile, &sink, detail, detail_size) ||
        !xtc_probe_command_add(&command, source, detail, detail_size) ||
        !xtc_command_emit_link_output(selection->provider, executable, &sink, detail,
                                      detail_size) ||
        !xtc_command_emit_link(selection, &selection->target, &link, &sink, detail, detail_size))
        return false;
    bool ok = xtc_probe_run_process(&command.spec, &process, detail, detail_size);
    xtc_process_result_free(&process);
    return ok;
}

static bool xtc_probe_link_object(const XrToolchainSelection *selection, const char *object,
                                  const char *executable, bool lto, char *detail,
                                  size_t detail_size) {
    XrProcessResult process;
    XtcProbeCommand command;
    if (!xtc_probe_command_init(selection, &command, detail, detail_size))
        return false;
    XrToolchainArgSink sink = xtc_probe_command_sink(&command);
    XrNativeLinkSpec link = {0};
    link.lto = lto;
    if (!xtc_probe_command_add(&command, object, detail, detail_size) ||
        !xtc_command_emit_link_output(selection->provider, executable, &sink, detail,
                                      detail_size) ||
        !xtc_command_emit_link(selection, &selection->target, &link, &sink, detail, detail_size))
        return false;
    bool ok = xtc_probe_run_process(&command.spec, &process, detail, detail_size);
    xtc_process_result_free(&process);
    return ok;
}

static bool xtc_probe_run_executable(const char *executable, char *detail, size_t detail_size) {
    XrProcessSpec spec;
    XrProcessResult process;
    xtc_process_spec_init(&spec, executable, xtc_probe_timeout(XTC_PROBE_RUN_TIMEOUT_MS));
    spec.argv[1] = NULL;
    bool ok = xtc_probe_run_process(&spec, &process, detail, detail_size);
    xtc_process_result_free(&process);
    return ok;
}

static bool xtc_probe_target_matches(const XrToolchainSelection *selection, char *detail,
                                     size_t detail_size) {
    if (selection && selection->provider == XR_TOOLCHAIN_PROVIDER_MSVC)
        return selection->target.abi == XR_TOOLCHAIN_TARGET_ABI_MSVC;
    XrProcessSpec spec;
    XrProcessResult process;
    size_t index;
    XtcProbeCommand command;
    if (!xtc_probe_command_init(selection, &command, detail, detail_size))
        return false;
    spec = command.spec;
    index = command.index;
    spec.timeout_ms = xtc_probe_timeout(XTC_PROBE_DUMPMACHINE_TIMEOUT_MS);
    spec.argv[index++] = "-dumpmachine";
    spec.argv[index] = NULL;
    if (!xtc_probe_run_process(&spec, &process, detail, detail_size)) {
        xtc_process_result_free(&process);
        return false;
    }
    char triple[256];
    const char *target = selection->target.name;
    if (!xtc_process_copy_ascii_line(&process.stdout_bytes, triple, sizeof(triple))) {
        snprintf(detail, detail_size, "compiler target triple is not strict ASCII");
        xtc_process_result_free(&process);
        return false;
    }
    bool arch_ok =
        (strstr(target, "aarch64") && (strstr(triple, "aarch64") || strstr(triple, "arm64"))) ||
        (strstr(target, "x86_64") && strstr(triple, "x86_64")) ||
        (strstr(target, "i386") &&
         (strstr(triple, "i386") || strstr(triple, "i686") || strstr(triple, "x86-"))) ||
        (strncmp(target, "arm-", 4) == 0 &&
         (strncmp(triple, "arm-", 4) == 0 || strncmp(triple, "armv", 4) == 0)) ||
        (strstr(target, "powerpc64le") && strstr(triple, "powerpc64le")) ||
        (strstr(target, "powerpc64") && strstr(triple, "powerpc64") &&
         !strstr(target, "powerpc64le") && !strstr(triple, "powerpc64le")) ||
        (strstr(target, "loongarch64") && strstr(triple, "loongarch64")) ||
        (strstr(target, "riscv32") && strstr(triple, "riscv32")) ||
        (strstr(target, "riscv64") && strstr(triple, "riscv64")) ||
        (strstr(target, "thumb") && strstr(triple, "thumb"));
    bool os_ok =
        (strstr(target, "apple-darwin") && (strstr(triple, "apple") || strstr(triple, "darwin"))) ||
        (strstr(target, "linux") && strstr(triple, "linux")) ||
        (strstr(target, "windows") && (strstr(triple, "windows") || strstr(triple, "mingw"))) ||
        (strstr(target, "freestanding") && (strstr(triple, "none") || strstr(triple, "unknown")));
    if (!arch_ok || !os_ok)
        snprintf(detail, detail_size, "compiler target '%.*s' does not match '%s'", 160,
                 triple ? triple : "", target);
    xtc_process_result_free(&process);
    return arch_ok && os_ok;
}

static void xtc_probe_sha256_string(const char *text, char out[72]) {
    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    xr_sha256((const uint8_t *) text, strlen(text), digest);
    memcpy(out, "sha256:", 7);
    for (size_t i = 0; i < 32; i++) {
        out[7 + i * 2] = hex[digest[i] >> 4];
        out[7 + i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[71] = '\0';
}

static void xtc_probe_fingerprint(const XrToolchainProbeOptions *options,
                                  XrToolchainProbeResult *result) {
    XrFsStat compiler_stat = {0};
    (void) xr_fs_stat(result->selection.program, &compiler_stat);
    char identity[4096];
    int written = snprintf(
        identity, sizeof(identity),
        "xtc-probe-v5|xray=%s|build=%s|provider=%s|compiler=%s|size=%llu|mtime=%lld|version=%s|"
        "target=%s|sdk=%s|runtime=%s|systemSdk=%s|profile=%s|noRun=%d|requiredCodegen=0x%x|"
        "options=semantic-v1|PATH=%s|SDKROOT=%s",
        XRAY_VERSION_STRING, XRAY_BUILD_COMMIT, xtc_provider_name(result->selection.provider),
        result->selection.program ? result->selection.program : "",
        (unsigned long long) compiler_stat.size, (long long) compiler_stat.mtime_ns,
        result->selection.version, result->selection.target.name, result->runtime.sdk_digest,
        result->selection.runtime_artifact, result->selection.system_sdk,
        xtc_profile_name(options->profile), options->no_run ? 1 : 0,
        options->required_codegen_capabilities, getenv("PATH") ? getenv("PATH") : "",
        getenv("SDKROOT") ? getenv("SDKROOT") : "");
    if (written < 0 || (size_t) written >= sizeof(identity))
        identity[0] = '\0';
    xtc_probe_sha256_string(identity, result->selection.probe_fingerprint);
    char compiler_identity[2048];
    snprintf(compiler_identity, sizeof(compiler_identity), "%s|%llu|%lld|%s",
             result->selection.program ? result->selection.program : "",
             (unsigned long long) compiler_stat.size, (long long) compiler_stat.mtime_ns,
             result->selection.version);
    xtc_probe_sha256_string(compiler_identity, result->selection.compiler_fingerprint);
}

static void xtc_probe_make_id(char out[40]) {
    unsigned char random[16];
    static const char hex[] = "0123456789abcdef";
    xr_random_bytes(random, sizeof(random));
    memcpy(out, "probe-", 6);
    for (size_t i = 0; i < 16; i++) {
        out[6 + i * 2] = hex[random[i] >> 4];
        out[6 + i * 2 + 1] = hex[random[i] & 0x0f];
    }
    out[38] = '\0';
}

static void xtc_probe_copy_runtime_selection(XrToolchainProbeResult *result) {
    snprintf(result->selection.sdk_digest, sizeof(result->selection.sdk_digest), "%s",
             result->runtime.sdk_digest);
    snprintf(result->selection.public_include, sizeof(result->selection.public_include), "%s",
             result->runtime.public_include);
    snprintf(result->selection.private_aot_include, sizeof(result->selection.private_aot_include),
             "%s", result->runtime.private_aot_include);
    result->selection.runtime_count = result->runtime.artifact_count;
    for (size_t i = 0; i < result->runtime.artifact_count; i++) {
        snprintf(result->selection.runtime_ids[i], sizeof(result->selection.runtime_ids[i]), "%s",
                 result->runtime.artifacts[i].id);
        snprintf(result->selection.runtime_paths[i], sizeof(result->selection.runtime_paths[i]),
                 "%s", result->runtime.artifacts[i].path);
    }
    result->selection.system_library_count = result->runtime.system_library_count;
    for (size_t i = 0; i < result->runtime.system_library_count; i++)
        snprintf(result->selection.system_libraries[i],
                 sizeof(result->selection.system_libraries[i]), "%s",
                 result->runtime.system_libraries[i]);
}

static void xtc_probe_add_environment_diagnostics(XrToolchainProbeResult *result) {
    const char *legacy_include = getenv("XRAY_INCLUDE");
    const char *legacy_lib = getenv("XRAY_LIB");
    if (legacy_include && legacy_include[0] &&
        strcmp(legacy_include, result->selection.public_include) != 0) {
        xtc_probe_add_diagnostic(
            result, XR_TOOLCHAIN_REASON_SDK_VERSION_MISMATCH, "environment",
            "XRAY_INCLUDE points outside the verified SDK and is ignored by native AOT");
    }
    if (legacy_lib && legacy_lib[0]) {
        bool owns_runtime = false;
        size_t prefix_len = strlen(legacy_lib);
        for (size_t i = 0; i < result->selection.runtime_count; i++) {
            const char *path = result->selection.runtime_paths[i];
            if (strncmp(path, legacy_lib, prefix_len) == 0 &&
                (path[prefix_len] == '/' || path[prefix_len] == '\\' || path[prefix_len] == '\0')) {
                owns_runtime = true;
                break;
            }
        }
        if (!owns_runtime)
            xtc_probe_add_diagnostic(result, XR_TOOLCHAIN_REASON_SDK_VERSION_MISMATCH,
                                     "environment",
                                     "XRAY_LIB points outside the verified runtime manifest and is "
                                     "ignored by native AOT");
    }
}

static bool xtc_probe_msvc_environment_ready(char *detail, size_t detail_size) {
#ifdef XR_OS_WINDOWS
    const char *include = getenv("INCLUDE");
    const char *lib = getenv("LIB");
    if (include && include[0] && lib && lib[0])
        return true;
    const char *missing = (!include || !include[0]) && (!lib || !lib[0])
                              ? "INCLUDE and LIB"
                              : ((!include || !include[0]) ? "INCLUDE" : "LIB");
    snprintf(detail, detail_size,
             "MSVC environment is incomplete (missing %s); omit an explicit --cc to allow "
             "Visual Studio auto-discovery, or run from a Developer Command Prompt",
             missing);
    return false;
#else
    (void) detail;
    (void) detail_size;
    return true;
#endif
}

static bool xtc_probe_candidate(const XrToolchainProbeOptions *options,
                                XrToolchainCandidate *candidate, size_t candidate_index,
                                XrToolchainProbeResult *result, char *err, size_t err_size) {
    char detail[512] = {0};
    char temp_dir[1200] = {0};
    char minimal_source[1300] = {0};
    char minimal_object[1300] = {0};
    char sdk_source[1300] = {0};
    char sdk_object[1300] = {0};
    char lto_object[1300] = {0};
    char runtime_source[1300] = {0};
    char runtime_object[1300] = {0};
    char runtime_executable[1300] = {0};
    char codegen_source[1300] = {0};
    char codegen_object[1300] = {0};
    const char *cleanup_files[] = {
        minimal_source, minimal_object, sdk_source,         sdk_object,     lto_object,
        runtime_source, runtime_object, runtime_executable, codegen_source, codegen_object};

    memset(result, 0, sizeof(*result));
    snprintf(result->cache, sizeof(result->cache), "%s", "miss");
    result->selection.target = options->request.target;
    result->selection.provider = candidate->provider;
    result->selection.ownership = candidate->ownership;
    result->selection.readiness = XR_TOOLCHAIN_DISCOVERED;
    result->selection.fallback_used =
        options->request.selector == XR_TOOLCHAIN_SELECTOR_AUTO && candidate_index > 0;
    snprintf(result->selection.program_storage, sizeof(result->selection.program_storage), "%s",
             candidate->executable);
    result->selection.program = result->selection.program_storage;

    if (!xtc_candidate_read_version(candidate, detail, sizeof(detail))) {
        result->selection.reason = XR_TOOLCHAIN_REASON_TOOLCHAIN_ENV_INCOMPLETE;
        xtc_probe_add_diagnostic(result, result->selection.reason, "version", detail);
        return false;
    }
    result->selection.provider = candidate->provider;
    snprintf(result->selection.version, sizeof(result->selection.version), "%s",
             candidate->version);
    if (candidate->provider == XR_TOOLCHAIN_PROVIDER_APPLE_CLANG &&
        !xtc_active_apple_sdk(result->selection.system_sdk, sizeof(result->selection.system_sdk),
                              detail, sizeof(detail))) {
        result->selection.reason = XR_TOOLCHAIN_REASON_TOOLCHAIN_ENV_INCOMPLETE;
        xtc_probe_add_diagnostic(result, result->selection.reason, "discover", detail);
        return false;
    }
    if (!xtc_selector_accepts_provider(options->request.selector, candidate->provider)) {
        result->selection.reason = XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK;
        xtc_probe_add_diagnostic(result, result->selection.reason, "version",
                                 "compiler identity does not match the explicit provider");
        return false;
    }
    if (candidate->provider == XR_TOOLCHAIN_PROVIDER_MSVC &&
        !xtc_probe_msvc_environment_ready(detail, sizeof(detail))) {
        result->selection.reason = XR_TOOLCHAIN_REASON_TOOLCHAIN_ENV_INCOMPLETE;
        xtc_probe_add_diagnostic(result, result->selection.reason, "environment", detail);
        return false;
    }
    if (candidate->provider == XR_TOOLCHAIN_PROVIDER_ZIG &&
        strncmp(candidate->version, "0.16.", 5) != 0) {
        result->selection.reason = XR_TOOLCHAIN_REASON_TOOLCHAIN_VERSION_UNSUPPORTED;
        xtc_probe_add_diagnostic(result, result->selection.reason, "version",
                                 "Zig 0.16.x is required by toolchain protocol 1");
        return false;
    }
    result->selection.readiness = XR_TOOLCHAIN_RUNNABLE;
    if (!xtc_probe_target_matches(&result->selection, detail, sizeof(detail))) {
        result->selection.reason = XR_TOOLCHAIN_REASON_ABI_MISMATCH;
        xtc_probe_add_diagnostic(result, result->selection.reason, "target", detail);
        return false;
    }

    /* Native probing preserves the target ABI selected before provider
     * discovery, including windows-msvc when Zig is the fallback. A true
     * cross-target probe may borrow a provider-native host manifest only for
     * SDK header identity; its minimal link never consumes those runtime
     * archives, and runtime-dependent cross builds fail closed. */
    XrToolchainTarget sdk_target = result->selection.target;
    bool exact_runtime_target = options->request.target.is_native;
#ifdef XR_OS_WINDOWS
    exact_runtime_target =
        exact_runtime_target || (result->selection.provider == XR_TOOLCHAIN_PROVIDER_ZIG &&
                                 result->selection.target.os == XR_TOOLCHAIN_TARGET_OS_WINDOWS &&
                                 result->selection.target.abi == XR_TOOLCHAIN_TARGET_ABI_GNU);
#endif
    const char *host_sdk_target = "native";
#ifdef XR_OS_WINDOWS
    if (result->selection.provider == XR_TOOLCHAIN_PROVIDER_ZIG) {
#if defined(_M_ARM64) || defined(__aarch64__)
        host_sdk_target = "aarch64-windows-gnu";
#else
        host_sdk_target = "x86_64-windows-gnu";
#endif
    }
#endif
    if (!exact_runtime_target &&
        !xtc_target_parse(host_sdk_target, &sdk_target, detail, sizeof(detail))) {
        result->sdk_compile = XR_TOOLCHAIN_CAPABILITY_FAILED;
        result->selection.reason = XR_TOOLCHAIN_REASON_SDK_MISSING;
        xtc_probe_add_diagnostic(result, result->selection.reason, "sdk-compile", detail);
        return false;
    }
    XrToolchainReasonCode runtime_reason;
    if (!xtc_runtime_manifest_load(&sdk_target, result->selection.provider,
                                   options->request.program_hint, &result->runtime, &runtime_reason,
                                   detail, sizeof(detail))) {
        result->sdk_compile = XR_TOOLCHAIN_CAPABILITY_FAILED;
        result->selection.reason = runtime_reason == XR_TOOLCHAIN_REASON_NONE
                                       ? XR_TOOLCHAIN_REASON_SDK_MISSING
                                       : runtime_reason;
        xtc_probe_add_diagnostic(result, result->selection.reason, "sdk-compile", detail);
        return false;
    }
    xtc_probe_copy_runtime_selection(result);
    if (options->profile == XR_TOOLCHAIN_PROFILE_HOSTED && options->request.target.is_native) {
        const XrRuntimeArtifact *coro =
            xtc_runtime_artifact_find(&result->runtime, "xray-rt-coro-");
        if (coro)
            snprintf(result->selection.runtime_artifact, sizeof(result->selection.runtime_artifact),
                     "%s", coro->id);
    }
    xtc_probe_fingerprint(options, result);
    if (!options->refresh) {
        bool cache_hit = false;
        char cache_err[256];
        if (xtc_probe_cache_load(options, result, &cache_hit, cache_err, sizeof(cache_err)) &&
            cache_hit) {
            xtc_probe_make_id(result->probe_id);
            xtc_probe_add_environment_diagnostics(result);
            return true;
        }
    }

    if (xr_temp_dir_create("xray-toolchain", temp_dir, sizeof(temp_dir)) != 0 ||
        !xtc_probe_join(minimal_source, sizeof(minimal_source), temp_dir, "minimal.c") ||
        !xtc_probe_join(minimal_object, sizeof(minimal_object), temp_dir, "minimal.o") ||
        !xtc_probe_join(sdk_source, sizeof(sdk_source), temp_dir, "sdk.c") ||
        !xtc_probe_join(sdk_object, sizeof(sdk_object), temp_dir, "sdk.o") ||
        !xtc_probe_join(lto_object, sizeof(lto_object), temp_dir, "lto.o") ||
        !xtc_probe_join(runtime_source, sizeof(runtime_source), temp_dir, "runtime.c") ||
        !xtc_probe_join(runtime_object, sizeof(runtime_object), temp_dir, "runtime.o") ||
        !xtc_probe_join(codegen_source, sizeof(codegen_source), temp_dir, "codegen.c") ||
        !xtc_probe_join(codegen_object, sizeof(codegen_object), temp_dir, "codegen.o") ||
        !xtc_probe_join(runtime_executable, sizeof(runtime_executable), temp_dir,
                        options->request.target.os == XR_TOOLCHAIN_TARGET_OS_WINDOWS ? "runtime.exe"
                                                                                     : "runtime")) {
        xtc_probe_error(err, err_size, "failed to create secure probe directory", NULL);
        return false;
    }
    xtc_probe_make_id(result->probe_id);
    const char *sdk_probe_source = options->profile == XR_TOOLCHAIN_PROFILE_FREESTANDING
                                       ? xtc_probe_freestanding_sdk_c
                                       : xtc_probe_sdk_c;
    if (!xtc_probe_write_file(minimal_source, xtc_probe_minimal_c) ||
        !xtc_probe_write_file(sdk_source, sdk_probe_source) ||
        !xtc_probe_write_file(runtime_source, xtc_probe_runtime_c)) {
        xtc_probe_cleanup(temp_dir, cleanup_files, sizeof(cleanup_files) / sizeof(cleanup_files[0]),
                          options->keep_probe);
        xtc_probe_error(err, err_size, "failed to write probe fixtures", NULL);
        return false;
    }

    if (!xtc_probe_compile(&result->selection, minimal_source, minimal_object, NULL, false, false,
                           detail, sizeof(detail))) {
        result->c_compile = XR_TOOLCHAIN_CAPABILITY_FAILED;
        result->selection.reason = XR_TOOLCHAIN_REASON_COMPILE_PROBE_FAILED;
        xtc_probe_add_diagnostic(result, result->selection.reason, "c-compile", detail);
        goto failed;
    }
    result->c_compile = XR_TOOLCHAIN_CAPABILITY_OK;
    result->selection.readiness = XR_TOOLCHAIN_C_COMPILE_OK;

    if (!xtc_probe_compile(&result->selection, sdk_source, sdk_object, &result->runtime, true,
                           false, detail, sizeof(detail))) {
        result->sdk_compile = XR_TOOLCHAIN_CAPABILITY_FAILED;
        result->selection.reason = XR_TOOLCHAIN_REASON_COMPILE_PROBE_FAILED;
        xtc_probe_add_diagnostic(result, result->selection.reason, "sdk-compile", detail);
        goto failed;
    }
    result->sdk_compile = XR_TOOLCHAIN_CAPABILITY_OK;
    result->selection.readiness = XR_TOOLCHAIN_SDK_COMPILE_OK;

#define XTC_PROBE_CODEGEN_CAP(field, source_text)                                                  \
    do {                                                                                           \
        if (!xtc_probe_write_file(codegen_source, (source_text))) {                                \
            result->field = XR_TOOLCHAIN_CAPABILITY_FAILED;                                        \
        } else if (xtc_probe_compile(&result->selection, codegen_source, codegen_object, NULL,     \
                                     false, false, detail, sizeof(detail))) {                      \
            result->field = XR_TOOLCHAIN_CAPABILITY_OK;                                            \
        } else {                                                                                   \
            result->field = XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED;                                   \
        }                                                                                          \
        (void) xr_fs_remove(codegen_object);                                                       \
    } while (0)
    XTC_PROBE_CODEGEN_CAP(force_inline, xtc_probe_force_inline_c);
    XTC_PROBE_CODEGEN_CAP(preserve_call, xtc_probe_preserve_call_c);
    XTC_PROBE_CODEGEN_CAP(value_opaque, xtc_probe_value_opaque_c);
    XTC_PROBE_CODEGEN_CAP(compiler_fence, xtc_probe_compiler_fence_c);
#undef XTC_PROBE_CODEGEN_CAP
    uint32_t missing_codegen = 0;
    if (result->force_inline != XR_TOOLCHAIN_CAPABILITY_OK)
        missing_codegen |= XR_TOOLCHAIN_CODEGEN_FORCE_INLINE;
    if (result->preserve_call != XR_TOOLCHAIN_CAPABILITY_OK)
        missing_codegen |= XR_TOOLCHAIN_CODEGEN_PRESERVE_CALL;
    if (result->value_opaque != XR_TOOLCHAIN_CAPABILITY_OK)
        missing_codegen |= XR_TOOLCHAIN_CODEGEN_VALUE_OPAQUE;
    if (result->compiler_fence != XR_TOOLCHAIN_CAPABILITY_OK)
        missing_codegen |= XR_TOOLCHAIN_CODEGEN_COMPILER_FENCE;
    missing_codegen &= options->required_codegen_capabilities;
    if (missing_codegen != 0) {
        result->selection.reason = XR_TOOLCHAIN_REASON_CODEGEN_CAPABILITY_UNSUPPORTED;
        snprintf(detail, sizeof(detail),
                 "provider lacks required native code-shape capabilities 0x%x "
                 "(forceInline=%s preserveCall=%s valueOpaque=%s compilerFence=%s)",
                 missing_codegen, xtc_capability_state_name(result->force_inline),
                 xtc_capability_state_name(result->preserve_call),
                 xtc_capability_state_name(result->value_opaque),
                 xtc_capability_state_name(result->compiler_fence));
        xtc_probe_add_diagnostic(result, result->selection.reason, "codegen-capability", detail);
        goto failed;
    }

    if (options->profile == XR_TOOLCHAIN_PROFILE_FREESTANDING) {
        result->runtime_link = XR_TOOLCHAIN_CAPABILITY_SKIPPED;
        result->native_run = XR_TOOLCHAIN_CAPABILITY_SKIPPED;
        result->cross = options->request.target.is_native ? XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED
                                                          : XR_TOOLCHAIN_CAPABILITY_OK;
        result->selection.readiness = XR_TOOLCHAIN_READY;
    } else if (!options->request.target.is_native) {
        if (!xtc_probe_link_minimal(&result->selection, minimal_source, runtime_executable, detail,
                                    sizeof(detail))) {
            result->runtime_link = XR_TOOLCHAIN_CAPABILITY_FAILED;
            result->cross = XR_TOOLCHAIN_CAPABILITY_FAILED;
            result->selection.reason = XR_TOOLCHAIN_REASON_LINK_PROBE_FAILED;
            xtc_probe_add_diagnostic(result, result->selection.reason, "runtime-link", detail);
            goto failed;
        }
        result->runtime_link = XR_TOOLCHAIN_CAPABILITY_OK;
        result->native_run = XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED;
        result->cross = XR_TOOLCHAIN_CAPABILITY_OK;
        result->selection.readiness = XR_TOOLCHAIN_READY;
    } else {
        if (!xtc_probe_compile(&result->selection, runtime_source, runtime_object, NULL, false,
                               false, detail, sizeof(detail)) ||
            !xtc_probe_link_runtime(&result->selection, &result->runtime, runtime_object,
                                    runtime_executable, detail, sizeof(detail))) {
            result->runtime_link = XR_TOOLCHAIN_CAPABILITY_FAILED;
            result->selection.reason = XR_TOOLCHAIN_REASON_LINK_PROBE_FAILED;
            xtc_probe_add_diagnostic(result, result->selection.reason, "runtime-link", detail);
            goto failed;
        }
        result->runtime_link = XR_TOOLCHAIN_CAPABILITY_OK;
        result->selection.readiness = XR_TOOLCHAIN_RUNTIME_LINK_OK;
        if (options->no_run) {
            result->native_run = XR_TOOLCHAIN_CAPABILITY_SKIPPED;
        } else if (!xtc_probe_run_executable(runtime_executable, detail, sizeof(detail))) {
            result->native_run = XR_TOOLCHAIN_CAPABILITY_FAILED;
            result->selection.reason = XR_TOOLCHAIN_REASON_RUN_PROBE_FAILED;
            xtc_probe_add_diagnostic(result, result->selection.reason, "native-run", detail);
            goto failed;
        } else {
            result->native_run = XR_TOOLCHAIN_CAPABILITY_OK;
            result->selection.readiness = XR_TOOLCHAIN_NATIVE_RUN_OK;
        }
        result->cross = XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED;
        result->selection.readiness = XR_TOOLCHAIN_READY;
    }

    if (xtc_probe_compile(&result->selection, minimal_source, lto_object, NULL, false, true, detail,
                          sizeof(detail)) &&
        xtc_probe_link_object(&result->selection, lto_object, runtime_executable, true, detail,
                              sizeof(detail)) &&
        (!options->request.target.is_native || options->no_run ||
         xtc_probe_run_executable(runtime_executable, detail, sizeof(detail))))
        result->lto = XR_TOOLCHAIN_CAPABILITY_OK;
    else
        result->lto = XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED;
    result->selection.reason = XR_TOOLCHAIN_REASON_NONE;
    xtc_probe_fingerprint(options, result);
    char cache_err[256];
    (void) xtc_probe_cache_store(options, result, cache_err, sizeof(cache_err));
    xtc_probe_add_environment_diagnostics(result);
    xtc_probe_cleanup(temp_dir, cleanup_files, sizeof(cleanup_files) / sizeof(cleanup_files[0]),
                      options->keep_probe);
    return true;

failed:
    xtc_probe_fingerprint(options, result);
    xtc_probe_cleanup(temp_dir, cleanup_files, sizeof(cleanup_files) / sizeof(cleanup_files[0]),
                      options->keep_probe);
    return false;
}

XR_FUNC bool xtc_probe(const XrToolchainProbeOptions *options, XrToolchainProbeResult *out,
                       char *err, size_t err_size) {
    uint64_t start_ms = xr_time_monotonic_ms();
    XrToolchainCandidates candidates;
    if (!options || !out || !xtc_discover_candidates(&options->request, &candidates, err, err_size))
        return false;
    if (candidates.count == 0) {
        memset(out, 0, sizeof(*out));
        out->selection.target = options->request.target;
        out->selection.reason = options->request.selector == XR_TOOLCHAIN_SELECTOR_AUTO
                                    ? XR_TOOLCHAIN_REASON_TOOLCHAIN_NOT_FOUND
                                    : XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK;
        xtc_probe_add_diagnostic(out, out->selection.reason, "discover",
                                 "no provider candidate was found");
        out->duration_ms = xr_time_monotonic_ms() - start_ms;
        xtc_probe_error(err, err_size, "no toolchain provider candidate found", NULL);
        return false;
    }

    XrToolchainProbeResult best = {0};
    bool have_best = false;
    for (size_t i = 0; i < candidates.count; i++) {
        XrToolchainProbeResult candidate_result;
        char candidate_err[512] = {0};
        bool ready = xtc_probe_candidate(options, &candidates.items[i], i, &candidate_result,
                                         candidate_err, sizeof(candidate_err));
        candidate_result.duration_ms = xr_time_monotonic_ms() - start_ms;
        if (ready) {
            *out = candidate_result;
            out->selection.program = out->selection.program_storage;
            if (i > 0 && have_best) {
                const char *rejected_detail =
                    best.diagnostic_count > 0 ? best.diagnostics[best.diagnostic_count - 1].message
                                              : xtc_reason_code_name(best.selection.reason);
                char fallback_detail[512];
                snprintf(fallback_detail, sizeof(fallback_detail),
                         "provider '%s' did not satisfy the generated-C contract (%.*s); "
                         "selected '%s' with the same target ABI",
                         xtc_provider_name(best.selection.provider), 260,
                         rejected_detail ? rejected_detail : "unknown reason",
                         xtc_provider_name(out->selection.provider));
                xtc_probe_add_diagnostic(out,
                                         best.selection.reason == XR_TOOLCHAIN_REASON_NONE
                                             ? XR_TOOLCHAIN_REASON_COMPILE_PROBE_FAILED
                                             : best.selection.reason,
                                         "fallback", fallback_detail);
            }
            return true;
        }
        if (!have_best || candidate_result.selection.readiness > best.selection.readiness) {
            best = candidate_result;
            best.selection.program = best.selection.program_storage;
            have_best = true;
        }
        if (options->request.selector != XR_TOOLCHAIN_SELECTOR_AUTO)
            break;
    }
    *out = best;
    out->selection.program = out->selection.program_storage;
    out->duration_ms = xr_time_monotonic_ms() - start_ms;
    if (options->request.selector != XR_TOOLCHAIN_SELECTOR_AUTO &&
        out->selection.reason != XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK) {
        xtc_probe_add_diagnostic(out, XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK,
                                 "selection", "explicit provider failed; fallback is disabled");
    }
    xtc_probe_error(err, err_size, "no provider reached READY for target '%s'",
                    options->request.target.name);
    return false;
}
