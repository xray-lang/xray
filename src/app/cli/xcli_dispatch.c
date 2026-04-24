/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_dispatch.c - Top-level CLI dispatch and command suggestion
 *
 * KEY CONCEPT:
 *   Routes argv to the correct command handler via spec-driven dispatch.
 *   Zero-arg -> REPL, .xr suffix -> implicit "run", unknown -> suggest.
 *   Every command goes through the unified parser before reaching its handler.
 */

#include "xcli_dispatch.h"
#include "xcli_parser.h"
#include "xcli_help.h"
#include "xcli_diag.h"
#include "xcli_fs.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <string.h>
#include <stdio.h>

/* Levenshtein edit distance (single-row DP, O(min(m,n)) space).
 * Command names are short (<16 chars) so this is effectively free. */
static int string_distance(const char *s1, const char *s2) {
    int len1 = (int)strlen(s1);
    int len2 = (int)strlen(s2);
    if (len1 == 0) return len2;
    if (len2 == 0) return len1;
    if (len1 > len2) {
        const char *tmp_s = s1; s1 = s2; s2 = tmp_s;
        int tmp_n = len1; len1 = len2; len2 = tmp_n;
    }
    int row[33];
    for (int j = 0; j <= len1; j++) row[j] = j;
    for (int i = 1; i <= len2; i++) {
        int prev = row[0];
        row[0] = i;
        for (int j = 1; j <= len1; j++) {
            int cost = (s1[j - 1] == s2[i - 1]) ? 0 : 1;
            int val = prev + cost;
            if (row[j] + 1 < val) val = row[j] + 1;
            if (row[j - 1] + 1 < val) val = row[j - 1] + 1;
            prev = row[j];
            row[j] = val;
        }
    }
    return row[len1];
}

/* ========== Handler Forward Declarations ========== */

XR_FUNC int cmd_run(const XrCliInvocation *inv);
XR_FUNC int cmd_eval(const XrCliInvocation *inv);
XR_FUNC int cmd_repl(const XrCliInvocation *inv);
XR_FUNC int cmd_test(const XrCliInvocation *inv);
XR_FUNC int cmd_check(const XrCliInvocation *inv);
XR_FUNC int cmd_fmt(const XrCliInvocation *inv);
XR_FUNC int cmd_compile(const XrCliInvocation *inv);
XR_FUNC int cmd_build(const XrCliInvocation *inv);
XR_FUNC int cmd_deps(const XrCliInvocation *inv);
XR_FUNC int cmd_pkg(const XrCliInvocation *inv);
/* Defined below in this file */
XR_FUNC int cmd_info(const XrCliInvocation *inv);
XR_FUNC int cmd_help(const XrCliInvocation *inv);
#ifdef XR_HAS_LSP
XR_FUNC int cmd_lsp(const XrCliInvocation *inv);
#endif
#ifdef XR_HAS_DAP
XR_FUNC int cmd_dap(const XrCliInvocation *inv);
#endif
#ifdef XR_HAS_MCP
XR_FUNC int cmd_mcp_server(const XrCliInvocation *inv);
#endif

/* Register all command handlers into the spec table.
 * Must be called once before xr_cli_main(). */
void xr_cli_register_all_handlers(void) {
    xr_cli_register_handler("run",     cmd_run);
    xr_cli_register_handler("eval",    cmd_eval);
    xr_cli_register_handler("repl",    cmd_repl);
    xr_cli_register_handler("test",    cmd_test);
    xr_cli_register_handler("check",   cmd_check);
    xr_cli_register_handler("fmt",     cmd_fmt);
    xr_cli_register_handler("compile", cmd_compile);
    xr_cli_register_handler("build",   cmd_build);
    xr_cli_register_handler("deps",    cmd_deps);
    xr_cli_register_handler("pkg",     cmd_pkg);
    xr_cli_register_handler("info",    cmd_info);
    xr_cli_register_handler("help",    cmd_help);
#ifdef XR_HAS_LSP
    xr_cli_register_handler("lsp",     cmd_lsp);
#endif
#ifdef XR_HAS_DAP
    xr_cli_register_handler("dap",     cmd_dap);
#endif
#ifdef XR_HAS_MCP
    xr_cli_register_handler("mcp-server", cmd_mcp_server);
#endif
}

/* ========== Handler Dispatch ========== */

/* Parse command args via unified parser, then call handler.
 * cmd_argc/cmd_argv start AFTER the command name. */
static int dispatch_new_handler(const XrCliCommandSpec *spec,
                                XrCliHandler handler,
                                int cmd_argc, char **cmd_argv,
                                const XrCliContext *ctx) {
    XR_DCHECK(spec != NULL, "spec is NULL");
    XR_DCHECK(handler != NULL, "handler is NULL");

    XrCliInvocation inv;
    XrCliExitCode rc = xr_cli_parse_command(spec, cmd_argc, cmd_argv,
                                             ctx, &inv);
    if (rc != XR_CLI_EXIT_OK) {
        return (int)rc;
    }

    int result = handler(&inv);
    xr_cli_invocation_free(&inv);
    return result;
}

/* ========== Info Command ========== */

XR_FUNC int cmd_info(const XrCliInvocation *inv) {
    (void)inv;
    xr_cli_print_version();
    printf("\nEnvironment:\n");
    const char *home = getenv("HOME");
    if (home) {
        printf("  Package cache: %s/.xray/packages/\n", home);
    }
    printf("  Config file: xray.toml\n");
    printf("  Registry: pkg.xray-lang.org\n");
    return XR_CLI_EXIT_OK;
}

/* ========== Help Command ========== */

XR_FUNC int cmd_help(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    if (inv->positional_count < 1) {
        xr_cli_print_usage();
        return XR_CLI_EXIT_OK;
    }
    const char *topic = inv->positionals[0];
    const XrCliCommandSpec *spec = xr_cli_find_command(topic);
    if (!spec) {
        xr_cli_error("help", "unknown command '%s'", topic);
        xr_cli_suggest_command(topic);
        return XR_CLI_EXIT_USAGE;
    }
    xr_cli_print_command_help(spec);
    return XR_CLI_EXIT_OK;
}

/* ========== Command Suggestion (Levenshtein) ========== */

bool xr_cli_suggest_command(const char *input) {
    XR_DCHECK(input != NULL, "input is NULL");

    const XrCliCommandSpec *cmds = xr_cli_get_commands();
    const char *best = NULL;
    int best_dist = 999;

    for (int i = 0; cmds[i].name != NULL; i++) {
        if (cmds[i].hidden) continue;
        int dist = string_distance(input, cmds[i].name);
        if (dist < best_dist && dist <= 2) {
            best_dist = dist;
            best = cmds[i].name;
        }
    }

    if (best) {
        fprintf(stderr, "\nDid you mean?\n");
        fprintf(stderr, "  xray %s\n\n", best);
        return true;
    }
    return false;
}

/* ========== Main Dispatch ========== */

int xr_cli_main(int argc, char **argv) {
    XR_DCHECK(argc >= 1, "argc must be >= 1");
    XR_DCHECK(argv != NULL, "argv is NULL");

    /* Parse global flags */
    XrCliContext ctx;
    int consumed = xr_cli_parse_global(argc, argv, &ctx);

    /* Handle --help / --version */
    if (consumed == -1) {
        xr_cli_print_usage();
        return XR_CLI_EXIT_OK;
    }
    if (consumed == -2) {
        xr_cli_print_version();
        return XR_CLI_EXIT_OK;
    }

    /* Adjust for consumed global flags */
    int cmd_argc = argc - 1 - consumed;  /* skip argv[0] + consumed flags */
    char **cmd_argv = argv + 1 + consumed;

    /* Zero arguments after global flags -> default to REPL */
    if (cmd_argc < 1) {
        const XrCliCommandSpec *spec = xr_cli_find_command("repl");
        XR_DCHECK(spec != NULL, "repl command not found");
        XR_DCHECK(spec->handler != NULL, "repl handler not found");
        return dispatch_new_handler(spec, spec->handler, 0, NULL, &ctx);
    }

    const char *cmd_name = cmd_argv[0];

    /* Route 1: Registered command name match */
    const XrCliCommandSpec *spec = xr_cli_find_command(cmd_name);
    if (spec) {
        XR_DCHECK(spec->handler != NULL, "no handler for command");

        /* Subcommand routing (e.g. pkg <subcommand>) */
        if (spec->subcommands && spec->subcommand_count > 0) {
            if (cmd_argc < 2) {
                xr_cli_print_subcommand_help(spec);
                return XR_CLI_EXIT_OK;
            }
            if (strcmp(cmd_argv[1], "--help") == 0 ||
                strcmp(cmd_argv[1], "-h") == 0) {
                xr_cli_print_subcommand_help(spec);
                return XR_CLI_EXIT_OK;
            }
            return dispatch_new_handler(spec, spec->handler,
                                         cmd_argc - 1, cmd_argv + 1, &ctx);
        }

        /* --help check before full parse */
        for (int i = 1; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], "--help") == 0 ||
                strcmp(cmd_argv[i], "-h") == 0) {
                xr_cli_print_command_help(spec);
                return XR_CLI_EXIT_OK;
            }
        }

        /* Dispatch via unified parser -> handler */
        return dispatch_new_handler(spec, spec->handler,
                                     cmd_argc - 1, cmd_argv + 1, &ctx);
    }

    /* Route 2: argv[1] ends with .xr -> implicit "run" */
    const char *ext = strrchr(cmd_name, '.');
    if (ext && strcmp(ext, ".xr") == 0) {
        spec = xr_cli_find_command("run");
        XR_DCHECK(spec != NULL, "run command not found");
        XR_DCHECK(spec->handler != NULL, "run handler not found");
        return dispatch_new_handler(spec, spec->handler,
                                     cmd_argc, cmd_argv, &ctx);
    }

    /* Route 3: xray -e 'code' -> shortcut for eval */
    if (cmd_name[0] == '-' && cmd_name[1] == 'e' && cmd_name[2] == '\0') {
        spec = xr_cli_find_command("eval");
        XR_DCHECK(spec != NULL, "eval command not found");
        XR_DCHECK(spec->handler != NULL, "eval handler not found");
        return dispatch_new_handler(spec, spec->handler,
                                     cmd_argc - 1, cmd_argv + 1, &ctx);
    }

    /* Route 4: Unknown command -> error + suggestion */
    xr_cli_unknown_command(cmd_name);
    xr_cli_suggest_command(cmd_name);
    return XR_CLI_EXIT_USAGE;
}
