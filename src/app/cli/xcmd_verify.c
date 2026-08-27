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
#include "../toolchain/xtc_shape_oracle.h"
#include "../toolchain/xtc_target_profile.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VerifyAnalysis {
    XrProject *project;
    XrVMRuntime *isolate;
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    char *entry;
    XrModuleIdentityAuthority entry_authority;
    char *entry_authority_namespace;
    char *entry_authority_root;
} VerifyAnalysis;

static bool verify_native_target_profile(const char *target_name, XaotBuildProfile profile,
                                         const XaotTarget *aot_target, XrTargetProfile **out) {
    if (out)
        *out = NULL;
    if (!out || profile != XAOT_BUILD_PROFILE_HOSTED)
        return false;
    XrTargetCodegenFacts codegen;
    XrToolchainTarget target;
    char error[256];
    return xaot_target_profile_codegen_facts(aot_target, &codegen) &&
           xtc_target_parse(target_name ? target_name : "native", &target, error, sizeof(error)) &&
           xtc_target_profile_build_native_hosted(&target, &codegen, out, error, sizeof(error));
}

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
    static const char *const root_keys[] = {"version", "function", "codegen"};
    static const char *const function_keys[] = {"symbol",  "scope",    "backend", "target",
                                                "profile", "requires", "shape"};
    static const char *const shape_keys[] = {"forbid", "allow"};
    static const char *const codegen_keys[] = {"control", "edge"};
    static const char *const control_keys[] = {"subject", "kind",     "minimum_stage",
                                               "target",  "provider", "profile"};
    static const char *const edge_keys[] = {"caller", "callee",   "expect", "minimum_stage",
                                            "target", "provider", "profile"};
    if (!verify_table_schema(contract, "contract", root_keys,
                             sizeof(root_keys) / sizeof(root_keys[0])))
        return false;
    XrTomlValue *functions = xtoml_get_array(contract, "function");
    XrTomlValue *codegen = xtoml_get_table(contract, "codegen");
    XrTomlValue *controls = codegen ? xtoml_get_array(codegen, "control") : NULL;
    XrTomlValue *edges = codegen ? xtoml_get_array(codegen, "edge") : NULL;
    if ((!functions || xtoml_array_len(functions) == 0) &&
        (!controls || xtoml_array_len(controls) == 0) && (!edges || xtoml_array_len(edges) == 0)) {
        xr_cli_error("verify",
                     "contract requires [[function]], [[codegen.control]], or [[codegen.edge]]");
        return false;
    }
    for (int i = 0; functions && i < xtoml_array_len(functions); i++) {
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
    if (codegen && !verify_table_schema(codegen, "codegen", codegen_keys,
                                        sizeof(codegen_keys) / sizeof(codegen_keys[0])))
        return false;
    for (int i = 0; controls && i < xtoml_array_len(controls); i++) {
        XrTomlValue *item = xtoml_array_get(controls, i);
        if (!verify_table_schema(item, "codegen.control", control_keys,
                                 sizeof(control_keys) / sizeof(control_keys[0])))
            return false;
        if (!xtoml_get_string(item, "subject") || !xtoml_get_string(item, "kind") ||
            !xtoml_get_string(item, "minimum_stage")) {
            xr_cli_error("verify",
                         "codegen.control requires subject, kind, and minimum_stage strings");
            return false;
        }
    }
    for (int i = 0; edges && i < xtoml_array_len(edges); i++) {
        XrTomlValue *item = xtoml_array_get(edges, i);
        if (!verify_table_schema(item, "codegen.edge", edge_keys,
                                 sizeof(edge_keys) / sizeof(edge_keys[0])))
            return false;
        if (!xtoml_get_string(item, "caller") || !xtoml_get_string(item, "callee") ||
            !xtoml_get_string(item, "expect") || !xtoml_get_string(item, "minimum_stage")) {
            xr_cli_error("verify",
                         "codegen.edge requires caller, callee, expect, and minimum_stage strings");
            return false;
        }
    }
    return true;
}

static const char *verify_final_component(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot ? dot + 1 : name;
}

static XaScope *verify_file_scope(XaAnalyzer *analyzer, const char *source_path) {
    if (!analyzer || !source_path)
        return NULL;
    for (XaFileEntry *entry = analyzer->files; entry; entry = entry->next)
        if (entry->path && strcmp(entry->path, source_path) == 0)
            return entry->file_scope;
    return NULL;
}

static XaSymbol *verify_find_symbol(XrModuleGraph *graph, XaAnalyzer *analyzer,
                                    const char *subject) {
    if (!graph || !subject)
        return NULL;

    /* An unqualified contract subject names a declaration, not an arbitrary
     * re-export key. Resolve package declarations first so unrelated facade
     * aliases with the same final component cannot make a unique private hot
     * path appear ambiguous. Duplicate declarations still fail closed. */
    if (!strchr(subject, '.')) {
        XaSymbol *declared_match = NULL;
        for (int mi = 0; mi < graph->spec_count; mi++) {
            AstNode *root = graph->specs[mi].ast;
            if (!root || root->type != AST_PROGRAM)
                continue;
            for (int si = 0; si < root->as.program.count; si++) {
                AstNode *decl = root->as.program.statements[si];
                if (!decl || decl->type != AST_FUNCTION_DECL || !decl->as.function_decl.name ||
                    strcmp(decl->as.function_decl.name, subject) != 0)
                    continue;
                XaScope *file_scope = verify_file_scope(analyzer, graph->specs[mi].source_path);
                XaSymbol *candidate = xa_scope_lookup_local(file_scope, subject);
                if (!candidate || (declared_match && declared_match != candidate))
                    return NULL;
                declared_match = candidate;
            }
        }
        if (declared_match)
            return declared_match;
    }

    XaSymbol *exact_match = NULL;
    for (int i = 0; i < graph->spec_count; i++) {
        XrHashMap *exports = graph->specs[i].export_symbols;
        if (!exports)
            continue;
        XaSymbol *exact = (XaSymbol *) xr_hashmap_get(exports, subject);
        if (exact) {
            if (exact_match && exact_match != exact)
                return NULL;
            exact_match = exact;
        }
    }
    if (exact_match)
        return exact_match;

    XaSymbol *short_match = NULL;
    for (int i = 0; i < graph->spec_count; i++) {
        XrHashMap *exports = graph->specs[i].export_symbols;
        if (!exports)
            continue;
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
    const char *final_name = verify_final_component(subject);
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
    xr_free(state->entry_authority_namespace);
    xr_free(state->entry_authority_root);
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
    if (!state->entry) {
        xr_cli_error("verify", "cannot resolve entry path for '%s'", state->project->main);
        return false;
    }
    state->isolate = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    if (!state->isolate) {
        xr_cli_error("verify", "cannot create the analysis isolate");
        return false;
    }
    if (!xr_project_module_identity_authority(state->project, &state->entry_authority,
                                              &state->entry_authority_namespace,
                                              &state->entry_authority_root)) {
        xr_cli_error("verify", "failed to establish exact entry module identity authority");
        return false;
    }
    xr_module_system_init_with_script(state->isolate, state->entry);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(state->isolate);
    xr_compiler_session_set_native_package_plan(session, state->project->native_plan);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(state->isolate);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    state->graph = resolver ? xr_module_graph_new(session, resolver) : NULL;
    if (!state->graph ||
        xr_module_graph_build(state->graph, state->entry, &state->entry_authority, &graph_error) !=
            0 ||
        xr_module_graph_topological_sort(state->graph) != 0 || state->graph->has_cycle) {
        xr_cli_error("verify", "%s", graph_error ? graph_error : "module graph build failed");
        xr_free(graph_error);
        return false;
    }
    xr_free(graph_error);
    state->analyzer = xa_analyzer_new(session);
    if (!state->analyzer) {
        xr_cli_error("verify", "cannot create the analyzer");
        return false;
    }
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
        /* A budget failure leaves calls unspecialized, so any shape contract
         * proven below would describe code that was never going to be built.
         * The diagnostic is also counted in the analysis loop; failing here
         * makes verify's dependence on it explicit rather than incidental. */
        if (!xa_mono_pass(spec->ast, roots, state->graph->topo_count, state->isolate,
                          state->analyzer))
            errors++;
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

/* Semantic effect bits that an analysis pass in this compiler build actually
 * infers.  A requirement naming a bit outside this mask has no evidence source
 * at all, so granting it would report an unproven guarantee as proven.  Such a
 * requirement fails closed until its inference lands, and this mask is what
 * grows when it does. */
#define VERIFY_INFERRED_SEMANTIC_EFFECTS                                                           \
    (XA_SEM_EFFECT_ALLOC | XA_SEM_EFFECT_SCHED_SUSPEND | XA_SEM_EFFECT_GEN_SUSPEND)

/* Does this symbol's own type close a cycle in the L0 reference graph?
 *
 * `is_cycle_candidate` is set by xa_mark_cycle_candidates on every class the
 * DFS found on a cycle. A `weak` field breaks the edge there, so annotating one
 * is exactly how a user takes their class out of the candidate set — and thus
 * how they make this contract pass. */
static bool verify_no_reference_cycles(XaAnalyzer *analyzer, XaSymbol *symbol, char *reason,
                                       size_t reason_size) {
    (void) analyzer;
    XrType *type = symbol ? symbol->links.type : NULL;
    if (!type)
        type = symbol ? symbol->links.declared_type : NULL;
    if (type && type->is_cycle_candidate) {
        snprintf(reason, reason_size,
                 "cannot prove acyclicity: '%s' is on a cycle in the compile-time type graph "
                 "(a recursive type reads the same as a cycle at this level). Break the edge "
                 "with a `weak` field, or drop the contract here and use the test-mode detector",
                 symbol->name ? symbol->name : "?");
        return false;
    }
    return true;
}

static bool verify_requirement_effect(const char *requirement, XaAnalyzer *analyzer,
                                      XaSymbol *symbol, char *reason, size_t reason_size) {
    /* Answered from the L0 type graph, before the effect lookup: this contract
     * is normally written over a CLASS, and a class carries no effect summary.
     * Requiring one would fail it for the wrong reason. */
    if (strcmp(requirement, "no_reference_cycles") == 0)
        return verify_no_reference_cycles(analyzer, symbol, reason, reason_size);

    const XaEffectSummary *effect = xa_effect_db_get(analyzer->effect_db, symbol->links.effect_id);
    if (!effect) {
        snprintf(reason, reason_size, "effect summary is missing");
        return false;
    }
    XaSemanticEffectSet forbidden = XA_SEM_EFFECT_NONE;
    if (strcmp(requirement, "no_semantic_alloc") == 0)
        forbidden = XA_SEM_EFFECT_ALLOC;
    /* `no_suspend` is the strong form: control never leaves this frame at all.
     * `no_reschedule` is the weaker and more often useful one: the frame may be
     * a generator, but control never reaches the scheduler, so the body is not a
     * cancellation point and cannot migrate to another OS thread. */
    else if (strcmp(requirement, "no_suspend") == 0)
        forbidden = XA_SEM_EFFECT_ANY_SUSPEND;
    else if (strcmp(requirement, "no_reschedule") == 0)
        forbidden = XA_SEM_EFFECT_SCHED_SUSPEND;
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
    XaSemanticEffectSet uninferred =
        forbidden & ~(XaSemanticEffectSet) VERIFY_INFERRED_SEMANTIC_EFFECTS;
    if (uninferred != 0) {
        snprintf(reason, reason_size,
                 "requirement '%s' has no inference source in this compiler build "
                 "(semantic effect bits 0x%x are never computed); it cannot be proven",
                 requirement, uninferred);
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
    if (strcmp(name, "rc_traffic") == 0)
        return XI_RESIDUE_R7_RC_TRAFFIC;
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
            unsigned values[8];
            if (sscanf(line, "%255[^\t]\t%511[^\t]\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u", name, source,
                       &values[0], &values[1], &values[2], &values[3], &values[4], &values[5],
                       &values[6], &values[7]) == 10 &&
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
    XrTargetProfile *target_profile = NULL;
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
    if (!verify_native_target_profile(target_name, options.profile, &target, &target_profile)) {
        xr_cli_error("verify", "exact TargetProfile authority is unavailable for '%s'",
                     target_name);
        xaot_target_free(&target);
        return false;
    }
    options.target_profile = target_profile;
    options.entry_module_authority = state->entry_authority;
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
            strcmp(value->as.string, "no_runtime_heap") == 0) {
            /* A heap-free export cannot hide ownership traffic behind ARC.
             * Both facts come from the live backend residue row, not from a
             * generated-C token scan: R2 is allocation and R7 is retain /
             * release traffic after all plan-driven lowering has finished. */
            forbid |= 1u << XI_RESIDUE_R2_HEAP_ALLOC;
            forbid |= 1u << XI_RESIDUE_R7_RC_TRAFFIC;
        }
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
    xr_target_profile_free(target_profile);
    xaot_target_free(&target);
    return ok;
}

static int verify_codegen_stage_rank(const char *stage) {
    if (stage && strcmp(stage, "requested") == 0)
        return 0;
    if (stage && strcmp(stage, "planned") == 0)
        return 1;
    if (stage && strcmp(stage, "lowered") == 0)
        return 2;
    if (stage && strcmp(stage, "realized") == 0)
        return 3;
    return -1;
}

static bool verify_codegen_line_field(const char *line, size_t line_len, const char *field,
                                      char *out, size_t out_size) {
    const char *limit = line + line_len;
    size_t field_len = strlen(field);
    const char *match = line;
    if (!out || out_size < 2)
        return false;
    while (match + field_len <= limit) {
        match = strstr(match, field);
        if (!match || match + field_len > limit)
            return false;
        if (match == line || match[-1] == ' ') {
            const char *start = match + field_len;
            const char *end = start;
            while (end < limit && *end != ' ' && *end != '\t' && *end != '\r')
                end++;
            size_t len = (size_t) (end - start);
            if (len == 0 || len >= out_size)
                return false;
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
        match += field_len;
    }
    return false;
}

static bool verify_codegen_symbol_matches(const char *expected, const char *actual) {
    return expected && actual &&
           (strcmp(expected, actual) == 0 ||
            strcmp(verify_final_component(expected), verify_final_component(actual)) == 0);
}

static const char *verify_codegen_ledger_kind(const char *contract_kind) {
    if (!contract_kind)
        return NULL;
    if (strcmp(contract_kind, "prefer_inline") == 0 || strcmp(contract_kind, "force_inline") == 0 ||
        strcmp(contract_kind, "inline") == 0)
        return "inline";
    if (strcmp(contract_kind, "preserve_call") == 0 || strcmp(contract_kind, "noinline") == 0)
        return "noinline";
    if (strcmp(contract_kind, "value_opaque") == 0 || strcmp(contract_kind, "opaque") == 0)
        return "opaque";
    if (strcmp(contract_kind, "compiler_fence") == 0 || strcmp(contract_kind, "compilerFence") == 0)
        return "compilerFence";
    return NULL;
}

static bool verify_codegen_scope_supported(XrTomlValue *item, const char *label) {
    const char *minimum_stage = xtoml_get_string(item, "minimum_stage");
    const char *profile = xtoml_get_string(item, "profile");
    const char *provider = xtoml_get_string(item, "provider");
    int minimum_rank = verify_codegen_stage_rank(minimum_stage);
    if (minimum_rank < 0) {
        xr_cli_error("verify", "%s has unknown minimum_stage '%s'", label,
                     minimum_stage ? minimum_stage : "");
        return false;
    }
    if (profile && strcmp(profile, "native") != 0 && strcmp(profile, "hosted") != 0 &&
        strcmp(profile, "freestanding") != 0 && strcmp(profile, "*") != 0) {
        xr_cli_error("verify", "%s has unsupported profile '%s'", label, profile);
        return false;
    }
    if (provider && strcmp(provider, "*") != 0) {
        XrToolchainSelector ignored;
        char parse_err[128];
        if (!xtc_selector_parse(provider, &ignored, parse_err, sizeof(parse_err))) {
            xr_cli_error("verify", "%s has unsupported provider '%s'", label, provider);
            return false;
        }
    }
    return true;
}

static bool verify_codegen_control(const char *dump, XrTomlValue *item) {
    const char *subject = xtoml_get_string(item, "subject");
    const char *contract_kind = xtoml_get_string(item, "kind");
    const char *ledger_kind = verify_codegen_ledger_kind(contract_kind);
    char label[512];
    snprintf(label, sizeof(label), "codegen.control '%s:%s'", subject ? subject : "",
             contract_kind ? contract_kind : "");
    if (!ledger_kind) {
        xr_cli_error("verify", "%s has unknown kind", label);
        return false;
    }
    if (!verify_codegen_scope_supported(item, label))
        return false;
    int required_rank = verify_codegen_stage_rank(xtoml_get_string(item, "minimum_stage"));
    int ledger_rank = required_rank > 2 ? 2 : required_rank;
    const char *line = dump;
    unsigned matches = 0;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t) (end - line) : strlen(line);
        char function[256], kind[64], stage[32];
        if (len >= 16 && strncmp(line, "codegen-control ", 16) == 0 &&
            verify_codegen_line_field(line, len, "function=", function, sizeof(function)) &&
            verify_codegen_line_field(line, len, "kind=", kind, sizeof(kind)) &&
            verify_codegen_line_field(line, len, "stage=", stage, sizeof(stage)) &&
            verify_codegen_symbol_matches(subject, function) && strcmp(kind, ledger_kind) == 0 &&
            verify_codegen_stage_rank(stage) >= ledger_rank)
            matches++;
        line = end ? end + 1 : NULL;
    }
    if (matches == 0) {
        xr_cli_error("verify", "%s failed: no canonical control reached minimum stage", label);
        return false;
    }
    printf("verified %s stage=%s matches=%u\n", label,
           required_rank > 2 ? "lowered (realization pending)"
                             : xtoml_get_string(item, "minimum_stage"),
           matches);
    return true;
}

static bool verify_codegen_edge(const char *dump, XrTomlValue *item) {
    const char *caller = xtoml_get_string(item, "caller");
    const char *callee = xtoml_get_string(item, "callee");
    const char *expect = xtoml_get_string(item, "expect");
    char label[600];
    snprintf(label, sizeof(label), "codegen.edge '%s->%s:%s'", caller ? caller : "",
             callee ? callee : "", expect ? expect : "");
    if (!verify_codegen_scope_supported(item, label))
        return false;
    bool expects_preserved =
        expect && (strcmp(expect, "preserved") == 0 || strcmp(expect, "preserve_call") == 0);
    bool expects_eliminated = expect && strcmp(expect, "eliminated") == 0;
    int required_rank = verify_codegen_stage_rank(xtoml_get_string(item, "minimum_stage"));
    if (!expects_preserved && !expects_eliminated) {
        xr_cli_error("verify", "%s has unknown edge expectation", label);
        return false;
    }
    if (expects_eliminated && required_rank < 3) {
        xr_cli_error("verify",
                     "%s cannot be proven before native realization; eliminated edges require "
                     "the realized object/assembly oracle",
                     label);
        return false;
    }
    if (expects_eliminated) {
        printf("verified %s stage=Xi absence (realization pending) matches=0\n", label);
        return true;
    }
    const char *line = dump;
    unsigned matches = 0;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t) (end - line) : strlen(line);
        char line_caller[256], line_callee[256], stage[32];
        if (len >= 13 && strncmp(line, "codegen-edge ", 13) == 0 &&
            verify_codegen_line_field(line, len, "caller=", line_caller, sizeof(line_caller)) &&
            verify_codegen_line_field(line, len, "callee=", line_callee, sizeof(line_callee)) &&
            verify_codegen_line_field(line, len, "stage=", stage, sizeof(stage)) &&
            verify_codegen_symbol_matches(caller, line_caller) &&
            verify_codegen_symbol_matches(callee, line_callee) &&
            verify_codegen_stage_rank(stage) >= (required_rank > 2 ? 2 : required_rank))
            matches++;
        line = end ? end + 1 : NULL;
    }
    if (matches == 0) {
        xr_cli_error("verify", "%s failed: no surviving canonical direct edge", label);
        return false;
    }
    printf("verified %s stage=%s matches=%u\n", label,
           required_rank > 2 ? "lowered (realization pending)"
                             : xtoml_get_string(item, "minimum_stage"),
           matches);
    return true;
}

typedef struct VerifyRealizedConfig {
    bool needed;
    char target[128];
    char provider[32];
    char profile[32];
    uint32_t required_capabilities;
} VerifyRealizedConfig;

static bool verify_codegen_merge_scope(char *slot, size_t slot_size, const char *value,
                                       const char *field) {
    if (!value || !value[0] || strcmp(value, "*") == 0)
        return true;
    if (slot[0] && strcmp(slot, value) != 0) {
        xr_cli_error("verify", "realized codegen contracts mix %s '%s' and '%s'", field, slot,
                     value);
        return false;
    }
    snprintf(slot, slot_size, "%s", value);
    return true;
}

static uint32_t verify_codegen_capability_for_kind(const char *kind) {
    const char *ledger = verify_codegen_ledger_kind(kind);
    if (!ledger)
        return 0;
    if (strcmp(ledger, "inline") == 0)
        return XR_TOOLCHAIN_CODEGEN_FORCE_INLINE;
    if (strcmp(ledger, "noinline") == 0)
        return XR_TOOLCHAIN_CODEGEN_PRESERVE_CALL;
    if (strcmp(ledger, "opaque") == 0)
        return XR_TOOLCHAIN_CODEGEN_VALUE_OPAQUE;
    if (strcmp(ledger, "compilerFence") == 0)
        return XR_TOOLCHAIN_CODEGEN_COMPILER_FENCE;
    return 0;
}

static bool verify_codegen_realized_config(XrTomlValue *controls, XrTomlValue *edges,
                                           VerifyRealizedConfig *config) {
    memset(config, 0, sizeof(*config));
    for (int group = 0; group < 2; group++) {
        XrTomlValue *items = group == 0 ? controls : edges;
        for (int i = 0; items && i < xtoml_array_len(items); i++) {
            XrTomlValue *item = xtoml_array_get(items, i);
            if (verify_codegen_stage_rank(xtoml_get_string(item, "minimum_stage")) < 3)
                continue;
            config->needed = true;
            if (!verify_codegen_merge_scope(config->target, sizeof(config->target),
                                            xtoml_get_string(item, "target"), "target") ||
                !verify_codegen_merge_scope(config->provider, sizeof(config->provider),
                                            xtoml_get_string(item, "provider"), "provider") ||
                !verify_codegen_merge_scope(config->profile, sizeof(config->profile),
                                            xtoml_get_string(item, "profile"), "profile"))
                return false;
            if (group == 0)
                config->required_capabilities |=
                    verify_codegen_capability_for_kind(xtoml_get_string(item, "kind"));
        }
    }
    return true;
}

static bool verify_codegen_ident_char(char ch) {
    return isalnum((unsigned char) ch) || ch == '_';
}

static bool verify_codegen_generated_symbol(const char *generated_c, const char *subject, char *out,
                                            size_t out_size, char *reason, size_t reason_size) {
    const char *component = verify_final_component(subject);
    size_t component_len = component ? strlen(component) : 0;
    const char *scan = generated_c;
    char unique[512] = {0};
    if (!generated_c || component_len == 0) {
        snprintf(reason, reason_size, "missing generated C or subject");
        return false;
    }
    while ((scan = strstr(scan, component)) != NULL) {
        const char *start = scan;
        const char *end = scan + component_len;
        while (start > generated_c && verify_codegen_ident_char(start[-1]))
            start--;
        while (verify_codegen_ident_char(*end))
            end++;
        bool component_boundary =
            (scan == start || scan[-1] == '_') &&
            (scan[component_len] == '_' || !verify_codegen_ident_char(scan[component_len]));
        const char *after = end;
        while (*after == ' ' || *after == '\t')
            after++;
        size_t len = (size_t) (end - start);
        if (component_boundary && *after == '(' && len > component_len && len < sizeof(unique)) {
            char candidate[512];
            memcpy(candidate, start, len);
            candidate[len] = '\0';
            if (!strstr(candidate, "_cfn") && !strstr(candidate, "_bridge")) {
                if (!unique[0])
                    snprintf(unique, sizeof(unique), "%s", candidate);
                else if (strcmp(unique, candidate) != 0) {
                    snprintf(reason, reason_size,
                             "subject maps to multiple generated symbols ('%s', '%s')", unique,
                             candidate);
                    return false;
                }
            }
        }
        scan += component_len;
    }
    if (!unique[0]) {
        snprintf(reason, reason_size, "no unique generated symbol for '%s'", subject);
        return false;
    }
    if (strlen(unique) >= out_size) {
        snprintf(reason, reason_size, "generated symbol is too long");
        return false;
    }
    snprintf(out, out_size, "%s", unique);
    return true;
}

static const char *verify_codegen_line_end(const char *line) {
    const char *end = strchr(line, '\n');
    return end ? end : line + strlen(line);
}

static bool verify_codegen_line_contains_token(const char *line, size_t len, const char *token) {
    size_t token_len = strlen(token);
    for (const char *p = line; p + token_len <= line + len; p++) {
        if (memcmp(p, token, token_len) != 0)
            continue;
        bool left = p == line || !verify_codegen_ident_char(p[-1]);
        bool right = p + token_len == line + len || !verify_codegen_ident_char(p[token_len]);
        if (left && right)
            return true;
    }
    return false;
}

static bool verify_codegen_asm_function(const char *assembly, const char *symbol,
                                        const char **out_start, const char **out_end) {
    const char *line = assembly;
    size_t symbol_len = strlen(symbol);
    while (line && *line) {
        const char *end = verify_codegen_line_end(line);
        const char *trim = line;
        while (trim < end && (*trim == ' ' || *trim == '\t'))
            trim++;
        const char *name = trim[0] == '_' ? trim + 1 : trim;
        bool label = (size_t) (end - name) > symbol_len && memcmp(name, symbol, symbol_len) == 0 &&
                     name[symbol_len] == ':';
        bool proc = verify_codegen_line_contains_token(trim, (size_t) (end - trim), symbol) &&
                    verify_codegen_line_contains_token(trim, (size_t) (end - trim), "PROC");
        if (label || proc) {
            const char *body = end < line + strlen(line) ? end + 1 : end;
            const char *cursor = body;
            while (cursor && *cursor) {
                const char *cursor_end = verify_codegen_line_end(cursor);
                size_t cursor_len = (size_t) (cursor_end - cursor);
                bool terminal =
                    verify_codegen_line_contains_token(cursor, cursor_len, ".seh_endproc") ||
                    verify_codegen_line_contains_token(cursor, cursor_len, ".cfi_endproc") ||
                    (verify_codegen_line_contains_token(cursor, cursor_len, symbol) &&
                     verify_codegen_line_contains_token(cursor, cursor_len, "ENDP")) ||
                    (verify_codegen_line_contains_token(cursor, cursor_len, ".size") &&
                     verify_codegen_line_contains_token(cursor, cursor_len, symbol));
                if (terminal) {
                    *out_start = body;
                    *out_end = cursor_end;
                    return true;
                }
                cursor = *cursor_end ? cursor_end + 1 : NULL;
            }
            *out_start = body;
            *out_end = assembly + strlen(assembly);
            return true;
        }
        line = *end ? end + 1 : NULL;
    }
    return false;
}

static unsigned verify_codegen_asm_direct_edges(const char *start, const char *end,
                                                const char *callee) {
    unsigned matches = 0;
    const char *line = start;
    static const char *const mnemonics[] = {"call", "callq", "jmp", "jmpq", "bl", "b"};
    while (line && line < end) {
        const char *line_end = strchr(line, '\n');
        if (!line_end || line_end > end)
            line_end = end;
        const char *comment = memchr(line, '#', (size_t) (line_end - line));
        const char *semicolon = memchr(line, ';', (size_t) (line_end - line));
        const char *code_end = line_end;
        if (comment && comment < code_end)
            code_end = comment;
        if (semicolon && semicolon < code_end)
            code_end = semicolon;
        size_t code_len = (size_t) (code_end - line);
        bool instruction = false;
        for (size_t i = 0; i < sizeof(mnemonics) / sizeof(mnemonics[0]); i++)
            instruction |= verify_codegen_line_contains_token(line, code_len, mnemonics[i]);
        if (instruction && verify_codegen_line_contains_token(line, code_len, callee))
            matches++;
        line = line_end < end ? line_end + 1 : NULL;
    }
    return matches;
}

static unsigned verify_codegen_asm_inbound_edges(const char *assembly, const char *callee) {
    return verify_codegen_asm_direct_edges(assembly, assembly + strlen(assembly), callee);
}

static bool verify_codegen_realized_control(const char *generated_c,
                                            const XrToolchainAssemblyArtifact *artifact,
                                            XrTomlValue *item) {
    const char *subject = xtoml_get_string(item, "subject");
    const char *kind = verify_codegen_ledger_kind(xtoml_get_string(item, "kind"));
    char symbol[512], reason[768];
    bool shape_ok = true;
    unsigned inbound = 0;
    const char *body_start = NULL, *body_end = NULL;
    bool has_generated_symbol =
        strcmp(kind, "inline") != 0 && strcmp(kind, "noinline") != 0
            ? false
            : verify_codegen_generated_symbol(generated_c, subject, symbol, sizeof(symbol), reason,
                                              sizeof(reason));
    if (strcmp(kind, "noinline") == 0 && !has_generated_symbol) {
        xr_cli_error("verify", "realized control '%s:%s' failed: %s", subject, kind, reason);
        return false;
    }
    if (strcmp(kind, "inline") == 0 || strcmp(kind, "noinline") == 0) {
        bool has_body = has_generated_symbol &&
                        verify_codegen_asm_function(artifact->text, symbol, &body_start, &body_end);
        inbound =
            has_generated_symbol ? verify_codegen_asm_inbound_edges(artifact->text, symbol) : 0;
        shape_ok = strcmp(kind, "noinline") == 0 ? (has_body && inbound > 0) : inbound == 0;
        if (!shape_ok) {
            xr_cli_error("verify",
                         "realized control '%s:%s' failed: symbol=%s body=%s inbound-direct=%u",
                         subject, kind, symbol, has_body ? "present" : "absent", inbound);
            return false;
        }
    }
    printf("verified codegen.control '%s:%s' stage=realized provider=%s target=%s%s%u\n", subject,
           kind, xtc_provider_name(artifact->probe.selection.provider),
           artifact->probe.selection.target.name,
           strcmp(kind, "inline") == 0 || strcmp(kind, "noinline") == 0
               ? " inbound-direct="
               : " provider-capability=ok matches=",
           inbound);
    return true;
}

static bool verify_codegen_realized_edge(const char *generated_c,
                                         const XrToolchainAssemblyArtifact *artifact,
                                         XrTomlValue *item) {
    const char *caller = xtoml_get_string(item, "caller");
    const char *callee = xtoml_get_string(item, "callee");
    const char *expect = xtoml_get_string(item, "expect");
    char caller_symbol[512], callee_symbol[512], reason[768];
    const char *body_start = NULL, *body_end = NULL;
    bool preserved = strcmp(expect, "preserved") == 0 || strcmp(expect, "preserve_call") == 0;
    if (!verify_codegen_generated_symbol(generated_c, caller, caller_symbol, sizeof(caller_symbol),
                                         reason, sizeof(reason))) {
        xr_cli_error("verify", "realized edge '%s->%s' failed: %s", caller, callee, reason);
        return false;
    }
    bool has_callee = verify_codegen_generated_symbol(
        generated_c, callee, callee_symbol, sizeof(callee_symbol), reason, sizeof(reason));
    if (preserved && !has_callee) {
        xr_cli_error("verify", "realized edge '%s->%s' failed: %s", caller, callee, reason);
        return false;
    }
    if (!verify_codegen_asm_function(artifact->text, caller_symbol, &body_start, &body_end)) {
        xr_cli_error("verify", "realized edge '%s->%s' failed: caller symbol '%s' is absent",
                     caller, callee, caller_symbol);
        return false;
    }
    unsigned matches =
        has_callee ? verify_codegen_asm_direct_edges(body_start, body_end, callee_symbol) : 0;
    if ((preserved && matches == 0) || (!preserved && matches != 0)) {
        xr_cli_error("verify", "realized edge '%s->%s:%s' failed: caller=%s callee=%s direct=%u",
                     caller, callee, expect, caller_symbol, callee_symbol, matches);
        return false;
    }
    printf("verified codegen.edge '%s->%s:%s' stage=realized provider=%s target=%s direct=%u\n",
           caller, callee, expect, xtc_provider_name(artifact->probe.selection.provider),
           artifact->probe.selection.target.name, matches);
    return true;
}

static bool verify_codegen_realize(const XrCliInvocation *inv, const XaotBuildResult *result,
                                   XrTomlValue *controls, XrTomlValue *edges,
                                   const VerifyRealizedConfig *config, int *failures) {
    if (!config->needed)
        return true;
    XrToolchainProbeOptions options = {0};
    XrToolchainAssemblyArtifact artifact = {0};
    char err[1024];
    const char *target_name = config->target[0] ? config->target : "native";
    const char *provider_name = config->provider[0] ? config->provider : "auto";
    const char *profile_name = config->profile[0] ? config->profile : "hosted";
    if (strcmp(profile_name, "native") == 0)
        profile_name = "hosted";
    if (!xtc_target_parse(target_name, &options.request.target, err, sizeof(err)) ||
        !xtc_selector_parse(provider_name, &options.request.selector, err, sizeof(err)) ||
        !xtc_profile_parse(profile_name, &options.profile, err, sizeof(err))) {
        xr_cli_error("verify", "invalid realized provider scope: %s", err);
        (*failures)++;
        return false;
    }
    options.request.cc = xr_cli_opt_string(&inv->options, "cc", getenv("CC"));
    options.request.zig = xr_cli_opt_string(&inv->options, "zig", getenv("XRAY_ZIG"));
    options.request.program_hint = inv->ctx ? inv->ctx->program : NULL;
    options.refresh = xr_cli_opt_bool(&inv->options, "refresh");
    options.required_codegen_capabilities = config->required_capabilities;
    size_t generated_size = 0;
    char *generated_c = xaot_build_result_amalgamate(result, &generated_size);
    (void) generated_size;
    if (!generated_c)
        snprintf(err, sizeof(err), "cannot create the canonical generated-C realization unit");
    bool realized_ok =
        generated_c && xtc_shape_oracle_realize(&options, generated_c, &artifact, err, sizeof(err));
    if (!realized_ok && options.request.selector == XR_TOOLCHAIN_SELECTOR_AUTO) {
        char primary_err[1024];
        snprintf(primary_err, sizeof(primary_err), "%s", err);
        xtc_shape_oracle_free(&artifact);
        options.request.selector = XR_TOOLCHAIN_SELECTOR_ZIG;
        options.refresh = false;
        realized_ok = xtc_shape_oracle_realize(&options, generated_c, &artifact, err, sizeof(err));
        if (!realized_ok) {
            char fallback_err[1024];
            snprintf(fallback_err, sizeof(fallback_err), "%s", err);
            snprintf(err, sizeof(err), "primary provider: %.430s; Zig fallback: %.430s",
                     primary_err, fallback_err);
        }
    }
    if (!realized_ok) {
        xr_cli_error("verify", "realized object/assembly oracle failed: %s", err);
        xr_free(generated_c);
        xtc_shape_oracle_free(&artifact);
        (*failures)++;
        return false;
    }
    for (int i = 0; controls && i < xtoml_array_len(controls); i++) {
        XrTomlValue *item = xtoml_array_get(controls, i);
        if (verify_codegen_stage_rank(xtoml_get_string(item, "minimum_stage")) >= 3 &&
            !verify_codegen_realized_control(generated_c, &artifact, item))
            (*failures)++;
    }
    for (int i = 0; edges && i < xtoml_array_len(edges); i++) {
        XrTomlValue *item = xtoml_array_get(edges, i);
        if (verify_codegen_stage_rank(xtoml_get_string(item, "minimum_stage")) >= 3 &&
            !verify_codegen_realized_edge(generated_c, &artifact, item))
            (*failures)++;
    }
    printf("realized-oracle probe=%s fingerprint=%s\n", artifact.probe.probe_id,
           artifact.probe.selection.probe_fingerprint);
    xr_free(generated_c);
    xtc_shape_oracle_free(&artifact);
    return true;
}

static bool verify_codegen_contracts(const XrCliInvocation *inv, const VerifyAnalysis *state,
                                     XrTomlValue *controls, XrTomlValue *edges, int *failures) {
    XaotTarget target = {0};
    XrTargetProfile *target_profile = NULL;
    XaotBuildOptions options = {0};
    XaotBuildResult result = {0};
    bool wants_freestanding = false;
    bool wants_hosted = false;
    for (int group = 0; group < 2; group++) {
        XrTomlValue *items = group == 0 ? controls : edges;
        for (int i = 0; items && i < xtoml_array_len(items); i++) {
            const char *profile = xtoml_get_string(xtoml_array_get(items, i), "profile");
            wants_freestanding |= profile && strcmp(profile, "freestanding") == 0;
            wants_hosted |= !profile || strcmp(profile, "freestanding") != 0;
        }
    }
    if (wants_freestanding && wants_hosted) {
        xr_cli_error("verify",
                     "codegen contracts cannot mix hosted/native and freestanding profiles");
        (*failures)++;
        return false;
    }
    if (!xaot_target_init(&target, "native-c90")) {
        xr_cli_error("verify", "cannot initialize native-c90 codegen contract target");
        (*failures)++;
        return false;
    }
    options.target = &target;
    options.native_package_plan = state->project->native_plan;
    options.profile =
        wants_freestanding ? XAOT_BUILD_PROFILE_FREESTANDING : XAOT_BUILD_PROFILE_HOSTED;
    if (!verify_native_target_profile("native", options.profile, &target, &target_profile)) {
        xr_cli_error("verify", "exact native TargetProfile authority is unavailable");
        xaot_target_free(&target);
        (*failures)++;
        return false;
    }
    options.target_profile = target_profile;
    options.entry_module_authority = state->entry_authority;
    options.type_name_profile = XI_CGEN_TYPE_NAMES_ALL;
    options.emit_local_evidence_dump = true;
    options.quiet = true;
    bool built = xaot_build(state->entry, &options, &result) == 0 && result.local_evidence_dump;
    if (!built) {
        xr_cli_error("verify", "could not build canonical codegen evidence");
        (*failures)++;
    } else {
        for (int i = 0; controls && i < xtoml_array_len(controls); i++)
            if (!verify_codegen_control(result.local_evidence_dump, xtoml_array_get(controls, i)))
                (*failures)++;
        for (int i = 0; edges && i < xtoml_array_len(edges); i++)
            if (!verify_codegen_edge(result.local_evidence_dump, xtoml_array_get(edges, i)))
                (*failures)++;
        VerifyRealizedConfig realized;
        if (!verify_codegen_realized_config(controls, edges, &realized)) {
            (*failures)++;
        } else {
            (void) verify_codegen_realize(inv, &result, controls, edges, &realized, failures);
        }
    }
    xaot_build_result_free(&result);
    xr_target_profile_free(target_profile);
    xaot_target_free(&target);
    return built;
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
    XrTomlValue *codegen = xtoml_get_table(contract, "codegen");
    XrTomlValue *controls = codegen ? xtoml_get_array(codegen, "control") : NULL;
    XrTomlValue *edges = codegen ? xtoml_get_array(codegen, "edge") : NULL;
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
    for (int i = 0; functions && i < xtoml_array_len(functions); i++) {
        int item_failures = failures;
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
                xr_cli_error("verify", "contract '%s' failed: %s; witness: %s -> %s", symbol_name,
                             reason, symbol_name,
                             (requirement && requirement->as.string &&
                              strcmp(requirement->as.string, "no_reference_cycles") == 0)
                                 ? "L0 type graph"
                                 : "effect summary");
                failures++;
            }
        }
        if (strcmp(scope, "backend") == 0 &&
            !verify_backend_contract(&state, item, symbol_name, requires))
            failures++;
        if (failures == item_failures)
            printf("verified symbol-id=%u symbol=%s scope=%s\n", symbol->id, symbol_name, scope);
    }
    if ((controls && xtoml_array_len(controls) > 0) || (edges && xtoml_array_len(edges) > 0))
        (void) verify_codegen_contracts(inv, &state, controls, edges, &failures);
    verify_analysis_clear(&state);
    xtoml_free(contract);
    if (failures) {
        xr_cli_error("verify", "%d contract check(s) failed", failures);
        return XR_CLI_EXIT_FAIL;
    }
    return XR_CLI_EXIT_OK;
}
