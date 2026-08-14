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
#include "../../ir/xi_pass_policy.h"
#include <string.h>

/* ========== Option Specs per Command ========== */

static const XrCliOptionSpec run_options[] = {
    {"trace", 't', XR_CLI_VALUE_NONE, false, false, NULL, "Trace execution"},
    {"dump-bytecode", 'd', XR_CLI_VALUE_NONE, false, false, NULL, "Dump bytecode"},
    {"semantic-plan", 0, XR_CLI_VALUE_STRING, false, false, "FILE",
     "Bind an exact XSM authority to an XTP input"},
    {"timings", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Report exact artifact execution stage timings"},
    {"workers", 'W', XR_CLI_VALUE_INT, false, false, "N", "Number of worker threads"},
    {"coro-watch", 'w', XR_CLI_VALUE_INT, false, false, "MS", "Coroutine watch interval (ms)"},
    {"coro-http", 'H', XR_CLI_VALUE_INT, false, false, "PORT", "Coroutine HTTP monitor port"},
    {"dump-ic", 'I', XR_CLI_VALUE_NONE, false, false, NULL, "Dump inline cache feedback"},
    XR_CLI_XI_OPT_SPEC,
    XR_CLI_OPT_END};

static const XrCliOptionSpec repl_options[] = {
    {"no-color", 'n', XR_CLI_VALUE_NONE, false, false, NULL, "Disable color output"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec test_options[] = {
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Verbose output"},
    {"fail-fast", 'F', XR_CLI_VALUE_NONE, false, false, NULL, "Stop on first failure"},
    {"filter", 'f', XR_CLI_VALUE_STRING, false, false, "PATTERN", "Only run matching tests"},
    {"quiet", 'q', XR_CLI_VALUE_NONE, false, false, NULL, "Quiet mode (exit code only)"},
    {"jobs", 'j', XR_CLI_VALUE_INT, false, false, "N", "Parallel threads (default 1)"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec check_options[] = {
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Show all checked files"},
    {"quiet", 'q', XR_CLI_VALUE_NONE, false, false, NULL, "Show errors only"},
    {"syntax-only", 'S', XR_CLI_VALUE_NONE, false, false, NULL,
     "Skip semantic analysis (parse only)"},
    {"strict", 's', XR_CLI_VALUE_NONE, false, false, NULL,
     "Enable strict analyzer mode (extra checks)"},
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

static const XrCliOptionSpec compile_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"format", 'f', XR_CLI_VALUE_STRING, false, false, "FMT", "Output format: c"},
    {"strip-debug", 's', XR_CLI_VALUE_NONE, false, false, NULL, "Remove debug info"},
    {"strip-source", 'S', XR_CLI_VALUE_NONE, false, false, NULL, "Remove source file path"},
    {"name", 'n', XR_CLI_VALUE_STRING, false, false, "NAME", "C variable name prefix"},
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
     "Tune for CPU via -march (e.g. native); host --native builds only"},
    {"simd", 0, XR_CLI_VALUE_STRING, false, false, "MODE",
     "Portable SIMD lowering: auto, scalar, native, neon, sve, sse2, avx2, avx512, vsx, lsx, or "
     "dispatch"},
    {"sysroot", 'r', XR_CLI_VALUE_STRING, false, false, "DIR", "System root directory"},
    {"strip", 'S', XR_CLI_VALUE_NONE, false, false, NULL, "Strip debug symbols"},
    {"native", 'N', XR_CLI_VALUE_NONE, false, false, NULL, "Use AOT native backend"},
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
    {"verify-arc", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Force the RC/ownership verifier on after every lifetime/CFG-invalidating pass (task 219)"},
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Verbose output"},
    XR_CLI_XI_OPT_SPEC,
    XR_CLI_OPT_END};

static const XrCliOptionSpec deps_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"shell", 's', XR_CLI_VALUE_NONE, false, false, NULL, "Shell script format"},
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "JSON format"},
    {"list", 'l', XR_CLI_VALUE_NONE, false, false, NULL, "Simple list format"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec toolchain_options[] = {
    {"target", 0, XR_CLI_VALUE_STRING, false, false, "TARGET", "AOT target or native"},
    {"provider", 0, XR_CLI_VALUE_STRING, false, false, "SELECTOR",
     "Provider selector: auto, host, clang, gcc, msvc, or zig"},
    {"profile", 0, XR_CLI_VALUE_STRING, false, false, "PROFILE",
     "Capability profile: hosted or freestanding"},
    {"cc", 0, XR_CLI_VALUE_STRING, false, false, "PATH", "Path to system C compiler"},
    {"zig", 0, XR_CLI_VALUE_STRING, false, false, "PATH", "Path to zig executable"},
    {"no-run", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Skip native executable run stage"},
    {"refresh", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Bypass cached probe result"},
    {"keep-probe", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Keep the private probe directory for debugging"},
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "Emit schema-v1 JSON"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec language_options[] = {
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "Emit machine-readable JSON"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec pkg_options[] = {XR_CLI_OPT_END};

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

static const XrCliOptionSpec explain_options[] = {
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "Emit machine-readable JSON"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec verify_options[] = {
    {"contract", 0, XR_CLI_VALUE_STRING, true, false, "FILE",
     "Verify a versioned semantic/backend contract"},
    {"cc", 0, XR_CLI_VALUE_STRING, false, false, "PATH",
     "Explicit native compiler for realized code-shape verification"},
    {"zig", 0, XR_CLI_VALUE_STRING, false, false, "PATH",
     "Explicit Zig executable available as a capability fallback"},
    {"refresh", 0, XR_CLI_VALUE_NONE, false, false, NULL,
     "Bypass cached provider probes for realized verification"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec plan_options[] = {
    {"semantic-plan", 0, XR_CLI_VALUE_STRING, false, false, "FILE",
     "Exact XSM semantic authority required to verify a plan"},
    {"context", 0, XR_CLI_VALUE_INT, false, false, "N",
     "Rows shown around the first difference (default 3)"},
    XR_CLI_OPT_END};

static const XrCliCommandSpec plan_subcommands[] = {
    {"dump", "Render a TargetPlan artifact as deterministic text", NULL, plan_options, 1, 1, false,
     false, NULL, NULL, 0},
    {"verify", "Run the complete TargetPlan verification chain", NULL, plan_options, 1, 1, false,
     false, NULL, NULL, 0},
    {"diff", "Report the first difference between two TargetPlan artifacts", NULL, plan_options, 2,
     2, false, false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

#ifdef XR_HAS_LSP
static const XrCliOptionSpec lsp_options[] = {
    {"stdio", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Use stdio transport (default)"},
    XR_CLI_OPT_END};
#endif

#ifdef XR_HAS_DAP
static const XrCliOptionSpec dap_options[] = {
    {"port", 'p', XR_CLI_VALUE_INT, false, false, "PORT",
     "TCP port (0 for random, omit for stdio)"},
    {"native", 'N', XR_CLI_VALUE_NONE, false, false, NULL,
     "Debug a native AOT binary via the lldb/gdb backend"},
    {"debugger", 0, XR_CLI_VALUE_STRING, false, false, "PATH",
     "Path to lldb-dap for native mode (default: autodetect)"},
    XR_CLI_OPT_END};
#endif

#ifdef XR_HAS_MCP
static const XrCliOptionSpec mcp_options[] = {
    {"log-level", 'l', XR_CLI_VALUE_STRING, false, false, "LEVEL",
     "Log level: error,warn,info,debug"},
    {"log-file", 'f', XR_CLI_VALUE_STRING, false, false, "PATH", "Log to file"},
    {"enable-runner", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Enable xray_run tool"},
    XR_CLI_OPT_END};
#endif

/* ========== pkg Subcommands ========== */

static const XrCliCommandSpec pkg_subcommands[] = {
    {"init", "Initialize new project", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"add", "Add dependency", NULL, empty_options, 1, -1, false, false, NULL, NULL, 0},
    {"remove", "Remove dependency", NULL, empty_options, 1, -1, false, false, NULL, NULL, 0},
    {"install", "Install all dependencies", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"update", "Update dependencies", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"tree", "Show dependency tree", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"login", "Login to registry", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"publish", "Publish package", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

static const XrCliCommandSpec toolchain_subcommands[] = {
    {"list", "List providers, configuration, and cached status", NULL, empty_options, 0, 0, false,
     false, NULL, NULL, 0},
    {"detect", "Discover provider candidates and read their versions", NULL, empty_options, 0, 0,
     false, false, NULL, NULL, 0},
    {"probe", "Run compile, SDK, runtime-link, and native-run capability probes", NULL,
     empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"doctor", "Select a provider and diagnose the first failing capability", NULL, empty_options,
     0, 0, false, false, NULL, NULL, 0},
    {"use", "Persist a user provider preference", NULL, empty_options, 1, 1, false, false, NULL,
     NULL, 0},
    {"reset", "Reset a user preference and related probe cache", NULL, empty_options, 0, 0, false,
     false, NULL, NULL, 0},
    {"config-path", "Print the user toolchain configuration path", NULL, empty_options, 0, 0, false,
     false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

static const XrCliCommandSpec language_subcommands[] = {
    {"attributes", "List the complete public attribute registry", NULL, language_options, 0, 0,
     false, false, NULL, NULL, 0},
    {"conversions", "Inventory analyzer-classified source conversions", NULL, language_options, 1,
     1, false, false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

/* ========== Top-level Command Table ========== */

static XrCliCommandSpec cli_commands[] = {
    /* Execution commands */
    {"run", "Run source, project, or exact target artifacts", NULL, run_options, 0, -1, true, false, NULL, NULL,
     0},
    {"repl", "Interactive environment", NULL, repl_options, 0, 0, false, false, NULL, NULL, 0},
    {"test", "Run tests", NULL, test_options, 0, -1, false, false, NULL, NULL, 0},
    {"check", "Syntax check", NULL, check_options, 0, -1, false, false, NULL, NULL, 0},
    {"fmt", "Format source code", NULL, fmt_options, 0, -1, false, false, NULL, NULL, 0},
    /* Artifact commands */
    {"compile", "Compile to a C bytecode container", NULL, compile_options, 1, 1, false, false, NULL, NULL,
     0},
    {"build", "Compile to binary", NULL, build_options, 1, 1, false, false, NULL, NULL, 0},
    {"deps", "Analyze dependencies", NULL, deps_options, 1, 1, false, false, NULL, NULL, 0},
    {"toolchain", "Inspect AOT toolchains", NULL, toolchain_options, 0, -1, false, false, NULL,
     toolchain_subcommands, 7},
    {"language", "Inspect the public language surface", NULL, language_options, 0, -1, false, false,
     NULL, language_subcommands, 2},
    {"explain", "Explain compiler evidence and native provenance", NULL, explain_options, 1, 2,
     false, false, NULL, NULL, 0},
    {"verify", "Verify semantic and backend contracts", NULL, verify_options, 0, 0, false, false,
     NULL, NULL, 0},
    {"plan", "Inspect, verify, and compare exact TargetPlan artifacts", NULL, plan_options, 1, -1,
     false, false, NULL, plan_subcommands, 3},

    /* Package management (has subcommands) */
    {"pkg", "Package management", NULL, pkg_options, 0, -1, false, false, NULL, pkg_subcommands, 8},

    /* Utility commands */
    {"info", "Environment and installation info", NULL, info_options, 0, 0, false, false, NULL,
     NULL, 0},
    {"doctor", "Diagnose Xray installation state", NULL, info_options, 0, 1, false, false, NULL,
     doctor_subcommands, 1},
    {"self", "Update or uninstall through the active provider", NULL, self_options, 0, 1, false,
     false, NULL, self_subcommands, 2},
    {"builtin-dump", "Dump analyzer builtin metadata", NULL, empty_options, 0, 0, false, true, NULL,
     NULL, 0},
    {"help", "Show help for a command", NULL, empty_options, 0, 1, false, false, NULL, NULL, 0},

/* IDE integration (conditional compilation) */
#ifdef XR_HAS_LSP
    {"lsp", "Start LSP server", NULL, lsp_options, 0, 0, false, false, NULL, NULL, 0},
#endif
#ifdef XR_HAS_DAP
    {"dap", "Start DAP debug server", NULL, dap_options, 0, 0, false, false, NULL, NULL, 0},
#endif
#ifdef XR_HAS_MCP
    {"mcp-server", "Start MCP server", NULL, mcp_options, 0, 0, false, false, NULL, NULL, 0},
#endif

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

/* ========== Session Optimizer Policy ========== */

bool xr_cli_apply_xi_opt(const XrCliInvocation *inv, const char *cmd) {
    const char *spec;
    char err[256];

    XR_DCHECK(inv != NULL, "inv is NULL");
    spec = xr_cli_opt_string(&inv->options, "xi-opt", NULL);
    if (!spec)
        return true;

    err[0] = '\0';
    if (xi_pass_session_policy_apply_spec(spec, "--xi-opt", err, sizeof(err)))
        return true;
    xr_cli_error(cmd, "%s", err);
    return false;
}
