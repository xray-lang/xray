/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain.c - 'xray toolchain' diagnostics
 */

#include "xcli_spec.h"
#include "xcli_toolchain.h"
#include "xcli_diag.h"
#include "../../base/xchecks.h"
#include "../../os/os_proc.h"

#include <stdio.h>
#include <string.h>

static bool resolve_zig_plan(const char *zig_arg, const char *program_hint,
                             XrCliToolchainPlan *plan, char *err, size_t err_size) {
    XrCliBuildTarget target;

    if (!xr_cli_build_target_parse("x86_64-linux-musl", &target, err, err_size))
        return false;
    return xr_cli_toolchain_resolve_ex(XR_CLI_TOOLCHAIN_ZIG, &target, "cc", zig_arg, program_hint,
                                       plan, err, err_size);
}

static bool resolve_zig_executable(const char *zig_arg, char *out, size_t out_size,
                                   const char *program_hint, char *err, size_t err_size) {
    XrCliToolchainPlan plan;

    if (!resolve_zig_plan(zig_arg, program_hint, &plan, err, err_size))
        return false;
    if (!xr_cli_toolchain_find_executable(plan.program, out, out_size)) {
        snprintf(err, err_size, "Zig not found (resolution order: --zig, XRAY_ZIG, bundled, PATH)");
        return false;
    }
    return true;
}

static void print_supported_targets(const char *program_hint) {
    size_t count;
    const char *const *targets = xr_cli_build_target_supported_names(&count);

    printf("Supported AOT targets:\n");
    for (size_t i = 0; i < count; i++) {
        XrCliBuildTarget target;
        XrCliToolchainPlan plan;
        char err[256];

        if (!xr_cli_build_target_parse(targets[i], &target, err, sizeof(err))) {
            printf("  %-24s error: %s\n", targets[i], err);
            continue;
        }
        if (!xr_cli_toolchain_resolve_ex(XR_CLI_TOOLCHAIN_AUTO, &target, "cc", NULL, program_hint,
                                         &plan, err, sizeof(err))) {
            printf("  %-24s error: %s\n", target.name, err);
            continue;
        }
        if (target.is_native)
            printf("  %-24s toolchain=%s program=%s\n", target.name,
                   xr_cli_toolchain_kind_name(plan.kind), plan.program);
        else
            printf("  %-24s toolchain=%s command=\"%s cc -target %s\"\n", target.name,
                   xr_cli_toolchain_kind_name(plan.kind), plan.program, target.zig_triple);
    }
}

static int cmd_toolchain_list(const char *zig_arg, const char *program_hint) {
    char zig_path[1200];
    char err[256];

    printf("AOT toolchain resolution:\n");
    printf("  Zig order: --zig, XRAY_ZIG, bundled, PATH\n");
    if (resolve_zig_executable(zig_arg, zig_path, sizeof(zig_path), program_hint, err, sizeof(err)))
        printf("  Zig:       %s\n", zig_path);
    else
        printf("  Zig:       missing (%s)\n", err);
    printf("\n");
    print_supported_targets(program_hint);
    printf("\n");
    printf("Install helper: scripts/install_zig_toolchain.sh\n");
    printf("Bundled layouts: ./zig, ./tools/zig/zig, ./tools/zig/bin/zig, "
           "../lib/xray/zig/zig, ../libexec/xray/zig/zig\n");
    return XR_CLI_EXIT_OK;
}

static int cmd_toolchain_doctor(const char *zig_arg, const char *program_hint) {
    char zig_path[1200];
    char err[256];
    const char *argv[3];
    XrProcId pid;
    int code = -1;

    printf("AOT toolchain doctor:\n");
    if (!resolve_zig_executable(zig_arg, zig_path, sizeof(zig_path), program_hint, err,
                                sizeof(err))) {
        printf("  Zig: missing\n");
        printf("  Error: %s\n", err);
        printf("  Fix: run scripts/install_zig_toolchain.sh, bundle Zig with xray, "
               "or set XRAY_ZIG=/path/to/zig\n");
        return XR_CLI_EXIT_UNAVAILABLE;
    }

    printf("  Zig: %s\n", zig_path);
    printf("  Zig version: ");
    fflush(stdout);
    argv[0] = zig_path;
    argv[1] = "version";
    argv[2] = NULL;
    pid = xr_proc_spawn(zig_path, argv);
    if (pid == XR_PROC_INVALID || xr_proc_wait(pid, &code) != 0 || code != 0) {
        printf("\n  Error: failed to run '%s version'\n", zig_path);
        return XR_CLI_EXIT_UNAVAILABLE;
    }

    printf("\n");
    print_supported_targets(program_hint);
    printf("\n");
    printf("Status: OK\n");
    return XR_CLI_EXIT_OK;
}

XR_FUNC int cmd_toolchain(const XrCliInvocation *inv) {
    const char *subcmd;
    const char *zig_arg;

    XR_DCHECK(inv != NULL, "inv is NULL");
    if (inv->positional_count < 1) {
        xr_cli_error("toolchain", "missing subcommand");
        return XR_CLI_EXIT_USAGE;
    }

    subcmd = inv->positionals[0];
    zig_arg = xr_cli_opt_string(&inv->options, "zig", NULL);

    if (strcmp(subcmd, "list") == 0)
        return cmd_toolchain_list(zig_arg, inv->ctx ? inv->ctx->program : NULL);
    if (strcmp(subcmd, "doctor") == 0)
        return cmd_toolchain_doctor(zig_arg, inv->ctx ? inv->ctx->program : NULL);

    xr_cli_error("toolchain", "unknown subcommand '%s'", subcmd);
    return XR_CLI_EXIT_USAGE;
}
