/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_shape_oracle.c - Provider-backed generated-C to assembly realization
 */

#include "xtc_shape_oracle.h"

#include "xtc_command.h"
#include "xtc_process.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../os/os_fs.h"
#include "../../os/os_temp.h"

#include <stdio.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct XtcShapeCommand {
    XrProcessSpec spec;
    char owned[32][1400];
    size_t index;
    size_t owned_count;
} XtcShapeCommand;

static bool shape_add(void *context, const char *arg, char *err, size_t err_size) {
    XtcShapeCommand *command = (XtcShapeCommand *) context;
    if (!arg || !arg[0])
        return true;
    if (!command || command->index >= XTC_PROCESS_MAX_ARGS - 1) {
        snprintf(err, err_size, "shape-oracle command has too many arguments");
        return false;
    }
    command->spec.argv[command->index++] = arg;
    command->spec.argv[command->index] = NULL;
    return true;
}

static bool shape_add_joined(void *context, const char *prefix, const char *value, char *err,
                             size_t err_size) {
    XtcShapeCommand *command = (XtcShapeCommand *) context;
    if (!value || !value[0])
        return true;
    if (!command || command->owned_count >= 32) {
        snprintf(err, err_size, "shape-oracle command has too many generated arguments");
        return false;
    }
    char *slot = command->owned[command->owned_count];
    int written = snprintf(slot, sizeof(command->owned[0]), "%s%s", prefix ? prefix : "", value);
    if (written < 0 || (size_t) written >= sizeof(command->owned[0])) {
        snprintf(err, err_size, "shape-oracle argument is too long");
        return false;
    }
    command->owned_count++;
    return shape_add(command, slot, err, err_size);
}

static bool shape_write(const char *path, const char *source) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t len = strlen(source);
    bool ok = fwrite(source, 1, len, file) == len;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static bool shape_join(char *out, size_t out_size, const char *dir, const char *name) {
    int written = snprintf(out, out_size, "%s/%s", dir, name);
    return written >= 0 && (size_t) written < out_size;
}

static void shape_cleanup(const char *dir, const char *source, const char *assembly,
                          const char *object) {
    if (source && source[0])
        (void) xr_fs_remove(source);
    if (assembly && assembly[0])
        (void) xr_fs_remove(assembly);
    if (object && object[0])
        (void) xr_fs_remove(object);
#ifdef XR_OS_WINDOWS
    if (dir && dir[0])
        (void) RemoveDirectoryA(dir);
#else
    if (dir && dir[0])
        (void) rmdir(dir);
#endif
}

static void shape_process_error(const XrProcessSpec *spec, const XrProcessResult *process,
                                char *err, size_t err_size) {
    if (process->timed_out) {
        snprintf(err, err_size, "shape-oracle compiler timed out after %u ms", spec->timeout_ms);
        return;
    }
    const char *output = process->stderr_data && process->stderr_data[0]
                             ? process->stderr_data
                             : process->stdout_data;
    size_t len = output ? strlen(output) : 0;
    if (len > 0) {
        char redacted[512];
        if (len >= sizeof(redacted))
            len = sizeof(redacted) - 1;
        xtc_process_redact_output(output, len, redacted, sizeof(redacted));
        snprintf(err, err_size, "shape-oracle compiler failed: %s", redacted);
    } else {
        snprintf(err, err_size, "shape-oracle compiler exited with status %d", process->exit_code);
    }
}

XR_FUNC bool xtc_shape_oracle_realize(const XrToolchainProbeOptions *options,
                                      const char *generated_c, XrToolchainAssemblyArtifact *out,
                                      char *err, size_t err_size) {
    XrToolchainProbeResult probe = {0};
    XtcShapeCommand command = {0};
    XrProcessResult process = {0};
    XrNativeCompileSpec compile = {0};
    char temp_dir[1200] = {0};
    char source[1400] = {0};
    char assembly[1400] = {0};
    char object[1400] = {0};
    bool ok = false;
    if (!options || !generated_c || !out) {
        snprintf(err, err_size, "missing shape-oracle input");
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!xtc_probe(options, &probe, err, err_size))
        return false;
    if (xr_temp_dir_create("xray-shape", temp_dir, sizeof(temp_dir)) != 0 ||
        !shape_join(source, sizeof(source), temp_dir, "generated.c") ||
        !shape_join(assembly, sizeof(assembly), temp_dir,
                    probe.selection.provider == XR_TOOLCHAIN_PROVIDER_MSVC ? "generated.asm"
                                                                          : "generated.s") ||
        !shape_join(object, sizeof(object), temp_dir, "generated.obj") ||
        !shape_write(source, generated_c)) {
        snprintf(err, err_size, "cannot create private shape-oracle inputs");
        goto cleanup;
    }

    xtc_process_spec_init(&command.spec, probe.selection.program, 120000u);
    command.index = 0;
    XrToolchainArgSink sink = {&command, shape_add, shape_add_joined};
    if (!xtc_command_emit_driver(&probe.selection, &probe.selection.target, &sink, err, err_size))
        goto cleanup;
    compile.optimization = XR_OPTIMIZATION_RELEASE;
    compile.fp_contract = XR_FP_CONTRACT_OFF;
    compile.warnings = XR_WARNING_POLICY_SUPPRESS;
    compile.language_standard = "c11";
    if (!xtc_command_emit_compile(&probe.selection, &compile, &sink, err, err_size) ||
        !xtc_command_emit_include(probe.selection.provider, probe.selection.public_include, &sink,
                                  err, err_size) ||
        !xtc_command_emit_include(probe.selection.provider, probe.selection.private_aot_include,
                                  &sink, err, err_size) ||
        !xtc_command_emit_assembly_io(probe.selection.provider, source, assembly, object, &sink,
                                      err, err_size))
        goto cleanup;
    char process_err[512];
    if (!xtc_process_run(&command.spec, &process, process_err, sizeof(process_err))) {
        snprintf(err, err_size, "%s", process_err);
        goto cleanup;
    }
    if (process.timed_out || process.exit_code != 0) {
        shape_process_error(&command.spec, &process, err, err_size);
        goto cleanup;
    }
    out->text = xr_file_read_all(assembly, "rb", &out->size);
    if (!out->text) {
        snprintf(err, err_size, "shape-oracle compiler did not produce assembly");
        goto cleanup;
    }
    out->probe = probe;
    out->probe.selection.program = out->probe.selection.program_storage;
    ok = true;

cleanup:
    xtc_process_result_free(&process);
    shape_cleanup(temp_dir, source, assembly, object);
    if (!ok)
        xtc_shape_oracle_free(out);
    return ok;
}

XR_FUNC void xtc_shape_oracle_free(XrToolchainAssemblyArtifact *artifact) {
    if (!artifact)
        return;
    xr_free(artifact->text);
    memset(artifact, 0, sizeof(*artifact));
}
