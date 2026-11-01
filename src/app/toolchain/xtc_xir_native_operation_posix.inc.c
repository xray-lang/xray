/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_operation_posix.inc.c - Frozen POSIX driver commands
 *
 * KEY CONCEPT:
 *   One discovered driver compiles generated.c and launcher.c with -MD
 *   reports, then links them with the SDK archives and asks the linker for
 *   its input report. The invocation owner re-derives and checks every word.
 */
#ifdef XR_OS_MACOS
static const char native_report_option[] = "-Wl,-dependency_info,";
#else
static const char native_report_option[] = "-Wl,--dependency-file=";
#endif

static bool native_request_valid(const XtcXirNativeOperationRequest *request) {
    if (!request->posix.path || !request->posix.root_count || request->library_count) return false;
#ifdef XR_OS_MACOS
    if (!request->posix.arch || !request->posix.sysroot) return false;
#endif
    return true;
}
static bool native_spec(XtcXirNativeOperation *owner, const XtcXirNativeOperationRequest *request,
    const char *executable, XrProcessSpec *spec, size_t *argc) {

    xtc_process_spec_init(spec, executable, owner->limits.timeout_ms);
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner->workspace);
    spec->completion_policy = XR_PROC_COMPLETE_ROOT;
    spec->environment_source = XTC_PROCESS_ENV_EXPLICIT;
    spec->cwd = paths->input; spec->output_limit = owner->limits.process_output_bytes;
    spec->env_count = 2;
    spec->env_keys[0] = "PATH"; spec->env_values[0] = request->posix.path;
    spec->env_keys[1] = "TMPDIR"; spec->env_values[1] = paths->output;
    *argc = 1;
#ifdef XR_OS_MACOS
    spec->argv[(*argc)++] = "-arch"; spec->argv[(*argc)++] = request->posix.arch;
    spec->argv[(*argc)++] = "-isysroot"; spec->argv[(*argc)++] = request->posix.sysroot;
#endif
    return true;
}
static bool native_commands(XtcXirNativeOperation *owner, const XtcXirNativeOperationRequest *request,
    XrXirInvocationRequest *invocation) {
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner->workspace);
    static const char *const leaves[8] = {"generated.c", "launcher.c", "generated.o", "launcher.o",
        "generated.d", "launcher.d", "link.d", "program"};
    char *files[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        owner->stage = i >= 6 ? XR_XIR_INVOCATION_LINK :
            i % 2 ? XR_XIR_INVOCATION_LAUNCHER : XR_XIR_INVOCATION_GENERATED;
        if (!native_path(owner, i < 2 ? paths->input : paths->output, leaves[i], &files[i])) return false;
    }
    owner->stage = XR_XIR_INVOCATION_NO_STAGE;
    const char *sdk_root = xr_xir_runtime_sdk_root(request->sdk);
    static const char *const children[3] = {"src", "include", "generated"};
    char *includes[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        char *path = NULL;
        if (!native_path(owner, sdk_root, children[i], &path) ||
            !native_join(owner, "-I", path, "", &includes[i])) return false;
    }
    /* Generated C must be warning-free, except two GCC diagnostics the emitter's
     * nested zero initializers and zero-cost budget guards always trigger. */
    static const char *const flags[9] = {"-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-missing-braces", "-Wno-type-limits", "-ffp-contract=off", "-DNDEBUG"};
    for (unsigned stage = 0; stage < 2; ++stage) {
        owner->stage = stage ? XR_XIR_INVOCATION_LAUNCHER : XR_XIR_INVOCATION_GENERATED;
        XrProcessSpec spec; size_t argc = 0;
        if (!native_spec(owner, request, request->compiler, &spec, &argc)) return false;
        for (unsigned i = 0; i < 9; ++i) spec.argv[argc++] = flags[i];
        for (unsigned i = 0; i < 3; ++i) spec.argv[argc++] = includes[i];
        spec.argv[argc++] = "-c"; spec.argv[argc++] = files[stage];
        spec.argv[argc++] = "-o"; spec.argv[argc++] = files[2 + stage];
        spec.argv[argc++] = "-MD"; spec.argv[argc++] = "-MF"; spec.argv[argc++] = files[4 + stage];
        if (stage) {
            char *symbol = NULL;
            const XrXirNativeProjectionFacts *facts = xr_compile_native_projection_facts(request->projection);
            if (!native_join(owner, "-DXIR_SDK_PROGRAM_SYMBOL=", facts->prefix, "_program", &symbol)) return false;
            spec.argv[argc++] = symbol;
        }
        if (!native_process(owner, xtc_process_prepare(owner->resources, &spec, &owner->processes[stage]))) return false;
        invocation->compile[stage] = (XrXirInvocationCompile){owner->processes[stage], files[stage],
            files[2 + stage], files[4 + stage]};
    }
    XrProcessSpec spec; size_t argc = 0;
    owner->stage = XR_XIR_INVOCATION_LINK;
    if (!native_spec(owner, request, request->linker, &spec, &argc)) return false;
    char *report_argument = NULL;
    if (!native_join(owner, native_report_option, files[6], "", &report_argument)) return false;
    spec.argv[argc++] = "-o"; spec.argv[argc++] = files[7];
    spec.argv[argc++] = files[2]; spec.argv[argc++] = files[3];
#ifndef XR_OS_MACOS
    /* The admission and declaration archives reference each other. */
    spec.argv[argc++] = "-Wl,--start-group";
#endif
    static const char *const archives[5] = {"lib/libxray_xir_runtime_host.a", "lib/libxray_xir_scalar.a",
        "lib/libxray_xir_admission.a", "lib/libxray_xir_declarations.a", "lib/libxray_compile_resources.a"};
    for (unsigned i = 0; i < 5; ++i)
        if (!native_sdk(owner, xr_xir_runtime_sdk_file(request->sdk, archives[i], &spec.argv[argc++]))) return false;
#ifndef XR_OS_MACOS
    spec.argv[argc++] = "-Wl,--end-group";
#endif
    spec.argv[argc++] = "-lm"; spec.argv[argc++] = report_argument;
    if (!native_process(owner, xtc_process_prepare(owner->resources, &spec, &owner->processes[2]))) return false;
    invocation->link = owner->processes[2]; invocation->input_directory = paths->input;
    invocation->output_directory = paths->output; invocation->link_report = files[6]; invocation->output = files[7];
    return true;
}
