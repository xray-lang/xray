/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build_program.inc.c - Physical compilation of validated Program output
 */

static const char *const program_native_runtime_units[] = {"time", "pipe", "random"};
enum {
    XR_PROGRAM_NATIVE_RUNTIME_COUNT = sizeof(program_native_runtime_units) / sizeof(program_native_runtime_units[0]),
    XR_PROGRAM_NATIVE_OBJECT_COUNT = XR_PROGRAM_NATIVE_RUNTIME_COUNT + 1u,
    XR_PROGRAM_NATIVE_COMMAND_COUNT = XR_PROGRAM_NATIVE_OBJECT_COUNT + 1u
};

typedef struct XrCliProgramNativeBuild {
    const XrCliInvocation *inv;
    const XrToolchainSelection *plan;
    const XrToolchainTarget *target;
    XrNativeCompileSpec compile;
    XrNativeLinkSpec link;
    const char *sysroot;
    bool dry_run;
    bool dump;
    XrFingerprint commands[XR_PROGRAM_NATIVE_COMMAND_COUNT];
    XrFingerprint objects[XR_PROGRAM_NATIVE_OBJECT_COUNT];
} XrCliProgramNativeBuild;

static bool program_native_command(XrCliProgramNativeBuild *build, XaotCliLinkCommand *command,
                                   unsigned index, char *error, size_t error_size) {
    size_t size = 0u;
    for (int arg = 0; arg < command->argc; ++arg)
        size += strlen(command->argv[arg]) + 1u;
    char *bytes = xr_malloc(size ? size : 1u);
    if (!bytes)
        return false;
    size_t offset = 0u;
    for (int arg = 0; arg < command->argc; ++arg) {
        size_t length = strlen(command->argv[arg]) + 1u;
        memcpy(bytes + offset, command->argv[arg], length);
        offset += length;
    }
    xr_semantic_fingerprint((const uint8_t *) bytes, size, &build->commands[index]);
    xr_free(bytes);
    if (build->dump || build->dry_run)
        xaot_cli_print_command(index == 3u ? "Link command" : "Compile command", command);
    if (build->dry_run)
        return true;
    XrProcId process = xr_proc_spawn(command->program, command->argv);
    int code = -1;
    if (process == XR_PROC_INVALID || xr_proc_wait(process, &code) != 0 || code != 0) {
        snprintf(error, error_size, "native toolchain command %u failed (exit %d)", index, code);
        return false;
    }
    return true;
}

static bool program_native_compile(XrCliProgramNativeBuild *build, const char *source,
                                   const char *object, unsigned index, char *error,
                                   size_t error_size) {
    XaotCliLinkCommand command = {0};
    XrToolchainArgSink sink = xaot_cli_command_sink(&command);
    XrNativeCompileSpec compile = build->compile;
    bool embedded_debug = build->plan->provider == XR_TOOLCHAIN_PROVIDER_MSVC &&
                          compile.debug_info == XR_DEBUG_INFO_FULL;
    if (embedded_debug)
        compile.debug_info = XR_DEBUG_INFO_NONE;
    if (embedded_debug && !xaot_cli_link_add_arg(&command, "/Z7", error, error_size))
        return false;
    char source_root[1400];
    int length =
        snprintf(source_root, sizeof(source_root), "%s/..", build->plan->private_aot_include);
    if (length < 0 || (size_t) length >= sizeof(source_root) ||
        !build->plan->private_aot_include[0] || !build->plan->public_include[0]) {
        snprintf(error, error_size, "verified native SDK include roots are missing");
        return false;
    }
    if (!xaot_cli_add_provider_driver_prefix(&command, build->plan, build->target, error,
                                             error_size) ||
        !xtc_command_emit_compile(build->plan, &compile, &sink, error, error_size) ||
        !xtc_command_emit_include(build->plan->provider, source_root, &sink, error, error_size) ||
        !xtc_command_emit_include(build->plan->provider, build->plan->public_include, &sink, error,
                                  error_size) ||
        !xtc_command_emit_sysroot(build->plan->provider, build->sysroot, &sink, error,
                                  error_size) ||
        !xtc_command_emit_compile_io(build->plan->provider, source, object, &sink, error,
                                     error_size))
        return false;
    if (build->target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS &&
        !xtc_command_emit_define(build->plan->provider, "_CRT_SECURE_NO_WARNINGS", &sink, error,
                                 error_size))
        return false;
    if (build->target->os != XR_TOOLCHAIN_TARGET_OS_WINDOWS &&
        !xtc_command_emit_define(build->plan->provider, "_POSIX_C_SOURCE=200809L", &sink, error,
                                 error_size))
        return false;
    /* Keep debug side files inside the private build directory. */
    if (build->plan->provider == XR_TOOLCHAIN_PROVIDER_MSVC &&
        !xaot_cli_link_add_prefixed(&command, "/Fd", object, error, error_size))
        return false;
    if (!program_native_command(build, &command, index, error, error_size))
        return false;
    if (build->dry_run)
        return true;
    uint8_t *bytes = NULL;
    size_t size = 0u;
    if (xr_fs_read_regular_file(object, 256u * 1024u * 1024u, &bytes, &size) != 0)
        return false;
    xr_semantic_fingerprint(bytes, size, &build->objects[index]);
    xr_free(bytes);
    return true;
}

static bool program_native_link(XrCliProgramNativeBuild *build, char objects[XR_PROGRAM_NATIVE_OBJECT_COUNT][1400],
                                const char *output, char *error, size_t error_size) {
    XaotCliLinkCommand command = {0};
    XrToolchainArgSink sink = xaot_cli_command_sink(&command);
    if (!xaot_cli_add_provider_driver_prefix(&command, build->plan, build->target, error,
                                             error_size))
        return false;
    for (unsigned index = 0u; index < XR_PROGRAM_NATIVE_OBJECT_COUNT; ++index)
        if (!xaot_cli_link_add_arg(&command, objects[index], error, error_size))
            return false;
    if (!xtc_command_emit_link_output(build->plan->provider, output, &sink, error, error_size) ||
        !xtc_command_emit_sysroot(build->plan->provider, build->sysroot, &sink, error,
                                  error_size) ||
        !xtc_command_emit_link(build->plan, build->target, &build->link, &sink, error, error_size))
        return false;
    if (build->target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS &&
        !xtc_command_emit_system_library(build->plan->provider, build->target, "bcrypt", &sink,
                                         error, error_size))
        return false;
    /* The compiler driver appends .exe to extensionless /Fe paths. The linker
     * must produce the exact path that artifact verification will consume. */
    if (build->plan->provider == XR_TOOLCHAIN_PROVIDER_MSVC &&
        (!xaot_cli_link_add_prefixed(&command, "/OUT:", output, error, error_size) ||
         !xaot_cli_link_add_arg(&command, "/INCREMENTAL:NO", error, error_size)))
        return false;
    if (build->plan->provider == XR_TOOLCHAIN_PROVIDER_MSVC &&
        build->compile.debug_info == XR_DEBUG_INFO_FULL) {
        char pdb[1500];
        int length = snprintf(pdb, sizeof(pdb), "%s.pdb", output);
        if (length < 0 || (size_t) length >= sizeof(pdb) ||
            !xaot_cli_link_add_arg(&command, "/DEBUG:FULL", error, error_size) ||
            !xaot_cli_link_add_prefixed(&command, "/PDB:", pdb, error, error_size) ||
            !xaot_cli_link_add_arg(&command, "/PDBALTPATH:%_PDB%", error, error_size))
            return false;
    }
    return program_native_command(build, &command, XR_PROGRAM_NATIVE_OBJECT_COUNT, error, error_size);
}

static bool program_native_seal(XrCliProgramNativeBuild *build, const XrGeneratedC *generated,
                                const char *binary, const char *output, char *error,
                                size_t error_size) {
    XrAotToolchainInput input = {0};
    input.schema_version = 1u;
    switch (build->plan->provider) {
        case XR_TOOLCHAIN_PROVIDER_APPLE_CLANG:
        case XR_TOOLCHAIN_PROVIDER_LLVM_CLANG:
            input.provider = XR_AOT_TOOLCHAIN_CLANG;
            break;
        case XR_TOOLCHAIN_PROVIDER_GCC:
            input.provider = XR_AOT_TOOLCHAIN_GCC;
            break;
        case XR_TOOLCHAIN_PROVIDER_MSVC:
            input.provider = XR_AOT_TOOLCHAIN_MSVC;
            break;
        case XR_TOOLCHAIN_PROVIDER_ZIG:
            input.provider = XR_AOT_TOOLCHAIN_ZIG;
            break;
        default:
            return false;
    }
    XrFingerprint commands;
    xr_semantic_fingerprint((const uint8_t *) build->commands, sizeof(build->commands), &commands);
    char options[XR_FINGERPRINT_BYTES * 2u + 1u];
    xr_fingerprint_hex(commands, options);
    input.provider_version = build->plan->compiler_fingerprint;
    input.target_triple = build->target->name;
    input.codegen_options = options;
    input.target_profile_id = generated->target_profile_id;
    xr_semantic_fingerprint((const uint8_t *) build->plan->sdk_digest,
                            strlen(build->plan->sdk_digest), &input.sysroot_id);
    xr_semantic_fingerprint((const uint8_t *) build->objects, sizeof(build->objects),
                            &input.runtime_objects_id);
    XrAotToolchainBinding binding = {0};
    XrNativeArtifact artifact = {0};
    uint8_t *bytes = NULL;
    size_t size = 0u;
    bool ok =
        build->plan->sdk_digest[0] && build->plan->compiler_fingerprint[0] &&
        xr_aot_toolchain_binding_build(&input, &binding) &&
        xr_fs_read_regular_file(binary, 256u * 1024u * 1024u, &bytes, &size) == 0 &&
        xr_native_artifact_seal(generated, &binding, bytes, size, &artifact) == XR_BACKEND_OK &&
        xr_native_artifact_verify(&artifact, generated->execution_id, generated->backend_id,
                                  generated->optimization_policy_id, &binding);
    if (ok) {
        char staging[XR_PATH_MAX];
        int length = snprintf(staging, sizeof(staging), "%s.xray-%s", output, options);
        ok = length >= 0 && (size_t) length < sizeof(staging);
        bool created = ok && xr_fs_write_new_file_sync(staging, artifact.bytes, artifact.size) == 0;
        ok = created;
        if (ok)
            ok = XR_CLI_CHMOD(staging, 0755) == 0 && xr_fs_rename(staging, output) == 0;
        if (created && !ok)
            xr_fs_remove(staging);
    }
    xr_free(bytes);
    xr_native_artifact_free(&artifact);
    if (!ok)
        snprintf(error, error_size, "native artifact verification or atomic publication failed");
    return ok;
}

static int program_native_build(XrCliProgramNativeBuild *build, const XrGeneratedC *generated,
                                const char *output) {
    char directory[1200] = {0}, source[1400] = {0}, binary[1400] = {0};
    char objects[XR_PROGRAM_NATIVE_OBJECT_COUNT][1400] = {{0}};
    char runtime[XR_PROGRAM_NATIVE_RUNTIME_COUNT][1400] = {{0}}, error[512] = {0};
    int result = XR_CLI_EXIT_FAIL;
    if (xr_temp_dir_create("xray-program", directory, sizeof(directory)) != 0)
        return result;
    snprintf(source, sizeof(source), "%s/program.c", directory);
    const char *basename = output;
    for (const char *cursor = output; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\')
            basename = cursor + 1;
    int binary_length = snprintf(binary, sizeof(binary), "%s/%s", directory, basename);
    if (!basename[0] || binary_length < 0 || (size_t) binary_length >= sizeof(binary)) {
        snprintf(error, sizeof(error), "native output name is empty or too long");
        goto cleanup;
    }
    const bool windows = build->target->os == XR_TOOLCHAIN_TARGET_OS_WINDOWS;
    for (unsigned index = 0u; index < XR_PROGRAM_NATIVE_OBJECT_COUNT; ++index)
        snprintf(objects[index], sizeof(objects[index]), "%s/unit%u.obj", directory, index);
    for (unsigned index = 0u; index < XR_PROGRAM_NATIVE_RUNTIME_COUNT; ++index) {
        int length = snprintf(runtime[index], sizeof(runtime[index]), "%s/../os/%s/%s_%s.c",
                              build->plan->private_aot_include, windows ? "win" : "unix",
                              program_native_runtime_units[index], windows ? "win" : "unix");
        if (length < 0 || (size_t) length >= sizeof(runtime[index]))
            goto cleanup;
    }
    if (xr_fs_write_new_file_sync(source, (const uint8_t *) generated->bytes, generated->size) !=
            0 ||
        !program_native_compile(build, source, objects[0], 0u, error, sizeof(error)))
        goto cleanup;
    for (unsigned index = 0u; index < XR_PROGRAM_NATIVE_RUNTIME_COUNT; ++index)
        if (!program_native_compile(build, runtime[index], objects[index + 1u], index + 1u, error,
                                    sizeof(error)))
            goto cleanup;
    if (!program_native_link(build, objects, binary, error, sizeof(error)))
        goto cleanup;
    if (!build->dry_run && build->plan->provider == XR_TOOLCHAIN_PROVIDER_MSVC &&
        build->compile.debug_info == XR_DEBUG_INFO_FULL) {
        char pdb[1500], published_pdb[XR_PATH_MAX];
        int length = snprintf(published_pdb, sizeof(published_pdb), "%s.pdb", output);
        snprintf(pdb, sizeof(pdb), "%s.pdb", binary);
        if (length < 0 || (size_t) length >= sizeof(published_pdb) ||
            xaot_copy_file(pdb, published_pdb, 0644) != 0)
            goto cleanup;
    }
    if (!build->dry_run &&
        !program_native_seal(build, generated, binary, output, error, sizeof(error)))
        goto cleanup;
    if (xr_cli_opt_bool(&build->inv->options, "keep-c")) {
        char kept[XR_PATH_MAX];
        int length = snprintf(kept, sizeof(kept), "%s.c", output);
        if (length < 0 || (size_t) length >= sizeof(kept) ||
            xaot_copy_file(source, kept, 0644) != 0)
            goto cleanup;
    }
    result = XR_CLI_EXIT_OK;
cleanup:
    if (result != XR_CLI_EXIT_OK)
        fprintf(stderr, "XR_BUILD_6006: canonical native build failed: %s\n", error);
    xr_fs_remove(source);
    xr_fs_remove(binary);
    for (unsigned index = 0u; index < XR_PROGRAM_NATIVE_OBJECT_COUNT; ++index)
        xr_fs_remove(objects[index]);
    XrDirIter *iterator = xr_dir_open(directory);
    XrDirEntry entry;
    while (iterator && xr_dir_next(iterator, &entry)) {
        char side_file[1600];
        int length = snprintf(side_file, sizeof(side_file), "%s/%s", directory, entry.name);
        if (!entry.is_dir && length >= 0 && (size_t) length < sizeof(side_file))
            xr_fs_remove(side_file);
    }
    if (iterator)
        xr_dir_close(iterator);
#ifdef XR_OS_WINDOWS
    (void) _rmdir(directory);
#else
    (void) rmdir(directory);
#endif
    return result;
}
