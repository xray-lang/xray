/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_help.c - Auto-generated help text from command specs
 *
 * KEY CONCEPT:
 *   All help text is generated from the XrCliCommandSpec table.
 *   No command has its own print_*_help() function.
 */

#include "xcli_help.h"
#include "xray.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

void xr_cli_print_version(void) {
    printf("xray v%d.%d.%d (", XRAY_VERSION_MAJOR, XRAY_VERSION_MINOR, XRAY_VERSION_PATCH);
#ifdef XRAY_HAS_JIT
    printf("JIT");
#elif defined(XRAY_HAS_AOT)
    printf("AOT");
#else
    printf("VM");
#endif

#if defined(__x86_64__) || defined(_M_X64)
    printf(", x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    printf(", arm64");
#elif defined(__i386__) || defined(_M_IX86)
    printf(", x86");
#endif

#if defined(__linux__)
    printf("-linux");
#elif defined(__APPLE__)
    printf("-darwin");
#elif defined(_WIN32)
    printf("-windows");
#endif

    printf(")\n");
}

/* Print option help lines for a command's option spec. */
static void print_options(const XrCliOptionSpec *opts) {
    printf("\nOptions:\n");
    if (!opts || !opts[0].long_name)
        return;
    for (int i = 0; opts[i].long_name != NULL; i++) {
        const XrCliOptionSpec *o = &opts[i];
        char short_buf[8] = "";
        if (o->short_name) {
            snprintf(short_buf, sizeof(short_buf), "-%c, ", (char) o->short_name);
        } else {
            snprintf(short_buf, sizeof(short_buf), "    ");
        }

        char long_buf[40];
        if (o->value_kind != XR_CLI_VALUE_NONE && o->value_name) {
            snprintf(long_buf, sizeof(long_buf), "--%s <%s>", o->long_name, o->value_name);
        } else {
            snprintf(long_buf, sizeof(long_buf), "--%s", o->long_name);
        }

        printf("  %s%-24s %s\n", short_buf, long_buf, o->help ? o->help : "");
    }
}

void xr_cli_print_usage(void) {
    printf("Xray - High-performance scripting language\n");
    printf("\nUsage:\n");
    printf("  xray <file.xr> [-- args...]       Run script\n");
    printf("  xray <command> [options] [args...] Execute command\n");
    printf("  xray                               Start REPL\n");

    printf("\nCommands:\n");
    const XrCliCommandSpec *cmds = xr_cli_get_commands();
    XR_DCHECK(cmds != NULL, "command table is NULL");
    for (int i = 0; cmds[i].name != NULL; i++) {
        if (cmds[i].hidden)
            continue;
        printf("  %-14s %s\n", cmds[i].name, cmds[i].summary ? cmds[i].summary : "");
    }

    printf("\nGlobal Options:\n");
    printf("  -v, --version    Show version\n");
    printf("  -h, --help       Show help\n");
    printf("  --color          Force color output\n");
    printf("  --no-color       Disable color output\n");
    printf("\nRun 'xray help <command>' for command details.\n");
}

void xr_cli_print_command_help(const XrCliCommandSpec *spec) {
    XR_DCHECK(spec != NULL, "spec is NULL");

    /* If command has subcommands, delegate to subcommand help. */
    if (spec->subcommands) {
        xr_cli_print_subcommand_help(spec);
        return;
    }

    printf("Usage: xray %s", spec->name);

    /* Show [options] if there are any non-help options */
    int opt_count = xr_cli_option_count(spec->options);
    if (opt_count > 0) {
        printf(" [options]");
    }

    /* Show positionals hint */
    if (spec->positional_max != 0) {
        if (spec->positional_min > 0) {
            printf(" <file>");
        } else {
            printf(" [file...]");
        }
    }

    if (spec->allow_passthrough) {
        printf(" [-- args...]");
    }

    printf("\n");

    if (spec->summary) {
        printf("\n%s\n", spec->summary);
    }

    print_options(spec->options);
    printf("  -h, %-24s %s\n", "--help", "Show help");
    printf("\n");
}

void xr_cli_print_subcommand_help(const XrCliCommandSpec *parent) {
    XR_DCHECK(parent != NULL, "parent is NULL");

    printf("Usage: xray %s <subcommand> [options]\n", parent->name);
    if (parent->summary) {
        printf("\n%s\n", parent->summary);
    }

    printf("\nSubcommands:\n");
    for (int i = 0; i < parent->subcommand_count; i++) {
        const XrCliCommandSpec *sub = &parent->subcommands[i];
        if (!sub->name)
            break;
        if (sub->hidden)
            continue;
        printf("  %-14s %s\n", sub->name, sub->summary ? sub->summary : "");
    }
    printf("\nRun 'xray %s <subcommand> --help' for subcommand details.\n", parent->name);
}
