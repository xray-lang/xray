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
#include "../../module/xbytecode_io.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../module/xproject.h"
#include "../../runtime/xisolate_api.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../plan/format/xr_artifact_kind.h"
#include "../../plan/format/xr_xtp_schema.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static XrArtifactProbeResult classify_file_artifact(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return xr_artifact_probe(path, NULL, 0);
    uint8_t header[XR_ARTIFACT_PROBE_SIZE] = {0};
    size_t header_size = fread(header, 1, sizeof(header), file);
    fclose(file);
    return xr_artifact_probe(path, header, header_size);
}

static bool read_file_bytes(const char *path, uint8_t **bytes, size_t *size) {
    *bytes = NULL;
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end < 0 || (uint64_t) end > XR_XTP_MAX_ARTIFACT_SIZE ||
        (uint64_t) end > XR_XTP_MAX_RUNTIME_LOAD_PEAK_BYTES / 2u ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t length = (size_t) end;
    uint8_t *storage = (uint8_t *) xr_malloc(length ? length : 1);
    if (!storage) {
        fclose(file);
        return false;
    }
    bool complete = length == 0 || fread(storage, 1, length, file) == length;
    fclose(file);
    if (!complete) {
        xr_free(storage);
        return false;
    }
    *bytes = storage;
    *size = length;
    return true;
}

static int reject_non_executable_artifact(const char *path,
                                          XrArtifactProbeResult probe) {
    if (probe.status == XR_ARTIFACT_PROBE_CONFLICT) {
        fprintf(stderr,
                "XR_ARTIFACT_2006: artifact extension conflicts with its canonical magic\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (probe.status == XR_ARTIFACT_PROBE_NEED_MORE) {
        fprintf(stderr,
                "XR_ARTIFACT_2001: artifact header is a truncated reserved prefix\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (probe.status == XR_ARTIFACT_PROBE_UNKNOWN_RESERVED) {
        fprintf(stderr,
                "XR_ARTIFACT_2000: artifact uses an unknown or removed reserved identity\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (probe.kind == XR_ARTIFACT_KIND_XSM) {
        fprintf(stderr,
                "XR_ARTIFACT_2005: semantic module artifacts are planning inputs and are not executable\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (probe.kind != XR_ARTIFACT_KIND_XTP)
        return -1;

    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!read_file_bytes(path, &bytes, &size)) {
        fprintf(stderr,
                "XR_ARTIFACT_2001: TargetPlan artifact cannot be read within its byte budget\n");
        return XR_CLI_EXIT_FAIL;
    }
    XrXtpCandidate *candidate = NULL;
    char detail[512] = {0};
    bool decoded = xr_xtp_decode_candidate(bytes, size, &candidate, detail,
                                           sizeof(detail));
    xr_free(bytes);
    if (!decoded) {
        fprintf(stderr, "%s\n",
                detail[0] ? detail
                          : "XR_ARTIFACT_2000: XTP v12 candidate decoding failed");
        return XR_CLI_EXIT_FAIL;
    }
    xr_xtp_candidate_release(candidate);
    fprintf(stderr,
            "XR_ARTIFACT_2007: XTP materialization requires explicit semantic and target authorities\n");
    return XR_CLI_EXIT_FAIL;
}

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
static XrVMRuntime *create_run_isolate(const RunOptions *opts) {
    XrVMConfig params;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &params);
    params.trace_execution = opts->trace;
    params.dump_bytecode = opts->dump_bytecode;
    params.dump_ic_feedback = opts->dump_ic;
    params.scheduler_workers = opts->num_workers;

    XrVMRuntime *iso = xr_isolate_profile_create(&params);
    if (!iso)
        return NULL;
    return iso;
}

// Execute code string and cleanup isolate
static int run_string(const RunOptions *opts, const char *code) {
    XrVMRuntime *iso = create_run_isolate(opts);
    if (!iso)
        return 1;

    int result = xray_vm_dostring(iso, code);
    xray_vm_multicore_destroy(iso);
    xray_vm_delete(iso);
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

    XrArtifactProbeResult artifact_probe = classify_file_artifact(file);
    XrArtifactKind artifact_kind = artifact_probe.kind;
    int artifact_result =
        reject_non_executable_artifact(file, artifact_probe);
    if (artifact_result >= 0)
        return artifact_result;

    /* Script arguments: passthrough args after -- */
    int script_argc = inv->passthrough_argc;
    char **script_argv = inv->passthrough_argv;

    /* Create isolate with runtime */
    XrVMRuntime *iso = create_run_isolate(&opts);
    if (!iso)
        return XR_CLI_EXIT_INTERNAL;

    /* Set script info (for args/__file__/__dir__) */
    xray_vm_set_script_info(iso, file, script_argc, script_argv);

    /* Bytecode is identified by its stable magic, not by an extension. This
     * keeps renamed artifacts executable and never misclassifies text merely
     * because it happens to end in .xrc. */
    if (artifact_kind == XR_ARTIFACT_KIND_LEGACY_XRC) {
        xr_module_system_init_with_script(iso, file);
        int result = xr_run_bytecode_file(iso, file);
        xray_vm_multicore_destroy(iso);
        xray_vm_delete(iso);
        return result == 0 ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
    }

    /* Re-initialize module system (with script path) */
    xr_module_system_init_with_script(iso, file);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);
    XrProject *project = NULL;
    char project_root[XR_CLI_PATH_MAX];
    if (xr_cli_find_project_root(file, project_root, sizeof(project_root))) {
        project = xr_project_load(NULL, project_root);
        if (project && !project->initialized) {
            fprintf(stderr, "Error: %s\n",
                    project->native_plan && project->native_plan->error
                        ? project->native_plan->error
                        : "invalid xray.toml project configuration");
            xr_project_free(project);
            xray_vm_multicore_destroy(iso);
            xray_vm_delete(iso);
            return XR_CLI_EXIT_FAIL;
        }
        if (project && project->native_plan &&
            project->native_plan->vm_policy == XR_NATIVE_VM_UNSUPPORTED) {
            fprintf(stderr, "Error: native package '%s' does not support the VM backend\n",
                    project->native_plan->name ? project->native_plan->name : "?");
            xr_project_free(project);
            xray_vm_multicore_destroy(iso);
            xray_vm_delete(iso);
            return XR_CLI_EXIT_FAIL;
        }
        xr_compiler_session_set_native_package_plan(session, project ? project->native_plan : NULL);
    }
    // Graph export symbols point at analyzer-owned semantic symbols.
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
                        xr_project_free(project);
                        xray_vm_multicore_destroy(iso);
                        xray_vm_delete(iso);
                        return XR_CLI_EXIT_FAIL;
                    }

                    /* Cross-module type analysis (multi-module projects only).
                     * Analyze all modules in topo order so import types resolve
                     * to concrete signatures.  Errors are fatal: the VM must
                     * not run a program that the graph-aware type system
                     * rejected. */
                    if (graph->topo_count > 1) {
                        XaAnalyzer *analyzer = xa_analyzer_new(session);
                        int graph_errors = 0;
                        if (!analyzer) {
                            fprintf(stderr, "Error: cannot create analyzer for module graph\n");
                            xr_module_graph_free(graph);
                            xr_project_free(project);
                            xray_vm_multicore_destroy(iso);
                            xray_vm_delete(iso);
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
                                spec->export_symbols = xa_analyzer_collect_export_symbols(
                                    analyzer, (XrAstNode *) spec->ast);

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
                                xr_project_free(project);
                                xray_vm_multicore_destroy(iso);
                                xray_vm_delete(iso);
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
                        XrModule **mod_table = NULL;
                        if (!xr_module_graph_preload(iso, graph, &mod_table)) {
                            xr_free(err);
                            xr_compiler_session_set_module_graph(session, NULL);
                            xa_analyzer_set_graph(active_graph_analyzer, NULL);
                            xa_analyzer_free(active_graph_analyzer);
                            active_graph_analyzer = NULL;
                            xr_module_graph_free(active_graph);
                            active_graph = NULL;
                            xr_project_free(project);
                            xray_vm_multicore_destroy(iso);
                            xray_vm_delete(iso);
                            return XR_CLI_EXIT_FAIL;
                        }
                        registry->module_table = mod_table;
                        registry->module_table_count = graph->topo_count;
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
        xray_vm_coro_monitor_start(iso, opts.coro_watch_interval, opts.coro_http_port);
    }

    /* Execute file */
    int result = xray_vm_dofile(iso, file);

    xr_compiler_session_set_module_graph(session, NULL);
    if (active_graph_analyzer) {
        xa_analyzer_set_graph(active_graph_analyzer, NULL);
        xa_analyzer_free(active_graph_analyzer);
    }
    if (active_graph)
        xr_module_graph_free(active_graph);

    xr_compiler_session_set_native_package_plan(session, NULL);
    xr_project_free(project);
    xray_vm_multicore_destroy(iso);
    xray_vm_delete(iso);

    return (result != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
