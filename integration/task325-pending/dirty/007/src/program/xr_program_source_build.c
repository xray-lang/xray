/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_source_build.c - App-neutral source-to-XrProgram build owner
 *
 * KEY CONCEPT:
 *   Compiler-private graphs are scoped to one build. Only canonical bytes and
 *   an independently retained validated program cross the owner boundary.
 */

#include <stdio.h>

#include "xr_program_source_build.h"

#include "xr_program_from_xi.h"
#include "../analysis/xglobal_producer.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/analyzer/xanalyzer_mono.h"
#include "../frontend/canonical/xcanon.h"
#include "../frontend/parser/xast_nodes.h"
#include "../ir/xi_import_resolve.h"
#include "../ir/xi_cleanup.h"
#include "../ir/xi_pipeline.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_identity.h"
#include "../module/xmodule_resolver.h"
#include "../runtime/xerror_codes.h"
#include "../toolchain/xcompiler_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct XrProgramSourceSelection {
    uint32_t module_index;
    const AstNode *syntax;
    XgFuncId body_id;
    const XiFunc *function;
    XrProgramSourceTestKind test_kind;
    uint32_t timeout_seconds;
} XrProgramSourceSelection;

typedef struct XrProgramSourceBuildContext {
    const XrProgramSourceBuildInput *input;
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    XgGlobalEvidence evidence;
    AstNode **ast_roots;
    XiPipelineResult *pipelines;
    XiModule **modules;
    const XiFunc **module_roots;
    uint32_t module_count;
    uint32_t entry_topological_index;
    XrProgramSourceSelection *selections;
    uint32_t selection_count;
    uint32_t *export_selections;
    uint32_t export_count;
    uint32_t test_count;
} XrProgramSourceBuildContext;

XrProgramSourceBuildBudget xr_program_source_build_default_budget(void) {
    return (XrProgramSourceBuildBudget) {
        .max_modules = XR_PROGRAM_SOURCE_BUILD_DEFAULT_MAX_MODULES,
        .max_monomorphization_depth = XR_MONO_MAX_DEPTH,
        .max_monomorphization_instances = XR_MONO_MAX_INSTANCES,
        .max_program_bytes = XR_PROGRAM_LIMIT_ARTIFACT_BYTES,
    };
}

static void clear_diagnostic(XrProgramSourceDiagnostic *diagnostic) {
    if (!diagnostic)
        return;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->module_index = UINT32_MAX;
}

static XrProgramSourceBuildStatus reject(XrProgramSourceDiagnostic *diagnostic,
                                         XrProgramSourceBuildStatus status,
                                         XrProgramSourceBuildStage stage, uint32_t module_index,
                                         uint32_t source_line, uint32_t underlying_status,
                                         const char *format, ...) {
    if (diagnostic) {
        diagnostic->status = status;
        diagnostic->stage = stage;
        diagnostic->module_index = module_index;
        diagnostic->source_line = source_line;
        diagnostic->underlying_status = underlying_status;
        if (format) {
            va_list arguments;
            va_start(arguments, format);
            (void) vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, arguments);
            va_end(arguments);
        }
    }
    return status;
}

static void set_source_location(XrProgramSourceDiagnostic *diagnostic, const XrLocation *location) {
    if (!diagnostic || !location)
        return;
    diagnostic->source_line = location->line > 0 ? (uint32_t) location->line : 0u;
    diagnostic->source_column = location->column > 0 ? (uint32_t) location->column : 0u;
    if (location->file) {
        (void) snprintf(diagnostic->source_path, sizeof(diagnostic->source_path), "%s",
                        location->file);
    }
}

static bool fingerprint_present(XrFingerprint fingerprint) {
    uint8_t combined = 0u;
    for (size_t index = 0u; index < sizeof(fingerprint.bytes); ++index)
        combined |= fingerprint.bytes[index];
    return combined != 0u;
}

static bool source_profile_valid(uint8_t profile) {
    return profile >= XR_PROGRAM_SOURCE_PROFILE_CHECK &&
           profile <= XR_PROGRAM_SOURCE_PROFILE_DEBUG_TOOLING;
}

static XgBuildProfile evidence_profile(uint8_t profile) {
    switch ((XrProgramSourceProfile) profile) {
        case XR_PROGRAM_SOURCE_PROFILE_CHECK:
            return XG_BUILD_CHECK;
        case XR_PROGRAM_SOURCE_PROFILE_DEVELOPMENT:
            return XG_BUILD_DEV;
        case XR_PROGRAM_SOURCE_PROFILE_NATIVE_RELEASE:
            return XG_BUILD_NATIVE_RELEASE;
        case XR_PROGRAM_SOURCE_PROFILE_FREESTANDING:
            return XG_BUILD_FREESTANDING;
        case XR_PROGRAM_SOURCE_PROFILE_DEBUG_TOOLING:
            return XG_BUILD_DEBUG_TOOLING;
        case XR_PROGRAM_SOURCE_PROFILE_INVALID:
            break;
    }
    return XG_BUILD_CHECK;
}

static void build_context_free(XrProgramSourceBuildContext *context) {
    if (!context)
        return;
    if (context->pipelines)
        for (uint32_t module = 0u; module < context->module_count; ++module)
            xi_pipeline_result_free(&context->pipelines[module]);
    xr_free(context->module_roots);
    xr_free(context->modules);
    xr_free(context->pipelines);
    xr_free(context->ast_roots);
    xr_free(context->selections);
    xr_free(context->export_selections);
    xg_global_evidence_free(&context->evidence);
    if (context->analyzer) {
        xa_analyzer_set_graph(context->analyzer, NULL);
        xa_analyzer_free(context->analyzer);
    }
    xr_module_graph_free(context->graph);
    memset(context, 0, sizeof(*context));
}

static XrProgramSourceBuildStatus allocate_module_storage(XrProgramSourceBuildContext *context,
                                                          XrProgramSourceDiagnostic *diagnostic) {
    size_t count = context->module_count;
    context->ast_roots = xr_calloc(count, sizeof(*context->ast_roots));
    context->pipelines = xr_calloc(count, sizeof(*context->pipelines));
    context->modules = xr_calloc(count, sizeof(*context->modules));
    context->module_roots = xr_calloc(count, sizeof(*context->module_roots));
    if (!context->ast_roots || !context->pipelines || !context->modules || !context->module_roots)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH, UINT32_MAX, 0u, 0u,
                      "module build storage allocation failed");
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static XrProgramSourceBuildStatus build_module_graph(XrProgramSourceBuildContext *context,
                                                     XrProgramSourceDiagnostic *diagnostic) {
    const XrProgramSourceBuildInput *input = context->input;
    context->graph = xr_module_graph_new(input->session, input->resolver);
    if (!context->graph)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH, UINT32_MAX, 0u, 0u,
                      "module graph allocation failed");
    char *graph_error = NULL;
    int graph_status = xr_module_graph_build(context->graph, input->entry_source_path,
                                             input->entry_authority, &graph_error);
    if (graph_status != 0) {
        XrProgramSourceBuildStatus status =
            reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_GRAPH_REJECTED,
                   XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH, UINT32_MAX, 0u, (uint32_t) graph_status,
                   "%s", graph_error ? graph_error : "module graph build failed");
        xr_free(graph_error);
        return status;
    }
    xr_free(graph_error);
    if (xr_module_graph_topological_sort(context->graph) != 0 || context->graph->has_cycle ||
        !context->graph->topo_order || context->graph->topo_count <= 0 ||
        context->graph->entry_index < 0 ||
        context->graph->entry_index >= context->graph->spec_count)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_GRAPH_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH, UINT32_MAX, 0u, 0u, "%s",
                      context->graph->cycle_desc ? context->graph->cycle_desc
                                                 : "module graph is incomplete");
    if ((uint32_t) context->graph->topo_count > input->budget.max_modules)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT,
                      XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH, UINT32_MAX, 0u,
                      (uint32_t) context->graph->topo_count,
                      "module count %d exceeds request limit %u", context->graph->topo_count,
                      input->budget.max_modules);
    context->module_count = (uint32_t) context->graph->topo_count;
    XrProgramSourceBuildStatus status = allocate_module_storage(context, diagnostic);
    if (status != XR_PROGRAM_SOURCE_BUILD_OK)
        return status;
    for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
        int spec_index = context->graph->topo_order[topo];
        if (spec_index < 0 || spec_index >= context->graph->spec_count)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_GRAPH_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_MODULE_GRAPH, topo, 0u, 0u,
                          "topological module index is out of range");
        context->ast_roots[topo] = context->graph->specs[spec_index].ast;
        if (spec_index == context->graph->entry_index)
            context->entry_topological_index = topo;
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static const XrProgramSourceEntryIdentity *
source_entry_identity(const XrProgramSourceBuildContext *context, uint32_t index) {
    return index == 0u ? &context->input->entry : &context->input->retained_entries[index - 1u];
}

static XrProgramSourceBuildStatus validate_entry_identity(XrProgramSourceBuildContext *context,
                                                          XrProgramSourceDiagnostic *diagnostic) {
    context->selection_count = context->input->retained_entry_count + 1u;
    context->selections = xr_calloc(context->selection_count, sizeof(*context->selections));
    if (!context->selections)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, UINT32_MAX, 0u, 0u,
                      "source entry selection allocation failed");
    for (uint32_t index = 0u; index < context->selection_count; ++index) {
        const XrProgramSourceEntryIdentity *entry = source_entry_identity(context, index);
        XrProgramSourceSelection *selection = &context->selections[index];
        const XrModuleSpec *spec = NULL;
        selection->module_index = UINT32_MAX;
        for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
            const XrModuleSpec *candidate =
                &context->graph->specs[context->graph->topo_order[topo]];
            if (candidate->canonical && strcmp(candidate->canonical, entry->module_identity) == 0) {
                spec = candidate;
                selection->module_index = topo;
                break;
            }
        }
        if (!spec || (index == 0u && selection->module_index != context->entry_topological_index))
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index, 0u,
                          index, "entry module identity does not match the authoritative graph");
        if (memcmp(spec->source_content_fingerprint.bytes, entry->source_content_fingerprint.bytes,
                   sizeof(entry->source_content_fingerprint.bytes)) != 0)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index, 0u,
                          index, "entry source fingerprint does not match the authoritative graph");
        if (!spec->ast || spec->ast->type != AST_PROGRAM)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index, 0u,
                          index, "entry module has no exact source program");
        if (entry->kind == XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER) {
            selection->syntax = spec->ast;
            continue;
        }
        uint32_t matches = 0u;
        for (int statement = 0; statement < spec->ast->as.program.count; ++statement) {
            const AstNode *node = spec->ast->as.program.statements[statement];
            if (!node || node->type != AST_FUNCTION_DECL || !node->as.function_decl.name ||
                strcmp(node->as.function_decl.name, entry->function_name) != 0)
                continue;
            matches++;
            selection->syntax = node;
        }
        if (matches != 1u)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index, 0u,
                          matches, "entry function '%s' has %u exact source declarations",
                          entry->function_name, matches);
        for (uint32_t prior = 0u; prior < index; ++prior)
            if (context->selections[prior].syntax == selection->syntax)
                return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                              XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index, 0u,
                              index, "source entry request contains a duplicate declaration");
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static XrProgramSourceBuildStatus discover_export_entries(XrProgramSourceBuildContext *context,
                                                         XrProgramSourceDiagnostic *diagnostic) {
    if (!context->input->discover_exports)
        return XR_PROGRAM_SOURCE_BUILD_OK;
    const XrNativePackagePlan *plan =
        xr_compiler_session_native_package_plan(context->input->session);
    if (!plan)
        return XR_PROGRAM_SOURCE_BUILD_OK;
    uint32_t module = context->entry_topological_index;
    if (!plan->valid || (plan->export_count && !plan->exports))
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, 0u, 0u,
                      "C export manifest is invalid");
    if (plan->export_count > XR_PROGRAM_LIMIT_FUNCTIONS)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT,
                      XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, 0u, 0u,
                      "C export count exceeds the function limit");
    context->export_count = plan->export_count;
    context->export_selections = plan->export_count
        ? xr_calloc(plan->export_count, sizeof(*context->export_selections)) : NULL;
    if (plan->export_count && !context->export_selections)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, 0u, 0u,
                      "C export selection allocation failed");
    const AstNode *root = context->ast_roots[module];
    for (uint32_t index = 0u; index < plan->export_count; ++index) {
        const XrCExportPlan *entry = &plan->exports[index];
        const AstNode *selected = NULL;
        uint32_t matches = 0u;
        if (!entry->xray_name || !entry->symbol)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, 0u, index,
                          "C export declaration is incomplete");
        for (int statement = 0; statement < root->as.program.count; ++statement) {
            const AstNode *node = root->as.program.statements[statement];
            if (node && node->type == AST_FUNCTION_DECL && node->as.function_decl.name &&
                strcmp(node->as.function_decl.name, entry->xray_name) == 0) {
                selected = node;
                matches++;
            }
        }
        if (matches != 1u || !selected->as.function_decl.body ||
            selected->as.function_decl.type_param_count != 0)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, 0u, index,
                          "C export '%s' requires one concrete entry-module declaration",
                          entry->xray_name);
        uint32_t selection = 0u;
        while (selection < context->selection_count &&
               context->selections[selection].syntax != selected)
            selection++;
        if (selection == context->selection_count) {
            if (selection == XR_PROGRAM_LIMIT_FUNCTIONS)
                return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT,
                              XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, selected->line,
                              index, "C export roots exceed the function limit");
            XrProgramSourceSelection *grown = xr_realloc(
                context->selections, (selection + 1u) * sizeof(*grown));
            if (!grown)
                return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                              XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, selected->line,
                              index, "C export root allocation failed");
            context->selections = grown;
            grown[context->selection_count++] = (XrProgramSourceSelection) {
                .module_index = module, .syntax = selected,
            };
        }
        context->export_selections[index] = selection;
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static XrProgramSourceTestKind test_attribute_kind(AttributeKind kind) {
    switch (kind) {
        case ATTR_TEST:
        case ATTR_TEST_TIMEOUT: return XR_PROGRAM_TEST_CASE;
        case ATTR_TEST_SKIP: return XR_PROGRAM_TEST_SKIP;
        case ATTR_BEFORE_EACH: return XR_PROGRAM_TEST_BEFORE_EACH;
        case ATTR_AFTER_EACH: return XR_PROGRAM_TEST_AFTER_EACH;
        case ATTR_BEFORE_ALL: return XR_PROGRAM_TEST_BEFORE_ALL;
        case ATTR_AFTER_ALL: return XR_PROGRAM_TEST_AFTER_ALL;
        default: return XR_PROGRAM_TEST_NONE;
    }
}

static XrProgramSourceBuildStatus discover_test_entries(XrProgramSourceBuildContext *context,
                                                       XrProgramSourceDiagnostic *diagnostic) {
    if (!context->input->discover_tests)
        return XR_PROGRAM_SOURCE_BUILD_OK;
    uint32_t module = context->entry_topological_index;
    const AstNode *root = context->ast_roots[module];
    for (int statement = 0; statement < root->as.program.count; ++statement) {
        const AstNode *node = root->as.program.statements[statement];
        if (!node || node->type != AST_FUNCTION_DECL)
            continue;
        const FunctionDeclNode *function = &node->as.function_decl;
        XrProgramSourceTestKind kind = XR_PROGRAM_TEST_NONE;
        uint32_t timeout = 0u;
        for (int index = 0; index < function->attr_count; ++index) {
            const XrAttribute *attribute = function->attributes[index];
            XrProgramSourceTestKind candidate = test_attribute_kind(attribute->kind);
            if (candidate == XR_PROGRAM_TEST_NONE)
                continue;
            if (kind != XR_PROGRAM_TEST_NONE || attribute->timeout < 0)
                return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                              XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, node->line, 0u,
                              "test declaration has conflicting attributes or invalid timeout");
            kind = candidate;
            timeout = (uint32_t) attribute->timeout;
        }
        if (kind == XR_PROGRAM_TEST_NONE)
            continue;
        if (!function->name || function->param_count != 0 || function->type_param_count != 0 ||
            function->is_generator || !function->body)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, node->line, 0u,
                          "test and hook declarations require a concrete zero-argument body");
        for (uint32_t prior = 0u; prior < context->selection_count; ++prior)
            if (context->selections[prior].syntax == node)
                return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                              XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, node->line, 0u,
                              "test entry duplicates an explicit retained declaration");
        if (context->selection_count == XR_PROGRAM_LIMIT_FUNCTIONS)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, node->line, 0u,
                          "test entry count exceeds the function limit");
        XrProgramSourceSelection *grown = xr_realloc(
            context->selections, (context->selection_count + 1u) * sizeof(*grown));
        if (!grown)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, module, node->line, 0u,
                          "test entry selection allocation failed");
        context->selections = grown;
        grown[context->selection_count++] = (XrProgramSourceSelection) {
            .module_index = module, .syntax = node, .test_kind = kind,
            .timeout_seconds = timeout,
        };
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static const XgBodySummary *entry_body_summary(const XrProgramSourceBuildContext *context,
                                               uint32_t index) {
    const XrProgramSourceSelection *selection = &context->selections[index];
    if (!selection->syntax || !context->evidence.bodies || context->evidence.nbodies == 0u)
        return NULL;
    XgModuleId module_id = (XgModuleId) (selection->module_index + 1u);
    const XgBodySummary *match = NULL;
    if (selection->syntax->type == AST_PROGRAM) {
        for (uint32_t body = 0u; body < context->evidence.nbodies; ++body) {
            const XgBodySummary *candidate = &context->evidence.bodies[body];
            if (candidate->module_id != module_id || candidate->kind != XG_BODY_MODULE_INIT)
                continue;
            if (match)
                return NULL;
            match = candidate;
        }
        return match;
    }

    uint32_t name_id = xg_name_id(selection->syntax->as.function_decl.name);
    uint32_t source_span_id = selection->syntax->line > 0 ? (uint32_t) selection->syntax->line : 0u;
    const XgDeclSummary *declaration = NULL;
    for (uint32_t decl = 0u; decl < context->evidence.ndecls; ++decl) {
        const XgDeclSummary *candidate = &context->evidence.decls[decl];
        if (candidate->module_id != module_id || candidate->kind != XG_DECL_FUNC ||
            candidate->name_id != name_id || candidate->source_span_id != source_span_id)
            continue;
        if (declaration)
            return NULL;
        declaration = candidate;
    }
    if (!declaration)
        return NULL;
    for (uint32_t body = 0u; body < context->evidence.nbodies; ++body) {
        const XgBodySummary *candidate = &context->evidence.bodies[body];
        if (candidate->module_id != module_id || candidate->kind != XG_BODY_FUNCTION ||
            candidate->owner_decl_id != declaration->decl_id || candidate->name_id != name_id)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

/* Bind stable source entry identities here. The Program writer closes the
 * executable Xi graph, retains every initializer and validates each call's
 * exact target/effect contract before publishing a validated Program. */
static XrProgramSourceBuildStatus
prepare_entry_identities(XrProgramSourceBuildContext *context,
                          XrProgramSourceDiagnostic *diagnostic) {
    for (uint32_t index = 0u; index < context->selection_count; ++index) {
        const XgBodySummary *entry = entry_body_summary(context, index);
        if (!entry || entry->func_id == XG_NO_ID)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_EVIDENCE_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_GLOBAL_EVIDENCE,
                          context->selections[index].module_index, 0u, index,
                          "entry function has no unique global-evidence body");
        context->selections[index].body_id = entry->func_id;
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static XrProgramSourceBuildStatus analyze_modules(XrProgramSourceBuildContext *context,
                                                  XrProgramSourceDiagnostic *diagnostic) {
    for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
        int spec_index = context->graph->topo_order[topo];
        XrModuleSpec *spec = &context->graph->specs[spec_index];
        xa_analyzer_analyze(context->analyzer, spec->source_path, spec->ast);
        int diagnostic_count = 0;
        XaDiagnostic *analysis = xa_analyzer_get_diagnostics(context->analyzer, &diagnostic_count);
        for (; analysis; analysis = analysis->next) {
            if (analysis->severity != XR_DIAG_SEV_ERROR)
                continue;
            XrProgramSourceBuildStatus status =
                reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED,
                       XR_PROGRAM_SOURCE_STAGE_ANALYSIS, topo,
                       analysis->location.line > 0 ? (uint32_t) analysis->location.line : 0u,
                       analysis->code >= 0 ? (uint32_t) analysis->code : 0u, "%s",
                       analysis->message ? analysis->message : "source analysis failed");
            set_source_location(diagnostic, &analysis->location);
            xa_analyzer_clear_diagnostics(context->analyzer);
            return status;
        }
        if (spec->export_symbols)
            xr_hashmap_free(spec->export_symbols);
        spec->export_symbols = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(context->analyzer, spec->ast,
                                                        &spec->export_symbols)) {
            xa_analyzer_clear_diagnostics(context->analyzer);
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ANALYSIS, topo, 0u, 0u,
                          "module export analysis is incomplete");
        }
        spec->status = XR_MODSPEC_ANALYZED;
        xa_analyzer_clear_diagnostics(context->analyzer);
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static XrProgramSourceBuildStatus prepare_semantic_graph(XrProgramSourceBuildContext *context,
                                                         XrProgramSourceDiagnostic *diagnostic) {
    context->analyzer = xa_analyzer_new(context->input->session);
    if (!context->analyzer)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_ANALYSIS, UINT32_MAX, 0u, 0u,
                      "analyzer allocation failed");
    xa_analyzer_set_build_profile(context->analyzer, context->input->source_profile ==
                                                             XR_PROGRAM_SOURCE_PROFILE_FREESTANDING
                                                         ? XA_ANALYZER_BUILD_PROFILE_FREESTANDING
                                                         : XA_ANALYZER_BUILD_PROFILE_HOSTED);
    xa_analyzer_set_graph(context->analyzer, context->graph);
    XrProgramSourceBuildStatus status = analyze_modules(context, diagnostic);
    if (status != XR_PROGRAM_SOURCE_BUILD_OK)
        return status;
    XrVMRuntime *isolate = xr_compiler_session_vm_host(context->input->session);
    XaMonoBudget mono_budget = {
        .max_depth = context->input->budget.max_monomorphization_depth,
        .max_instances = context->input->budget.max_monomorphization_instances,
    };
    XaMonoUsage mono_usage = {0};
    if (!xa_mono_graph_pass(context->ast_roots, (int) context->module_count, isolate, &mono_budget,
                            &mono_usage, context->analyzer)) {
        int diagnostic_count = 0;
        XaDiagnostic *analysis = xa_analyzer_get_diagnostics(context->analyzer, &diagnostic_count);
        for (; analysis; analysis = analysis->next) {
            if (analysis->severity != XR_DIAG_SEV_ERROR)
                continue;
            uint32_t module_index = UINT32_MAX;
            if (analysis->location.file) {
                for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
                    int spec_index = context->graph->topo_order[topo];
                    const char *source_path = context->graph->specs[spec_index].source_path;
                    if (source_path && strcmp(source_path, analysis->location.file) == 0) {
                        module_index = topo;
                        break;
                    }
                }
            }
            uint32_t code = analysis->code >= 0 ? (uint32_t) analysis->code : 0u;
            XrProgramSourceBuildStatus failure =
                code == XR_ERR_ANALYZE_MONO_BUDGET || code == XR_ERR_ANALYZE_MONO_DEPTH
                    ? XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT
                    : XR_PROGRAM_SOURCE_BUILD_MONOMORPHIZATION_REJECTED;
            const char *message =
                analysis->message ? analysis->message : "graph monomorphization failed";
            XrProgramSourceBuildStatus failure_status =
                code > 0u
                    ? reject(diagnostic, failure, XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION,
                             module_index,
                             analysis->location.line > 0 ? (uint32_t) analysis->location.line : 0u,
                             code, "E%04u: %s", code, message)
                    : reject(diagnostic, failure, XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION,
                             module_index,
                             analysis->location.line > 0 ? (uint32_t) analysis->location.line : 0u,
                             0u, "%s", message);
            set_source_location(diagnostic, &analysis->location);
            xa_analyzer_clear_diagnostics(context->analyzer);
            return failure_status;
        }
        xa_analyzer_clear_diagnostics(context->analyzer);
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_MONOMORPHIZATION_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_MONOMORPHIZATION, UINT32_MAX, 0u, 0u,
                      "graph monomorphization failed without a diagnostic");
    }
    for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
        int spec_index = context->graph->topo_order[topo];
        XrModuleSpec *spec = &context->graph->specs[spec_index];
        XrCompilerSessionScope scope = {0};
        if (!spec->ast || spec->ast->type != AST_PROGRAM || !spec->ast->as.program.arena ||
            !xr_compiler_session_push_arena(context->input->session, spec->ast->as.program.arena,
                                            spec->source_path, &scope))
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_CANONICALIZATION_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_CANONICALIZATION, topo, 0u, 0u,
                          "module canonicalization arena is unavailable");
        XrCanonStatus canonical =
            xr_canon_program(spec->ast, context->analyzer, context->input->session);
        xr_compiler_session_pop_arena(&scope);
        if (canonical != XR_CANON_OK)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_CANONICALIZATION_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_CANONICALIZATION, topo, 0u, (uint32_t) canonical,
                          "module canonicalization failed");
    }
    status = analyze_modules(context, diagnostic);
    if (status != XR_PROGRAM_SOURCE_BUILD_OK)
        return status;
    if (!xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
            &context->evidence, context->graph, evidence_profile(context->input->source_profile),
            0u, NULL, 0u, context->analyzer))
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_EVIDENCE_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_GLOBAL_EVIDENCE, UINT32_MAX, 0u, 0u,
                      "global evidence construction failed");
    return prepare_entry_identities(context, diagnostic);
}

static XrProgramSourceBuildStatus compile_xi_modules(XrProgramSourceBuildContext *context,
                                                     XrProgramSourceDiagnostic *diagnostic) {
    XiPipelineConfig config = xi_pipeline_program_input_config();
    config.run_canonicalize = false;
    config.module_graph = context->graph;
    config.graph_modules = context->modules;
    config.graph_module_count = (int) context->module_count;
    config.global_evidence = &context->evidence;
    XrVMRuntime *isolate = xr_compiler_session_vm_host(context->input->session);
    for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
        int spec_index = context->graph->topo_order[topo];
        XrModuleSpec *spec = &context->graph->specs[spec_index];
        config.source_file = spec->source_path;
        config.module_identity = spec->canonical;
        config.module_name = spec->canonical;
        config.global_evidence_module_id = topo + 1u;
        context->pipelines[topo] =
            xi_pipeline_compile_program(spec->ast, context->analyzer, isolate, &config);
        XiPipelineResult *result = &context->pipelines[topo];
        if (result->status != XI_PIPE_OK || !result->ir || !result->ir->module)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_PIPELINE_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_XI_PIPELINE, topo, result->error.source_line,
                          (uint32_t) result->status, "%s: %s",
                          xi_pipeline_stage_str(result->error.stage), result->error.detail);
        context->modules[topo] = result->ir->module;
        context->module_roots[topo] = result->ir;
    }
    for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
        int spec_index = context->graph->topo_order[topo];
        xi_resolve_imports(context->pipelines[topo].ir, context->graph,
                           context->graph->specs[spec_index].source_path, context->modules,
                           (int) context->module_count);
    }
    for (uint32_t topo = 0u; topo < context->module_count; ++topo) {
        char error[256] = {0};
        if (!xi_normalize_panic_exits(context->pipelines[topo].ir, &context->evidence, isolate,
                                      error, sizeof(error)))
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_PIPELINE_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_XI_PIPELINE, topo, 0u, 0u, "%s", error);
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static XrProgramSourceBuildStatus write_product(XrProgramSourceBuildContext *context,
                                                XrProgramSourceProduct *product,
                                                XrProgramSourceDiagnostic *diagnostic) {
    for (uint32_t index = 0u; index < context->selection_count; ++index) {
        XrProgramSourceSelection *selection = &context->selections[index];
        XiFunc *initializer = context->pipelines[selection->module_index].ir;
        XiModule *module = initializer ? initializer->module : NULL;
        uint32_t matches = 0u;
        if (initializer && initializer->xg_body_func_id == selection->body_id) {
            selection->function = initializer;
            matches++;
        }
        for (uint16_t function = 0u; module && function < module->nfuncs; ++function) {
            XiFunc *candidate = module->functions[function];
            if (!candidate || candidate == initializer ||
                candidate->xg_body_func_id != selection->body_id)
                continue;
            selection->function = candidate;
            matches++;
        }
        if (matches != 1u)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index, 0u,
                          matches, "entry body identity resolved to %u Xi functions", matches);
        if (selection->test_kind != XR_PROGRAM_TEST_NONE &&
            (!selection->function->return_type ||
             (selection->function->return_type->kind != XR_KIND_UNIT &&
              selection->function->return_type->kind != XR_KIND_NEVER)))
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED,
                          XR_PROGRAM_SOURCE_STAGE_ENTRY_SELECTION, selection->module_index,
                          selection->syntax->line, 0u, "test and hook declarations cannot return a value");
    }
    uint32_t retained_count = context->selection_count - 1u;
    const XiFunc **retained = retained_count ? xr_calloc(retained_count, sizeof(*retained)) : NULL;
    product->retained_function_ids =
        retained_count ? xr_calloc(retained_count, sizeof(*product->retained_function_ids)) : NULL;
    if (retained_count && (!retained || !product->retained_function_ids)) {
        xr_free(retained);
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, UINT32_MAX, 0u, 0u,
                      "retained function output allocation failed");
    }
    product->retained_function_count = context->input->retained_entry_count;
    for (uint32_t index = 0u; index < retained_count; ++index)
        retained[index] = context->selections[index + 1u].function;
    XrProgramFromXiInput writer_input = {
        .module_roots = context->module_roots,
        .module_count = context->module_count,
        .entry_function = context->selections[0].function,
        .retained_functions = retained,
        .retained_function_count = retained_count,
        .global_evidence = &context->evidence,
        .module_graph = context->graph,
        .semantic_profile_fingerprint = context->input->semantic_profile_fingerprint.bytes,
    };
    char writer_diagnostic[XR_PROGRAM_SOURCE_DIAGNOSTIC_MESSAGE_SIZE] = {0};
    XrProgramBuildStatus writer = xr_program_write_from_xi(
        &writer_input, &product->artifact, &product->program, product->retained_function_ids,
        writer_diagnostic, sizeof(writer_diagnostic));
    xr_free(retained);
    if (writer != XR_PROGRAM_BUILD_OK) {
        if (diagnostic)
            diagnostic->writer_status = writer;
        return reject(diagnostic, writer == XR_PROGRAM_BUILD_OUT_OF_MEMORY
                                      ? XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY
                                      : XR_PROGRAM_SOURCE_BUILD_PROGRAM_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, context->entry_topological_index, 0u,
                      (uint32_t) writer, "%s",
                      writer_diagnostic[0] ? writer_diagnostic
                                           : xr_program_build_status_name(writer));
    }
    if (product->artifact.size > context->input->budget.max_program_bytes)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT,
                      XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, context->entry_topological_index, 0u,
                      (uint32_t) product->artifact.size,
                      "canonical Program size %zu exceeds request limit %u bytes",
                      product->artifact.size, context->input->budget.max_program_bytes);
    uint32_t test_count = context->test_count;
    product->tests = test_count ? xr_calloc(test_count, sizeof(*product->tests)) : NULL;
    if (test_count && !product->tests)
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, UINT32_MAX, 0u, 0u,
                      "test entry metadata allocation failed");
    product->test_entry_count = test_count;
    for (uint32_t index = 0u; index < test_count; ++index) {
        uint32_t retained_index = context->input->retained_entry_count + index;
        const XrProgramSourceSelection *selection = &context->selections[retained_index + 1u];
        XrProgramSourceTestEntry *test = &product->tests[index];
        test->name = xr_strdup(selection->syntax->as.function_decl.name);
        if (!test->name)
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                          XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, selection->module_index, 0u, 0u,
                          "test entry name allocation failed");
        test->function_id = product->retained_function_ids[retained_index];
        test->kind = selection->test_kind;
        test->timeout_seconds = selection->timeout_seconds;
    }
    const XrNativePackagePlan *native_plan =
        xr_compiler_session_native_package_plan(context->input->session);
    product->exports = context->export_count
        ? xr_calloc(context->export_count, sizeof(*product->exports)) : NULL;
    product->export_function_ids = context->export_count
        ? xr_calloc(context->export_count, sizeof(*product->export_function_ids)) : NULL;
    if (context->export_count && (!product->exports || !product->export_function_ids))
        return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                      XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, UINT32_MAX, 0u, 0u,
                      "C export output allocation failed");
    product->export_count = context->export_count;
    for (uint32_t index = 0u; index < context->export_count; ++index) {
        const XrCExportPlan *source = &native_plan->exports[index];
        XrCExportPlan *copy = &product->exports[index];
        copy->xray_name = xr_strdup(source->xray_name);
        copy->symbol = xr_strdup(source->symbol);
        copy->visibility = source->visibility ? xr_strdup(source->visibility) : NULL;
        copy->abi = source->abi ? xr_strdup(source->abi) : NULL;
        copy->header = source->header;
        if (!copy->xray_name || !copy->symbol || (source->visibility && !copy->visibility) ||
            (source->abi && !copy->abi))
            return reject(diagnostic, XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY,
                          XR_PROGRAM_SOURCE_STAGE_PROGRAM_WRITE, UINT32_MAX, 0u, index,
                          "C export declaration copy failed");
        uint32_t selection = context->export_selections[index];
        product->export_function_ids[index] = selection == 0u
            ? xr_validated_program_entry_function(product->program)
            : product->retained_function_ids[selection - 1u];
    }
    if (!product->retained_function_count) {
        xr_free(product->retained_function_ids);
        product->retained_function_ids = NULL;
    }
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

static bool entry_identity_valid(const XrProgramSourceEntryIdentity *entry,
                                 bool allow_initializer) {
    bool function = entry->kind == XR_PROGRAM_SOURCE_ENTRY_FUNCTION;
    bool initializer =
        allow_initializer && entry->kind == XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER;
    return (function || initializer) && entry->reserved8[0] == 0u && entry->reserved8[1] == 0u &&
           entry->reserved8[2] == 0u && entry->module_identity &&
           xr_module_identity_valid(entry->module_identity, NULL) &&
           fingerprint_present(entry->source_content_fingerprint) &&
           (function ? entry->function_name && entry->function_name[0] : !entry->function_name);
}

static bool input_valid(const XrProgramSourceBuildInput *input) {
    XaMonoBudget mono_budget = {
        .max_depth = input ? input->budget.max_monomorphization_depth : 0u,
        .max_instances = input ? input->budget.max_monomorphization_instances : 0u,
    };
    if (!input || input->schema_version != XR_PROGRAM_SOURCE_BUILD_SCHEMA_VERSION ||
        input->budget.max_modules == 0u ||
        input->budget.max_modules > XR_PROGRAM_SOURCE_BUILD_DEFAULT_MAX_MODULES ||
        !xa_mono_budget_valid(&mono_budget) || input->budget.max_program_bytes == 0u ||
        input->budget.max_program_bytes > XR_PROGRAM_LIMIT_ARTIFACT_BYTES || !input->session ||
        !input->resolver || !input->entry_source_path || !input->entry_authority ||
        !entry_identity_valid(&input->entry, true) ||
        input->retained_entry_count >= XR_PROGRAM_LIMIT_FUNCTIONS ||
        (input->retained_entry_count && !input->retained_entries) ||
        (!input->retained_entry_count && input->retained_entries) ||
        input->discover_tests > 1u || input->discover_exports > 1u ||
        !source_profile_valid(input->source_profile) ||
        !fingerprint_present(input->semantic_profile_fingerprint) ||
        !xr_module_identity_authority_valid(input->entry_authority) ||
        !xr_compiler_session_vm_host(input->session))
        return false;
    for (uint32_t index = 0u; index < input->retained_entry_count; ++index)
        if (!entry_identity_valid(&input->retained_entries[index], false))
            return false;
    for (size_t index = 0u; index < sizeof(input->reserved8); ++index)
        if (input->reserved8[index] != 0u)
            return false;
    return true;
}

XrProgramSourceBuildStatus xr_program_source_build(const XrProgramSourceBuildInput *input,
                                                   XrProgramSourceProduct *product_out,
                                                   XrProgramSourceDiagnostic *diagnostic_out) {
    if (product_out)
        memset(product_out, 0, sizeof(*product_out));
    clear_diagnostic(diagnostic_out);
    if (!product_out || !input_valid(input))
        return reject(diagnostic_out, XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT,
                      XR_PROGRAM_SOURCE_STAGE_REQUEST, UINT32_MAX, 0u, 0u,
                      "source build request is incomplete");

    XrCompilerSessionOperationScope operation = {0};
    if (!xr_compiler_session_operation_begin(input->session, &operation))
        return reject(diagnostic_out, XR_PROGRAM_SOURCE_BUILD_SESSION_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_REQUEST, UINT32_MAX, 0u, 0u,
                      "compiler session is unavailable");

    XrProgramSourceBuildContext context = {.input = input};
    context.entry_topological_index = UINT32_MAX;
    XrProgramSourceProduct product = {0};
    XrProgramSourceBuildStatus status = build_module_graph(&context, diagnostic_out);
    if (status == XR_PROGRAM_SOURCE_BUILD_OK)
        status = validate_entry_identity(&context, diagnostic_out);
    if (status == XR_PROGRAM_SOURCE_BUILD_OK)
        status = discover_test_entries(&context, diagnostic_out);
    if (status == XR_PROGRAM_SOURCE_BUILD_OK) {
        context.test_count = context.selection_count - input->retained_entry_count - 1u;
        status = discover_export_entries(&context, diagnostic_out);
    }
    if (status == XR_PROGRAM_SOURCE_BUILD_OK)
        status = prepare_semantic_graph(&context, diagnostic_out);
    if (status == XR_PROGRAM_SOURCE_BUILD_OK)
        status = compile_xi_modules(&context, diagnostic_out);
    if (status == XR_PROGRAM_SOURCE_BUILD_OK)
        status = write_product(&context, &product, diagnostic_out);
    build_context_free(&context);

    if (status != XR_PROGRAM_SOURCE_BUILD_OK) {
        xr_program_source_product_free(&product);
        if (operation.active)
            (void) xr_compiler_session_operation_fail(&operation,
                                                      XR_COMPILER_SESSION_OPERATION_FATAL);
        return status;
    }
    if (!xr_compiler_session_operation_succeed(&operation)) {
        xr_program_source_product_free(&product);
        return reject(diagnostic_out, XR_PROGRAM_SOURCE_BUILD_SESSION_REJECTED,
                      XR_PROGRAM_SOURCE_STAGE_SESSION_COMMIT, UINT32_MAX, 0u, 0u,
                      "compiler session rejected the completed source build");
    }
    *product_out = product;
    clear_diagnostic(diagnostic_out);
    return XR_PROGRAM_SOURCE_BUILD_OK;
}

void xr_program_source_product_free(XrProgramSourceProduct *product) {
    if (!product)
        return;
    xr_validated_program_free(product->program);
    xr_program_artifact_free(&product->artifact);
    xr_free(product->retained_function_ids);
    for (uint32_t index = 0u; index < product->test_entry_count; ++index)
        xr_free(product->tests[index].name);
    xr_free(product->tests);
    for (uint32_t index = 0u; index < product->export_count; ++index) {
        xr_free(product->exports[index].xray_name);
        xr_free(product->exports[index].symbol);
        xr_free(product->exports[index].visibility);
        xr_free(product->exports[index].abi);
    }
    xr_free(product->exports);
    xr_free(product->export_function_ids);
    memset(product, 0, sizeof(*product));
}

const char *xr_program_source_build_status_name(XrProgramSourceBuildStatus status) {
    switch (status) {
        case XR_PROGRAM_SOURCE_BUILD_OK:
            return "ok";
        case XR_PROGRAM_SOURCE_BUILD_INVALID_INPUT:
            return "invalid-input";
        case XR_PROGRAM_SOURCE_BUILD_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_SOURCE_BUILD_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_PROGRAM_SOURCE_BUILD_GRAPH_REJECTED:
            return "graph-rejected";
        case XR_PROGRAM_SOURCE_BUILD_ANALYSIS_REJECTED:
            return "analysis-rejected";
        case XR_PROGRAM_SOURCE_BUILD_MONOMORPHIZATION_REJECTED:
            return "monomorphization-rejected";
        case XR_PROGRAM_SOURCE_BUILD_CANONICALIZATION_REJECTED:
            return "canonicalization-rejected";
        case XR_PROGRAM_SOURCE_BUILD_EVIDENCE_REJECTED:
            return "evidence-rejected";
        case XR_PROGRAM_SOURCE_BUILD_PIPELINE_REJECTED:
            return "pipeline-rejected";
        case XR_PROGRAM_SOURCE_BUILD_ENTRY_REJECTED:
            return "entry-rejected";
        case XR_PROGRAM_SOURCE_BUILD_PROGRAM_REJECTED:
            return "program-rejected";
        case XR_PROGRAM_SOURCE_BUILD_SESSION_REJECTED:
            return "session-rejected";
    }
    return "unknown";
}
