/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain_probe.inc.c - Compile and run a real LIR native program
 */
#include "xcli_lir.h"
#include "api/xsource_compile.h"
#include "lir/xlir_c.h"
#include "app/toolchain/xtc_host_toolchain.h"
#include "app/toolchain/xtc_xir_workspace.h"
#include "base/xfileio.h"
#include "os/os_fs.h"

static bool probe_workspace_close(XtcXirWorkspace **workspace) {
    for (unsigned attempt = 0; attempt < 8; ++attempt)
        if (xtc_xir_workspace_close(workspace, 1024) == XTC_XIR_WORKSPACE_OK) return true;
    return false;
}

static int toolchain_probe(XrCompileState *state, const XrCliInvocation *inv) {
    if (inv->positional_count != 1 || inv->passthrough_argc) return XR_CLI_EXIT_USAGE;
    const char *target_name = xr_cli_opt_string(&inv->options, "target", "native");
    XrToolchainTarget target = {0};
    char error[256];
    if (!xtc_target_parse(target_name, &target, error, sizeof(error))) return XR_CLI_EXIT_USAGE;
    bool ready = false;
    const char *stage = "target";
    XrCompileResources *resources = xr_compile_state_resources(state), *compiled = NULL;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XtcHostToolchain *host_owner = NULL;
    XtcXirWorkspace *workspace = NULL;
    XrLirLowered lowered = {0};
    XrLirCOutput c = {0};
    XrToolchainProcess *process = NULL;
    XrProcessResult outcome = {0};
    char *source = NULL, *program = NULL;
    XrCliNativeOptions options = {xr_cli_opt_string(&inv->options, "cc", NULL),
        xr_cli_opt_string(&inv->options, "provider", NULL), false};
    if (!target.is_native || xr_cli_opt_present(&inv->options, "zig") ||
        strcmp(xr_cli_opt_string(&inv->options, "profile", "hosted"), "hosted"))
        goto cleanup;
    stage = "workspace";
    XtcHostDiagnostic diagnostic = {0};
    XtcHostToolchainRequest discovery = {.timeout_ms = 30000, .output_limit = 1024 * 1024};
    if (xtc_host_toolchain_open(resources, &discovery, &host_owner, &diagnostic) != XTC_HOST_OK)
        goto cleanup;
    const XtcHostToolchainView *host = xtc_host_toolchain_view(host_owner);
    XtcXirWorkspaceRequest temporary = {host->workspace_parent, {XR_PATH_LIMIT_MAX_PATH, 8, 65536}};
    if (xtc_xir_workspace_new(resources, &temporary, &workspace) != XTC_XIR_WORKSPACE_OK ||
        xtc_xir_workspace_create(workspace) != XTC_XIR_WORKSPACE_OK)
        goto cleanup;
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(workspace);
    static const char text[] = "print(42)\n";
    if (xr_path_join_owned(&policy, paths->input, "probe.xr", &source) != XR_OS_IO_OK ||
        xr_path_join_owned(&policy, paths->output, "probe.exe", &program) != XR_OS_IO_OK ||
        xr_os_io_write_new_file_sync(&policy, source, (const uint8_t *)text, sizeof(text) - 1) != XR_OS_IO_OK)
        goto cleanup;
    stage = "source";
    if (xr_cli_lir_compile(source, &compiled, &lowered) != XR_CLI_EXIT_OK) goto cleanup;
    XrLirCRequest emit = {lowered.module, lowered.entry, compiled, source, NULL};
    XrLirCDiagnostic emitted = {0};
    stage = "C emission";
    if (xr_lir_emit_c_standalone(&emit, &c, &emitted) != XR_LIR_C_OK) goto cleanup;
    xr_lir_lowered_free(&lowered);
    stage = "native compilation";
    if (xr_cli_lir_native(compiled, &options, c.bytes, c.length, program) != XR_CLI_EXIT_OK)
        goto cleanup;
    stage = "native execution";
    XrProcessSpec command;
    xtc_process_spec_init(&command, program, 30000);
    command.environment_source = XTC_PROCESS_ENV_SNAPSHOT;
    command.cwd = paths->output;
    command.output_limit = 4096;
    if (xtc_process_prepare(resources, &command, &process) != XTC_PROCESS_OK ||
        xtc_process_run(process, NULL, NULL, &outcome) != XTC_PROCESS_OK)
        goto cleanup;
    ready = outcome.exit_code == 0 && !outcome.stdout_bytes.truncated &&
        outcome.stdout_bytes.length == 3 && !memcmp(outcome.stdout_bytes.data, "42\n", 3) &&
        !outcome.stderr_bytes.length;
cleanup:
    xtc_process_result_free(&outcome);
    xtc_process_free(process);
    xr_lir_c_output_free(&c);
    xr_lir_lowered_free(&lowered);
    xr_compile_resources_release(compiled);
    if (!probe_workspace_close(&workspace)) {
        fputs("native probe workspace cleanup remains pending\n", stderr);
        fflush(stderr);
        _Exit(XR_CLI_EXIT_INTERNAL);
    }
    xr_compile_resources_free(source);
    xr_compile_resources_free(program);
    xtc_host_toolchain_free(host_owner);
    if (toolchain_json(inv)) {
        fputs("{\"target\":", stdout);
        toolchain_string(target.name);
        fputs(",\"compilerRequest\":", stdout);
        toolchain_string(options.compiler);
        fputs(",\"stage\":", stdout);
        toolchain_string(ready ? "complete" : stage);
        printf(",\"ready\":%s}\n", ready ? "true" : "false");
    } else {
        printf("Native compiler %s (%s)\n", ready ? "ready" : "unavailable", ready ? "complete" : stage);
    }
    return ready ? XR_CLI_EXIT_OK : XR_CLI_EXIT_UNAVAILABLE;
}
