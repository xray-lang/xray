/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_spec.h - Declarative command and option specification
 *
 * KEY CONCEPT:
 *   Every CLI command is described by a static XrCliCommandSpec.
 *   The parser, help generator, and dispatcher all consume these specs.
 *   No command handler parses argc/argv directly.
 */

#ifndef XCLI_SPEC_H
#define XCLI_SPEC_H

#include "../../base/xdefs.h"
#include "xcli_diag.h"
#include <stdbool.h>

/* ========== Option Value Types ========== */

typedef enum {
    XR_CLI_VALUE_NONE,      /* flag, no argument */
    XR_CLI_VALUE_BOOL,      /* --flag / --no-flag */
    XR_CLI_VALUE_INT,       /* --opt=N */
    XR_CLI_VALUE_STRING,    /* --opt=VALUE */
} XrCliValueKind;

/* ========== Global Context ========== */

/* Parsed once from global flags, read-only for all handlers. */
typedef struct {
    bool color;             /* resolved: --color / --no-color / auto */
    bool verbose;
    bool quiet;
    bool json_output;
    const char *program;    /* argv[0], for diagnostics */
} XrCliContext;

/* ========== Option Specification ========== */

typedef struct {
    const char *long_name;      /* e.g. "verbose" (without --) */
    int short_name;             /* e.g. 'v', or 0 if none */
    XrCliValueKind value_kind;
    bool required;
    bool repeatable;            /* can appear multiple times */
    const char *value_name;     /* placeholder in help, e.g. "FILE" */
    const char *help;           /* one-line help text */
} XrCliOptionSpec;

/* Sentinel: end of option spec array */
#define XR_CLI_OPT_END { NULL, 0, XR_CLI_VALUE_NONE, false, false, NULL, NULL }

/* ========== Option Query Model ========== */

/* Flat array indexed by option position in spec.
 * CLI option count is always small (<20), so linear scan is fine. */
typedef struct {
    const XrCliOptionSpec *spec;    /* back-pointer to spec array */
    int count;                      /* number of options in spec */
    bool *present;                  /* present[i]: option i was given */
    const char **values;            /* values[i]: string value (NULL if not) */
} XrCliOptionMap;

/* ========== Command Specification ========== */

/* Forward declaration for handler signature */
typedef struct XrCliInvocation XrCliInvocation;

typedef int (*XrCliHandler)(const XrCliInvocation *inv);

typedef struct XrCliCommandSpec XrCliCommandSpec;
struct XrCliCommandSpec {
    const char *name;           /* e.g. "run", "pkg" */
    const char *summary;        /* one-line description for help listing */
    const char *description;    /* longer description for command help */
    const XrCliOptionSpec *options;   /* NULL-terminated array */
    int positional_min;
    int positional_max;         /* -1 = unlimited */
    bool allow_passthrough;     /* support `--` separator */
    bool hidden;                /* hide from help listing */
    XrCliHandler handler;       /* NULL if has subcommands */

    /* Subcommand support (for `pkg` etc.) */
    const XrCliCommandSpec *subcommands;  /* NULL-terminated array, or NULL */
    int subcommand_count;
};

/* ========== Invocation Model ========== */

/* The fully parsed, validated invocation passed to every handler. */
struct XrCliInvocation {
    const XrCliContext *ctx;
    const XrCliCommandSpec *spec;
    XrCliOptionMap options;
    int positional_count;
    const char **positionals;
    int passthrough_argc;       /* args after `--` */
    char **passthrough_argv;
};

/* ========== Option Map Accessors ========== */

/* Check if an option was provided (by long name). */
XR_FUNC bool xr_cli_opt_present(const XrCliOptionMap *map, const char *name);

/* Get string value of an option (NULL if not provided). */
XR_FUNC const char *xr_cli_opt_string(const XrCliOptionMap *map,
                                       const char *name,
                                       const char *default_val);

/* Get integer value of an option (default_val if not provided). */
XR_FUNC int xr_cli_opt_int(const XrCliOptionMap *map, const char *name,
                            int default_val);

/* Get boolean flag value (false if not provided). */
XR_FUNC bool xr_cli_opt_bool(const XrCliOptionMap *map, const char *name);

/* ========== Command Registry ========== */

/* Get the top-level command table (NULL-terminated). */
XR_FUNC const XrCliCommandSpec *xr_cli_get_commands(void);

/* Find a command by name in the top-level table. Returns NULL if not found. */
XR_FUNC const XrCliCommandSpec *xr_cli_find_command(const char *name);

/* Count the number of options in a spec array (up to sentinel). */
XR_FUNC int xr_cli_option_count(const XrCliOptionSpec *opts);

#endif // XCLI_SPEC_H
