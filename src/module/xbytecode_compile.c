/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_compile.c - Source file to bytecode file compiler helper
 */

#include "xproto_codec.h"
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
#include <string.h>

static bool stdlib_compile_authority(const char *module_name, const char *source_file,
                                     XrModuleIdentityAuthority *authority, char **root_out,
                                     char **identity_out) {
    *root_out = NULL;
    *identity_out = NULL;
    char *source = xr_realpath(source_file);
    char *module_dir = source ? xr_path_dirname(source) : NULL;
    char *root = module_dir ? xr_path_dirname(module_dir) : NULL;
    char *logical = NULL;
    if (!source || !module_dir || !root) {
        xr_free(source);
        xr_free(module_dir);
        xr_free(root);
        return false;
    }
    *authority = (XrModuleIdentityAuthority) {
        .kind = XR_MODULE_IDENTITY_STDLIB,
        .namespace_id = module_name,
        .physical_root = root,
    };
    bool valid = xr_module_identity_from_source(authority, source, identity_out, &logical);
    char expected[512];
    int expected_length = snprintf(expected, sizeof(expected), "%s/%s.xr", module_name,
                                   module_name);
    valid = valid && expected_length > 0 && (size_t) expected_length < sizeof(expected) &&
            strcmp(logical, expected) == 0;
    if (!valid) {
        xr_free(*identity_out);
        *identity_out = NULL;
        xr_free(root);
        root = NULL;
    }
    xr_free(logical);
    xr_free(module_dir);
    xr_free(source);
    *root_out = root;
    authority->physical_root = root;
    return valid;
}

static bool compile_to_file_impl(XrCompilerSession *session, const char *stdlib_module_name,
                                 const char *source_file, const char *output_file, int flags,
                                 const XrModuleIdentityAuthority *authority) {
    if (!session) {
        xr_log_warning("compile", "compiler session is required");
        return false;
    }
    XrVMRuntime *X = xr_compiler_session_vm_host(session);
    if (!X) {
        xr_log_warning("compile", "compiler session has no VM host");
        return false;
    }
    XrCompilerSessionOperationScope operation_scope;
    if (!xr_compiler_session_operation_begin(session, &operation_scope)) {
        xr_log_warning("compile", "compiler session is busy");
        return false;
    }

    // Read source file
    char *source = xr_file_read_all(source_file, "r", NULL);
    if (!source) {
        xr_log_warning("compile", "cannot open: %s", source_file);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }

    XrProto *proto = xr_compile_source_with_path(session, source, source_file, authority);
    xr_free(source);

    if (!proto) {
        xr_log_warning("compile", "compilation failed: %s", source_file);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }

    // Serialize
    size_t bc_size;
    XrBootstrapContainerError bc_error = XR_BOOTSTRAP_CONTAINER_OK;
    uint8_t *bc = stdlib_module_name
                      ? xr_bootstrap_container_write_stdlib(X, stdlib_module_name, proto, flags,
                                                           &bc_size, &bc_error)
                      : xr_bootstrap_container_write(X, proto, flags, &bc_size, &bc_error);
    if (!bc) {
        xr_instruction_unit_free(proto);
        xr_log_warning("compile", "bytecode serialization failed: %s",
                       xr_bootstrap_container_error_string(bc_error));
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }

    xr_instruction_unit_free(proto);

    // Write to file
    FILE *f = fopen(output_file, "wb");
    if (!f) {
        xr_free(bc);
        xr_log_warning("compile", "cannot create: %s", output_file);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }

    bool wrote_all = fwrite(bc, 1, bc_size, f) == bc_size;
    bool closed = fclose(f) == 0;
    xr_free(bc);
    if (!wrote_all || !closed) {
        xr_log_warning("compile", "cannot complete: %s", output_file);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }
    return xr_compiler_session_operation_succeed(&operation_scope);
}

bool xr_compile_to_file(XrCompilerSession *session, const char *source_file,
                        const char *output_file, int flags,
                        const XrModuleIdentityAuthority *authority) {
    return compile_to_file_impl(session, NULL, source_file, output_file, flags, authority);
}

bool xr_compile_stdlib_to_file(XrCompilerSession *session, const char *canonical_module,
                               const char *source_file, const char *output_file, int flags) {
    if (!session || !canonical_module || !canonical_module[0])
        return false;
    XrCompilerSessionOperationScope operation_scope;
    if (!xr_compiler_session_operation_begin(session, &operation_scope))
        return false;
    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    char *module_identity = NULL;
    if (!stdlib_compile_authority(canonical_module, source_file, &authority, &authority_root,
                                  &module_identity)) {
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }
    XrCompileUnitIdentity identity = {
        .kind = XR_COMPILE_UNIT_STDLIB,
        .module_identity = module_identity,
        .stdlib_module_name = canonical_module,
    };
    if (!xr_compiler_session_set_compile_unit_identity(session, &identity)) {
        xr_free(module_identity);
        xr_free(authority_root);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return false;
    }

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
                           ? xr_module_graph_build(graph, source_file, &authority, &graph_err)
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

    bool ok = compile_to_file_impl(session, canonical_module, source_file, output_file, flags,
                                   &authority);

    xr_compiler_session_set_module_graph(session, NULL);
    if (graph_analyzer) {
        xa_analyzer_set_graph(graph_analyzer, NULL);
        xa_analyzer_free(graph_analyzer);
    }
    if (graph)
        xr_module_graph_free(graph);
    xr_compiler_session_set_compile_unit_identity(session, NULL);
    xr_free(module_identity);
    xr_free(authority_root);
    return ok ? xr_compiler_session_operation_succeed(&operation_scope)
              : xr_compiler_session_operation_fail(
                    &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL) && false;
}
