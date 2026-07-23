/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_verify.c - Versioned semantic/backend contract verification
 */

#include "xcli.h"
#include "xcli_fs.h"
#include "xcli_spec.h"
#include "../../aot/xaot_driver.h"
#include "../../api/xisolate_profile.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../base/xtoml.h"
#include "../../frontend/analyzer/xa_effect_db.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_mono.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../module/xproject.h"
#include "../../runtime/xisolate_api.h"
#include "../../toolchain/xcompiler_session.h"
#include <stdio.h>
#include <string.h>

typedef struct VerifyAnalysis {
    XrProject *project;
    XrVMRuntime *isolate;
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    char *entry;
} VerifyAnalysis;

static bool verify_key_allowed(const char *key, const char *const *allowed, size_t count) {
    if (!key)
        return false;
    for (size_t i = 0; i < count; i++)
        if (strcmp(key, allowed[i]) == 0)
            return true;
    return false;
}

static bool verify_table_schema(XrTomlValue *table, const char *label, const char *const *allowed,
                                size_t allowed_count) {
    if (!table || table->type != XR_TOML_TABLE) {
        xr_cli_error("verify", "%s must be a table", label);
        return false;
    }
    for (int i = 0; i < table->as.table.count; i++) {
        const char *key = table->as.table.members[i].key;
        if (verify_key_allowed(key, allowed, allowed_count))
            continue;
        xr_cli_error("verify", "unknown %s key '%s'", label, key ? key : "");
        return false;
    }
    return true;
}

static bool verify_contract_schema(XrTomlValue *contract) {
    static const char *const root_keys[] = {"version", "function"};
    static const char *const function_keys[] = {"symbol",  "scope",    "backend", "target",
                                                "profile", "requires", "shape"};
    static const char *const shape_keys[] = {"forbid", "allow"};
    if (!verify_table_schema(contract, "contract", root_keys,
                             sizeof(root_keys) / sizeof(root_keys[0])))
        return false;
    XrTomlValue *functions = xtoml_get_array(contract, "function");
    if (!functions || xtoml_array_len(functions) == 0) {
        xr_cli_error("verify", "contract requires at least one [[function]]");
        return false;
    }
    for (int i = 0; i < xtoml_array_len(functions); i++) {
        XrTomlValue *item = xtoml_array_get(functions, i);
        if (!verify_table_schema(item, "function", function_keys,
                                 sizeof(function_keys) / sizeof(function_keys[0])))
            return false;
        XrTomlValue *shape = xtoml_get(item, "shape");
        if (shape && !verify_table_schema(shape, "function.shape", shape_keys,
                                          sizeof(shape_keys) / sizeof(shape_keys[0])))
            return false;
        XrTomlValue *requires = xtoml_get(item, "requires");
        if (!requires || requires->type != XR_TOML_ARRAY || xtoml_array_len(requires) == 0) {
            xr_cli_error("verify", "function requires must be a non-empty string array");
            return false;
        }
        for (int ri = 0; ri < xtoml_array_len(requires); ri++) {
            XrTomlValue *requirement = xtoml_array_get(requires, ri);
            if (!requirement || requirement->type != XR_TOML_STRING) {
                xr_cli_error("verify", "function requires must contain only strings");
                return false;
            }
        }
    }
    return true;
}

static const char *verify_final_component(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot ? dot + 1 : name;
}

static XaSymbol *verify_find_symbol(XrModuleGraph *graph, XaAnalyzer *analyzer,
                                    const char *subject) {
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
            if (strcmp(verify_final_component(entry->key), subject) != 0)
                continue;
            if (short_match && short_match != (XaSymbol *) entry->value)
                return NULL;
            short_match = (XaSymbol *) entry->value;
        }
    }
    if (short_match)
        return short_match;
    XaSymbol *declared_match = NULL;
    const char *final_name = verify_final_component(subject);
    for (int mi = 0; graph && mi < graph->spec_count; mi++) {
        AstNode *root = graph->specs[mi].ast;
        if (!root || root->type != AST_PROGRAM)
            continue;
        for (int si = 0; si < root->as.program.count; si++) {
            AstNode *decl = root->as.program.statements[si];
            if (!decl || decl->type != AST_FUNCTION_DECL || !decl->as.function_decl.name ||
                strcmp(decl->as.function_decl.name, final_name) != 0)
                continue;
            XaSymbol *candidate = xa_scope_lookup_by_id(analyzer ? analyzer->global_scope : NULL,
                                                        decl->as.function_decl.symbol_id);
            if (!candidate || (declared_match && declared_match != candidate))
                return NULL;
            declared_match = candidate;
        }
    }
    if (declared_match)
        return declared_match;
    /* Package contracts also govern private hot paths. They are deliberately
     * not forced into the language's export surface merely to become
     * verifiable. Qualified cross-module names still resolve through the
     * canonical export tables above; the local final component falls back to
     * the analyzed package scope. */
    return analyzer ? xa_scope_lookup(analyzer->global_scope, final_name) : NULL;
}

static void verify_analysis_clear(VerifyAnalysis *state) {
    if (!state)
        return;
    if (state->analyzer) {
        xa_analyzer_set_graph(state->analyzer, NULL);
        xa_analyzer_free(state->analyzer);
    }
    if (state->graph)
        xr_module_graph_free(state->graph);
    if (state->isolate)
        xray_vm_delete(state->isolate);
    xr_free(state->entry);
    xr_project_free(state->project);
    memset(state, 0, sizeof(*state));
}

static bool verify_analyze_project(const char *root, VerifyAnalysis *state) {
    char *graph_error = NULL;
    int errors = 0;
    memset(state, 0, sizeof(*state));
    state->project = xr_project_load(NULL, root);
    if (!state->project || !state->project->initialized || !state->project->main) {
        xr_cli_error("verify", "%s",
                     state->project && state->project->native_plan &&
                             state->project->native_plan->error
                         ? state->project->native_plan->error
                         : "project has no valid main entry");
        return false;
    }
    state->entry = xr_path_join(root, state->project->main);
    state->isolate = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    if (!state->entry || !state->isolate)
        return false;
    xr_module_system_init_with_script(state->isolate, state->entry);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(state->isolate);
    xr_compiler_session_set_native_package_plan(session, state->project->native_plan);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(state->isolate);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    state->graph = resolver ? xr_module_graph_new(session, resolver) : NULL;
    if (!state->graph || xr_module_graph_build(state->graph, state->entry, &graph_error) != 0 ||
        xr_module_graph_topological_sort(state->graph) != 0 || state->graph->has_cycle) {
        xr_cli_error("verify", "%s", graph_error ? graph_error : "module graph build failed");
        xr_free(graph_error);
        return false;
    }
    xr_free(graph_error);
    state->analyzer = xa_analyzer_new(session);
    if (!state->analyzer)
        return false;
    xa_analyzer_set_graph(state->analyzer, state->graph);
    for (int ti = 0; ti < state->graph->topo_count; ti++) {
        XrModuleSpec *spec = &state->graph->specs[state->graph->topo_order[ti]];
        xa_analyzer_analyze(state->analyzer, spec->source_path, (XrAstNode *) spec->ast);
        spec->export_symbols =
            xa_analyzer_collect_export_symbols(state->analyzer, (XrAstNode *) spec->ast);
        int ignored = 0;
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(state->analyzer, &ignored); diag;
             diag = diag->next)
            if (diag->severity == XR_DIAG_SEV_ERROR)
                errors++;
        xa_analyzer_clear_diagnostics(state->analyzer);
    }
    AstNode **roots = (AstNode **) xr_calloc((size_t) state->graph->topo_count, sizeof(AstNode *));
    if (!roots)
        return false;
    for (int ti = 0; ti < state->graph->topo_count; ti++)
        roots[ti] = state->graph->specs[state->graph->topo_order[ti]].ast;
    for (int ti = 0; ti < state->graph->topo_count; ti++) {
        XrModuleSpec *spec = &state->graph->specs[state->graph->topo_order[ti]];
        xa_mono_pass_with_external_structs_and_analyzer(spec->ast, roots, state->graph->topo_count,
                                                        state->isolate, state->analyzer);
    }
    for (int ti = 0; ti < state->graph->topo_count; ti++) {
        XrModuleSpec *spec = &state->graph->specs[state->graph->topo_order[ti]];
        xa_analyzer_analyze(state->analyzer, spec->source_path, (XrAstNode *) spec->ast);
        if (spec->export_symbols)
            xr_hashmap_free(spec->export_symbols);
        spec->export_symbols =
            xa_analyzer_collect_export_symbols(state->analyzer, (XrAstNode *) spec->ast);
        int ignored = 0;
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(state->analyzer, &ignored); diag;
             diag = diag->next)
            if (diag->severity == XR_DIAG_SEV_ERROR)
                errors++;
        xa_analyzer_clear_diagnostics(state->analyzer);
    }
    xr_free(roots);
    if (errors) {
        xr_cli_error("verify", "project analysis failed with %d error(s)", errors);
        return false;
    }
    return true;
}

static bool verify_requirement_effect(const char *requirement, XaAnalyzer *analyzer,
                                      XaSymbol *symbol, char *reason, size_t reason_size) {
    const XaEffectSummary *effect = xa_effect_db_get(analyzer->effect_db, symbol->links.effect_id);
    if (!effect) {
        snprintf(reason, reason_size, "effect summary is missing");
        return false;
    }
    XaSemanticEffectSet forbidden = XA_SEM_EFFECT_NONE;
    if (strcmp(requirement, "no_semantic_alloc") == 0)
        forbidden = XA_SEM_EFFECT_ALLOC;
    else if (strcmp(requirement, "no_suspend") == 0)
        forbidden = XA_SEM_EFFECT_SUSPEND;
    else if (strcmp(requirement, "no_block") == 0)
        forbidden = XA_SEM_EFFECT_MAY_BLOCK | XA_SEM_EFFECT_THREAD_BLOCK;
    else if (strcmp(requirement, "no_thread_block") == 0)
        forbidden = XA_SEM_EFFECT_THREAD_BLOCK;
    else if (strcmp(requirement, "no_panic") == 0)
        forbidden = XA_SEM_EFFECT_PANIC;
    else if (strcmp(requirement, "no_abort") == 0)
        forbidden = XA_SEM_EFFECT_ABORT;
    else if (strcmp(requirement, "no_throw") == 0) {
        if (xa_effect_summary_is_nothrow(effect))
            return true;
        snprintf(reason, reason_size, "escaping-errors=%u completeness=%u unknown=0x%x",
                 effect->escaping.count, (unsigned) effect->error_set_completeness,
                 effect->error_unknown_reasons);
        return false;
    } else if (strcmp(requirement, "no_runtime_heap") == 0) {
        /* Checked against backend residue after code generation. */
        return true;
    } else {
        snprintf(reason, reason_size, "unknown requirement '%s'", requirement);
        return false;
    }
    if ((effect->semantic_effects & forbidden) != 0) {
        snprintf(reason, reason_size, "forbidden semantic effect bits 0x%x",
                 effect->semantic_effects & forbidden);
        return false;
    }
    /* Completeness is a product. A missing error/native-return contract must
     * not erase an independently complete allocation or suspend fact. Only
     * unknownness for the specific forbidden semantic bits blocks this
     * requirement; no_throw handles its error-set completeness above. */
    if ((effect->unknown_semantic_effects & forbidden) != 0) {
        snprintf(reason, reason_size, "proof incomplete unknown=0x%x unknown-semantic=0x%x",
                 effect->unknown_reasons, effect->unknown_semantic_effects & forbidden);
        return false;
    }
    return true;
}

static int verify_shape_category(const char *name) {
    if (!name)
        return -1;
    if (strcmp(name, "runtime_dispatch") == 0)
        return XI_RESIDUE_R1_RUNTIME_CALL;
    if (strcmp(name, "runtime_heap") == 0)
        return XI_RESIDUE_R2_HEAP_ALLOC;
    if (strcmp(name, "pending_error") == 0)
        return XI_RESIDUE_R3_PENDING_ERROR;
    if (strcmp(name, "bounds_in_loop") == 0)
        return XI_RESIDUE_R4_BOUNDS_PANIC;
    if (strcmp(name, "box") == 0)
        return XI_RESIDUE_R5_BOX_UNBOX;
    if (strcmp(name, "lane_spill") == 0)
        return XI_RESIDUE_R6_LANES_ROUNDTRIP;
    return -1;
}

static bool verify_parse_residue_row(const char *dump, const char *symbol,
                                     uint32_t counts[XI_RESIDUE_CATEGORY_COUNT]) {
    const char *line = dump;
    bool found = false;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t) (end - line) : strlen(line);
        if (len && line[0] != '#' && strncmp(line, "function\t", 9) != 0) {
            char name[256], source[512];
            unsigned values[7];
            if (sscanf(line, "%255[^\t]\t%511[^\t]\t%u\t%u\t%u\t%u\t%u\t%u\t%u", name, source,
                       &values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
                       &values[6]) == 9 &&
                (strcmp(name, symbol) == 0 ||
                 strcmp(verify_final_component(name), verify_final_component(symbol)) == 0)) {
                if (found)
                    return false;
                for (int i = 0; i < XI_RESIDUE_CATEGORY_COUNT; i++)
                    counts[i] = values[i];
                found = true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return found;
}

static bool verify_backend_contract(const VerifyAnalysis *state, XrTomlValue *item,
                                    const char *symbol, XrTomlValue *requires) {
    const char *backend = xtoml_get_string(item, "backend");
    const char *target_name = xtoml_get_string(item, "target");
    const char *profile_name = xtoml_get_string(item, "profile");
    if (!backend || strcmp(backend, "aot") != 0 || !target_name || !profile_name ||
        strcmp(target_name, "*") == 0 || strcmp(profile_name, "*") == 0) {
        xr_cli_error("verify",
                     "backend contract '%s' requires concrete backend=aot, target, and profile",
                     symbol);
        return false;
    }
    XaotTarget target = {0};
    XaotBuildResult result = {0};
    XaotBuildOptions options = {0};
    if (!xaot_target_init(&target, target_name)) {
        xr_cli_error("verify", "unsupported contract target '%s'", target_name);
        return false;
    }
    options.target = &target;
    options.native_package_plan = state->project->native_plan;
    options.profile = strcmp(profile_name, "freestanding") == 0 ? XAOT_BUILD_PROFILE_FREESTANDING
                                                                : XAOT_BUILD_PROFILE_HOSTED;
    options.type_name_profile = XI_CGEN_TYPE_NAMES_ALL;
    options.emit_residue_dump = true;
    options.quiet = true;
    bool ok = xaot_build(state->entry, &options, &result) == 0 && result.residue_dump;
    uint32_t counts[XI_RESIDUE_CATEGORY_COUNT] = {0};
    if (ok)
        ok = verify_parse_residue_row(result.residue_dump, symbol, counts);
    if (!ok)
        xr_cli_error("verify", "backend residue for '%s' is missing or ambiguous", symbol);
    uint32_t forbid = 0;
    XrTomlValue *shape = xtoml_get_table(item, "shape");
    XrTomlValue *forbid_values = shape ? xtoml_get_array(shape, "forbid") : NULL;
    XrTomlValue *allow_values = shape ? xtoml_get_array(shape, "allow") : NULL;
    for (int i = 0; forbid_values && i < xtoml_array_len(forbid_values); i++) {
        XrTomlValue *value = xtoml_array_get(forbid_values, i);
        int category =
            value && value->type == XR_TOML_STRING ? verify_shape_category(value->as.string) : -1;
        if (category < 0) {
            xr_cli_error("verify", "unknown shape category in '%s'", symbol);
            ok = false;
        } else {
            forbid |= 1u << category;
        }
    }
    for (int i = 0; allow_values && i < xtoml_array_len(allow_values); i++) {
        XrTomlValue *value = xtoml_array_get(allow_values, i);
        int category =
            value && value->type == XR_TOML_STRING ? verify_shape_category(value->as.string) : -1;
        if (category < 0) {
            xr_cli_error("verify", "unknown shape allow category in '%s'", symbol);
            ok = false;
        } else {
            forbid &= ~(1u << category);
        }
    }
    for (int i = 0; requires && i < xtoml_array_len(requires); i++) {
        XrTomlValue *value = xtoml_array_get(requires, i);
        if (value && value->type == XR_TOML_STRING &&
            strcmp(value->as.string, "no_runtime_heap") == 0)
            forbid |= 1u << XI_RESIDUE_R2_HEAP_ALLOC;
    }
    for (int i = 0; i < XI_RESIDUE_CATEGORY_COUNT; i++) {
        if ((forbid & (1u << i)) == 0 || counts[i] == 0)
            continue;
        xr_cli_error("verify", "contract '%s' failed: %s count=%u; witness: %s -> %s", symbol,
                     xi_residue_category_label((XiResidueCategory) i), counts[i], symbol,
                     xi_residue_category_short((XiResidueCategory) i));
        ok = false;
    }
    xaot_build_result_free(&result);
    xaot_target_free(&target);
    return ok;
}

XR_FUNC int cmd_verify(const XrCliInvocation *inv) {
    const char *contract_path = xr_cli_opt_string(&inv->options, "contract", NULL);
    size_t size = 0;
    char *source = contract_path ? xr_file_read_all(contract_path, "r", &size) : NULL;
    if (!source) {
        xr_cli_error("verify", "cannot read contract '%s'", contract_path ? contract_path : "");
        return XR_CLI_EXIT_USAGE;
    }
    XrTomlValue *contract = xtoml_parse(source, size);
    xr_free(source);
    if (!contract || xtoml_get_int_or(contract, "version", 0) != 1) {
        xr_cli_error("verify", "contract schema version must be 1");
        xtoml_free(contract);
        return XR_CLI_EXIT_USAGE;
    }
    if (!verify_contract_schema(contract)) {
        xtoml_free(contract);
        return XR_CLI_EXIT_USAGE;
    }
    XrTomlValue *functions = xtoml_get_array(contract, "function");
    char root[XR_CLI_PATH_MAX];
    if (!xr_cli_find_project_root(".", root, sizeof(root))) {
        xr_cli_error("verify", "no xray.toml found");
        xtoml_free(contract);
        return XR_CLI_EXIT_USAGE;
    }
    VerifyAnalysis state;
    if (!verify_analyze_project(root, &state)) {
        verify_analysis_clear(&state);
        xtoml_free(contract);
        return XR_CLI_EXIT_FAIL;
    }
    int failures = 0;
    for (int i = 0; i < xtoml_array_len(functions); i++) {
        XrTomlValue *item = xtoml_array_get(functions, i);
        const char *symbol_name = xtoml_get_string(item, "symbol");
        const char *scope = xtoml_get_string(item, "scope");
        XrTomlValue *requires = xtoml_get_array(item, "requires");
        XaSymbol *symbol = verify_find_symbol(state.graph, state.analyzer, symbol_name);
        if (!symbol) {
            xr_cli_error("verify", "canonical symbol '%s' is missing or ambiguous",
                         symbol_name ? symbol_name : "");
            failures++;
            continue;
        }
        if (!requires || !scope ||
            (strcmp(scope, "semantic") != 0 && strcmp(scope, "backend") != 0)) {
            xr_cli_error("verify", "function '%s' requires scope and requires[]", symbol_name);
            failures++;
            continue;
        }
        for (int ri = 0; ri < xtoml_array_len(requires); ri++) {
            XrTomlValue *requirement = xtoml_array_get(requires, ri);
            char reason[256];
            if (!requirement || requirement->type != XR_TOML_STRING ||
                !verify_requirement_effect(requirement ? requirement->as.string : "",
                                           state.analyzer, symbol, reason, sizeof(reason))) {
                xr_cli_error("verify", "contract '%s' failed: %s; witness: %s -> effect summary",
                             symbol_name, reason, symbol_name);
                failures++;
            }
        }
        if (strcmp(scope, "backend") == 0 &&
            !verify_backend_contract(&state, item, symbol_name, requires))
            failures++;
        if (failures == 0)
            printf("verified symbol-id=%u symbol=%s scope=%s\n", symbol->id, symbol_name, scope);
    }
    verify_analysis_clear(&state);
    xtoml_free(contract);
    if (failures) {
        xr_cli_error("verify", "%d contract check(s) failed", failures);
        return XR_CLI_EXIT_FAIL;
    }
    return XR_CLI_EXIT_OK;
}
