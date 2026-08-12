/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_compile.c - 'xray compile' command implementation
 *
 * KEY CONCEPT:
 *   Compiles source files to bytecode (.xrc) or C source (.c/.h).
 *   Output format is auto-detected from extension or specified via --format.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "../../api/xisolate_profile.h"
#include "xray.h"
#include "xray_vm.h"
#include "../../runtime/xisolate_api.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../module/xbytecode_io.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../runtime/value/xchunk.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

// Generate C variable name from filename
static char *generate_var_name(const char *filename) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    // Remove extension
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t) (dot - base) : strlen(base);

    char *name = xr_malloc(len + 8);
    if (!name)
        return NULL;

    strcpy(name, "xr_bc_");
    size_t j = 6;

    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            name[j++] = c;
        } else {
            name[j++] = '_';
        }
    }
    name[j] = '\0';
    return name;
}

// Parse --format argument
static XrOutputFormat parse_format(const char *fmt) {
    if (strcmp(fmt, "bytecode") == 0 || strcmp(fmt, "bc") == 0) {
        return XR_OUTPUT_BYTECODE;
    } else if (strcmp(fmt, "c") == 0 || strcmp(fmt, "source") == 0) {
        return XR_OUTPUT_C_SOURCE;
    } else if (strcmp(fmt, "h") == 0 || strcmp(fmt, "header") == 0) {
        return XR_OUTPUT_C_HEADER;
    }
    return XR_OUTPUT_AUTO;
}

static bool prepare_compile_graph(XrVMRuntime *X, XrCompilerSession *session,
                                  const char *input_file, XrModuleGraph **out_graph,
                                  XaAnalyzer **out_analyzer) {
    *out_graph = NULL;
    *out_analyzer = NULL;
    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolver *resolver = registry ? xr_module_registry_get_resolver(registry) : NULL;
    if (!resolver) {
        xr_cli_error("compile", "cannot create module resolver");
        return false;
    }

    XrModuleGraph *graph = xr_module_graph_new(session, resolver);
    if (!graph) {
        xr_cli_error("compile", "cannot create module graph");
        return false;
    }
    char *graph_error = NULL;
    if (xr_module_graph_build(graph, input_file, &graph_error) != 0) {
        xr_cli_error("compile", "%s", graph_error ? graph_error : "module graph build failed");
        xr_free(graph_error);
        xr_module_graph_free(graph);
        return false;
    }
    xr_free(graph_error);
    if (xr_module_graph_topological_sort(graph) != 0 || graph->has_cycle) {
        xr_cli_error("compile", "%s",
                     graph->cycle_desc ? graph->cycle_desc : "circular dependency detected");
        xr_module_graph_free(graph);
        return false;
    }

    if (graph->topo_count > 1) {
        XaAnalyzer *analyzer = xa_analyzer_new(session);
        if (!analyzer) {
            xr_cli_error("compile", "cannot create analyzer for module graph");
            xr_module_graph_free(graph);
            return false;
        }
        xa_analyzer_set_graph(analyzer, graph);
        int error_count = 0;
        for (int ti = 0; ti < graph->topo_count; ti++) {
            int index = graph->topo_order[ti];
            if (index == graph->entry_index)
                continue;
            XrModuleSpec *spec = &graph->specs[index];
            if (!spec->ast || !spec->source_path)
                continue;
            xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
            spec->export_symbols =
                xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);
            int diagnostic_count = 0;
            XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count);
            for (XaDiagnostic *diag = diagnostics; diag; diag = diag->next) {
                if (diag->severity != XR_DIAG_SEV_ERROR)
                    continue;
                error_count++;
                fprintf(stderr, "%s:%d:%d: error: %s\n", spec->source_path, diag->location.line,
                        diag->location.column, diag->message);
            }
            xa_analyzer_clear_diagnostics(analyzer);
        }
        if (error_count > 0) {
            xr_cli_error("compile", "module graph analysis failed with %d error%s", error_count,
                         error_count == 1 ? "" : "s");
            xa_analyzer_set_graph(analyzer, NULL);
            xa_analyzer_free(analyzer);
            xr_module_graph_free(graph);
            return false;
        }
        *out_analyzer = analyzer;
    }

    xr_compiler_session_set_module_graph(session, graph);
    *out_graph = graph;
    return true;
}

XR_FUNC int cmd_compile(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "compile expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);
    const char *var_name = xr_cli_opt_string(&inv->options, "name", NULL);
    const char *fmt_str = xr_cli_opt_string(&inv->options, "format", NULL);

    int flags = 0;
    if (xr_cli_opt_present(&inv->options, "strip-debug"))
        flags |= XR_BC_STRIP_DEBUG;
    if (xr_cli_opt_present(&inv->options, "strip-source"))
        flags |= XR_BC_STRIP_SOURCE;

    /* Parse explicit format */
    XrOutputFormat explicit_format = XR_OUTPUT_AUTO;
    if (fmt_str) {
        explicit_format = parse_format(fmt_str);
        if (explicit_format == XR_OUTPUT_AUTO) {
            xr_cli_error("compile", "unknown format '%s'", fmt_str);
            return XR_CLI_EXIT_USAGE;
        }
    }

    /* Default output file */
    char default_output[512];
    if (!output_file) {
        const char *base = strrchr(input_file, '/');
        base = base ? base + 1 : input_file;
        const char *dot = strrchr(base, '.');
        size_t len = dot ? (size_t) (dot - base) : strlen(base);
        snprintf(default_output, sizeof(default_output), "%.*s.xrc", (int) len, base);
        output_file = default_output;
    }

    /* Determine output format */
    XrOutputFormat format = xr_detect_output_format(output_file, explicit_format);

    /* Resources to clean up */
    int result = XR_CLI_EXIT_FAIL;
    char *gen_var_name = NULL;
    XrVMRuntime *X = NULL;
    char *source = NULL;
    XrProto *proto = NULL;
    XrModuleGraph *graph = NULL;
    XaAnalyzer *graph_analyzer = NULL;
    XrCompilerSession *session = NULL;
    XrCompilerSessionOperationScope operation_scope = {0};

    /* Generate variable name */
    if (!var_name && (format == XR_OUTPUT_C_SOURCE || format == XR_OUTPUT_C_HEADER)) {
        gen_var_name = generate_var_name(input_file);
        var_name = gen_var_name;
    }

    /* Create isolate */
    X = xr_isolate_profile_new(XR_ISOLATE_PROFILE_RUN);
    if (!X) {
        xr_cli_error("compile", "failed to create isolate");
        result = XR_CLI_EXIT_INTERNAL;
        goto cleanup;
    }
    xr_module_system_init_with_script(X, input_file);
    session = xr_compiler_session_current_for_isolate(X);
    if (!xr_compiler_session_operation_begin(session, &operation_scope)) {
        xr_cli_error("compile", "compiler session is busy");
        goto cleanup;
    }
    if (!prepare_compile_graph(X, session, input_file, &graph, &graph_analyzer))
        goto cleanup;

    /* Graph analysis annotates its owned ASTs. Compile from a fresh parse so
     * analyzer-owned links from the preflight cannot leak into codegen. */
    source = xr_cli_read_file(input_file);
    if (!source) {
        xr_cli_error("compile", "cannot open '%s'", input_file);
        goto cleanup;
    }
    proto = xr_compile_source_with_path(session, source, input_file);
    if (!proto) {
        xr_cli_error("compile", "compilation failed");
        goto cleanup;
    }

    /* Output */
    bool success = false;
    bool output_error_reported = false;

    switch (format) {
        case XR_OUTPUT_BYTECODE: {
            size_t bc_size;
            XrBcError bc_error = XR_BC_OK;
            uint8_t *bc = xr_bytecode_write(X, proto, flags, &bc_size, &bc_error);
            if (bc) {
                FILE *out = fopen(output_file, "wb");
                if (out) {
                    bool wrote_all = fwrite(bc, 1, bc_size, out) == bc_size;
                    bool closed = fclose(out) == 0;
                    success = wrote_all && closed;
                    if (success)
                        printf("Compiled: %s (%zu bytes)\n", output_file, bc_size);
                    else {
                        xr_cli_error("compile", "failed to write bytecode output '%s'",
                                     output_file);
                        output_error_reported = true;
                    }
                } else {
                    xr_cli_error("compile", "cannot create bytecode output '%s'", output_file);
                    output_error_reported = true;
                }
                xr_free(bc);
            } else {
                xr_cli_error("compile", "bytecode serialization failed: %s",
                             xr_bytecode_error_string(bc_error));
                output_error_reported = true;
            }
            break;
        }

        case XR_OUTPUT_C_SOURCE:
        case XR_OUTPUT_C_HEADER:
            success = xr_output_c_source(X, proto, output_file, var_name, flags);
            if (success) {
                printf("Compiled: %s\n", output_file);
            }
            break;

        default:
            xr_cli_error("compile", "unknown output format");
            break;
    }

    if (!success && !output_error_reported) {
        xr_cli_error("compile", "cannot write to '%s'", output_file);
    }

    result = success ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;

cleanup:
    if (proto)
        xr_vm_proto_free(proto);
    xr_free(source);
    if (X) {
        xr_compiler_session_set_module_graph(session, NULL);
    }
    if (graph_analyzer) {
        xa_analyzer_set_graph(graph_analyzer, NULL);
        xa_analyzer_free(graph_analyzer);
    }
    if (graph)
        xr_module_graph_free(graph);
    if (operation_scope.active) {
        if (result == XR_CLI_EXIT_OK) {
            if (!xr_compiler_session_operation_succeed(&operation_scope))
                result = XR_CLI_EXIT_INTERNAL;
        } else {
            (void) xr_compiler_session_operation_fail(
                &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        }
    }
    if (X)
        xray_vm_delete(X);
    xr_free(gen_var_name);
    return result;
}
