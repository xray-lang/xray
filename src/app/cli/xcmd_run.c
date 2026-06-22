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
 *   Options parsed via unified XrCliInvocation. Supports -- separator.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "../../api/xisolate_profile.h"

#include "xray.h"
#include "xray_isolate.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../runtime/xisolate_api.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// Run options collected from CLI flags
typedef struct {
    bool trace;
    bool dump_bytecode;
    bool dump_ic;
    int num_workers;          // 0 = auto-detect
    int coro_watch_interval;  // 0 = disabled, >0 = refresh interval(ms)
    int coro_http_port;       // 0 = disabled, >0 = HTTP port
} RunOptions;

/* Create isolate via profile factory, then apply run-specific overrides */
static XrayIsolate *create_run_isolate(const RunOptions *opts) {
    XrayIsolateParams params;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &params);
    params.trace_execution = opts->trace;
    params.dump_bytecode = opts->dump_bytecode;
    params.dump_ic_feedback = opts->dump_ic;

    XrayIsolate *iso = xr_isolate_profile_create(&params);
    if (!iso)
        return NULL;
    xr_multicore_init(iso, opts->num_workers);
    return iso;
}

// Execute code string and cleanup isolate
static int run_string(const RunOptions *opts, const char *code) {
    XrayIsolate *iso = create_run_isolate(opts);
    if (!iso)
        return 1;

    int result = xray_isolate_dostring(iso, code);
    xr_multicore_destroy(iso);
    xray_isolate_delete(iso);
    return (result != 0) ? 1 : 0;
}

/* Build RunOptions from parsed invocation */
static void fill_run_options(RunOptions *opts, const XrCliInvocation *inv) {
    XR_DCHECK(opts != NULL, "opts is NULL");
    XR_DCHECK(inv != NULL, "inv is NULL");

    opts->trace = xr_cli_opt_bool(&inv->options, "trace");
    opts->dump_bytecode = xr_cli_opt_bool(&inv->options, "dump-bytecode");
    opts->dump_ic = xr_cli_opt_bool(&inv->options, "dump-ic");
    opts->num_workers = xr_cli_opt_int(&inv->options, "workers", 0);
    opts->coro_watch_interval = xr_cli_opt_int(&inv->options, "coro-watch", 0);
    opts->coro_http_port = xr_cli_opt_int(&inv->options, "coro-http", 0);
}

XR_FUNC int cmd_run(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    RunOptions opts = {0};
    fill_run_options(&opts, inv);

    /* Need a script file */
    if (inv->positional_count < 1) {
        xr_cli_error("run", "no input file specified");
        return XR_CLI_EXIT_USAGE;
    }

    const char *file = inv->positionals[0];

    /* "-" as filename: read script from stdin */
    if (strcmp(file, "-") == 0) {
        char *stdin_code = xr_cli_read_stdin();
        if (stdin_code == NULL || stdin_code[0] == '\0') {
            xr_cli_error("run", "no input from stdin");
            xr_free(stdin_code);
            return XR_CLI_EXIT_FAIL;
        }
        int result = run_string(&opts, stdin_code);
        xr_free(stdin_code);
        return (result != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
    }

    /* Script arguments: passthrough args after -- */
    int script_argc = inv->passthrough_argc;
    char **script_argv = inv->passthrough_argv;

    /* Create isolate with runtime */
    XrayIsolate *iso = create_run_isolate(&opts);
    if (!iso)
        return XR_CLI_EXIT_INTERNAL;

    /* Set script info (for args/__file__/__dir__) */
    xray_isolate_set_script_info(iso, file, script_argc, script_argv);

    /* Re-initialize module system (with script path) */
    xr_module_system_init_with_script(iso, file);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);
    // Graph exports point at analyzer-owned XrType/XrClassInfo objects.
    XrModuleGraph *active_graph = NULL;
    XaAnalyzer *active_graph_analyzer = NULL;

    /* Pre-flight: build module graph, detect cycles, analyze cross-module types */
    {
        XrModuleRegistry *registry = xr_isolate_get_module_registry(iso);
        XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
        if (resolver) {
            XrModuleGraph *graph =
                xr_module_graph_new(xr_compiler_session_current_for_isolate(iso), resolver);
            if (graph) {
                char *err = NULL;
                if (xr_module_graph_build(graph, file, &err) == 0) {
                    xr_module_graph_topological_sort(graph);
                    if (graph->has_cycle) {
                        fprintf(stderr, "Error: %s\n",
                                graph->cycle_desc ? graph->cycle_desc
                                                  : "circular dependency detected");
                        xr_module_graph_free(graph);
                        xr_multicore_destroy(iso);
                        xray_isolate_delete(iso);
                        return XR_CLI_EXIT_FAIL;
                    }

                    /* Cross-module type analysis (multi-module projects only).
                     * Analyze all modules in topo order so import types resolve
                     * to concrete signatures.  Errors are fatal: the VM must
                     * not run a program that the graph-aware type system
                     * rejected. */
                    if (graph->topo_count > 1) {
                        XaAnalyzer *analyzer = xa_analyzer_new(iso);
                        int graph_errors = 0;
                        if (!analyzer) {
                            fprintf(stderr, "Error: cannot create analyzer for module graph\n");
                            xr_module_graph_free(graph);
                            xr_multicore_destroy(iso);
                            xray_isolate_delete(iso);
                            return XR_CLI_EXIT_FAIL;
                        }
                        if (analyzer) {
                            xa_analyzer_set_graph(analyzer, graph);
                            for (int ti = 0; ti < graph->topo_count; ti++) {
                                int idx = graph->topo_order[ti];
                                XrModuleSpec *spec = &graph->specs[idx];
                                if (!spec->ast || !spec->source_path)
                                    continue;
                                xa_analyzer_analyze(analyzer, spec->source_path,
                                                    (XrAstNode *) spec->ast);
                                spec->exports =
                                    xa_analyzer_collect_exports(analyzer, (XrAstNode *) spec->ast);

                                int diag_count = 0;
                                XaDiagnostic *diags =
                                    xa_analyzer_get_diagnostics(analyzer, &diag_count);
                                for (XaDiagnostic *d = diags; d; d = d->next) {
                                    if (d->severity == XR_DIAG_SEV_ERROR) {
                                        graph_errors++;
                                        fprintf(stderr, "%s:%d:%d: error: %s\n", spec->source_path,
                                                d->location.line, d->location.column, d->message);
                                    }
                                }
                                xa_analyzer_clear_diagnostics(analyzer);
                            }
                            if (graph_errors > 0) {
                                xa_analyzer_set_graph(analyzer, NULL);
                                xa_analyzer_free(analyzer);
                                xr_module_graph_free(graph);
                                xr_multicore_destroy(iso);
                                xray_isolate_delete(iso);
                                return XR_CLI_EXIT_FAIL;
                            }
                            active_graph_analyzer = analyzer;
                        }

                        xr_compiler_session_set_module_graph(session, graph);
                        active_graph = graph;

                        /* Pre-load dependency modules in topo order.
                         * Ensures correct initialization order and populates
                         * module_table for indexed access (OP_LOAD_MODULE_SLOT).
                         * The entry module is skipped — it runs via dofile. */
                        int nmod = graph->topo_count;
                        XrModule **mod_table =
                            (XrModule **) xr_calloc((size_t) nmod, sizeof(XrModule *));
                        if (mod_table) {
                            for (int ti = 0; ti < nmod; ti++) {
                                int idx = graph->topo_order[ti];
                                if (idx == graph->entry_index)
                                    continue; /* entry runs via dofile */
                                XrModuleSpec *spec = &graph->specs[idx];
                                if (!spec->source_path)
                                    continue;
                                XrValue val = xr_module_import(iso, spec->source_path);
                                if (!XR_IS_NULL(val))
                                    mod_table[ti] = xr_value_to_module(val);
                            }
                            registry->module_table = mod_table;
                            registry->module_table_count = nmod;
                        }
                        graph = NULL;
                    }
                }
                xr_free(err);
                if (graph)
                    xr_module_graph_free(graph);
            }
        }
    }

    /* Start coroutine monitor (if enabled) */
    if (opts.coro_watch_interval > 0 || opts.coro_http_port > 0) {
        xr_coro_monitor_start(iso, opts.coro_watch_interval, opts.coro_http_port);
    }

    /* Execute file */
    int result = xray_isolate_dofile(iso, file);

    xr_compiler_session_set_module_graph(session, NULL);
    if (active_graph_analyzer) {
        xa_analyzer_set_graph(active_graph_analyzer, NULL);
        xa_analyzer_free(active_graph_analyzer);
    }
    if (active_graph)
        xr_module_graph_free(active_graph);

    xr_multicore_destroy(iso);
    xray_isolate_delete(iso);

    return (result != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
