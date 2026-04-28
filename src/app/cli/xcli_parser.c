/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_parser.c - Unified CLI argument parser
 *
 * KEY CONCEPT:
 *   Parses CLI arguments against a declarative XrCliCommandSpec.
 *   Replaces all per-command getopt_long usage with a single parser.
 *   Produces a fully validated XrCliInvocation for the handler.
 */

#include "xcli_parser.h"
#include "xcli_diag.h"
#include "xcli_output.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_fd.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/* ========== Global Flag Parsing ========== */

int xr_cli_parse_global(int argc, char **argv, XrCliContext *ctx) {
    XR_DCHECK(ctx != NULL, "ctx is NULL");
    /* Defaults */
    ctx->color = xr_isatty(xr_stdout_fd()) && xr_isatty(xr_stderr_fd());
    ctx->verbose = false;
    ctx->quiet = false;
    ctx->json_output = false;
    ctx->program = (argc > 0) ? argv[0] : "xray";

    if (argc < 2)
        return 0;

    int consumed = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        /* Stop at first non-flag or end-of-flags */
        if (arg[0] != '-')
            break;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
            return -1;
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0)
            return -2;
        if (strcmp(arg, "--color") == 0) {
            ctx->color = true;
            xr_cli_set_color(XR_COLOR_ON);
            consumed++;
        } else if (strcmp(arg, "--no-color") == 0) {
            ctx->color = false;
            xr_cli_set_color(XR_COLOR_OFF);
            consumed++;
        } else if (strcmp(arg, "--verbose") == 0) {
            ctx->verbose = true;
            consumed++;
        } else if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
            ctx->quiet = true;
            consumed++;
        } else if (strcmp(arg, "--json") == 0) {
            ctx->json_output = true;
            consumed++;
        } else {
            /* Unknown flag — not a global flag, let command parse it */
            break;
        }
    }

    /* Apply to output layer (color is only forced when explicitly requested) */
    if (ctx->verbose) {
        xr_cli_set_output_level(XR_OUTPUT_VERBOSE);
    } else if (ctx->quiet) {
        xr_cli_set_output_level(XR_OUTPUT_QUIET);
    }
    if (ctx->json_output) {
        xr_cli_set_json(true);
    }

    return consumed;
}

/* ========== Option Matching Helpers ========== */

/* Match --long-name or --long-name=value. Returns option index or -1. */
static int match_long_option(const XrCliOptionSpec *opts, int count, const char *arg,
                             const char **value_out) {
    XR_DCHECK(arg != NULL, "arg is NULL");
    *value_out = NULL;

    /* Skip leading -- */
    const char *name = arg + 2;
    const char *eq = strchr(name, '=');
    size_t name_len = eq ? (size_t) (eq - name) : strlen(name);

    for (int i = 0; i < count; i++) {
        if (!opts[i].long_name)
            continue;
        if (strlen(opts[i].long_name) == name_len &&
            strncmp(opts[i].long_name, name, name_len) == 0) {
            if (eq)
                *value_out = eq + 1;
            return i;
        }
    }
    return -1;
}

/* Match -X short option. Returns option index or -1. */
static int match_short_option(const XrCliOptionSpec *opts, int count, int ch) {
    for (int i = 0; i < count; i++) {
        if (opts[i].short_name == ch) {
            return i;
        }
    }
    return -1;
}

/* ========== Command Parsing ========== */

XrCliExitCode xr_cli_parse_command(const XrCliCommandSpec *spec, int argc, char **argv,
                                   const XrCliContext *ctx, XrCliInvocation *inv) {
    XR_DCHECK(spec != NULL, "spec is NULL");
    XR_DCHECK(ctx != NULL, "ctx is NULL");
    XR_DCHECK(inv != NULL, "inv is NULL");

    memset(inv, 0, sizeof(*inv));
    inv->ctx = ctx;
    inv->spec = spec;

    /* Count options in spec */
    int opt_count = xr_cli_option_count(spec->options);
    inv->options.spec = spec->options;
    inv->options.count = opt_count;

    /* Allocate option tracking arrays */
    if (opt_count > 0) {
        inv->options.present = (bool *) xr_calloc((size_t) opt_count, sizeof(bool));
        if (!inv->options.present)
            return XR_CLI_EXIT_INTERNAL;
        inv->options.values = (const char **) xr_calloc((size_t) opt_count, sizeof(const char *));
        if (!inv->options.values) {
            xr_free(inv->options.present);
            inv->options.present = NULL;
            return XR_CLI_EXIT_INTERNAL;
        }
    }

    /* Collect positionals in a temporary array (max = argc) */
    const char **positionals = NULL;
    int pos_count = 0;
    if (argc > 0) {
        positionals = (const char **) xr_calloc((size_t) argc, sizeof(const char *));
        if (!positionals) {
            xr_cli_invocation_free(inv);
            return XR_CLI_EXIT_INTERNAL;
        }
    }

    /* Parse arguments */
    bool passthrough = false;
    int passthrough_start = -1;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        XR_DCHECK(arg != NULL, "argv element is NULL");

        /* `--` separator: everything after goes to passthrough */
        if (strcmp(arg, "--") == 0) {
            if (!spec->allow_passthrough) {
                xr_cli_error(spec->name, "'--' separator not supported by this command");
                xr_free(positionals);
                xr_cli_invocation_free(inv);
                return XR_CLI_EXIT_USAGE;
            }
            passthrough = true;
            passthrough_start = i + 1;
            break;
        }

        /* Long option: --foo or --foo=value */
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            const char *value = NULL;
            int idx = match_long_option(spec->options, opt_count, arg, &value);
            if (idx < 0) {
                xr_cli_unknown_option(spec->name, arg);
                xr_free(positionals);
                xr_cli_invocation_free(inv);
                return XR_CLI_EXIT_USAGE;
            }
            inv->options.present[idx] = true;

            /* If option takes a value and none was in --opt=val form */
            if (spec->options[idx].value_kind != XR_CLI_VALUE_NONE && !value) {
                if (i + 1 >= argc) {
                    xr_cli_missing_argument(spec->name, arg);
                    xr_free(positionals);
                    xr_cli_invocation_free(inv);
                    return XR_CLI_EXIT_USAGE;
                }
                value = argv[++i];
            }
            inv->options.values[idx] = value;
            continue;
        }

        /* Short option: -X or -Xvalue */
        if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
            /* Handle short option (single char only for simplicity) */
            int ch = (unsigned char) arg[1];
            int idx = match_short_option(spec->options, opt_count, ch);
            if (idx < 0) {
                /* Special case: -j<N> for test parallelism (short numeric) */
                char short_buf[3] = {'-', (char) arg[1], '\0'};
                xr_cli_unknown_option(spec->name, short_buf);
                xr_free(positionals);
                xr_cli_invocation_free(inv);
                return XR_CLI_EXIT_USAGE;
            }
            inv->options.present[idx] = true;

            if (spec->options[idx].value_kind != XR_CLI_VALUE_NONE) {
                /* Value might be attached: -Xvalue or separate: -X value */
                if (arg[2] != '\0') {
                    inv->options.values[idx] = arg + 2;
                } else if (i + 1 < argc) {
                    inv->options.values[idx] = argv[++i];
                } else {
                    char short_buf[3] = {'-', (char) arg[1], '\0'};
                    xr_cli_missing_argument(spec->name, short_buf);
                    xr_free(positionals);
                    xr_cli_invocation_free(inv);
                    return XR_CLI_EXIT_USAGE;
                }
            }
            continue;
        }

        /* Positional argument */
        positionals[pos_count++] = arg;
    }

    /* Handle passthrough args */
    if (passthrough && passthrough_start >= 0 && passthrough_start < argc) {
        inv->passthrough_argc = argc - passthrough_start;
        inv->passthrough_argv = argv + passthrough_start;
    }

    /* Validate positional arity */
    if (pos_count < spec->positional_min) {
        if (spec->positional_min == 1) {
            xr_cli_error(spec->name, "missing required argument");
        } else {
            xr_cli_error(spec->name, "expected at least %d arguments, got %d", spec->positional_min,
                         pos_count);
        }
        xr_free(positionals);
        xr_cli_invocation_free(inv);
        return XR_CLI_EXIT_USAGE;
    }
    if (spec->positional_max >= 0 && pos_count > spec->positional_max) {
        xr_cli_error(spec->name, "too many arguments (max %d, got %d)", spec->positional_max,
                     pos_count);
        xr_free(positionals);
        xr_cli_invocation_free(inv);
        return XR_CLI_EXIT_USAGE;
    }

    inv->positional_count = pos_count;
    inv->positionals = positionals;

    return XR_CLI_EXIT_OK;
}

void xr_cli_invocation_free(XrCliInvocation *inv) {
    if (!inv)
        return;
    if (inv->options.present) {
        xr_free(inv->options.present);
        inv->options.present = NULL;
    }
    if (inv->options.values) {
        xr_free((void *) inv->options.values);
        inv->options.values = NULL;
    }
    if (inv->positionals) {
        xr_free((void *) inv->positionals);
        inv->positionals = NULL;
    }
}
