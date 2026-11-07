/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_lir_native.c - Ordinary C compilation with owned temporary artifacts
 *
 * KEY CONCEPT:
 *   Generated source is verified before this boundary. Compiler children finish
 *   before workspace cleanup, and completed bytes publish by named replacement.
 */
#include "xcli.h"
#include "xcli_lir.h"
#include "xcli_toolchain_config.h"
#include "app/toolchain/xtc_discovery.h"
#include "app/toolchain/xtc_host_toolchain.h"
#include "app/toolchain/xtc_xir_publication.h"
#include "app/toolchain/xtc_xir_workspace.h"
#include "base/xfileio.h"
#include "base/xchecks.h"
#include "os/os_fs.h"
#include "xray_native_link_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include "base/xwindows_utf8.h"
#endif

XR_FUNC int xr_cli_lir_publish(XrCompileResources *resources, const char *path,
    const void *bytes, size_t length, bool executable) {
    XtcXirPublication *publication = NULL;
    XtcXirPublicationRequest request = {path,
        {XR_PATH_LIMIT_MAX_PATH, 8, length ? (uint64_t)length : 1}, executable};
    XtcXirPublicationStatus status = xtc_xir_publication_new(resources, &request, &publication);
    if (status == XTC_XIR_PUBLICATION_OK)
        status = xtc_xir_publication_write(publication, bytes, length);
    if (status == XTC_XIR_PUBLICATION_OK)
        status = xtc_xir_publication_commit(publication);
    int result = status == XTC_XIR_PUBLICATION_OK ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
    if (result != XR_CLI_EXIT_OK) {
        const XtcXirPublicationDiagnostic *failure = xtc_xir_publication_diagnostic(publication);
        fprintf(stderr, "output publication failed (status=%u stage=%u published=%u os=%u)\n",
            (unsigned)status, failure ? (unsigned)failure->stage : 0,
            failure ? (unsigned)failure->published : 0, failure ? failure->os_error : 0);
    }
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (xtc_xir_publication_close(&publication) == XTC_XIR_PUBLICATION_OK)
            return result;
        result = XR_CLI_EXIT_FAIL;
    }
    fputs("output publication cleanup remains pending\n", stderr);
    fflush(stderr);
    _Exit(XR_CLI_EXIT_INTERNAL);
}

typedef struct NativeInputs {
    char *include;
    char *libraries[XRT_NATIVE_LIBRARY_COUNT];
} NativeInputs;

static void native_inputs_free(NativeInputs *inputs) {
    xr_compile_resources_free(inputs->include);
    for (size_t i = 0; i < XRT_NATIVE_LIBRARY_COUNT; ++i)
        xr_compile_resources_free(inputs->libraries[i]);
    *inputs = (NativeInputs){0};
}

static bool native_copy(XrCompileResources *resources, const char *text, char **output) {
    size_t length = strlen(text);
    if (length == SIZE_MAX || xr_compile_resources_alloc(resources, length + 1,
        (void **)output) != XR_COMPILE_RESOURCE_OK) return false;
    memcpy(*output, text, length + 1);
    return true;
}

static bool native_development_executable(const XrOsIoPolicy *policy, const char *executable) {
#if XR_OS_WINDOWS
    wchar_t *actual = NULL, *built = NULL;
    bool same = xr_win_utf8_path_owned(policy, executable, &actual) == XR_OS_IO_OK &&
        xr_win_utf8_path_owned(policy, XRT_NATIVE_BUILD_EXECUTABLE, &built) == XR_OS_IO_OK;
    if (same) {
        /* Short Windows locators preserve their input separator spelling. */
        for (wchar_t *p = actual; *p; ++p)
            if (*p == L'/') *p = L'\\';
        for (wchar_t *p = built; *p; ++p)
            if (*p == L'/') *p = L'\\';
        same = CompareStringOrdinal(actual, -1, built, -1, TRUE) == CSTR_EQUAL;
    }
#else
    char *actual = NULL, *built = NULL;
    bool same = xr_realpath_owned(policy, executable, &actual) == XR_OS_IO_OK &&
        xr_realpath_owned(policy, XRT_NATIVE_BUILD_EXECUTABLE, &built) == XR_OS_IO_OK &&
        !strcmp(actual, built);
#endif
    xr_compile_resources_free(built);
    xr_compile_resources_free(actual);
    if (!same)
        fprintf(stderr, "native runtime development path does not match executable %s\n", executable);
    return same;
}

static bool native_inputs_open(XrCompileResources *resources, NativeInputs *inputs) {
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    char *executable = NULL, *directory = NULL, *prefix = NULL, *root = NULL;
    bool okay = xr_os_io_self_exe_path(&policy, &executable) == XR_OS_IO_OK &&
        xr_path_dirname_owned(&policy, executable, &directory) == XR_OS_IO_OK &&
        xr_path_dirname_owned(&policy, directory, &prefix) == XR_OS_IO_OK &&
        xr_path_join_owned(&policy, prefix, "lib/xray/native", &root) == XR_OS_IO_OK;
    XrFsStat stat = {0};
    XrOsIoStatus found = okay ? xr_os_io_stat(&policy, root, &stat) : XR_OS_IO_IO;
    if (okay && found == XR_OS_IO_NOT_FOUND) {
        okay = native_development_executable(&policy, executable) &&
            native_copy(resources, XRT_SOURCE_INCLUDE_DIR, &inputs->include);
        for (size_t i = 0; okay && i < XRT_NATIVE_LIBRARY_COUNT; ++i)
            okay = native_copy(resources, xrt_native_development_libraries[i], &inputs->libraries[i]);
    } else if (okay && found == XR_OS_IO_OK && stat.kind == XR_FS_DIR) {
        okay = xr_path_join_owned(&policy, root, "include", &inputs->include) == XR_OS_IO_OK;
        for (size_t i = 0; okay && i < XRT_NATIVE_LIBRARY_COUNT; ++i)
            okay = xr_path_join_owned(&policy, root, xrt_native_library_names[i],
                &inputs->libraries[i]) == XR_OS_IO_OK;
    } else okay = false;
    char *header = NULL;
    bool exists = false;
    if (okay) okay = xr_path_join_owned(&policy, inputs->include,
        "xray_native_runtime.h", &header) == XR_OS_IO_OK &&
        xr_os_io_is_file(&policy, header, &exists) == XR_OS_IO_OK && exists;
    for (size_t i = 0; okay && i < XRT_NATIVE_LIBRARY_COUNT; ++i)
        okay = xr_os_io_is_file(&policy, inputs->libraries[i], &exists) == XR_OS_IO_OK && exists;
    if (!okay)
        fprintf(stderr, "native runtime inputs are unavailable (root=%s status=%u)\n",
            root ? root : "unknown", (unsigned)found);
    xr_compile_resources_free(header);
    xr_compile_resources_free(root);
    xr_compile_resources_free(prefix);
    xr_compile_resources_free(directory);
    xr_compile_resources_free(executable);
    return okay;
}

typedef struct NativeCommand {
    XrCompileResources *resources;
    XrProcessSpec spec;
    size_t count, owned_count;
    char *owned[32];
} NativeCommand;

static bool native_arg(NativeCommand *command, const char *argument) {
    if (!argument || command->count + 1 >= XTC_PROCESS_MAX_ARGS) return false;
    command->spec.argv[command->count++] = argument;
    return true;
}

static bool native_join(NativeCommand *command, const char *prefix, const char *text) {
    if (command->owned_count == sizeof(command->owned) / sizeof(command->owned[0])) return false;
    size_t a = strlen(prefix), b = strlen(text);
    if (b > SIZE_MAX - a - 1) return false;
    char *joined = NULL;
    if (xr_compile_resources_alloc(command->resources, a + b + 1,
        (void **)&joined) != XR_COMPILE_RESOURCE_OK) return false;
    memcpy(joined, prefix, a);
    memcpy(joined + a, text, b + 1);
    command->owned[command->owned_count++] = joined;
    return native_arg(command, joined);
}

static void native_command_init(NativeCommand *command, XrCompileResources *resources,
    const XtcHostToolchainView *host, const char *cwd) {
    *command = (NativeCommand){.resources = resources, .count = 1};
    xtc_process_spec_init(&command->spec, host->compiler, 30000);
    command->spec.environment_source = XTC_PROCESS_ENV_EXPLICIT;
    command->spec.cwd = cwd;
    command->spec.output_limit = 1024 * 1024;
#if XR_OS_WINDOWS
    command->spec.env_count = 4;
    command->spec.env_keys[0] = "SystemRoot";
    command->spec.env_values[0] = host->msvc.system_root;
    command->spec.env_keys[1] = "TEMP";
    command->spec.env_values[1] = host->workspace_parent;
    command->spec.env_keys[2] = "TMP";
    command->spec.env_values[2] = host->workspace_parent;
    command->spec.env_keys[3] = "VSLANG";
    command->spec.env_values[3] = "1033";
#else
    command->spec.env_count = 2;
    command->spec.env_keys[0] = "PATH";
    command->spec.env_values[0] = host->posix.path;
    command->spec.env_keys[1] = "TMPDIR";
    command->spec.env_values[1] = host->workspace_parent;
#endif
}

static bool native_command_run(NativeCommand *command, bool prepared) {
    XrToolchainProcess *process = NULL;
    XrProcessResult output = {0};
    XrProcessStatus status = prepared ? xtc_process_prepare(command->resources,
        &command->spec, &process) : XTC_PROCESS_INVALID;
    if (status == XTC_PROCESS_OK) status = xtc_process_run(process, NULL, NULL, &output);
    bool okay = status == XTC_PROCESS_OK && !output.exit_code &&
        !output.stdout_bytes.truncated && !output.stderr_bytes.truncated;
    if (!okay) {
        fprintf(stderr, "native compiler failed (status=%u child=%d)\n", (unsigned)status, output.exit_code);
        if (output.stdout_bytes.length)
            (void)fwrite(output.stdout_bytes.data, 1, output.stdout_bytes.length, stderr);
        if (output.stderr_bytes.length)
            (void)fwrite(output.stderr_bytes.data, 1, output.stderr_bytes.length, stderr);
    }
    xtc_process_result_free(&output);
    xtc_process_free(process);
    for (size_t i = 0; i < command->owned_count; ++i)
        xr_compile_resources_free(command->owned[i]);
    return okay;
}

static bool native_compile(NativeCommand *command, const XtcHostToolchainView *host,
    const NativeInputs *inputs, const char *source, const char *object) {
#if XR_OS_WINDOWS
    const char *crt[] = {"/MT", "/MTd", "/MD", "/MDd"};
    bool okay = native_arg(command, "/nologo") && native_arg(command, "/std:c11") &&
        native_arg(command, "/utf-8") && native_arg(command, "/W4") && native_arg(command, "/WX") &&
        native_arg(command, "/O2") && native_arg(command, crt[XRT_NATIVE_CRT]) &&
        native_arg(command, host->msvc.clang ? "/clang:-ffp-contract=off" : "/fp:precise");
    /* Integer division is checked before C emission. IEEE floating division
     * by a literal zero remains a valid source operation under MSVC C4723. */
    if (okay && !host->msvc.clang) okay = native_arg(command, "/wd4723");
    if (okay) okay = native_arg(command, "/X") && native_arg(command, "/c") &&
        native_join(command, "/I", inputs->include) &&
        native_join(command, "/I", host->msvc.vc_include) &&
        native_join(command, "/I", host->msvc.ucrt_include) &&
        native_join(command, "/I", host->msvc.shared_include) &&
        native_join(command, "/I", host->msvc.um_include) &&
        native_arg(command, source) && native_join(command, "/Fo", object);
#if XRT_NATIVE_ASAN
    if (okay) okay = native_arg(command, "/fsanitize=address");
#endif
#if XRT_NATIVE_UBSAN
    if (okay) okay = native_arg(command, "/clang:-fsanitize=undefined") &&
        native_arg(command, "/clang:-fno-sanitize=function");
#endif
#else
    bool okay = native_arg(command, "-std=c11") && native_arg(command, "-Wall") &&
        native_arg(command, "-Wextra") && native_arg(command, "-Werror") &&
        native_arg(command, "-pedantic") && native_arg(command, "-O2") &&
        native_arg(command, "-ffp-contract=off") && native_arg(command, "-c") &&
        native_join(command, "-I", inputs->include) && native_arg(command, source) &&
        native_arg(command, "-o") && native_arg(command, object);
    if (okay && host->posix.sysroot)
        okay = native_arg(command, "-isysroot") && native_arg(command, host->posix.sysroot);
    if (okay && host->posix.arch)
        okay = native_arg(command, "-arch") && native_arg(command, host->posix.arch);
#if XRT_NATIVE_ASAN
    if (okay) okay = native_arg(command, "-fsanitize=address");
#endif
#if XRT_NATIVE_UBSAN
    if (okay) okay = native_arg(command, "-fsanitize=undefined");
#endif
#endif
    return native_command_run(command, okay);
}

static bool native_link(NativeCommand *command, const XtcHostToolchainView *host,
    const NativeInputs *inputs, const char *object, const char *output) {
#if XR_OS_WINDOWS
    command->spec.executable = host->linker;
    command->spec.argv[0] = host->linker;
    bool okay = native_arg(command, "/nologo") && native_arg(command, "/INCREMENTAL:NO") &&
        native_arg(command, "/SUBSYSTEM:CONSOLE") && native_join(command, "/OUT:", output) &&
        native_join(command, "/LIBPATH:", host->msvc.vc_library) &&
        native_join(command, "/LIBPATH:", host->msvc.ucrt_library) &&
        native_join(command, "/LIBPATH:", host->msvc.um_library) &&
        native_arg(command, object);
#else
    bool okay = native_arg(command, object) && native_arg(command, "-o") && native_arg(command, output);
#if XR_OS_MACOS
    if (okay) okay = native_arg(command, "-Wl,-dead_strip");
#endif
    if (okay && host->posix.sysroot)
        okay = native_arg(command, "-isysroot") && native_arg(command, host->posix.sysroot);
    if (okay && host->posix.arch)
        okay = native_arg(command, "-arch") && native_arg(command, host->posix.arch);
#endif
    for (size_t i = 0; okay && i < XRT_NATIVE_LIBRARY_COUNT; ++i)
        okay = native_arg(command, inputs->libraries[i]);
    for (uint32_t i = 0; okay && i < host->library_count; ++i)
        okay = native_arg(command, host->libraries[i]);
#if XR_OS_WINDOWS
#if XRT_NATIVE_ASAN
    if (okay) okay = native_arg(command, XRT_BUILD_ASAN_WINDOWS_IMPORT) &&
        native_arg(command, "/INCLUDE:__asan_seh_interceptor") &&
        native_join(command, "/WHOLEARCHIVE:", XRT_BUILD_ASAN_WINDOWS_THUNK);
#elif XRT_NATIVE_UBSAN
    if (okay) okay = native_arg(command, XRT_BUILD_UBSAN_WINDOWS_RUNTIME);
#endif
#else
    if (okay) okay = native_arg(command, "-lm") && native_arg(command, "-pthread");
#if XRT_NATIVE_ASAN
    if (okay) okay = native_arg(command, "-fsanitize=address");
#endif
#if XRT_NATIVE_UBSAN
    if (okay) okay = native_arg(command, "-fsanitize=undefined");
#endif
#endif
    return native_command_run(command, okay);
}

static bool native_workspace_close(XtcXirWorkspace **workspace) {
    bool clean = true;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (xtc_xir_workspace_close(workspace, 1024) == XTC_XIR_WORKSPACE_OK) return clean;
        clean = false;
    }
    fputs("native workspace cleanup remains pending\n", stderr);
    fflush(stderr);
    _Exit(XR_CLI_EXIT_INTERNAL);
}

XR_FUNC int xr_cli_lir_native(XrCompileResources *resources, const XrCliNativeOptions *options,
    const void *source, size_t length, const char *output) {
    if (!resources || !options || !source || !length || !output) return XR_CLI_EXIT_INTERNAL;
    NativeInputs inputs = {0};
    XtcHostToolchain *toolchain = NULL;
    XtcHostDiagnostic diagnostic = {0};
    XtcHostToolchainRequest request = {.compiler = options->compiler, .timeout_ms = 30000,
        .output_limit = 1024 * 1024, .crt = XRT_NATIVE_CRT};
#if XR_OS_WINDOWS && (XRT_NATIVE_ASAN || XRT_NATIVE_UBSAN)
    if (!request.compiler) request.compiler = XRT_NATIVE_BUILD_COMPILER;
#endif
    XrCompileState *preference_state = NULL;
    XrCliToolchainConfig *preferences = NULL;
    XrCliToolchainChoice choice = {0};
    char selected_path[XR_PATH_LIMIT_MAX_PATH];
    XtcXirWorkspace *workspace = NULL;
    char *c_path = NULL, *object_path = NULL, *exe_path = NULL;
    uint8_t *bytes = NULL;
    size_t byte_count = 0;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    int result = XR_CLI_EXIT_FAIL;
    if (xr_compile_state_new(resources, &preference_state) != XR_COMPILE_RESOURCE_OK ||
        xr_cli_toolchain_config_open(preference_state, NULL, &preferences) != XR_CLI_TOOLCHAIN_CONFIG_OK ||
        xr_cli_toolchain_choose(xr_cli_toolchain_config_view(preferences), XTC_BUILD_HOST_TARGET,
            options->provider, request.compiler, &choice) != XR_CLI_TOOLCHAIN_CONFIG_OK) {
        fputs("cannot resolve native compiler preferences\n", stderr);
        goto cleanup;
    }
    if (choice.selector == XR_TOOLCHAIN_SELECTOR_ZIG) {
        fputs("Zig automatic native compilation is temporarily unavailable\n", stderr);
        goto cleanup;
    }
    request.compiler = choice.compiler;
#if XR_OS_WINDOWS
    if (choice.selector == XR_TOOLCHAIN_SELECTOR_GCC) {
        fputs("GCC is unavailable on this host\n", stderr);
        goto cleanup;
    }
    if (!request.compiler && choice.selector == XR_TOOLCHAIN_SELECTOR_CLANG) {
        if (!xtc_find_executable("clang-cl.exe", selected_path, sizeof(selected_path))) {
            fputs("clang-cl was not found\n", stderr);
            goto cleanup;
        }
        request.compiler = selected_path;
    }
#else
    (void)selected_path;
    if (choice.selector == XR_TOOLCHAIN_SELECTOR_MSVC) {
        fputs("MSVC is unavailable on this host\n", stderr);
        goto cleanup;
    }
#endif
    if (!native_inputs_open(resources, &inputs)) goto cleanup;
    if (xtc_host_toolchain_open(resources, &request, &toolchain, &diagnostic) != XTC_HOST_OK) {
        fprintf(stderr, "host compiler discovery failed (status=%u stage=%u code=%d os=%u)\n",
            (unsigned)diagnostic.status, (unsigned)diagnostic.stage, diagnostic.code, diagnostic.os_error);
        goto cleanup;
    }
    const XtcHostToolchainView *host = xtc_host_toolchain_view(toolchain);
    XtcXirWorkspaceRequest temporary = {host->workspace_parent, {XR_PATH_LIMIT_MAX_PATH, 8, 65536}};
    if (xtc_xir_workspace_new(resources, &temporary, &workspace) != XTC_XIR_WORKSPACE_OK ||
        xtc_xir_workspace_create(workspace) != XTC_XIR_WORKSPACE_OK) {
        fputs("cannot create native compiler workspace\n", stderr);
        goto cleanup;
    }
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(workspace);
    if (xr_path_join_owned(&policy, paths->input, "program.c", &c_path) != XR_OS_IO_OK ||
        xr_path_join_owned(&policy, paths->output, "program.obj", &object_path) != XR_OS_IO_OK ||
        xr_path_join_owned(&policy, paths->output, "program.exe", &exe_path) != XR_OS_IO_OK ||
        xr_os_io_write_new_file_sync(&policy, c_path, source, length) != XR_OS_IO_OK) {
        fputs("cannot stage generated C\n", stderr);
        goto cleanup;
    }
    if (options->verbose) fprintf(stderr, "C compiler %s\n", host->compiler);
    NativeCommand command;
    native_command_init(&command, resources, host, paths->input);
    if (!native_compile(&command, host, &inputs, c_path, object_path)) goto cleanup;
    native_command_init(&command, resources, host, paths->input);
    if (!native_link(&command, host, &inputs, object_path, exe_path)) goto cleanup;
    if (xr_os_io_read_regular_file(&policy, exe_path, SIZE_MAX - 1, &bytes, &byte_count) != XR_OS_IO_OK) {
        fputs("cannot read linked native program\n", stderr);
        goto cleanup;
    }
    if (!native_workspace_close(&workspace)) goto cleanup;
    result = xr_cli_lir_publish(resources, output, bytes, byte_count, true);
cleanup:
    if (!native_workspace_close(&workspace)) result = XR_CLI_EXIT_FAIL;
    xr_compile_resources_free(bytes);
    xr_compile_resources_free(c_path);
    xr_compile_resources_free(object_path);
    xr_compile_resources_free(exe_path);
    xtc_host_toolchain_free(toolchain);
    native_inputs_free(&inputs);
    xr_cli_toolchain_config_free(preferences);
    xr_compile_state_release(preference_state);
    return result;
}
