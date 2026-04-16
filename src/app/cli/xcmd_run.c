/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_run.c - 'xray run' command implementation
 *
 * KEY CONCEPT:
 *   Runs Xray source files using the backend selected at compile time.
 *   Uses getopt_long for argument parsing, supports -- separator.
 */

#include "xcli.h"
#include "xcli_utils.h"

#include "xray.h"
#include "xray_isolate.h"
#include "../../module/xmodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <limits.h> // PATH_MAX
#include "../../base/xmalloc.h"

// Long options definition
static struct option run_long_options[] = {
    {"help",          no_argument,       0, 'h'},
    {"eval",          required_argument, 0, 'e'},
    {"trace",         no_argument,       0, 't'},
    {"dump-bytecode", no_argument,       0, 'd'},
    {"workers",       required_argument, 0, 'W'},
    {"coro-watch",    optional_argument, 0, 'w'},
    {"coro-http",     required_argument, 0, 'H'},
    {"no-jit",        no_argument,       0, 'J'},
    {"jit-force",     no_argument,       0, 'F'},
    {"dump-ic",       no_argument,       0, 'I'},
    {0, 0, 0, 0}
};

// Run options collected from CLI flags
typedef struct {
    bool trace;
    bool dump_bytecode;
    bool dump_ic;
    bool jitless;
    bool jit_force;
    int num_workers;           // 0 = auto-detect
    int coro_watch_interval;   // 0 = disabled, >0 = refresh interval(ms)
    int coro_http_port;        // 0 = disabled, >0 = HTTP port
} RunOptions;

// Create isolate with runtime for execution
static XrayIsolate *create_run_isolate(const RunOptions *opts) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    params.trace_execution = opts->trace;
    params.dump_bytecode = opts->dump_bytecode;
    params.dump_ic_feedback = opts->dump_ic;
    params.enable_jit = !opts->jitless;
    if (opts->jit_force) params.jit_threshold = 1;
    
    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso) {
        fprintf(stderr, "Error: failed to create Xray isolate\n");
        return NULL;
    }
    xr_multicore_init(iso, opts->num_workers);
    return iso;
}

// Execute code string and cleanup isolate
static int run_string(const RunOptions *opts, const char *code) {
    XrayIsolate *iso = create_run_isolate(opts);
    if (!iso) return 1;
    
    int result = xray_isolate_dostring(iso, code);
    xr_multicore_destroy(iso);
    xray_isolate_delete(iso);
    return (result != 0) ? 1 : 0;
}

void print_run_help(void) {
    printf("Usage: xray [options] <file.xr> [-- script args...]\n");
    printf("       xray -e '<code>'\n");
    printf("       echo '<code>' | xray -e -\n");
    printf("       cat file.xr | xray -\n");
    printf("\n");
    printf("Run Xray program\n");
    printf("\n");
    printf("Options:\n");
    printf("    -e, --eval <code> Execute code string directly\n");
    printf("                      Use '-' to read from stdin\n");
    printf("    -t, --trace       Trace bytecode execution (debug)\n");
    printf("    -d, --dump-bytecode Dump compiled bytecode\n");
    printf("    --dump-ic         Dump IC type feedback after execution\n");
    printf("    --no-jit          Disable JIT compiler (interpreter only)\n");
    printf("    --jit-force       Force JIT on first call (threshold=1, debug only)\n");
    printf("    -h, --help        Show this help\n");
    printf("\n");
    printf("Coroutine monitoring options:\n");
    printf("    -W, --workers <n> Set number of worker threads (0=auto, default)\n");
    printf("    --coro-watch[=ms] Real-time coroutine status (default 1000ms refresh)\n");
    printf("    --coro-http=port  Start coroutine monitoring HTTP server\n");
    printf("\n");
    printf("Script arguments:\n");
    printf("    Use -- to separate xray options from script arguments\n");
    printf("    Access via args array in script, e.g. args[0]\n");
    printf("\n");
    printf("Global variables:\n");
    printf("    args      Array<string>  Script arguments array\n");
    printf("    __file__  string         Script absolute path\n");
    printf("    __dir__   string         Script directory\n");
    printf("\n");
    printf("Examples:\n");
    printf("    xray app.xr\n");
    printf("    xray app.xr -- arg1 arg2\n");
    printf("    xray -e 'print(1 + 2)'\n");
    printf("    echo 'print(1+2)' | xray -e -    # Read from stdin\n");
    printf("    cat app.xr | xray -              # Pipe script\n");
    printf("    xray --trace app.xr\n");
    printf("    xray --coro-watch app.xr         # Real-time monitoring\n");
    printf("    xray --coro-watch=500 app.xr     # 500ms refresh interval\n");
    printf("    xray --coro-http=9090 app.xr     # HTTP monitoring server\n");
    printf("\n");
}

int cmd_run(int argc, char **argv) {
    RunOptions opts = {0};
    const char *eval_code = NULL;
    
    // Reset getopt state (allow multiple calls)
    optind = 1;
    
    // Parse options
    int opt;
    while ((opt = getopt_long(argc, argv, "+e:tdW:w::H:hJFI", run_long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_run_help();
                return 0;
            case 'e':
                eval_code = optarg;
                break;
            case 't':
                opts.trace = true;
                break;
            case 'd':
                opts.dump_bytecode = true;
                break;
            case 'W': // --workers
                if (!cli_parse_int(optarg, &opts.num_workers) || opts.num_workers < 0) {
                    fprintf(stderr, "Error: --workers requires non-negative integer\n");
                    return 1;
                }
                break;
            case 'w': // --coro-watch
                if (optarg) {
                    if (!cli_parse_int(optarg, &opts.coro_watch_interval) || opts.coro_watch_interval <= 0) {
                        fprintf(stderr, "Error: --coro-watch requires positive interval in ms\n");
                        return 1;
                    }
                } else {
                    opts.coro_watch_interval = 1000;
                }
                break;
            case 'J': // --no-jit
                opts.jitless = true;
                break;
            case 'F': // --jit-force
                opts.jit_force = true;
                break;
            case 'I': // --dump-ic
                opts.dump_ic = true;
                break;
            case 'H': // --coro-http
                if (!cli_parse_port(optarg, &opts.coro_http_port) || opts.coro_http_port == 0) {
                    fprintf(stderr, "Error: --coro-http requires valid port number (1-65535)\n");
                    return 1;
                }
                break;
            default:
                fprintf(stderr, "Run 'xray --help' for usage\n");
                return 1;
        }
    }
    
    // -e mode: execute code directly
    if (eval_code != NULL) {
        char *stdin_code = NULL;
        
        // -e - : read code from stdin
        if (strcmp(eval_code, "-") == 0) {
            stdin_code = cli_read_stdin();
            if (stdin_code == NULL || stdin_code[0] == '\0') {
                fprintf(stderr, "Error: no input from stdin\n");
                xr_free(stdin_code);
                return 1;
            }
            eval_code = stdin_code;
        }
        
        int result = run_string(&opts, eval_code);
        xr_free(stdin_code);
        return result;
    }
    
    // After optind are non-option args: script file and script arguments
    if (optind >= argc) {
        fprintf(stderr, "Error: no input file specified\n");
        fprintf(stderr, "Usage: xray <file.xr> [-- script args...]\n");
        return 1;
    }
    
    const char *file = argv[optind];
    
    // "-" as filename: read script from stdin
    if (strcmp(file, "-") == 0) {
        char *stdin_code = cli_read_stdin();
        if (stdin_code == NULL || stdin_code[0] == '\0') {
            fprintf(stderr, "Error: no input from stdin\n");
            xr_free(stdin_code);
            return 1;
        }
        int result = run_string(&opts, stdin_code);
        xr_free(stdin_code);
        return result;
    }
    
    // Script arguments: all arguments after optind+1, skip '--' separator
    int script_argc = argc - optind - 1;
    char **script_argv = (script_argc > 0) ? &argv[optind + 1] : NULL;
    if (script_argc > 0 && script_argv && strcmp(script_argv[0], "--") == 0) {
        script_argc--;
        script_argv = (script_argc > 0) ? script_argv + 1 : NULL;
    }
    
    // Create Isolate with runtime
    XrayIsolate *iso = create_run_isolate(&opts);
    if (!iso) return 1;
    
    // Set script info (for args/__file__/__dir__)
    xray_isolate_set_script_info(iso, file, script_argc, script_argv);
    
    // Re-initialize module system (with script path, for project mode detection)
    xr_module_system_init_with_script(iso, file);
    
    // Start coroutine monitor (if enabled)
    if (opts.coro_watch_interval > 0 || opts.coro_http_port > 0) {
        xr_coro_monitor_start(iso, opts.coro_watch_interval, opts.coro_http_port);
    }
    
    // Execute file
    int result = xray_isolate_dofile(iso, file);
    
    // Destroy runtime
    xr_multicore_destroy(iso);
    
    // Cleanup
    xray_isolate_delete(iso);
    
    return (result != 0) ? 1 : 0;
}
