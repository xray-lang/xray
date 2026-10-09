/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_spec.c - Command/option specs, option map accessors, command registry
 *
 * KEY CONCEPT:
 *   Single source of truth for all CLI commands and their options.
 *   Every command is a static spec; help text, parsing, and dispatch
 *   all derive from these specs.
 */

#include "xcli_spec.h"
#include "xcli_diag.h"
#include "../../base/xchecks.h"
#include <string.h>

/* ========== Option Specs per Command ========== */

static const XrCliOptionSpec run_options[] = {XR_CLI_OPT_END};

static const XrCliOptionSpec test_options[] = {
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Verbose output"},
    {"fail-fast", 'F', XR_CLI_VALUE_NONE, false, false, NULL, "Stop on first failure"},
    {"filter", 'f', XR_CLI_VALUE_STRING, false, false, "PATTERN", "Only run matching tests"},
    {"quiet", 'q', XR_CLI_VALUE_NONE, false, false, NULL, "Quiet mode (exit code only)"},
    {"jobs", 'j', XR_CLI_VALUE_INT, false, false, "N", "Parallel threads (default 1)"},
    {"report", 0, XR_CLI_VALUE_STRING, false, false, "FILE",
     "Write every test outcome as JSON to FILE"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec check_options[] = {
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Show all checked files"},
    {"quiet", 'q', XR_CLI_VALUE_NONE, false, false, NULL, "Show errors only"},
    {"syntax-only", 'S', XR_CLI_VALUE_NONE, false, false, NULL,
     "Skip semantic analysis (parse only)"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec fmt_options[] = {
    {"check", 'c', XR_CLI_VALUE_NONE, false, false, NULL, "Check only, do not modify"},
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Show all processed files"},
    {"tabs", 't', XR_CLI_VALUE_NONE, false, false, NULL, "Use tab indent"},
    {"indent", 'i', XR_CLI_VALUE_INT, false, false, "N", "Indent spaces (default 4)"},
    {"line-length", 'L', XR_CLI_VALUE_INT, false, false, "N",
     "Max line length hint when wrapping (default 100)"},
    {"align-branch-arrows", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Column-align `->` of match/select branch arms (default)"},
    {"no-align-branch-arrows", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Do not column-align `->` of match/select branch arms"},
    {"align-enum", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Column-align `=` of enum members"},
    {"align-fields", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Column-align `:` of class/struct/interface fields"},
    {"align-comments", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Column-align `//` of consecutive trailing line comments"},
    {"wrap", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Wrap long literals/calls exceeding --line-length"},
    {"no-trailing-comma", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Omit trailing `,` when wrapping to multi-line (default: keep)"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec build_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"c-only", 'c', XR_CLI_VALUE_NONE, false, false, NULL, "Output C source only"},
    {"c-dialect", 0, XR_CLI_VALUE_STRING, false, false, "DIALECT",
     "Generated C dialect (c11 or restricted c90; default c11)"},
    {"cc", 'C', XR_CLI_VALUE_STRING, false, false, "CC", "C compiler to use"},
    {"opt", 'O', XR_CLI_VALUE_STRING, false, false, "LEVEL",
     "Optimization (0,1,2,3,s,fast; fast keeps O3 and enables native LTO/CPU tuning)"},
    {"debug", 'g', XR_CLI_VALUE_NONE, false, false, NULL, "Emit native debug information"},
    {"cpu", 0, XR_CLI_VALUE_STRING, false, false, "CPU",
     "Tune for CPU via -march (e.g. native); host builds only"},
    {"simd", 0, XR_CLI_VALUE_STRING, false, false, "MODE",
     "Portable SIMD lowering: auto, scalar, native, neon, sve, sse2, avx2, avx512, vsx, lsx, or "
     "dispatch"},
    {"sysroot", 'r', XR_CLI_VALUE_STRING, false, false, "DIR", "System root directory"},
    {"strip", 'S', XR_CLI_VALUE_NONE, false, false, NULL, "Strip debug symbols"},
    {"native", 'N', XR_CLI_VALUE_NONE, false, false, NULL, "Select the native AOT backend (default)"},
    {"profile", 0, XR_CLI_VALUE_STRING, false, false, "NAME",
     "Build profile: hosted or freestanding"},
    {"type-names", 0, XR_CLI_VALUE_STRING, false, false, "MODE",
     "AOT type-name profile: none, public, or all"},
    {"artifact", 0, XR_CLI_VALUE_STRING, false, false, "KIND",
     "AOT artifact: executable, shared-library, or hosted-fragment"},
    {"target", 0, XR_CLI_VALUE_STRING, false, false, "TRIPLE", "AOT target triple"},
    {"toolchain", 0, XR_CLI_VALUE_STRING, false, false, "KIND",
     "AOT provider: auto, host, clang, gcc, msvc, or zig"},
    {"zig", 0, XR_CLI_VALUE_STRING, false, false, "PATH", "Path to zig executable"},
    {"dump-xaot-plan", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Dump AOT prepare plan"},
    {"dump-global-evidence", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Dump global evidence facts"},
    {"dump-xi-evidence", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Dump subject-bound local Xi evidence"},
    {"dump-link-manifest", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Dump AOT link manifest"},
    {"dump-residue", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Dump per-function abstraction-cost residue (task 217)"},
    {"dump-link-command", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Dump resolved AOT link command"},
    {"dump-toolchain-plan", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Dump verified provider, ABI, runtime artifact, and probe fingerprint"},
    {"dry-run-link", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Resolve the AOT link command without invoking the native toolchain"},
    {"linker-script", 0, XR_CLI_VALUE_STRING, false, false, "FILE",
     "Pass a linker script to the native linker"},
    {"c-header", 0, XR_CLI_VALUE_STRING, false, false, "FILE",
     "Emit a C header for manifest export symbols"},
    {"c-export-prefix", 0, XR_CLI_VALUE_STRING, false, false, "PREFIX",
     "Prefix public manifest C export symbols"},
    {"c-export-exclude", 0, XR_CLI_VALUE_STRING, false, false, "SYMBOLS",
     "Exclude comma-separated manifest C export symbols"},
    {"keep-c", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Keep generated temporary C source"},
    {"cache-dir", 0, XR_CLI_VALUE_STRING, false, false, "DIR",
     "AOT object cache directory (default <out>/.xray-cache or $XRAY_CACHE_DIR)"},
    {"rebuild", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Force recompile all AOT modules, ignoring cached objects"},
    {"lto", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Whole-program link-time optimization (cross-module inlining)"},
    {"rc-guard", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Debug RC guard codegen: poison objects on release, abort on use-after-release (task 219)"},
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Verbose output"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec deps_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"shell", 's', XR_CLI_VALUE_NONE, false, false, NULL, "Shell script format (default)"},
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "JSON format"},
    {"list", 'l', XR_CLI_VALUE_NONE, false, false, NULL, "Simple list format"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec empty_options[] = {XR_CLI_OPT_END};

static const XrCliOptionSpec info_options[] = {
    {"installation", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Report installed payload identity and ownership paths"},
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "Emit machine-readable JSON"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec self_options[] = {
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "Emit machine-readable delegation"},
    XR_CLI_OPT_END};

static const XrCliCommandSpec self_subcommands[] = {
    {"update", "Update through the active installation provider", NULL, empty_options, 0, 0, false,
     false, NULL, NULL, 0},
    {"uninstall", "Uninstall through the active installation provider", NULL, empty_options, 0, 0,
     false, false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

static const XrCliCommandSpec doctor_subcommands[] = {
    {"installation", "Diagnose active installation ownership", NULL, empty_options, 0, 0, false,
     false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

/* ========== Top-level Command Table ========== */

static XrCliCommandSpec cli_commands[] = {
    /* Execution commands */
    {"run", "Run one exact .xr source entry", NULL, run_options, 1, 1, true, false, NULL, NULL, 0},
    {"test", "Run tests", NULL, test_options, 0, -1, false, false, NULL, NULL, 0},
    {"check", "Syntax check", NULL, check_options, 0, -1, false, false, NULL, NULL, 0},
    {"fmt", "Format source code", NULL, fmt_options, 0, -1, false, false, NULL, NULL, 0},
    /* Artifact commands */
    {"build", "Compile to a native binary", NULL, build_options, 1, 1, false, false, NULL, NULL, 0},

    /* Utility commands */
    {"deps", "Analyze source dependencies", NULL, deps_options, 1, 1, false, false, NULL, NULL, 0},
    {"info", "Environment and installation info", NULL, info_options, 0, 0, false, false, NULL,
     NULL, 0},
    {"doctor", "Diagnose Xray installation state", NULL, info_options, 0, 1, false, false, NULL,
     doctor_subcommands, 1},
    {"self", "Update or uninstall through the active provider", NULL, self_options, 0, 1, false,
     false, NULL, self_subcommands, 2},
    {"help", "Show help for a command", NULL, empty_options, 0, 1, false, false, NULL, NULL, 0},

    /* Sentinel */
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

/* ========== Handler Registration ========== */

void xr_cli_register_handler(const char *name, XrCliHandler handler) {
    XR_DCHECK(name != NULL, "name is NULL");
    XR_DCHECK(handler != NULL, "handler is NULL");
    for (int i = 0; cli_commands[i].name != NULL; i++) {
        if (strcmp(cli_commands[i].name, name) == 0) {
            cli_commands[i].handler = handler;
            return;
        }
    }
    XR_DCHECK(false, "unknown command for registration");
}

/* ========== Option Map Accessors ========== */

/* Find option index by long_name. Returns -1 if not found. */
static int find_option_index(const XrCliOptionMap *map, const char *name) {
    XR_DCHECK(map != NULL, "option map is NULL");
    XR_DCHECK(name != NULL, "option name is NULL");
    for (int i = 0; i < map->count; i++) {
        if (map->spec[i].long_name && strcmp(map->spec[i].long_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

bool xr_cli_opt_present(const XrCliOptionMap *map, const char *name) {
    int idx = find_option_index(map, name);
    if (idx < 0)
        return false;
    return map->present[idx];
}

const char *xr_cli_opt_string(const XrCliOptionMap *map, const char *name,
                              const char *default_val) {
    int idx = find_option_index(map, name);
    if (idx < 0 || !map->present[idx])
        return default_val;
    return map->values[idx] ? map->values[idx] : default_val;
}

int xr_cli_opt_int(const XrCliOptionMap *map, const char *name, int default_val) {
    const char *s = xr_cli_opt_string(map, name, NULL);
    if (!s)
        return default_val;
    /* Simple atoi; real validation is done in parser. */
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return default_val;
    return (int) v;
}

bool xr_cli_opt_bool(const XrCliOptionMap *map, const char *name) {
    return xr_cli_opt_present(map, name);
}

/* ========== Command Registry ========== */

const XrCliCommandSpec *xr_cli_get_commands(void) {
    return cli_commands;
}

const XrCliCommandSpec *xr_cli_find_command(const char *name) {
    XR_DCHECK(name != NULL, "command name is NULL");
    for (int i = 0; cli_commands[i].name != NULL; i++) {
        if (strcmp(cli_commands[i].name, name) == 0) {
            return &cli_commands[i];
        }
    }
    return NULL;
}

int xr_cli_option_count(const XrCliOptionSpec *opts) {
    if (!opts)
        return 0;
    int n = 0;
    while (opts[n].long_name != NULL) {
        n++;
    }
    return n;
}
