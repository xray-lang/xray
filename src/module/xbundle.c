/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbundle.c - Multi-file bundling implementation
 *
 * KEY CONCEPT:
 *   Builds one dependency graph, compiles it in topological order,
 *   and bundles source-backed modules into a single bytecode package.
 */

#include "xbundle.h"
#include "xmodule.h"
#include "xmodule_graph.h"
#include "xmodule_resolver.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "xproto_codec.h"
#include "../runtime/xisolate_api.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../ir/xi_module.h"
#include "../toolchain/xcompiler_session.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ========== Helper Functions ========== */

static bool bundle_add_entry(XrBundle *bundle, const char *path, const uint8_t *bc, size_t bc_size,
                             XrModuleKind kind) {
    if (!bundle || !path || ((bc == NULL) != (bc_size == 0)))
        return false;

    if (bundle->count >= bundle->capacity) {
        int new_cap = bundle->capacity * 2;
        if (new_cap < 16)
            new_cap = 16;
        if (!XR_REALLOC(bundle->entries, (size_t) new_cap * sizeof(XrBundleEntry)))
            return false;
        bundle->capacity = new_cap;
    }

    XrBundleEntry *entry = &bundle->entries[bundle->count];
    entry->path = xr_strdup(path);
    entry->bc = bc_size > 0 ? (uint8_t *) xr_malloc(bc_size) : NULL;
    if (!entry->path || (bc_size > 0 && !entry->bc)) {
        xr_free(entry->path);
        xr_free(entry->bc);
        memset(entry, 0, sizeof(*entry));
        return false;
    }
    if (bc_size > 0)
        memcpy(entry->bc, bc, bc_size);
    entry->bc_size = bc_size;
    entry->kind = kind;
    bundle->count++;
    return true;
}

static XrModuleGraph *bundle_build_graph(XrCompilerSession *session, XrModuleResolver *resolver,
                                         const char *entry_path,
                                         const XrModuleIdentityAuthority *authority) {
    XrModuleGraph *graph = xr_module_graph_new(session, resolver);
    if (!graph) {
        xr_log_warning("bundle", "failed to create module graph");
        return NULL;
    }

    char *err = NULL;
    if (xr_module_graph_build(graph, entry_path, authority, &err) != 0) {
        xr_log_warning("bundle", "module graph build failed: %s", err ? err : "?");
        xr_free(err);
        xr_module_graph_free(graph);
        return NULL;
    }
    xr_free(err);

    if (xr_module_graph_topological_sort(graph) != 0 || graph->has_cycle) {
        xr_log_warning("bundle", "%s",
                       graph->cycle_desc ? graph->cycle_desc
                                         : "E0504: circular dependency detected");
        xr_module_graph_free(graph);
        return NULL;
    }

    return graph;
}

static void bundle_report_diagnostics(XaAnalyzer *analyzer, const char *source_path,
                                      int *error_count) {
    int count = 0;
    XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(analyzer, &count);
    for (XaDiagnostic *diagnostic = diagnostics; diagnostic; diagnostic = diagnostic->next) {
        if (diagnostic->severity != XR_DIAG_SEV_ERROR)
            continue;
        (*error_count)++;
        fprintf(stderr, "%s:%d:%d: error: %s\n", source_path ? source_path : "<bundle>",
                diagnostic->location.line, diagnostic->location.column, diagnostic->message);
    }
}

static XaAnalyzer *bundle_analyze_dependency_exports(XrCompilerSession *session,
                                                     XrModuleGraph *graph) {
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer)
        return NULL;

    xa_analyzer_set_graph(analyzer, graph);
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int index = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[index];
        if (index == graph->entry_index || !spec->ast)
            continue;

        xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
        int errors = 0;
        bundle_report_diagnostics(analyzer, spec->source_path, &errors);
        if (errors == 0) {
            XrHashMap *exports = NULL;
            if (!xa_analyzer_collect_export_symbols_checked(analyzer, (XrAstNode *) spec->ast,
                                                            &exports)) {
                bundle_report_diagnostics(analyzer, spec->source_path, &errors);
                if (errors == 0)
                    errors = 1;
            } else {
                spec->export_symbols = exports;
            }
        }
        xa_analyzer_clear_diagnostics(analyzer);
        if (errors > 0)
            goto fail;
    }
    return analyzer;

fail:
    xa_analyzer_set_graph(analyzer, NULL);
    xa_analyzer_free(analyzer);
    return NULL;
}

static bool bundle_compile_graph(XrVMRuntime *X, XrCompilerSession *session, XaAnalyzer *analyzer,
                                 XrModuleGraph *graph, XrBundleFlags flags, XrBundle *bundle) {
    XiModule **graph_modules =
        (XiModule **) xr_calloc((size_t) graph->topo_count, sizeof(XiModule *));
    XrProto **compiled = (XrProto **) xr_calloc((size_t) graph->topo_count, sizeof(XrProto *));
    if (!graph_modules || !compiled) {
        xr_free(graph_modules);
        xr_free(compiled);
        return false;
    }

    bool complete = false;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        int index = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[index];

        /* An external module occupies its topological slot without carrying
         * bytecode: the runtime supplies its body, and the entry only has to
         * name it well enough for xr_module_import to find it again. That name
         * is the import specifier, never the canonical identity. */
        bool external = spec->kind == XR_MOD_STDLIB ||
                        (spec->kind == XR_MOD_PACKAGE && !(flags & XR_BUNDLE_STATIC_PACKAGES));
        if (external) {
            const char *import_name = xr_module_spec_import_name(spec);
            if (!import_name) {
                xr_log_warning("bundle", "external module has no import name: %s",
                               spec->canonical ? spec->canonical : "?");
                goto cleanup;
            }
            if (!bundle_add_entry(bundle, import_name, NULL, 0, spec->kind))
                goto cleanup;
            continue;
        }
        if (!spec->ast || !spec->source_path) {
            xr_log_warning("bundle", "source-backed module is incomplete: %s",
                           spec->canonical ? spec->canonical : "?");
            goto cleanup;
        }

        XrProto *proto =
            xr_compile_ast_in_graph(session, analyzer, spec->ast, spec->source_path, graph,
                                    graph_modules, graph->topo_count, &graph_modules[ti],
                                    &spec->authority);
        if (!proto) {
            xr_log_warning("bundle", "compilation failed: %s", spec->source_path);
            goto cleanup;
        }
        compiled[ti] = proto;
        size_t bc_size = 0;
        uint8_t *bc = xr_bootstrap_container_write(X, proto, 0, &bc_size, NULL);
        if (!bc) {
            xr_log_warning("bundle", "bytecode serialization failed: %s", spec->source_path);
            goto cleanup;
        }

        int bundle_index = bundle->count;
        bool added = bundle_add_entry(bundle, spec->source_path, bc, bc_size, spec->kind);
        xr_free(bc);
        if (!added) {
            xr_log_warning("bundle", "cannot add bytecode module: %s", spec->source_path);
            goto cleanup;
        }
        if (index == graph->entry_index)
            bundle->entry_index = bundle_index;
    }

    complete = bundle->entry_index >= 0;

cleanup:
    for (int ti = 0; ti < graph->topo_count; ti++)
        if (compiled[ti])
            xr_free_code(X, compiled[ti]);
    xr_free(compiled);
    xr_free(graph_modules);
    return complete;
}

/* ========== Public API ========== */

XrBundle *xr_bundle_create_ex(XrVMRuntime *X, const char *entry_file,
                              const XrModuleIdentityAuthority *authority, XrBundleFlags flags) {
    XR_DCHECK(X != NULL, "bundle_create_ex: NULL isolate");
    XR_DCHECK(entry_file != NULL, "bundle_create_ex: NULL entry_file");
    if (!X || !entry_file || !authority ||
        !xr_module_identity_authority_valid(authority) ||
        authority->kind == XR_MODULE_IDENTITY_MEMORY)
        return NULL;

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(X);
    if (!session) {
        xr_log_warning("bundle", "compiler session is required");
        return NULL;
    }
    XrCompilerSessionOperationScope operation_scope;
    if (!xr_compiler_session_operation_begin(session, &operation_scope)) {
        xr_log_warning("bundle", "compiler session is busy");
        return NULL;
    }

    XrBundle *bundle = xr_calloc(1, sizeof(XrBundle));
    if (!bundle) {
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return NULL;
    }
    bundle->entry_index = -1;

    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolverConfig rcfg = {
        .stdlib_path = registry ? registry->stdlib_path : NULL,
        .lockfile = NULL,
    };
    XrModuleResolver *resolver = xr_module_resolver_new(&rcfg);
    XrModuleGraph *graph =
        resolver ? bundle_build_graph(session, resolver, entry_file, authority) : NULL;
    XaAnalyzer *graph_analyzer = graph ? bundle_analyze_dependency_exports(session, graph) : NULL;
    XrModuleGraph *previous_graph = xr_compiler_session_module_graph(session);
    if (!resolver || !graph || !graph_analyzer) {
        if (graph_analyzer) {
            xa_analyzer_set_graph(graph_analyzer, NULL);
            xa_analyzer_free(graph_analyzer);
        }
        xr_module_graph_free(graph);
        xr_module_resolver_free(resolver);
        xr_bundle_free(bundle);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return NULL;
    }
    xr_compiler_session_set_module_graph(session, graph);

    XrModuleSpec *entry_spec = &graph->specs[graph->entry_index];
    const char *entry_path =
        entry_spec->source_path ? entry_spec->source_path : entry_spec->canonical;
    bundle->entry_path = xr_strdup(entry_path);
    bool complete = bundle->entry_path &&
                    bundle_compile_graph(X, session, graph_analyzer, graph, flags, bundle);
    xr_compiler_session_set_module_graph(session, previous_graph);
    xa_analyzer_set_graph(graph_analyzer, NULL);
    xa_analyzer_free(graph_analyzer);
    xr_module_graph_free(graph);
    xr_module_resolver_free(resolver);
    if (!complete) {
        xr_log_warning("bundle", "bundle compilation failed: %s", entry_file);
        xr_bundle_free(bundle);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        return NULL;
    }

    if (!xr_compiler_session_operation_succeed(&operation_scope)) {
        xr_bundle_free(bundle);
        return NULL;
    }

    return bundle;
}

void xr_bundle_free(XrBundle *bundle) {
    if (!bundle)
        return;

    for (int i = 0; i < bundle->count; i++) {
        xr_free(bundle->entries[i].path);
        xr_free(bundle->entries[i].bc);
    }
    xr_free(bundle->entries);
    xr_free(bundle->entry_path);
    xr_free(bundle);
}

static bool bundle_emitf(char **buf, size_t *cap, size_t *len, const char *fmt, ...) {
    if (!buf || !*buf || !cap || !len || !fmt)
        return false;
    for (;;) {
        size_t avail = (*cap > *len) ? (*cap - *len) : 0;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *len, avail, fmt, ap);
        va_end(ap);
        if (n < 0)
            return false;
        if ((size_t) n < avail) {
            *len += (size_t) n;
            return true;
        }
        size_t need = *len + (size_t) n + 1;
        size_t new_cap = *cap ? *cap : 4096;
        while (new_cap < need)
            new_cap *= 2;
        char *tmp = (char *) xr_realloc(*buf, new_cap);
        if (!tmp)
            return false;
        *buf = tmp;
        *cap = new_cap;
    }
}

static bool bundle_emit_c_string(char **buf, size_t *cap, size_t *len, const char *s) {
    if (!bundle_emitf(buf, cap, len, "\""))
        return false;
    for (const unsigned char *p = (const unsigned char *) (s ? s : ""); *p; p++) {
        switch (*p) {
            case '\\':
                if (!bundle_emitf(buf, cap, len, "\\\\"))
                    return false;
                break;
            case '"':
                if (!bundle_emitf(buf, cap, len, "\\\""))
                    return false;
                break;
            case '\n':
                if (!bundle_emitf(buf, cap, len, "\\n"))
                    return false;
                break;
            case '\r':
                if (!bundle_emitf(buf, cap, len, "\\r"))
                    return false;
                break;
            case '\t':
                if (!bundle_emitf(buf, cap, len, "\\t"))
                    return false;
                break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    if (!bundle_emitf(buf, cap, len, "\\%03o", (unsigned) *p))
                        return false;
                } else if (!bundle_emitf(buf, cap, len, "%c", *p)) {
                    return false;
                }
                break;
        }
    }
    return bundle_emitf(buf, cap, len, "\"");
}

char *xr_bundle_to_c_source(XrBundle *bundle, const char *var_prefix) {
    if (!bundle || bundle->count == 0)
        return NULL;

    const char *prefix = var_prefix ? var_prefix : "xr_app";

    // Estimate output size
    size_t total_bc = 0;
    for (int i = 0; i < bundle->count; i++) {
        total_bc += bundle->entries[i].bc_size;
    }
    size_t buf_size = total_bc * 6 + bundle->count * 512 + 4096;

    char *output = xr_malloc(buf_size);
    if (!output)
        return NULL;
    size_t len = 0;

#define EMIT(...)                                                                                  \
    do {                                                                                           \
        if (!bundle_emitf(&output, &buf_size, &len, __VA_ARGS__)) {                                \
            xr_free(output);                                                                       \
            return NULL;                                                                           \
        }                                                                                          \
    } while (0)

#define EMIT_C_STRING(s)                                                                           \
    do {                                                                                           \
        if (!bundle_emit_c_string(&output, &buf_size, &len, (s))) {                                \
            xr_free(output);                                                                       \
            return NULL;                                                                           \
        }                                                                                          \
    } while (0)

    // Header
    EMIT("/* Auto-generated by xray build */\n\n");
    EMIT("#include <stdint.h>\n");
    EMIT("#include <stddef.h>\n\n");

    // Bytecode for each module
    for (int i = 0; i < bundle->count; i++) {
        const XrBundleEntry *e = &bundle->entries[i];
        if (!e->bc)
            continue;
        EMIT("/* Module %d */\n", i);
        EMIT("static const uint8_t %s_mod%d_bc[%zu] = {\n", prefix, i, e->bc_size);

        for (size_t j = 0; j < e->bc_size; j++) {
            if (j % 12 == 0)
                EMIT("    ");
            EMIT("0x%02x", e->bc[j]);
            if (j < e->bc_size - 1)
                EMIT(",");
            if ((j + 1) % 12 == 0 || j == e->bc_size - 1)
                EMIT("\n");
            else
                EMIT(" ");
        }
        EMIT("};\n\n");
    }

    // Module table
    EMIT("/* Module table */\n");
    EMIT("const int %s_module_count = %d;\n\n", prefix, bundle->count);

    EMIT("const XrBytecodeModule %s_modules[%d] = {\n", prefix, bundle->count);
    for (int i = 0; i < bundle->count; i++) {
        const XrBundleEntry *e = &bundle->entries[i];
        EMIT("    {");
        EMIT_C_STRING(e->path);
        if (e->bc)
            EMIT(", %s_mod%d_bc, %zu},\n", prefix, i, e->bc_size);
        else
            EMIT(", NULL, 0},\n");
    }
    EMIT("};\n\n");

    // Entry module index
    EMIT("const int %s_entry_index = %d;\n\n", prefix, bundle->entry_index);

    EMIT("const XrBytecodeBundle %s_bundle = {\n", prefix);
    EMIT("    %s_modules,\n", prefix);
    EMIT("    (size_t)%s_module_count,\n", prefix);
    EMIT("    (size_t)%s_entry_index,\n", prefix);
    EMIT("};\n");

#undef EMIT
#undef EMIT_C_STRING

    return output;
}
