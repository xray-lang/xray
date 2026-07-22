/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_explain.c - Evidence/provenance explanation command
 */

#include "xcli.h"
#include "xcli_fs.h"
#include "xcli_spec.h"
#include "../../module/xproject.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../api/xisolate_profile.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_mono.h"
#include "../../frontend/analyzer/xa_effect_db.h"
#include "../../frontend/analyzer/xa_memory_effect_db.h"
#include "../../runtime/xisolate_api.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

static int explain_native(const XrCliInvocation *inv, const char *input) {
    char root[XR_CLI_PATH_MAX];
    if (!xr_cli_find_project_root(input ? input : ".", root, sizeof(root))) {
        xr_cli_error("explain", "no xray.toml found from '%s'", input ? input : ".");
        return XR_CLI_EXIT_USAGE;
    }
    XrProject *project = xr_project_load(NULL, root);
    if (!project) {
        xr_cli_error("explain", "cannot load project at '%s'", root);
        return XR_CLI_EXIT_FAIL;
    }
    if (!project->native_plan) {
        printf("native-plan: none\n");
        xr_project_free(project);
        return XR_CLI_EXIT_OK;
    }
    if (xr_cli_opt_bool(&inv->options, "json")) {
        const XrNativePackagePlan *plan = project->native_plan;
        printf("{\"package\":\"%s\",\"version\":\"%s\",\"audit\":\"%s\","
               "\"valid\":%s,\"fingerprint\":\"%016llx\",\"units\":%u,\"symbols\":%u}\n",
               plan->name ? plan->name : "", plan->version ? plan->version : "",
               xr_native_audit_mode_name(plan->audit_mode), plan->valid ? "true" : "false",
               (unsigned long long) plan->fingerprint, plan->unit_count, plan->symbol_count);
    } else {
        xr_native_package_explain(project->native_plan, stdout);
    }
    int result = project->native_plan->valid ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
    xr_project_free(project);
    return result;
}

static const char *explain_final_component(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot ? dot + 1 : name;
}

static XaSymbol *explain_find_export(XrModuleGraph *graph, const char *subject) {
    XaSymbol *short_match = NULL;
    if (!graph || !subject)
        return NULL;
    for (int i = 0; i < graph->spec_count; i++) {
        XrHashMap *exports = graph->specs[i].export_symbols;
        if (!exports)
            continue;
        XaSymbol *exact = (XaSymbol *) xr_hashmap_get(exports, subject);
        if (exact)
            return exact;
        for (uint32_t slot = 0; slot < exports->capacity; slot++) {
            XrHashMapEntry *entry = &exports->entries[slot];
            if (!entry->key || entry->value == XR_HASHMAP_TOMBSTONE)
                continue;
            if (strcmp(explain_final_component(entry->key), subject) == 0) {
                if (short_match && short_match != (XaSymbol *) entry->value)
                    return NULL;
                short_match = (XaSymbol *) entry->value;
            }
        }
    }
    return short_match;
}

static void explain_print_memory_effect(const XaMemoryEffectSummary *memory) {
    if (!memory) {
        printf("memory-effect: missing\n");
        return;
    }
    printf("memory-effect complete=%s unknown=0x%x fingerprint=%016llx roots=%u\n",
           xa_memory_effect_summary_is_complete(memory) ? "yes" : "no", memory->unknown_reasons,
           (unsigned long long) xa_memory_effect_summary_fingerprint(memory), memory->root_count);
    for (uint32_t i = 0; i < memory->root_count; i++) {
        const XaMemoryRootEffect *root = &memory->roots[i];
        printf("  root kind=%u index=%u writes=%u rebind=%u relocate=%u shorten=%u "
               "invalidate=%u\n",
               (unsigned) root->root.kind, root->root.index, root->write_count,
               root->descriptor_rebind, (unsigned) root->relocation, (unsigned) root->shortening,
               (unsigned) root->invalidation);
    }
}

static int explain_effect(const char *subject) {
    char root[XR_CLI_PATH_MAX];
    if (!xr_cli_find_project_root(".", root, sizeof(root))) {
        xr_cli_error("explain", "effect explanation requires a project xray.toml");
        return XR_CLI_EXIT_USAGE;
    }
    XrProject *project = xr_project_load(NULL, root);
    if (!project || !project->initialized || !project->main) {
        xr_cli_error("explain", "%s",
                     project && project->native_plan && project->native_plan->error
                         ? project->native_plan->error
                         : "project has no valid main entry");
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }
    char *entry = xr_path_join(root, project->main);
    XrVMRuntime *isolate = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    if (!entry || !isolate) {
        xr_free(entry);
        xr_project_free(project);
        if (isolate)
            xray_vm_delete(isolate);
        return XR_CLI_EXIT_INTERNAL;
    }
    xr_module_system_init_with_script(isolate, entry);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    xr_compiler_session_set_native_package_plan(session, project->native_plan);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(isolate);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    XrModuleGraph *graph = resolver ? xr_module_graph_new(session, resolver) : NULL;
    char *graph_error = NULL;
    if (!graph || xr_module_graph_build(graph, entry, &graph_error) != 0 ||
        xr_module_graph_topological_sort(graph) != 0 || graph->has_cycle) {
        xr_cli_error("explain", "%s", graph_error ? graph_error : "module graph build failed");
        xr_free(graph_error);
        if (graph)
            xr_module_graph_free(graph);
        xray_vm_delete(isolate);
        xr_free(entry);
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }
    xr_free(graph_error);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        xr_module_graph_free(graph);
        xray_vm_delete(isolate);
        xr_free(entry);
        xr_project_free(project);
        return XR_CLI_EXIT_INTERNAL;
    }
    xa_analyzer_set_graph(analyzer, graph);
    int errors = 0;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
        xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
        spec->export_symbols =
            xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);
        int diagnostic_count = 0;
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count); diag;
             diag = diag->next) {
            if (diag->severity == XR_DIAG_SEV_ERROR)
                errors++;
        }
        xa_analyzer_clear_diagnostics(analyzer);
    }
    AstNode **mono_roots = (AstNode **) xr_calloc((size_t) graph->topo_count, sizeof(AstNode *));
    if (mono_roots) {
        for (int ti = 0; ti < graph->topo_count; ti++)
            mono_roots[ti] = graph->specs[graph->topo_order[ti]].ast;
        for (int ti = 0; ti < graph->topo_count; ti++) {
            XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
            xa_mono_pass_with_external_structs_and_analyzer(spec->ast, mono_roots,
                                                            graph->topo_count, isolate, analyzer);
        }
        for (int ti = 0; ti < graph->topo_count; ti++) {
            XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
            xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
            if (spec->export_symbols)
                xr_hashmap_free(spec->export_symbols);
            spec->export_symbols =
                xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);
            int diagnostic_count = 0;
            for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count);
                 diag; diag = diag->next) {
                if (diag->severity == XR_DIAG_SEV_ERROR)
                    errors++;
            }
            xa_analyzer_clear_diagnostics(analyzer);
        }
        xr_free(mono_roots);
    }
    XaSymbol *symbol = explain_find_export(graph, subject);
    if (!symbol) {
        xr_cli_error("explain", "exported symbol '%s' is missing or ambiguous", subject);
        errors++;
    } else {
        const XaEffectSummary *effect =
            xa_effect_db_get(analyzer->effect_db, symbol->links.effect_id);
        const XaMemoryEffectSummary *memory =
            xa_memory_effect_db_get(analyzer->memory_effect_db, symbol->links.memory_effect_id);
        printf("symbol %s file=%s\n", symbol->name ? symbol->name : subject,
               symbol->links.file_path ? symbol->links.file_path : "?");
        if (effect) {
            printf("effect complete=%s semantic=0x%x unknown-semantic=0x%x unknown=0x%x "
                   "unsafe=%u fingerprint=%016llx\n",
                   xa_effect_summary_is_complete(effect) ? "yes" : "no", effect->semantic_effects,
                   effect->unknown_semantic_effects, effect->unknown_reasons,
                   effect->contains_unsafe_op,
                   (unsigned long long) xa_effect_summary_fingerprint(analyzer->effect_db, effect));
        } else {
            printf("effect: missing\n");
        }
        explain_print_memory_effect(memory);
    }
    xa_analyzer_set_graph(analyzer, NULL);
    xa_analyzer_free(analyzer);
    xr_module_graph_free(graph);
    xray_vm_delete(isolate);
    xr_free(entry);
    xr_project_free(project);
    return errors ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}

XR_FUNC int cmd_explain(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    const char *topic = inv->positionals[0];
    const char *input = inv->positional_count > 1 ? inv->positionals[1] : ".";
    if (strcmp(topic, "native") == 0)
        return explain_native(inv, input);
    if (strcmp(topic, "effect") == 0)
        return explain_effect(input);
    xr_cli_error("explain",
                 "unknown evidence topic '%s' (expected native, storage, transfer, ownership, "
                 "view, bounds, alias, effect, or residue)",
                 topic);
    return XR_CLI_EXIT_USAGE;
}
