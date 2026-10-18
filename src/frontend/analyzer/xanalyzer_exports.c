/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_exports.c - Checked collection of real module export symbols
 */
#include "xanalyzer_exports.h"
#include "xanalyzer_export_query.h"
#include "../../runtime/value/xtype_internal.h"
#include "../../module/xmodule_graph.h"
#include "../parser/xast_nodes.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

static XrCompileState *export_state(XaAnalyzer *analyzer) {
    return xa_symbol_registry_state(analyzer ? analyzer->symbol_registry : NULL);
}

static bool healthy(XaAnalyzer *analyzer) {
    XrCompileState *state = export_state(analyzer);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return false;
    if (!analyzer->compiler_session ||
        xr_compile_session_compile_state(analyzer->compiler_session) != state) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return false;
    }
    return true;
}

static bool graph_ready(XaAnalyzer *analyzer) {
    if (!healthy(analyzer)) return false;
    XrModuleGraph *graph = analyzer->graph;
    if (!graph) return true;
    XrCompileState *state = export_state(analyzer);
    if (graph->resources != xr_compile_state_resources(state) || !graph->id_index || graph->spec_count < 0 ||
        graph->spec_capacity < graph->spec_count || (graph->spec_count && !graph->specs)) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return false;
    }
    XR_DCHECK(graph->spec_count <= graph->spec_capacity, "Export graph storage is bounded");
    return true;
}

XR_FUNC XrModuleSpec *xa_graph_spec_for_ast(XaAnalyzer *analyzer, const AstNode *ast) {
    if (!graph_ready(analyzer) || !analyzer->graph || !ast) return NULL;
    XrModuleGraph *graph = analyzer->graph;
    for (int i = 0; i < graph->spec_count; ++i) {
        if (xr_compile_state_work(export_state(analyzer), 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        if (graph->specs[i].ast == ast) return &graph->specs[i];
    }
    return NULL;
}

XR_FUNC XrModuleSpec *xa_graph_spec_for_identity(XaAnalyzer *analyzer, const char *identity) {
    if (!graph_ready(analyzer) || !analyzer->graph || !identity) return NULL;
    XrCompileState *state = export_state(analyzer);
    size_t length = 0;
    if (xr_compile_state_string_length(state, identity, &length) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_state_work(state, length) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!xr_module_identity_valid(identity, NULL)) return NULL;
    XrModuleSpec *match = NULL;
    for (int i = 0; i < analyzer->graph->spec_count; ++i) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        bool equal = false;
        if (!xa_export_identity_equal(analyzer, analyzer->graph->specs[i].canonical, identity,
                                      &equal)) return NULL;
        if (equal) {
            if (match) return NULL;
            match = &analyzer->graph->specs[i];
        }
    }
    return match;
}

static const char *declared_name(const AstNode *decl) {
    switch (decl->type) {
        case AST_FUNCTION_DECL: return decl->as.function_decl.name;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL: return decl->as.class_decl.name;
        case AST_CONST_DECL:
        case AST_VAR_DECL: return decl->as.var_decl.name;
        case AST_ENUM_DECL: return decl->as.enum_decl.name;
        case AST_INTERFACE_DECL: return decl->as.interface_decl.name;
        case AST_TYPE_ALIAS: return decl->as.type_alias.name;
        default: return NULL;
    }
}

static bool poisoned(const XaSymbol *symbol) {
    return symbol && (xr_type_contains_error(symbol->links.type) ||
        xr_type_contains_error(symbol->links.declared_type) ||
        xr_type_contains_error((const XrType *) symbol->alias_type));
}

static void report_invalid(XaAnalyzer *analyzer, const AstNode *node, const char *message) {
    XrLocation location = {.file = analyzer->current_file, .line = node ? node->line : 0,
                           .column = node ? node->column : 0};
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                               message, &location);
}

typedef struct XaExportBuild {
    XaAnalyzer *analyzer;
    XrHashMap *exports;
    bool failed;
    bool invalid;
} XaExportBuild;

static bool set_export(XaExportBuild *build, const char *name, XaSymbol *symbol,
                       const AstNode *node) {
    if (build->failed || !healthy(build->analyzer)) { build->failed = true; return false; }
    if (xr_compile_state_work(export_state(build->analyzer), 1) != XR_COMPILE_RESOURCE_OK) {
        build->failed = true; return false;
    }
    if (poisoned(symbol)) {
        build->invalid = build->failed = true;
        char message[256];
        snprintf(message, sizeof(message),
            "Export '%s' contains compiler recovery ErrorType metadata; module export table is invalid",
            name ? name : "<unknown>");
        report_invalid(build->analyzer, node, message);
        return false;
    }
    if (!name || !symbol || (!symbol->links.type &&
        !(symbol->kind == XA_SYM_CLASS && symbol->links.class_info))) return true;
    if (!build->exports) {
        XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_state_resources(export_state(build->analyzer)));
        if (!xa_accept_export_io(build->analyzer, xr_hashmap_owned_new(&policy, &build->exports))) {
            build->failed = true; return false;
        }
    }
    if (!xa_accept_export_io(build->analyzer, xr_hashmap_owned_set(build->exports, name, symbol))) {
        build->failed = true; return false;
    }
    return true;
}

static bool collect_native(const char *key, void *value, void *context) {
    XaExportBuild *build = context;
    if (build->failed || !healthy(build->analyzer)) { build->failed = true; return false; }
    if (xr_compile_state_work(export_state(build->analyzer), 1) != XR_COMPILE_RESOURCE_OK) {
        build->failed = true; return false;
    }
    XaSymbol *symbol = value;
    if (symbol && symbol->is_builtin && symbol->is_exported)
        return set_export(build, key, symbol, NULL);
    return true;
}

typedef struct XaReexportAll {
    XaExportBuild *build;
    const AstNode *node;
} XaReexportAll;

static bool reexport_all(const char *key, void *value, void *context) {
    XaReexportAll *all = context;
    return set_export(all->build, key, value, all->node);
}

static bool accept_module(XaAnalyzer *analyzer, XrModuleStatus status) {
    if (status == XR_MODULE_OK) return true;
    if (status == XR_MODULE_OUT_OF_MEMORY || status == XR_MODULE_BUDGET)
        (void) xr_compile_state_fail(export_state(analyzer), status == XR_MODULE_BUDGET
            ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return false;
}

static bool source_exports(XaExportBuild *build, AstNode *ast, const AstNode *node,
                            const char *path, XrHashMap **output) {
    XaAnalyzer *analyzer = build->analyzer;
    if (!graph_ready(analyzer)) return false;
    XrModuleGraph *graph = analyzer->graph;
    if (!graph || !graph->resolver || !path) { *output = NULL; return true; }
    XrModuleSpec *owner = xa_graph_spec_for_ast(analyzer, ast);
    if (!healthy(analyzer)) return false;
    if (!owner) owner = xa_graph_spec_for_identity(analyzer, analyzer->current_module_identity);
    if (!healthy(analyzer)) return false;
    if (!owner || !owner->source_path || !xr_module_identity_authority_valid(&owner->authority)) {
        build->invalid = true;
        report_invalid(analyzer, node, "Re-export has no exact source module authority");
        return false;
    }
    if (graph->resolver->resources != xr_compile_state_resources(export_state(analyzer))) {
        (void) xr_compile_state_fail(export_state(analyzer), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return false;
    }
    XrModuleId id = {0}; char *error = NULL;
    XrModuleStatus resolved = xr_compile_module_resolver_resolve(graph->resolver, path,
        owner->source_path, &owner->authority, &id, &error);
    if (!accept_module(analyzer, resolved)) {
        if (healthy(analyzer)) {
            build->invalid = true;
            report_invalid(analyzer, node, error ? error : "Re-export source cannot be resolved");
        }
        xr_compile_resources_free(error);
        return false;
    }
    xr_compile_resources_free(error);
    int index = -1;
    XrModuleStatus found = xr_compile_module_graph_find(graph, id.canonical, &index);
    xr_compile_module_id_cleanup(&id);
    if (!accept_module(analyzer, found)) return false;
    if (index < 0 || index >= graph->spec_count || graph->specs[index].export_symbols_invalid) {
        build->invalid = true;
        report_invalid(analyzer, node, "Re-export source has no valid semantic export table");
        return false;
    }
    XR_DCHECK(index < graph->spec_count, "Resolved re-export belongs to the checked graph");
    *output = graph->specs[index].export_symbols;
    return true;
}

XR_FUNC bool xa_analyzer_collect_export_symbols_checked(XaAnalyzer *analyzer, XrAstNode *ast,
                                                        XrHashMap **output) {
    if (!healthy(analyzer)) return false;
    if (!ast || ast->type != AST_PROGRAM || !output || ast->as.program.count < 0 ||
        (ast->as.program.count && !ast->as.program.statements)) {
        (void) xr_compile_state_fail(export_state(analyzer), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return false;
    }
    XrModuleSpec *owner = xa_graph_spec_for_ast(analyzer, ast);
    if (!healthy(analyzer)) return false;
    XaExportBuild build = {.analyzer = analyzer};
    XaScope *scope = analyzer->current_scope ? analyzer->current_scope : analyzer->global_scope;
    if (!scope || scope->registry != analyzer->symbol_registry) {
        (void) xr_compile_state_fail(export_state(analyzer), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return false;
    }
    if (scope->symbols && !xa_accept_export_io(analyzer,
        xr_hashmap_owned_visit(scope->symbols, collect_native, &build))) goto invalid;
    if (build.failed) goto invalid;
    for (int i = 0; i < ast->as.program.count; ++i) {
        if (xr_compile_state_work(export_state(analyzer), 1) != XR_COMPILE_RESOURCE_OK) goto invalid;
        AstNode *node = ast->as.program.statements[i];
        if (!node) continue;
        if (node->is_exported) {
            const char *name = declared_name(node);
            if (name) {
                XaSymbol *symbol = xa_scope_lookup(scope, name);
                if (!healthy(analyzer) || !set_export(&build, name, symbol, node)) goto invalid;
            }
        }
        if (node->type != AST_EXPORT_STMT) continue;
        ExportStmtNode *export = &node->as.export_stmt;
        if (!export->from_path) continue;
        XrHashMap *source = NULL;
        if (!source_exports(&build, ast, node, export->from_path, &source)) goto invalid;
        if (!source) continue;
        if (export->is_reexport_all) {
            XaReexportAll all = {&build, node};
            if (!xa_accept_export_io(analyzer, xr_hashmap_owned_visit(source, reexport_all, &all)) ||
                build.failed) goto invalid;
        } else {
            if (export->reexport_count < 0 || (export->reexport_count && !export->reexport_members)) {
                (void) xr_compile_state_fail(export_state(analyzer), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
                goto invalid;
            }
            for (int j = 0; j < export->reexport_count; ++j) {
                if (xr_compile_state_work(export_state(analyzer), 1) != XR_COMPILE_RESOURCE_OK) goto invalid;
                ReexportMember *member = &export->reexport_members[j];
                if (!member->name) continue;
                XaSymbol *symbol = NULL;
                if (!xa_find_export_symbol(analyzer, source, member->name, &symbol) ||
                    !set_export(&build, member->alias ? member->alias : member->name, symbol, node))
                    goto invalid;
            }
        }
    }
    if (xr_compile_state_work(export_state(analyzer), 1) != XR_COMPILE_RESOURCE_OK) goto invalid;
    if (owner) owner->export_symbols_invalid = false;
    *output = build.exports;
    return true;
invalid:
    xr_hashmap_owned_free(build.exports);
    if (build.invalid && owner) {
        owner->export_symbols_invalid = true;
        xr_hashmap_owned_free(owner->export_symbols);
        owner->export_symbols = NULL;
    }
    return false;
}

XR_FUNC XrHashMap *xa_analyzer_collect_export_symbols(XaAnalyzer *analyzer, XrAstNode *ast) {
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(analyzer, ast, &exports)) return NULL;
    return exports;
}
