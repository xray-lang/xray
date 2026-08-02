/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_compile.c - Source file to bytecode file compiler helper
 */

#include "xbytecode_io.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"
#include "xmodule.h"
#include "xmodule_graph.h"
#include "xmodule_resolver.h"
#include "xray_vm.h"
#include <stdio.h>

static bool compile_to_file_impl(XrCompilerSession *session, const char *canonical_module,
                                 const char *source_file, const char *output_file, int flags) {
    if (!session) {
        xr_log_warning("compile", "compiler session is required");
        return false;
    }
    XrVMRuntime *X = xr_compiler_session_vm_host(session);
    if (!X) {
        xr_log_warning("compile", "compiler session has no VM host");
        return false;
    }

    // Read source file
    char *source = xr_file_read_all(source_file, "r", NULL);
    if (!source) {
        xr_log_warning("compile", "cannot open: %s", source_file);
        return false;
    }

    XrProto *proto = xr_compile_source_with_path(session, source, source_file);
    xr_free(source);

    if (!proto) {
        xr_log_warning("compile", "compilation failed: %s", source_file);
        return false;
    }

    // Serialize
    size_t bc_size;
    XrBcError bc_error = XR_BC_OK;
    uint8_t *bc = canonical_module
                      ? xr_bytecode_write_stdlib(X, canonical_module, proto, flags, &bc_size,
                                                 &bc_error)
                      : xr_bytecode_write(X, proto, flags, &bc_size, &bc_error);
    if (!bc) {
        xr_vm_proto_free(proto);
        xr_log_warning("compile", "bytecode serialization failed: %s",
                       xr_bytecode_error_string(bc_error));
        return false;
    }

    xr_vm_proto_free(proto);

    // Write to file
    FILE *f = fopen(output_file, "wb");
    if (!f) {
        xr_free(bc);
        xr_log_warning("compile", "cannot create: %s", output_file);
        return false;
    }

    fwrite(bc, 1, bc_size, f);
    fclose(f);
    xr_free(bc);

    return true;
}

bool xr_compile_to_file(XrCompilerSession *session, const char *source_file,
                        const char *output_file, int flags) {
    return compile_to_file_impl(session, NULL, source_file, output_file, flags);
}

bool xr_compile_stdlib_to_file(XrCompilerSession *session, const char *canonical_module,
                               const char *source_file, const char *output_file, int flags) {
    if (!session || !canonical_module || !canonical_module[0])
        return false;
    XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_STDLIB,
        .canonical_module = canonical_module,
    };
    xr_compiler_session_set_compile_unit_identity(session, &identity);

    /* Build a dependency module graph so this stdlib module's imports of other
     * modules' declarations resolve to real symbols — including constructable
     * classes (class_info + constructor), not just function signatures. Without
     * it a script layer like io.xr cannot construct an imported `path.Path`
     * (the import degrades to an unknown type). Best-effort: on any graph
     * failure we fall back to graph-less compilation, preserving the historical
     * behavior for modules that need no cross-module class metadata. */
    XrVMRuntime *X = xr_compiler_session_vm_host(session);
    XrModuleGraph *graph = NULL;
    XaAnalyzer *graph_analyzer = NULL;
    if (X) {
        xr_module_system_init_with_script(X, source_file);
        XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
        XrModuleResolver *resolver = registry ? xr_module_registry_get_resolver(registry) : NULL;
        char *graph_err = NULL;
        int build_rc = (resolver && (graph = xr_module_graph_new(session, resolver)) != NULL)
                           ? xr_module_graph_build(graph, source_file, &graph_err)
                           : -999;
        if (resolver && graph && build_rc == 0 && xr_module_graph_topological_sort(graph) == 0 &&
            !graph->has_cycle) {
            /* Analyze every dependency (all but the entry) so their exported
             * symbols are populated before the entry unit is compiled. A
             * dependency's own diagnostics are irrelevant here (each is validated
             * when compiled in its own right), so they are cleared. */
            if (graph->topo_count > 1) {
                graph_analyzer = xa_analyzer_new(session);
                if (graph_analyzer) {
                    xa_analyzer_set_graph(graph_analyzer, graph);
                    for (int ti = 0; ti < graph->topo_count; ti++) {
                        int index = graph->topo_order[ti];
                        if (index == graph->entry_index)
                            continue;
                        XrModuleSpec *spec = &graph->specs[index];
                        if (spec->ast && spec->source_path) {
                            xa_analyzer_analyze(graph_analyzer, spec->source_path,
                                                (XrAstNode *) spec->ast);
                            spec->export_symbols = xa_analyzer_collect_export_symbols(
                                graph_analyzer, (XrAstNode *) spec->ast);
                        }
                        xa_analyzer_clear_diagnostics(graph_analyzer);
                    }
                }
            }
            xr_compiler_session_set_module_graph(session, graph);
        } else {
            xr_free(graph_err);
            if (graph) {
                xr_module_graph_free(graph);
                graph = NULL;
            }
        }
    }

    bool ok =
        compile_to_file_impl(session, canonical_module, source_file, output_file, flags);

    xr_compiler_session_set_module_graph(session, NULL);
    if (graph_analyzer) {
        xa_analyzer_set_graph(graph_analyzer, NULL);
        xa_analyzer_free(graph_analyzer);
    }
    if (graph)
        xr_module_graph_free(graph);
    xr_compiler_session_set_compile_unit_identity(session, NULL);
    return ok;
}
