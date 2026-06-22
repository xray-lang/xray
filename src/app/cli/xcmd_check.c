/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_check.c - 'xray check' command for syntax checking
 *
 * KEY CONCEPT:
 *   Checks source files for syntax errors without executing code.
 *   Designed for IDE integration and CI/CD pipelines.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "../../api/xisolate_profile.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../runtime/xisolate_api.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_dir.h"
#include <stdio.h>
#include <string.h>
#include "../../os/os_fs.h"

// Check single file, returns: 0 = no error, 1 = has error
static int check_file(XrayIsolate *X, XaAnalyzer *analyzer, const char *path, int verbose) {
    char *source = xr_cli_read_file(path);
    if (!source) {
        fprintf(stderr, "Error: cannot read file '%s'\n", path);
        return 1;
    }

    // Parse source code - NULL result means syntax error
    AstNode *ast = xr_parse_with_source(xr_compiler_session_current_for_isolate(X), source, path);

    int has_error = (ast == NULL);
    if (has_error) {
        // Error message already printed by parser
    } else if (analyzer) {
        // Run semantic analysis (default for `xray check`)
        xa_analyzer_analyze(analyzer, path, (XrAstNode *) ast);
        int diag_count = 0;
        XaDiagnostic *diags = xa_analyzer_get_diagnostics(analyzer, &diag_count);
        for (XaDiagnostic *d = diags; d; d = d->next) {
            if (d->severity == XR_DIAG_SEV_ERROR) {
                has_error = 1;
            }
            const char *sev = "error";
            if (d->severity == XR_DIAG_SEV_WARNING)
                sev = "warning";
            else if (d->severity == XR_DIAG_SEV_INFO)
                sev = "info";
            else if (d->severity == XR_DIAG_SEV_HINT)
                sev = "hint";
            fprintf(stderr, "%s:%d:%d: %s: %s\n", path, d->location.line, d->location.column, sev,
                    d->message);
        }
        xa_analyzer_clear_diagnostics(analyzer);
        if (!has_error && verbose) {
            printf("ok %s\n", path);
        }
    } else if (verbose) {
        printf("ok %s\n", path);
    }

    // Release resources
    if (ast) {
        xr_program_destroy(ast);
    }
    xr_free(source);

    return has_error;
}

// Recursively check directory, returns: error file count
static int check_directory(XrayIsolate *X, XaAnalyzer *analyzer, const char *path, int verbose,
                           int *total, int *passed) {
    XrDirIter *it = xr_dir_open(path);
    if (!it) {
        fprintf(stderr, "Error: cannot open directory '%s'\n", path);
        return 1;
    }

    int errors = 0;
    char filepath[1024];
    XrDirEntry e;

    while (xr_dir_next(it, &e)) {
        snprintf(filepath, sizeof(filepath), "%s/%s", path, e.name);

        if (e.is_dir) {
            // Recursively check subdirectory
            errors += check_directory(X, analyzer, filepath, verbose, total, passed);
        } else if (xr_cli_is_xr_file(e.name)) {
            // Check .xr file
            (*total)++;
            if (check_file(X, analyzer, filepath, verbose) == 0) {
                (*passed)++;
            } else {
                errors++;
            }
        }
    }

    xr_dir_close(it);
    return errors;
}

/* Graph-based check: build module graph from entry file, then analyze
 * all reachable modules in topological order.  Returns error count. */
static int check_with_graph(XrayIsolate *X, XaAnalyzer *analyzer, const char *entry_path,
                            int verbose) {
    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    if (!resolver) {
        fprintf(stderr, "Error: cannot create module resolver\n");
        return 1;
    }

    XrModuleGraph *graph =
        xr_module_graph_new(xr_compiler_session_current_for_isolate(X), resolver);
    if (!graph) {
        fprintf(stderr, "Error: cannot create module graph\n");
        return 1;
    }

    char *err = NULL;
    int rc = xr_module_graph_build(graph, entry_path, &err);
    if (rc != 0) {
        fprintf(stderr, "Error: %s\n", err ? err : "graph build failed");
        xr_free(err);
        xr_module_graph_free(graph);
        return 1;
    }

    rc = xr_module_graph_topological_sort(graph);
    if (graph->has_cycle) {
        fprintf(stderr, "Error: %s\n",
                graph->cycle_desc ? graph->cycle_desc : "circular dependency detected");
        xr_module_graph_free(graph);
        return 1;
    }

    int errors = 0;
    int total = graph->topo_count;

    /* Set graph on analyzer for cross-module type resolution */
    if (analyzer)
        xa_analyzer_set_graph(analyzer, graph);

    /* Analyze each module in topological order (leaves first) */
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path)
            continue;

        if (analyzer) {
            xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);

            /* Collect exports so downstream modules can resolve import types */
            spec->exports = xa_analyzer_collect_exports(analyzer, (XrAstNode *) spec->ast);

            int file_errs = 0;
            int diag_count = 0;
            XaDiagnostic *diags = xa_analyzer_get_diagnostics(analyzer, &diag_count);
            for (XaDiagnostic *d = diags; d; d = d->next) {
                if (d->severity == XR_DIAG_SEV_ERROR)
                    file_errs++;
                const char *sev = "error";
                if (d->severity == XR_DIAG_SEV_WARNING)
                    sev = "warning";
                else if (d->severity == XR_DIAG_SEV_INFO)
                    sev = "info";
                else if (d->severity == XR_DIAG_SEV_HINT)
                    sev = "hint";
                fprintf(stderr, "%s:%d:%d: %s: %s\n", spec->source_path, d->location.line,
                        d->location.column, sev, d->message);
            }
            xa_analyzer_clear_diagnostics(analyzer);
            errors += file_errs;
            if (file_errs == 0) {
                spec->status = XR_MODSPEC_ANALYZED;
                if (verbose)
                    printf("ok %s\n", spec->source_path);
            }
        } else {
            /* Parse-only mode: AST already parsed during graph build */
            if (verbose)
                printf("ok %s\n", spec->source_path);
        }
    }

    if (analyzer)
        xa_analyzer_set_graph(analyzer, NULL);

    if (verbose && total > 1)
        printf("\nChecked %d modules in dependency order\n", total);

    xr_module_graph_free(graph);
    return errors;
}

XR_FUNC int cmd_check(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    bool verbose = xr_cli_opt_bool(&inv->options, "verbose");
    bool quiet = xr_cli_opt_bool(&inv->options, "quiet");
    bool syntax_only = xr_cli_opt_bool(&inv->options, "syntax-only");
    bool strict = xr_cli_opt_bool(&inv->options, "strict");

    /* Create shared isolate for all checks */
    XrayIsolate *X = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    if (!X) {
        xr_cli_error("check", "failed to create isolate");
        return XR_CLI_EXIT_INTERNAL;
    }

    /* Default: run the analyzer so semantic errors (e.g. throw on a
     * non-Exception value, type mismatches) are caught alongside syntax
     * errors. --syntax-only opts out for fast pre-flight checks; --strict
     * additionally enables stricter analyzer modes. */
    XaAnalyzer *analyzer = NULL;
    if (!syntax_only) {
        analyzer = xa_analyzer_new(X);
        if (strict)
            xa_analyzer_set_strict_mode(analyzer, true);
    }

    int total_files = 0;
    int passed_files = 0;
    int total_errors = 0;

    /* No positionals -> check current directory */
    if (inv->positional_count == 0) {
        if (xr_fs_is_dir(".")) {
            total_errors = check_directory(X, analyzer, ".", verbose, &total_files, &passed_files);
        }
    } else {
        /* Check specified files or directories */
        for (int i = 0; i < inv->positional_count; i++) {
            const char *path = inv->positionals[i];
            XrFsStat st;

            if (xr_fs_stat(path, &st) != 0) {
                xr_cli_error("check", "path does not exist '%s'", path);
                total_errors++;
                continue;
            }

            if (st.kind == XR_FS_DIR) {
                total_errors +=
                    check_directory(X, analyzer, path, verbose, &total_files, &passed_files);
            } else if (st.kind == XR_FS_FILE) {
                /* Single .xr file: use graph to discover + check all deps */
                int errs = check_with_graph(X, analyzer, path, verbose);
                total_files++;
                if (errs == 0)
                    passed_files++;
                else
                    total_errors += errs;
            }
        }
    }

    /* Output statistics */
    if (!quiet && total_files > 1) {
        printf("\n");
        if (total_errors == 0) {
            printf("OK: %d files checked, no errors\n", total_files);
        } else {
            printf("FAIL: %d files checked, %d errors\n", total_files, total_errors);
        }
    }

    if (analyzer)
        xa_analyzer_free(analyzer);
    xray_isolate_delete(X);
    return total_errors > 0 ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
