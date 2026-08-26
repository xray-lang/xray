/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xa_program_semantic_closure.c - Source-backed pre-Xi PSC KAT
 */

#include "../test_framework.h"
#include "../test_win_compat.h"

#include "frontend/analyzer/xa_program_semantic_closure.h"
#include "frontend/analyzer/xa_scalar_program_authority_internal.h"
#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_mono.h"
#include "frontend/canonical/xcanon.h"
#include "frontend/parser/xast.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xast_walk.h"
#include "frontend/parser/xparse.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "ir/xi_import_resolve.h"
#include "ir/xi_pipeline.h"
#include "ir/xi_program_semantic.h"
#include "ir/xi_program_semantic_plan.h"
#include "module/xmodule.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "plan/format/xr_xtp_internal.h"
#include "plan/format/xr_xsm_schema.h"
#include "plan/semantic/xr_program_semantic_closure.h"
#include "plan/semantic/xr_program_semantic_closure_internal.h"
#include "plan/semantic/xr_scalar_call_semantics.h"
#include "plan/semantic/xr_semantic_plan_internal.h"
#include "plan/semantic/xr_semantic_verify.h"
#include "plan/target/xr_target_builder.h"
#include "plan/target/xr_target_plan_internal.h"
#include "plan/target/xr_target_profile.h"
#include "plan/target/xr_target_verify.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "runtime/xr_runtime_artifact_authority_internal.h"
#include "shared/xr_exact_scalar_registry.h"
#include "toolchain/xcompiler_session.h"
#include "vm/xr_typed_dispatch.h"
#include "vm/xr_vm_decoded_cache.h"
#include "xray.h"
#include "xray_vm.h"
#include <stdio.h>
#include <string.h>

static const char kScalarSource[] = "fn add1(value: i64) -> i64 { return value + 1 }\n"
                                    "fn root() -> i64 { return add1(41) }\n";

static const char kLeafAggregateSource[] =
    "struct Pair { left: i64; right: i64 }\n"
    "fn swap(value: Pair) -> Pair { return Pair{left: value.right, right: value.left} }\n"
    "fn root() -> Pair { return swap(Pair{left: 1, right: 41}) }\n";

typedef struct ScalarFixture {
    XrModuleGraph *graph;
    XrModuleSpec *spec;
    XaAnalyzer *analyzer;
    XaTypedProgram *typed;
} ScalarFixture;

static XrModuleResolver *g_resolver;
static XrCompilerSession *g_session;
static XrVMRuntime *g_isolate;
static char g_scalar_graph_directory[256];
static char g_scalar_graph_producer_path[320];
static char g_scalar_graph_entry_path[320];

static void cleanup_registered_scalar_graph_fixture(void) {
    if (g_scalar_graph_producer_path[0])
        xr_test_unlink(g_scalar_graph_producer_path);
    if (g_scalar_graph_entry_path[0])
        xr_test_unlink(g_scalar_graph_entry_path);
    if (g_scalar_graph_directory[0])
        xr_test_rmdir(g_scalar_graph_directory);
    memset(g_scalar_graph_directory, 0, sizeof(g_scalar_graph_directory));
    memset(g_scalar_graph_producer_path, 0, sizeof(g_scalar_graph_producer_path));
    memset(g_scalar_graph_entry_path, 0, sizeof(g_scalar_graph_entry_path));
}

static void setup(void) {
    XrVMConfig vm_config = {0};
    XrModuleResolverConfig resolver_config = {0};
    g_isolate = xray_vm_new_full(&vm_config);
    ASSERT_NOT_NULL(g_isolate);
    g_session = xr_compiler_session_current_for_isolate(g_isolate);
    ASSERT_NOT_NULL(g_session);
    g_resolver = xr_module_resolver_new(&resolver_config);
    ASSERT_NOT_NULL(g_resolver);
}

static void teardown(void) {
    cleanup_registered_scalar_graph_fixture();
    xr_module_resolver_free(g_resolver);
    xray_vm_delete(g_isolate);
    g_resolver = NULL;
    g_session = NULL;
    g_isolate = NULL;
}

static bool fixture_analyze(ScalarFixture *fixture, const char *source, const char *namespace_id) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->graph = xr_module_graph_new(g_session, g_resolver);
    if (!fixture->graph)
        return false;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = namespace_id,
    };
    char *error = NULL;
    if (xr_module_graph_build_source(fixture->graph, &authority, source, &error) != 0) {
        xr_free(error);
        return false;
    }
    xr_free(error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 || fixture->graph->has_cycle ||
        fixture->graph->spec_count != 1 || fixture->graph->entry_index < 0)
        return false;
    fixture->spec = &fixture->graph->specs[fixture->graph->entry_index];
    fixture->analyzer = xa_analyzer_new(g_session);
    if (!fixture->analyzer)
        return false;
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    xa_analyzer_analyze(fixture->analyzer, "<psc-scalar>", fixture->spec->ast);
    int diagnostic_count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
         diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return false;
    }
    XrHashMap *exports = NULL;
    if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, fixture->spec->ast,
                                                    &exports))
        return false;
    fixture->spec->status = XR_MODSPEC_ANALYZED;
    return true;
}

static bool fixture_publish(ScalarFixture *fixture) {
    XaTypedProgramPublishResult result =
        xa_typed_program_publish(fixture->analyzer, fixture->spec->ast, NULL, 1);
    fixture->typed = result.program;
    return result.reason == XA_TYPED_PROGRAM_REASON_NONE && result.program;
}

static void fixture_cleanup(ScalarFixture *fixture) {
    xa_typed_program_free(fixture->typed);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    memset(fixture, 0, sizeof(*fixture));
}

typedef struct FindCallContext {
    AstNode *call;
} FindCallContext;

static bool find_call_child(AstNode *child, void *context);

static bool find_call(AstNode *node, FindCallContext *context) {
    if (!node || context->call)
        return true;
    if (node->type == AST_CALL_EXPR) {
        context->call = node;
        return true;
    }
    return xr_ast_for_each_child(node, find_call_child, context);
}

static bool find_call_child(AstNode *child, void *context) {
    return find_call(child, (FindCallContext *) context);
}

static bool fingerprint_equal(XrFingerprint left, XrFingerprint right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool generation_id_equal(XrGenerationClosureId left, XrGenerationClosureId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

typedef enum ScalarGraphOuterResignMutation {
    SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_ZERO,
    SCALAR_GRAPH_OUTER_RESIGN_CALL_BINDING_MISMATCH,
    SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_MUTATION,
    SCALAR_GRAPH_OUTER_RESIGN_FOREIGN_EXPORT,
    SCALAR_GRAPH_OUTER_RESIGN_WRONG_IMPORT_LOCATOR,
} ScalarGraphOuterResignMutation;

static bool scalar_graph_outer_resign_rejects(const XrProgramSemanticClosure *source,
                                              ScalarGraphOuterResignMutation mutation) {
    char error[256] = {0};
    XrProgramSemanticClosure *rebuilt = NULL;
    bool ok = xr_program_semantic_closure_create(&source->limits, source->policy_fingerprint,
                                                 &rebuilt, error, sizeof(error)) &&
              xr_program_semantic_closure_set_family(
                  rebuilt, XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL, error,
                  sizeof(error));
    XrProgramSemanticTypeInput scalar_input;
    XrStableId scalar_type = {{0}};
    ok = ok && xr_program_semantic_exact_scalar_type_input(XR_EXACT_SCALAR_I64, &scalar_input) &&
         xr_program_semantic_closure_add_type(rebuilt, &scalar_input, &scalar_type, error,
                                              sizeof(error));
    for (uint32_t i = 0; ok && i < source->module_count; i++) {
        const XrProgramSemanticModuleRecord *row = &source->modules[i];
        XrProgramSemanticModuleInput input = {
            .module_identity = row->module_identity,
            .module_authority_fingerprint = row->module_authority_fingerprint,
            .source_fingerprint = row->source_fingerprint,
            .export_fingerprint = row->export_fingerprint,
        };
        ok = xr_program_semantic_closure_add_module(rebuilt, &input, error, sizeof(error));
    }
    XrStableId entry_function = {{0}};
    for (uint32_t i = 0; ok && i < source->function_count; i++) {
        const XrProgramSemanticFunctionRecord *row = &source->functions[i];
        XrProgramSemanticFunctionParameterInput parameter = {0};
        if (row->parameter_count) {
            const XrProgramSemanticFunctionParameterRecord *record =
                &source->function_parameters[row->parameter_begin];
            parameter.type = record->type;
            parameter.declaration_ordinal = record->declaration_ordinal;
            parameter.mode = record->mode;
        }
        XrProgramSemanticFunctionInput input = {
            .module_identity = row->module_identity,
            .declaration_identity = row->declaration_identity,
            .concrete_instance_identity = row->concrete_instance_identity,
            .declaration_locator = row->declaration_locator,
            .signature_fingerprint = row->signature_fingerprint,
            .effect_fingerprint = row->effect_fingerprint,
            .return_type = row->return_type,
            .parameters = row->parameter_count ? &parameter : NULL,
            .parameter_count = row->parameter_count,
            .capability_mask = row->capability_mask,
            .flags = row->flags,
        };
        XrStableId id = {{0}};
        ok = xr_program_semantic_closure_add_function(rebuilt, &input, &id, error, sizeof(error));
        if (ok && row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)
            entry_function = id;
    }
    XrProgramSemanticDependencyInput dependency = {
        .source_module = source->dependencies[0].source_module,
        .dependency_module = source->dependencies[0].dependency_module,
        .import_locator = source->dependencies[0].import_locator,
        .exported_declaration = source->dependencies[0].exported_declaration,
        .exported_function = source->dependencies[0].exported_function,
        .resolver_binding = source->dependencies[0].resolver_binding,
        .contract_fingerprint = source->dependencies[0].contract_fingerprint,
        .kind = source->dependencies[0].kind,
    };
    XrProgramSemanticCallInput call = {
        .callsite_identity = source->calls[0].callsite_identity,
        .locator = source->calls[0].locator,
        .caller_function = source->calls[0].caller_function,
        .callee_function = source->calls[0].callee_function,
        .resolver_binding = source->calls[0].resolver_binding,
        .contract_fingerprint = source->calls[0].contract_fingerprint,
    };
    switch (mutation) {
        case SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_ZERO:
            memset(&dependency.resolver_binding, 0, sizeof(dependency.resolver_binding));
            break;
        case SCALAR_GRAPH_OUTER_RESIGN_CALL_BINDING_MISMATCH:
            call.resolver_binding.bytes[0] ^= UINT8_C(0x80);
            break;
        case SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_MUTATION:
            dependency.resolver_binding.bytes[0] ^= UINT8_C(0x40);
            call.resolver_binding = dependency.resolver_binding;
            break;
        case SCALAR_GRAPH_OUTER_RESIGN_FOREIGN_EXPORT:
            dependency.exported_declaration =
                source->functions[0].flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY
                    ? source->functions[0].declaration_identity
                    : source->functions[1].declaration_identity;
            dependency.exported_function = entry_function;
            break;
        case SCALAR_GRAPH_OUTER_RESIGN_WRONG_IMPORT_LOCATOR:
            dependency.import_locator.start_column++;
            break;
    }
    bool dependency_added = ok && xr_program_semantic_closure_add_dependency(rebuilt, &dependency,
                                                                             error, sizeof(error));
    if (mutation == SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_ZERO) {
        bool rejected = ok && !dependency_added &&
                        xr_program_semantic_closure_failure_kind(rebuilt) ==
                            XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_INVALID;
        xr_program_semantic_closure_free(rebuilt);
        return rejected;
    }
    XrStableId ignored = {{0}};
    ok = dependency_added &&
         xr_program_semantic_closure_add_call(rebuilt, &call, &ignored, error, sizeof(error));
    /* Freeze re-signs the aggregate PSC/GCI. The independent verifier then
     * rejects the
     * deliberately stale or contradictory typed inner join. */
    bool rejected = ok && !xr_program_semantic_closure_freeze(rebuilt, error, sizeof(error)) &&
                    xr_program_semantic_closure_failure_kind(rebuilt) ==
                        XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_INVALID;
    xr_program_semantic_closure_free(rebuilt);
    return rejected;
}

typedef struct ScalarGraphFixture {
    char directory[256];
    char producer_path[320];
    char entry_path[320];
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
} ScalarGraphFixture;

typedef struct ScalarGraphPlanFixture {
    ScalarGraphFixture *source;
    const XrProgramSemanticClosure *closure;
    XiPipelineResult pipelines[2];
    XiModule *modules[2];
    uint32_t entry_index;
    uint32_t producer_index;
} ScalarGraphPlanFixture;

static bool write_source_file(const char *path, const char *source) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t size = strlen(source);
    bool written = fwrite(source, 1, size, file) == size;
    return fclose(file) == 0 && written;
}

static void scalar_graph_fixture_cleanup(ScalarGraphFixture *fixture);

static bool scalar_graph_analyze_all(ScalarGraphFixture *fixture) {
    for (int topo = 0; topo < fixture->graph->topo_count; topo++) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[topo]];
        xa_analyzer_analyze(fixture->analyzer, spec->source_path, spec->ast);
        int diagnostic_count = 0;
        for (XaDiagnostic *diagnostic =
                 xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
             diagnostic; diagnostic = diagnostic->next) {
            if (diagnostic->severity == XR_DIAG_SEV_ERROR) {
                fprintf(stderr, "  scalar graph analysis failed: %s\n",
                        diagnostic->message);
                return false;
            }
        }
        if (spec->export_symbols)
            xr_hashmap_free(spec->export_symbols);
        XrHashMap *exports = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, spec->ast,
                                                        &exports)) {
            fprintf(stderr, "  scalar graph export collection failed: %s\n",
                    spec->source_path);
            return false;
        }
        spec->export_symbols = exports;
        spec->status = XR_MODSPEC_ANALYZED;
        xa_analyzer_clear_diagnostics(fixture->analyzer);
    }
    return true;
}

static bool scalar_graph_fixture_build(ScalarGraphFixture *fixture, const char *literal) {
    static unsigned int serial;
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->directory, sizeof(fixture->directory),
             "psc_scalar_graph_%u_XXXXXX", serial++);
    if (!xr_test_mkdtemp(fixture->directory))
        return false;
    char absolute_directory[sizeof(fixture->directory)];
    if (!xr_test_realpath_buf(fixture->directory, absolute_directory,
                              sizeof(absolute_directory)))
        goto fail;
    memcpy(fixture->directory, absolute_directory, strlen(absolute_directory) + 1u);
    snprintf(fixture->producer_path, sizeof(fixture->producer_path), "%s/producer.xr",
             fixture->directory);
    snprintf(fixture->entry_path, sizeof(fixture->entry_path), "%s/entry.xr",
             fixture->directory);
    memcpy(g_scalar_graph_directory, fixture->directory, strlen(fixture->directory) + 1u);
    memcpy(g_scalar_graph_producer_path, fixture->producer_path,
           strlen(fixture->producer_path) + 1u);
    memcpy(g_scalar_graph_entry_path, fixture->entry_path,
           strlen(fixture->entry_path) + 1u);
    char entry_source[160];
    snprintf(entry_source, sizeof(entry_source),
             "import { add1 } from \"./producer\"\n"
             "fn root() -> i64 { return add1(%s) }\n",
             literal);
    if (!write_source_file(fixture->producer_path,
                           "export fn add1(value: i64) -> i64 { return value + 1 }\n") ||
        !write_source_file(fixture->entry_path, entry_source))
        goto fail;

    fixture->graph = xr_module_graph_new(g_session, g_resolver);
    if (!fixture->graph)
        goto fail;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = fixture->directory,
    };
    char *graph_error = NULL;
    if (xr_module_graph_build(fixture->graph, fixture->entry_path, &authority,
                              &graph_error) != 0) {
        fprintf(stderr, "  scalar graph build failed: %s\n",
                graph_error ? graph_error : "unknown graph error");
        xr_free(graph_error);
        goto fail;
    }
    xr_free(graph_error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 ||
        fixture->graph->has_cycle || fixture->graph->spec_count != 2 ||
        fixture->graph->topo_count != 2 || fixture->graph->entry_index < 0) {
        fprintf(stderr,
                "  scalar graph topology failed: specs=%d topo=%d entry=%d cycle=%d\n",
                fixture->graph->spec_count, fixture->graph->topo_count,
                fixture->graph->entry_index, fixture->graph->has_cycle);
        goto fail;
    }

    fixture->analyzer = xa_analyzer_new(g_session);
    if (!fixture->analyzer)
        goto fail;
    xa_analyzer_set_build_profile(fixture->analyzer, XA_ANALYZER_BUILD_PROFILE_HOSTED);
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    if (!scalar_graph_analyze_all(fixture))
        goto fail;
    AstNode *roots[2];
    for (int topo = 0; topo < fixture->graph->topo_count; topo++)
        roots[topo] = fixture->graph->specs[fixture->graph->topo_order[topo]].ast;
    for (int topo = 0; topo < fixture->graph->topo_count; topo++)
        if (!xa_mono_pass(roots[topo], roots, fixture->graph->topo_count, g_isolate,
                          fixture->analyzer))
            goto fail;
    for (int topo = 0; topo < fixture->graph->topo_count; topo++) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[topo]];
        XrCompilerSessionScope scope;
        bool has_scope = spec->ast->type == AST_PROGRAM && spec->ast->as.program.arena &&
                         xr_compiler_session_push_arena(g_session, spec->ast->as.program.arena,
                                                        spec->source_path, &scope);
        xr_canon_program(spec->ast, fixture->analyzer, g_session);
        if (has_scope)
            xr_compiler_session_pop_arena(&scope);
    }
    if (!scalar_graph_analyze_all(fixture))
        goto fail;
    return true;

fail:
    scalar_graph_fixture_cleanup(fixture);
    return false;
}

static void scalar_graph_fixture_cleanup(ScalarGraphFixture *fixture) {
    if (!fixture)
        return;
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    if (strcmp(g_scalar_graph_directory, fixture->directory) == 0) {
        cleanup_registered_scalar_graph_fixture();
    } else {
        if (fixture->producer_path[0])
            xr_test_unlink(fixture->producer_path);
        if (fixture->entry_path[0])
            xr_test_unlink(fixture->entry_path);
        if (fixture->directory[0])
            xr_test_rmdir(fixture->directory);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static void scalar_graph_plan_fixture_cleanup(ScalarGraphPlanFixture *fixture) {
    if (!fixture)
        return;
    for (uint32_t i = 0; i < 2; i++)
        xi_pipeline_result_free(&fixture->pipelines[i]);
    memset(fixture, 0, sizeof(*fixture));
}

static bool scalar_graph_import_join_is_exact(const ScalarGraphPlanFixture *fixture) {
    if (!fixture || fixture->entry_index >= 2 || fixture->producer_index >= 2)
        return false;
    XiModule *entry = fixture->modules[fixture->entry_index];
    XiModule *producer = fixture->modules[fixture->producer_index];
    if (!entry || !producer || producer->nexports != 1 || !producer->exports ||
        producer->nfuncs != 1 || !producer->functions || !producer->slot_funcs)
        return false;
    XiImportRef *match = NULL;
    for (uint16_t slot = 0; slot < entry->nslots; slot++) {
        XiImportRef *candidate = entry->slot_imports ? entry->slot_imports[slot] : NULL;
        if (!candidate)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    return match && match->resolution_attempted &&
           match->resolved_mod_index == (int) fixture->producer_index &&
           match->resolved_module == producer && match->resolved_func == producer->functions[0] &&
           match->resolved_export_slot == 0 && match->resolved_shared_slot >= 0 &&
           (uint16_t) match->resolved_shared_slot == producer->exports[0].shared_slot &&
           (uint16_t) match->resolved_shared_slot < producer->nslots &&
           producer->slot_funcs[match->resolved_shared_slot] == producer->functions[0];
}

static bool scalar_graph_plan_fixture_build(ScalarGraphPlanFixture *fixture,
                                            ScalarGraphFixture *source,
                                            const XrProgramSemanticClosure *closure, char *error,
                                            size_t error_size) {
    memset(fixture, 0, sizeof(*fixture));
    if (error && error_size)
        error[0] = '\0';
    fixture->source = source;
    fixture->closure = closure;
    if (!source || !closure || xr_program_semantic_closure_schema(closure) !=
            XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL)
        goto fail;

    XiPipelineConfig config = xi_pipeline_aot_config();
    /* The fixture already ran whole-graph mono, canonicalization, and final
     * analysis. The product driver disables a second canonicalization here so
     * analyzer authority and the lowered AST remain the same snapshot. */
    config.run_canonicalize = false;
    config.program_semantic_closure = closure;
    config.module_graph = source->graph;
    config.graph_modules = fixture->modules;
    config.graph_module_count = 2;
    /* The pipeline resolves dependency-complete imports before ARC. The
     * product driver repeats the walk after the module array is complete so
     * every retained module/function/slot pointer is also exact. */
    for (uint32_t topo = 0; topo < 2; topo++) {
        int spec_index = source->graph->topo_order[topo];
        XrModuleSpec *spec = &source->graph->specs[spec_index];
        config.source_file = spec->source_path;
        config.module_identity = spec->canonical;
        config.module_name = spec_index == source->graph->entry_index
                                 ? "entry_graph_kat"
                                 : "producer_graph_kat";
        fixture->pipelines[topo] = xi_pipeline_compile_program(
            spec->ast, source->analyzer, g_isolate, &config);
        if (fixture->pipelines[topo].status != XI_PIPE_OK ||
            !fixture->pipelines[topo].ir || !fixture->pipelines[topo].ir->module) {
            if (error && error_size)
                snprintf(error, error_size, "%s", fixture->pipelines[topo].error.detail);
            goto fail;
        }
        fixture->modules[topo] = fixture->pipelines[topo].ir->module;
        if (spec_index == source->graph->entry_index)
            fixture->entry_index = topo;
    }
    fixture->producer_index = fixture->entry_index == 0 ? 1u : 0u;
    if (!scalar_graph_import_join_is_exact(fixture))
        goto fail;
    for (uint32_t topo = 0; topo < 2; topo++) {
        int spec_index = source->graph->topo_order[topo];
        xi_resolve_imports(fixture->pipelines[topo].ir, source->graph,
                           source->graph->specs[spec_index].source_path,
                           fixture->modules, 2);
    }
    if (!scalar_graph_import_join_is_exact(fixture) ||
        !xi_program_semantic_verify_module_set(fixture->modules, 2, fixture->entry_index, NULL,
                                               error, error_size) ||
        !xi_program_semantic_plan_verify_module_set(fixture->modules, 2,
                                                    fixture->entry_index, error, error_size))
        goto fail;
    return true;

fail:
    scalar_graph_plan_fixture_cleanup(fixture);
    return false;
}

static XrSemanticPlan *scalar_graph_plan(const ScalarGraphPlanFixture *fixture, uint32_t index) {
    return fixture && index < 2 && fixture->modules[index] && fixture->modules[index]->init
               ? fixture->modules[index]->init->semantic_plan
               : NULL;
}

static XiImportRef *scalar_graph_entry_import(const ScalarGraphPlanFixture *fixture) {
    XiModule *entry = fixture ? fixture->modules[fixture->entry_index] : NULL;
    XiImportRef *match = NULL;
    for (uint16_t slot = 0; entry && slot < entry->nslots; slot++) {
        XiImportRef *candidate = entry->slot_imports ? entry->slot_imports[slot] : NULL;
        if (!candidate)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static void scalar_graph_refresh_plan(XrSemanticPlan *plan) {
    if (plan)
        xr_semantic_plan_compute_fingerprint(plan, &plan->fingerprint);
}

static bool scalar_graph_plan_set_valid(const ScalarGraphPlanFixture *fixture) {
    char error[512] = {0};
    return xi_program_semantic_plan_verify_module_set(
        (XiModule *const *) fixture->modules, 2, fixture->entry_index, error, sizeof(error));
}

static bool scalar_graph_xsm_roundtrip(ScalarGraphPlanFixture *fixture) {
    XrSemanticPlan *producer = scalar_graph_plan(fixture, fixture->producer_index);
    XrSemanticPlan *entry = scalar_graph_plan(fixture, fixture->entry_index);
    uint8_t *producer_bytes = NULL;
    uint8_t *producer_again = NULL;
    uint8_t *entry_bytes = NULL;
    uint8_t *entry_again = NULL;
    size_t producer_size = 0, producer_again_size = 0;
    size_t entry_size = 0, entry_again_size = 0;
    XrSemanticPlan *decoded_producer = NULL;
    XrSemanticPlan *decoded_entry = NULL;
    XrSemanticPlan *plain_entry = NULL;
    char error[512] = {0};
    bool ok = producer && entry && producer->dependency_count == 0 &&
              producer->source_export_count == 1 && entry->dependency_count == 1 &&
              xr_xsm_encode(producer, &producer_bytes, &producer_size, error, sizeof(error)) &&
              xr_xsm_encode(producer, &producer_again, &producer_again_size, error,
                            sizeof(error)) &&
              producer_size == producer_again_size &&
              memcmp(producer_bytes, producer_again, producer_size) == 0 &&
              xr_xsm_encode(entry, &entry_bytes, &entry_size, error, sizeof(error)) &&
              xr_xsm_encode(entry, &entry_again, &entry_again_size, error, sizeof(error)) &&
              entry_size == entry_again_size && memcmp(entry_bytes, entry_again, entry_size) == 0 &&
              xr_xsm_decode(producer_bytes, producer_size, &decoded_producer, error,
                            sizeof(error)) &&
              !xr_xsm_decode(entry_bytes, entry_size, &plain_entry, error, sizeof(error));
    const XrSemanticPlan *dependencies[1] = {decoded_producer};
    ok = ok && !plain_entry && xr_xsm_decode_module_set(
                                    entry_bytes, entry_size, dependencies, 1, &decoded_entry,
                                    error, sizeof(error)) &&
         xr_semantic_plan_verify(decoded_producer, error, sizeof(error)) &&
         xr_semantic_plan_verify_module_set(decoded_entry, dependencies, 1, error,
                                            sizeof(error));
    uint8_t *producer_roundtrip = NULL;
    uint8_t *entry_roundtrip = NULL;
    size_t producer_roundtrip_size = 0, entry_roundtrip_size = 0;
    ok = ok && xr_xsm_encode(decoded_producer, &producer_roundtrip, &producer_roundtrip_size,
                             error, sizeof(error)) &&
         xr_xsm_encode(decoded_entry, &entry_roundtrip, &entry_roundtrip_size, error,
                       sizeof(error)) &&
         producer_roundtrip_size == producer_size && entry_roundtrip_size == entry_size &&
         memcmp(producer_roundtrip, producer_bytes, producer_size) == 0 &&
         memcmp(entry_roundtrip, entry_bytes, entry_size) == 0;

    XrSemanticPlan *saved_entry_root = fixture->modules[fixture->entry_index]->init->semantic_plan;
    XrSemanticPlan *saved_entry_local =
        fixture->modules[fixture->entry_index]->functions[0]->semantic_plan;
    XrSemanticPlan *saved_producer_root =
        fixture->modules[fixture->producer_index]->init->semantic_plan;
    XrSemanticPlan *saved_producer_local =
        fixture->modules[fixture->producer_index]->functions[0]->semantic_plan;
    fixture->modules[fixture->entry_index]->init->semantic_plan = decoded_entry;
    fixture->modules[fixture->entry_index]->functions[0]->semantic_plan = decoded_entry;
    fixture->modules[fixture->producer_index]->init->semantic_plan = decoded_producer;
    fixture->modules[fixture->producer_index]->functions[0]->semantic_plan = decoded_producer;
    ok = ok && xi_program_semantic_plan_verify_module_set(
                   fixture->modules, 2, fixture->entry_index, error, sizeof(error));
    fixture->modules[fixture->entry_index]->init->semantic_plan = saved_entry_root;
    fixture->modules[fixture->entry_index]->functions[0]->semantic_plan = saved_entry_local;
    fixture->modules[fixture->producer_index]->init->semantic_plan = saved_producer_root;
    fixture->modules[fixture->producer_index]->functions[0]->semantic_plan = saved_producer_local;

    xr_free(entry_roundtrip);
    xr_free(producer_roundtrip);
    xr_semantic_plan_free(plain_entry);
    xr_semantic_plan_free(decoded_entry);
    xr_semantic_plan_free(decoded_producer);
    xr_free(entry_again);
    xr_free(entry_bytes);
    xr_free(producer_again);
    xr_free(producer_bytes);
    return ok;
}

static bool scalar_graph_program_row_mutations(ScalarGraphPlanFixture *fixture) {
    XrSemanticPlan *producer = scalar_graph_plan(fixture, fixture->producer_index);
    XrSemanticPlan *entry = scalar_graph_plan(fixture, fixture->entry_index);
    if (!producer || !entry || !producer->program_type_bindings ||
        !producer->program_function_bindings || !entry->program_call_bindings ||
        !entry->program_dependency_bindings)
        return false;
    bool ok = true;

    uint8_t saved_scalar =
        producer->types[producer->program_type_bindings[0].semantic_type].scalar_rep;
    producer->types[producer->program_type_bindings[0].semantic_type].scalar_rep = XR_NATIVE_I32;
    scalar_graph_refresh_plan(producer);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    producer->types[producer->program_type_bindings[0].semantic_type].scalar_rep = saved_scalar;
    scalar_graph_refresh_plan(producer);

    uint32_t saved_export_count = producer->source_export_count;
    producer->source_export_count = 0;
    scalar_graph_refresh_plan(producer);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    producer->source_export_count = saved_export_count;
    scalar_graph_refresh_plan(producer);

    uint8_t saved_role = producer->program_function_bindings[0].flags;
    producer->program_function_bindings[0].flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
    scalar_graph_refresh_plan(producer);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    producer->program_function_bindings[0].flags = saved_role;
    scalar_graph_refresh_plan(producer);

    uint32_t saved_module_row = entry->program_provenance.program_module_row;
    entry->program_provenance.program_module_row = fixture->modules[fixture->producer_index]
                                                       ->psc_module_index;
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    entry->program_provenance.program_module_row = saved_module_row;
    scalar_graph_refresh_plan(entry);

    entry->program_provenance.program_module.bytes[0] ^= UINT8_C(0x80);
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    entry->program_provenance.program_module.bytes[0] ^= UINT8_C(0x80);
    scalar_graph_refresh_plan(entry);

    uint32_t saved_function_row = entry->program_function_bindings[0].program_row;
    entry->program_function_bindings[0].program_row =
        producer->program_function_bindings[0].program_row;
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    entry->program_function_bindings[0].program_row = saved_function_row;
    scalar_graph_refresh_plan(entry);
    return ok && scalar_graph_plan_set_valid(fixture);
}

static bool scalar_graph_export_carrier_mutations(ScalarGraphPlanFixture *fixture) {
    XiModule *producer = fixture ? fixture->modules[fixture->producer_index] : NULL;
    XiModule *entry = fixture ? fixture->modules[fixture->entry_index] : NULL;
    if (!producer || producer->nexports != 1 || !producer->exports || producer->nfuncs != 1 ||
        !producer->functions || !entry || entry->nfuncs != 1 || !entry->functions ||
        !producer->exports[0].value_type ||
        producer->exports[0].value_type->kind != XR_KIND_FUNCTION)
        return false;
    char error[512] = {0};
    XrType *carrier = producer->exports[0].value_type;
    producer->exports[0].value_type = producer->functions[0]->return_type;
    bool ok = !xi_program_semantic_verify_module_set(
        fixture->modules, 2, fixture->entry_index, NULL, error, sizeof(error));
    producer->exports[0].value_type = carrier;

    int saved_min_params = carrier->function.min_params;
    carrier->function.min_params = 0;
    ok = ok && !xi_program_semantic_verify_module_set(
                   fixture->modules, 2, fixture->entry_index, NULL, error, sizeof(error));
    carrier->function.min_params = saved_min_params;

    bool saved_nothrow = entry->functions[0]->error_effect_nothrow;
    entry->functions[0]->error_effect_nothrow = false;
    ok = ok && !xi_program_semantic_verify_module_set(
                   fixture->modules, 2, fixture->entry_index, NULL, error, sizeof(error));
    entry->functions[0]->error_effect_nothrow = saved_nothrow;
    return ok && scalar_graph_plan_set_valid(fixture);
}

static bool scalar_graph_external_join_mutations(ScalarGraphPlanFixture *fixture) {
    XrSemanticPlan *entry = scalar_graph_plan(fixture, fixture->entry_index);
    if (!entry || entry->program_dependency_binding_count != 1 ||
        entry->program_call_binding_count != 1 || entry->call_target_count != 1)
        return false;
    bool ok = true;
    XrSemanticProgramDependencyBinding *dependency = &entry->program_dependency_bindings[0];
    XrSemanticProgramCallBinding *call = &entry->program_call_bindings[0];
    XrSemanticCallTargetRecord *target = &entry->call_targets[0];

    dependency->resolver_binding.bytes[0] ^= UINT8_C(0x40);
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    dependency->resolver_binding.bytes[0] ^= UINT8_C(0x40);
    scalar_graph_refresh_plan(entry);

    uint32_t saved_dependency = call->program_dependency;
    call->program_dependency = XR_SEMANTIC_INDEX_NONE;
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    call->program_dependency = saved_dependency;
    scalar_graph_refresh_plan(entry);

    uint32_t saved_source_export = target->source_export;
    target->source_export = XR_SEMANTIC_INDEX_NONE;
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    target->source_export = saved_source_export;
    scalar_graph_refresh_plan(entry);

    call->callee_program_function.bytes[0] ^= UINT8_C(0x20);
    scalar_graph_refresh_plan(entry);
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    call->callee_program_function.bytes[0] ^= UINT8_C(0x20);
    scalar_graph_refresh_plan(entry);
    return ok && scalar_graph_plan_set_valid(fixture);
}

static bool scalar_graph_module_and_plan_mutations(ScalarGraphPlanFixture *fixture) {
    char error[512] = {0};
    XiModule *entry = fixture->modules[fixture->entry_index];
    XiModule *producer = fixture->modules[fixture->producer_index];
    XrSemanticPlan *entry_plan = entry->init->semantic_plan;
    XrSemanticPlan *producer_plan = producer->init->semantic_plan;
    XiModule *missing[2] = {producer, NULL};
    XiModule *duplicate[2] = {producer, producer};
    XiModule *reordered[2] = {entry, producer};
    XiModule foreign = *producer;
    foreign.identity = "script-module-v1:foreign-graph-kat";
    XiModule *foreign_set[2] = {&foreign, entry};
    bool ok = !xi_program_semantic_plan_verify_module_set(missing, 2, fixture->entry_index, error,
                                                          sizeof(error)) &&
              !xi_program_semantic_plan_verify_module_set(duplicate, 2, fixture->entry_index,
                                                          error, sizeof(error)) &&
              !xi_program_semantic_plan_verify_module_set(reordered, 2, 0, error,
                                                          sizeof(error)) &&
              !xi_program_semantic_plan_verify_module_set(foreign_set, 2, 1, error,
                                                          sizeof(error));

    const XrSemanticPlan *duplicate_plans[2] = {producer_plan, producer_plan};
    const XrSemanticPlan *foreign_plan[1] = {entry_plan};
    ok = ok && !xr_semantic_plan_verify_module_set(entry_plan, NULL, 0, error, sizeof(error)) &&
         !xr_semantic_plan_verify_module_set(entry_plan, duplicate_plans, 2, error,
                                             sizeof(error)) &&
         !xr_semantic_plan_verify_module_set(entry_plan, foreign_plan, 1, error, sizeof(error));

    XiImportRef *ref = scalar_graph_entry_import(fixture);
    if (!ref)
        return false;
    int saved_module_index = ref->resolved_mod_index;
    ref->resolved_mod_index = (int) fixture->entry_index;
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    ref->resolved_mod_index = saved_module_index;

    XiFunc *saved_resolved_function = ref->resolved_func;
    ref->resolved_func = entry->functions[0];
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    ref->resolved_func = saved_resolved_function;

    uint32_t saved_attachment = entry->functions[0]->semantic_plan_function_index;
    entry->functions[0]->semantic_plan_function_index = XR_SEMANTIC_INDEX_NONE;
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    entry->functions[0]->semantic_plan_function_index = saved_attachment;

    XrSemanticPlan *saved_plan = entry->functions[0]->semantic_plan;
    entry->functions[0]->semantic_plan = producer_plan;
    ok = ok && !scalar_graph_plan_set_valid(fixture);
    entry->functions[0]->semantic_plan = saved_plan;
    return ok && scalar_graph_plan_set_valid(fixture);
}

static bool scalar_graph_target_mutation_rejected(XrTargetPlan *target, void *field) {
    uint8_t *byte = (uint8_t *) field;
    char error[512] = {0};
    *byte ^= 1u;
    bool rejected = !xr_target_plan_verify(target, error, sizeof(error));
    *byte ^= 1u;
    return rejected;
}

static uint8_t *scalar_graph_xtp_directory_entry(uint8_t *bytes,
                                                 XrXtpSectionKind kind) {
    return bytes + XR_XTP_HEADER_SIZE +
           ((size_t) kind - 1u) * XR_XTP_DIRECTORY_ENTRY_SIZE;
}

static void scalar_graph_xtp_resign_artifact(uint8_t *bytes, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, bytes, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context,
                     bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET);
}

static void scalar_graph_xtp_resign_section(uint8_t *bytes,
                                            XrXtpSectionKind kind) {
    uint8_t *entry = scalar_graph_xtp_directory_entry(bytes, kind);
    size_t offset = (size_t) xr_xtp_take_u64(entry + 8u);
    size_t length = (size_t) xr_xtp_take_u64(entry + 16u);
    xr_sha256(bytes + offset, length, entry + 40u);
}

static bool scalar_graph_xtp_materialization_rejected(
    const XrXtpCandidate *candidate,
    const XrSemanticPlan *const *semantic_modules, uint32_t semantic_module_count,
    XrTargetProfile *profile) {
    XrTargetPlan *rejected = (XrTargetPlan *) (uintptr_t) 1;
    char error[512] = {0};
    bool accepted = xr_xtp_materialize_target_plan_program_graph(
        candidate, semantic_modules, semantic_module_count, profile, &rejected,
        error, sizeof(error));
    if (accepted)
        xr_target_plan_free(rejected);
    return !accepted && rejected == NULL && strncmp(error, "XR_", 3u) == 0;
}

static bool scalar_graph_xtp_bytes_rejected(
    const uint8_t *bytes, size_t size,
    const XrSemanticPlan *const *semantic_modules, XrTargetProfile *profile) {
    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    bool decoded = xr_xtp_decode_candidate(bytes, size, &candidate, error,
                                           sizeof(error));
    bool rejected = decoded && scalar_graph_xtp_materialization_rejected(
                                   candidate, semantic_modules, 2u, profile);
    xr_xtp_candidate_release(candidate);
    return rejected;
}

static bool scalar_graph_xtp_resigned_partition_semantic_rejected(
    XrTargetPlan *target, const uint8_t *bytes, size_t size,
    const XrSemanticPlan *const *semantic_modules, XrTargetProfile *profile) {
    if (!target || !bytes || !size || !semantic_modules || !profile ||
        target->program_graphs_count != 1u ||
        target->module_partitions_count != 2u)
        return false;
    uint32_t partition_index = target->program_graphs[0].producer_partition;
    if (partition_index >= target->module_partitions_count)
        return false;

    XrFingerprint saved_semantic =
        target->module_partitions[partition_index].semantic_fingerprint;
    target->module_partitions[partition_index].semantic_fingerprint.bytes[0] ^= 1u;
    XrFingerprint forged_semantic =
        target->module_partitions[partition_index].semantic_fingerprint;
    XrFingerprint forged_plan;
    xr_target_plan_compute_fingerprint(target, &forged_plan);
    target->module_partitions[partition_index].semantic_fingerprint = saved_semantic;

    XrFingerprint restored_plan;
    xr_target_plan_compute_fingerprint(target, &restored_plan);
    if (!xr_fingerprint_equal(restored_plan, target->fingerprint) ||
        xr_fingerprint_equal(forged_plan, restored_plan))
        return false;

    uint8_t *forged = (uint8_t *) xr_malloc(size);
    if (!forged)
        return false;
    memcpy(forged, bytes, size);
    uint8_t *partition_entry = scalar_graph_xtp_directory_entry(
        forged, XR_XTP_SECTION_MODULE_PARTITIONS);
    size_t partition_offset =
        (size_t) xr_xtp_take_u64(partition_entry + 8u);
    size_t partition_length =
        (size_t) xr_xtp_take_u64(partition_entry + 16u);
    uint32_t partition_count =
        (uint32_t) xr_xtp_take_u64(partition_entry + 24u);
    uint32_t partition_row_size = xr_xtp_take_u32(partition_entry + 32u);
    bool exact =
        partition_count == target->module_partitions_count &&
        partition_row_size ==
            xr_xtp_wire_row_size(XR_XTP_SECTION_MODULE_PARTITIONS) &&
        partition_index < partition_count &&
        partition_length == (size_t) partition_count * partition_row_size &&
        partition_offset <= size && partition_length <= size - partition_offset;
    if (exact) {
        uint8_t *partition_row =
            forged + partition_offset +
            (size_t) partition_index * partition_row_size;
        memcpy(partition_row + XR_STABLE_ID_BYTES, forged_semantic.bytes,
               sizeof(forged_semantic.bytes));
        memcpy(forged + 168u, forged_plan.bytes, sizeof(forged_plan.bytes));
        scalar_graph_xtp_resign_section(
            forged, XR_XTP_SECTION_MODULE_PARTITIONS);
        scalar_graph_xtp_resign_artifact(forged, size);
    }

    XrXtpCandidate *candidate = NULL;
    char error[512] = {0};
    exact = exact && xr_xtp_decode_candidate(forged, size, &candidate, error,
                                             sizeof(error));
    XrTargetModulePartitionRecord decoded_partitions[2] = {0};
    const XrXtpSectionView *view =
        exact ? xr_xtp_candidate_section(
                    candidate, XR_XTP_SECTION_MODULE_PARTITIONS)
              : NULL;
    exact = exact && view && view->count == 2u &&
            xr_xtp_decode_rows(XR_XTP_SECTION_MODULE_PARTITIONS,
                               candidate->bytes + view->offset, view->count,
                               decoded_partitions) &&
            xr_fingerprint_equal(candidate->identity.plan_fingerprint,
                                 forged_plan) &&
            xr_fingerprint_equal(
                decoded_partitions[partition_index].semantic_fingerprint,
                forged_semantic);
    XrTargetPlan *materialized = (XrTargetPlan *) (uintptr_t) 1;
    bool accepted =
        exact && xr_xtp_materialize_target_plan_program_graph(
                     candidate, semantic_modules, 2u, profile, &materialized,
                     error, sizeof(error));
    if (accepted)
        xr_target_plan_free(materialized);
    bool rejected_by_canonical_semantics =
        exact && !accepted && materialized == NULL &&
        strstr(error, "program graph module partition identity is invalid") !=
            NULL;
    xr_xtp_candidate_release(candidate);
    xr_free(forged);
    return rejected_by_canonical_semantics;
}

static bool scalar_graph_runtime_load_rejected(
    const uint8_t *bytes, size_t size,
    const XrSemanticPlan *const *semantic_modules) {
    const XrSemanticPlan *standalone = NULL;
    char error[512] = {0};
    for (uint32_t row = 0; row < 2u; row++)
        if (semantic_modules[row] &&
            xr_semantic_plan_dependency_count(semantic_modules[row]) == 0u &&
            xr_semantic_plan_verify(semantic_modules[row], error,
                                    sizeof(error)))
            standalone = semantic_modules[row];
    XrRuntimeArtifactAuthority *authority = NULL;
    bool ready = standalone && xr_runtime_artifact_authority_create_internal(
                                   standalone, &authority, error,
                                   sizeof(error));
    XrTargetPlan *loaded = (XrTargetPlan *) (uintptr_t) 1;
    bool accepted = ready && xr_runtime_target_plan_load(
                                 bytes, size, authority, &loaded, error,
                                 sizeof(error));
    if (accepted)
        xr_target_plan_free(loaded);
    xr_runtime_artifact_authority_free(authority);
    return ready && !accepted && loaded == NULL &&
           strncmp(error, "XR_", 3u) == 0;
}

static bool scalar_graph_typed_vm_is_exact(XrTargetPlan *target);

static bool scalar_graph_xtp_roundtrip(
    XrTargetPlan *target, const XrSemanticPlan *const *semantic_modules,
    XrTargetProfile *profile) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    XrXtpCandidate *candidate = NULL;
    XrTargetPlan *ordinary = NULL;
    XrTargetPlan *decoded = NULL;
    char error[512] = {0};
    bool exact =
        xr_xtp_encode_plan(target, &bytes, &size, error, sizeof(error)) &&
        xr_xtp_decode_candidate(bytes, size, &candidate, error, sizeof(error));
    if (exact) {
        ordinary = (XrTargetPlan *) (uintptr_t) 1;
        bool ordinary_result = xr_xtp_materialize_target_plan(
            candidate, semantic_modules[0], profile, &ordinary, error,
            sizeof(error));
        if (ordinary_result)
            xr_target_plan_free(ordinary);
        exact = !ordinary_result && ordinary == NULL;
    }
    if (exact) {
        const XrSemanticPlan *reordered[2] = {semantic_modules[1],
                                              semantic_modules[0]};
        const XrSemanticPlan *duplicate[2] = {semantic_modules[0],
                                              semantic_modules[0]};
        const XrSemanticPlan *missing[2] = {semantic_modules[0], NULL};
        exact = scalar_graph_xtp_materialization_rejected(
                    candidate, semantic_modules, 1u, profile) &&
                scalar_graph_xtp_materialization_rejected(
                    candidate, reordered, 2u, profile) &&
                scalar_graph_xtp_materialization_rejected(
                    candidate, duplicate, 2u, profile) &&
                scalar_graph_xtp_materialization_rejected(
                    candidate, missing, 2u, profile);
    }
    if (exact) {
        error[0] = '\0';
        exact = xr_xtp_materialize_target_plan_program_graph(
                    candidate, semantic_modules, 2u, profile, &decoded, error,
                    sizeof(error)) &&
                decoded && xr_target_plan_is_verified(decoded) &&
                xr_target_plan_program_module_count(decoded) == 2u &&
                xr_fingerprint_equal(xr_target_plan_fingerprint(decoded),
                                     xr_target_plan_fingerprint(target)) &&
                xr_fingerprint_equal(
                    xr_target_plan_semantic_fingerprint(decoded),
                    xr_target_plan_semantic_fingerprint(target)) &&
                scalar_graph_typed_vm_is_exact(decoded);
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    if (exact)
        exact = xr_xtp_encode_plan(decoded, &encoded, &encoded_size, error,
                                   sizeof(error)) &&
                encoded_size == size && memcmp(encoded, bytes, size) == 0;
    if (exact)
        exact = scalar_graph_runtime_load_rejected(bytes, size,
                                                   semantic_modules);
    if (exact)
        exact = scalar_graph_xtp_resigned_partition_semantic_rejected(
            target, bytes, size, semantic_modules, profile);
    uint8_t *mutated = exact ? (uint8_t *) xr_malloc(size) : NULL;
    if (exact)
        exact = mutated != NULL;
    if (exact) {
        memcpy(mutated, bytes, size);
        uint8_t *graph_entry = scalar_graph_xtp_directory_entry(
            mutated, XR_XTP_SECTION_PROGRAM_GRAPHS);
        size_t graph_offset = (size_t) xr_xtp_take_u64(graph_entry + 8u);
        mutated[graph_offset + 32u] ^= 1u;
        scalar_graph_xtp_resign_section(mutated,
                                        XR_XTP_SECTION_PROGRAM_GRAPHS);
        scalar_graph_xtp_resign_artifact(mutated, size);
        exact = scalar_graph_xtp_bytes_rejected(
            mutated, size, semantic_modules, profile);
    }
    if (exact) {
        memcpy(mutated, bytes, size);
        mutated[72u] ^= 1u;
        scalar_graph_xtp_resign_artifact(mutated, size);
        exact = scalar_graph_xtp_bytes_rejected(
            mutated, size, semantic_modules, profile);
    }
    xr_free(mutated);
    xr_xtp_encoded_free(encoded);
    xr_target_plan_free(decoded);
    xr_xtp_candidate_release(candidate);
    xr_xtp_encoded_free(bytes);
    return exact;
}

static void scalar_graph_target_refresh_fingerprints(XrTargetPlan *target) {
    for (uint32_t layout = 0; layout < target->layouts_count; layout++)
        xr_target_layout_compute_fingerprint(target, layout,
                                             &target->layouts[layout].fingerprint);
    for (uint32_t call = 0; call < target->calls_count; call++)
        xr_target_call_compute_fingerprint(target, call, &target->calls[call].fingerprint);
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
}

static bool scalar_graph_target_rehashed_mutation_rejected(XrTargetPlan *target) {
    char error[512] = {0};
    scalar_graph_target_refresh_fingerprints(target);
    return !xr_target_plan_verify(target, error, sizeof(error));
}

static bool scalar_graph_typed_vm_is_exact(XrTargetPlan *target) {
    uint32_t graph_count = 0;
    const XrTargetProgramGraphRecord *graphs =
        xr_target_plan_program_graphs(target, &graph_count);
    if (!target || !graphs || graph_count != 1u)
        return false;
    XrFingerprint fingerprint = xr_target_plan_fingerprint(target);
    XrVmDecodedCache *cache = NULL;
    if (xr_typed_decoded_cache_create(target, &fingerprint, &cache) !=
        XR_VM_DECODED_CACHE_OK)
        return false;
    static const XrTypedDispatchProvider providers[] = {
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    };
    bool exact = true;
    for (size_t provider = 0;
         exact && provider < sizeof(providers) / sizeof(providers[0]); provider++) {
        int64_t result = -1;
        XrTypedDispatchI64Request request = {
            .verified_plan = target,
            .required_plan_fingerprint = &fingerprint,
            .result = &result,
            .provider = providers[provider],
            .function = graphs[0].entry_target_function,
        };
        exact = xr_typed_dispatch_execute_i64(&request) == XR_TYPED_DISPATCH_OK &&
                result == 42;
        request.decoded_cache = cache;
        result = -1;
        exact = exact &&
                xr_typed_dispatch_execute_i64(&request) == XR_TYPED_DISPATCH_OK &&
                result == 42;
        request.function = graphs[0].producer_target_function;
        result = -1;
        exact = exact && xr_typed_dispatch_execute_i64(&request) ==
                             XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE &&
                result == 0;
    }
    xr_typed_decoded_cache_free(cache);
    return exact;
}

static bool scalar_graph_typed_vm_rejects_resigned_authority(
    XrTargetPlan *target) {
    if (!target || target->program_graphs_count != 1u)
        return false;
    XrTargetProgramGraphRecord *graph = &target->program_graphs[0];
    uint32_t saved_entry = graph->entry_target_function;
    XrFingerprint saved_fingerprint = xr_target_plan_fingerprint(target);
    if (saved_entry == graph->producer_target_function)
        return false;
    graph->entry_target_function = graph->producer_target_function;
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    XrFingerprint resigned = xr_target_plan_fingerprint(target);
    int64_t result = -1;
    XrTypedDispatchI64Request request = {
        .verified_plan = target,
        .required_plan_fingerprint = &resigned,
        .result = &result,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = saved_entry,
    };
    bool rejected = xr_typed_dispatch_execute_i64(&request) ==
                        XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED &&
                    result == 0;
    graph->entry_target_function = saved_entry;
    xr_target_plan_compute_fingerprint(target, &target->fingerprint);
    return rejected && xr_fingerprint_equal(
                           xr_target_plan_fingerprint(target), saved_fingerprint);
}

static bool scalar_graph_target_plan_is_exact(ScalarGraphPlanFixture *fixture) {
    XrTargetProfile *profile = NULL;
    XrTargetPlan *target = NULL;
    char error[512] = {0};
    if (!xr_runtime_target_profile_build_native_hosted(&profile, error, sizeof(error)))
        return false;
    XrSemanticPlan *entry = scalar_graph_plan(fixture, fixture->entry_index);
    XrSemanticPlan *producer = scalar_graph_plan(fixture, fixture->producer_index);
    const XrSemanticProgramProvenance *entry_program =
        xr_semantic_plan_program_provenance(entry);
    const XrSemanticProgramProvenance *producer_program =
        xr_semantic_plan_program_provenance(producer);
    const XrSemanticPlan *modules[2] = {NULL, NULL};
    if (entry_program && producer_program && entry_program->program_module_row < 2u &&
        producer_program->program_module_row < 2u) {
        modules[entry_program->program_module_row] = entry;
        modules[producer_program->program_module_row] = producer;
    }
    bool exact = xr_target_plan_build_program_graph(modules, 2u, profile, &target,
                                                    error, sizeof(error)) &&
                 target && xr_target_plan_is_verified(target) &&
                 xr_target_plan_schema_version(target) == XR_TARGET_PLAN_SCHEMA_VERSION;
    uint32_t graph_count = 0, partition_count = 0, call_count = 0, argument_count = 0;
    uint32_t instruction_count = 0, expectation_count = 0;
    const XrTargetProgramGraphRecord *graphs = xr_target_plan_program_graphs(target, &graph_count);
    const XrTargetModulePartitionRecord *partitions =
        xr_target_plan_module_partitions(target, &partition_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target, &argument_count);
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(target, &instruction_count);
    xr_target_plan_entry_expectations(target, &expectation_count);
    exact = exact && graphs && graph_count == 1u && partitions && partition_count == 2u &&
            calls && call_count == 1u && arguments && argument_count == 1u &&
            instructions && instruction_count != 0u && expectation_count == 0u;
    if (exact) {
        const XrTargetProgramGraphRecord *graph = &graphs[0];
        const XrTargetCallRecord *call = &calls[graph->target_call];
        const XrTargetCallArgumentRecord *argument = &arguments[graph->target_argument];
        const XrSemanticPlan *entry_owner =
            xr_target_plan_semantic_module(target, graph->entry_partition);
        const XrSemanticPlan *producer_owner =
            xr_target_plan_semantic_module(target, graph->producer_partition);
        const XrSemanticPlan *bound_semantic = NULL;
        uint32_t bound_function = XR_SEMANTIC_INDEX_NONE;
        uint32_t found_function = XR_SEMANTIC_INDEX_NONE;
        exact = entry_owner == entry && producer_owner == producer &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT &&
                call->target_kind == XR_TARGET_CALL_TARGET_PROGRAM_DIRECT &&
                call->callee_function == graph->producer_target_function &&
                argument->caller_slot == graph->caller_slot &&
                argument->callee_slot == graph->callee_slot &&
                xr_target_plan_function_semantic_binding(
                    target, graph->producer_target_function, &bound_semantic, &bound_function) &&
                bound_semantic == producer &&
                bound_function == graph->producer_semantic_function &&
                xr_target_plan_find_function(target, producer, bound_function, &found_function) &&
                found_function == graph->producer_target_function &&
                xr_target_plan_value_rep(target, 0u) == NULL;
        uint32_t direct = 0, dynamic = 0;
        for (uint32_t i = 0; i < instruction_count; i++) {
            direct += instructions[i].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64;
            dynamic += instructions[i].opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64;
        }
        exact = exact && direct == 1u && dynamic == 0u;
    }
    exact = exact && scalar_graph_typed_vm_is_exact(target) &&
            scalar_graph_xtp_roundtrip(target, modules, profile) &&
            scalar_graph_typed_vm_rejects_resigned_authority(target);
    if (exact) {
        XrTargetProgramGraphRecord *graph = &target->program_graphs[0];
        XrTargetModulePartitionRecord *entry_partition =
            &target->module_partitions[graph->entry_partition];
        XrTargetCallRecord *call = &target->calls[graph->target_call];
        XrTargetCallArgumentRecord *argument = &target->call_arguments[graph->target_argument];
        XrTargetInstructionRecord *direct = NULL;
        for (uint32_t i = 0; i < target->instructions_count; i++)
            if (target->instructions[i].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64)
                direct = &target->instructions[i];
        exact = direct &&
                scalar_graph_target_mutation_rejected(target, &target->semantic_fingerprint) &&
                scalar_graph_target_mutation_rejected(target,
                                                      &entry_partition->semantic_fingerprint) &&
                scalar_graph_target_mutation_rejected(target,
                                                      &entry_partition->module_identity) &&
                scalar_graph_target_mutation_rejected(target,
                                                      &entry_partition->semantic_module) &&
                scalar_graph_target_mutation_rejected(target,
                                                      &entry_partition->functions_begin) &&
                scalar_graph_target_mutation_rejected(target, &graph->program_fingerprint) &&
                scalar_graph_target_mutation_rejected(target, &graph->generation_identity) &&
                scalar_graph_target_mutation_rejected(
                    target, &graph->target_profile_fingerprint) &&
                scalar_graph_target_mutation_rejected(target,
                                                      &graph->entry_function_identity) &&
                scalar_graph_target_mutation_rejected(target,
                                                      &graph->producer_function_identity) &&
                scalar_graph_target_mutation_rejected(target, &graph->export_identity) &&
                scalar_graph_target_mutation_rejected(target, &graph->entry_identity) &&
                scalar_graph_target_mutation_rejected(target, &graph->call_identity) &&
                scalar_graph_target_mutation_rejected(target, &graph->callsite_identity) &&
                scalar_graph_target_mutation_rejected(target, &graph->resolver_binding) &&
                scalar_graph_target_mutation_rejected(target, &graph->argument_identity) &&
                scalar_graph_target_mutation_rejected(target, &graph->parameter_identity) &&
                scalar_graph_target_mutation_rejected(target, &call->calling_convention) &&
                scalar_graph_target_mutation_rejected(target, &call->callee_function) &&
                scalar_graph_target_mutation_rejected(target, &argument->callee_slot) &&
                scalar_graph_target_mutation_rejected(target, &direct->opcode) &&
                xr_target_plan_verify(target, error, sizeof(error)) &&
                xr_target_plan_fingerprint_is_intact(target);
    }
    if (exact) {
        XrTargetProgramGraphRecord *graph = &target->program_graphs[0];
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[graph->entry_partition];
        uint32_t saved_module = partition->semantic_module;
        uint32_t mutated_module = graph->producer_partition;
        partition->semantic_module = mutated_module;
        bool rejected = mutated_module != saved_module &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        partition->semantic_module = saved_module;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        uint32_t original_count = target->machine_reps_count;
        XrTargetMachineRepRecord *original_rows = target->machine_reps;
        XrTargetMachineRepRecord *expanded =
            xr_malloc((size_t) (original_count + 1u) * sizeof(*expanded));
        bool shape_exact = expanded && original_count == 4u &&
                           original_rows[1].kind == XR_MACHINE_REP_I64;
        if (shape_exact) {
            memcpy(expanded, original_rows,
                   (size_t) original_count * sizeof(*expanded));
            expanded[original_count] = original_rows[1];
            expanded[original_count].id = original_count;
            expanded[1].legal_conversion_mask[0] |=
                UINT64_C(1) << original_count;
            memset(expanded[original_count].legal_conversion_mask, 0,
                   sizeof(expanded[original_count].legal_conversion_mask));
            expanded[original_count].legal_conversion_mask[0] = UINT64_C(1) << 1u;
            target->machine_reps = expanded;
            target->machine_reps_count = original_count + 1u;
        }
        bool rejected = shape_exact &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        if (shape_exact) {
            target->machine_reps = original_rows;
            target->machine_reps_count = original_count;
        }
        xr_free(expanded);
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetExtentRecord *extent = &target->extents[partition->extents_begin];
        uint16_t saved_alignment = extent->alignment;
        extent->alignment = 1u;
        bool rejected = saved_alignment != extent->alignment &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        extent->alignment = saved_alignment;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetLayoutRecord *layout = &target->layouts[partition->layouts_begin];
        uint8_t saved_kind = layout->kind;
        layout->kind = saved_kind == XR_TARGET_LAYOUT_SCALAR
                           ? XR_TARGET_LAYOUT_DYNAMIC
                           : XR_TARGET_LAYOUT_SCALAR;
        bool rejected = saved_kind != layout->kind &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        layout->kind = saved_kind;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetValueRepRecord *value = &target->value_reps[partition->value_reps_begin];
        uint16_t saved_rep = value->register_rep;
        value->register_rep = (uint16_t) ((saved_rep + 1u) % target->machine_reps_count);
        bool rejected = target->machine_reps_count > 1u &&
                        saved_rep != value->register_rep &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        value->register_rep = saved_rep;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetFunctionRecord *function = &target->functions[partition->functions_begin];
        uint32_t saved_function = function->semantic_function;
        function->semantic_function = (saved_function + 1u) % partition->functions_count;
        bool rejected = partition->functions_count > 1u &&
                        saved_function != function->semantic_function &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        function->semantic_function = saved_function;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetSlotRecord *slot = &target->slots[partition->slots_begin];
        uint32_t saved_logical = slot->logical_slot;
        slot->logical_slot = 0u;
        bool rejected = saved_logical != slot->logical_slot &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        slot->logical_slot = saved_logical;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetInstructionRecord *constant = NULL;
        for (uint32_t row = 0; row < target->instructions_count; row++)
            if (target->instructions[row].opcode == XR_TARGET_INSTRUCTION_CONST_I64) {
                constant = &target->instructions[row];
                break;
            }
        uint64_t saved_immediate = constant ? constant->immediate_bits : 0u;
        if (constant)
            constant->immediate_bits++;
        bool rejected = constant && saved_immediate != constant->immediate_bits &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        if (constant)
            constant->immediate_bits = saved_immediate;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetDebugFactRecord *fact = &target->debug_facts[0];
        uint32_t saved_line = fact->source_start_line;
        fact->source_start_line++;
        bool rejected = saved_line != fact->source_start_line &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        fact->source_start_line = saved_line;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetCallRecord *call = &target->calls[target->program_graphs[0].target_call];
        uint32_t saved_operation = call->semantic_operation;
        uint32_t semantic_count = (uint32_t) xr_semantic_plan_operation_count(entry);
        call->semantic_operation = (saved_operation + 1u) % semantic_count;
        bool rejected = semantic_count > 1u &&
                        saved_operation != call->semantic_operation &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        call->semantic_operation = saved_operation;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetCallRecord *call = &target->calls[target->program_graphs[0].target_call];
        uint8_t saved_mode = call->result_mode;
        call->result_mode = XR_TARGET_CALL_CALLER_STORAGE;
        bool rejected = saved_mode != call->result_mode &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        call->result_mode = saved_mode;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetCallArgumentRecord *argument =
            &target->call_arguments[target->program_graphs[0].target_argument];
        uint8_t saved_ownership = argument->ownership;
        argument->ownership = saved_ownership == XR_TARGET_CALL_READ
                                  ? XR_TARGET_CALL_CONSUME
                                  : XR_TARGET_CALL_READ;
        bool rejected = saved_ownership != argument->ownership &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        argument->ownership = saved_ownership;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetCallArgumentRecord *argument =
            &target->call_arguments[target->program_graphs[0].target_argument];
        XrTargetCallRecord *call =
            &target->calls[target->program_graphs[0].target_call];
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(entry, call->semantic_operation);
        uint32_t saved_operand = argument->semantic_operand;
        uint32_t mutated_operand = operation ? operation->operand_begin
                                             : XR_SEMANTIC_INDEX_NONE;
        argument->semantic_operand = mutated_operand;
        bool rejected = operation && operation->operand_count == 2u &&
                        saved_operand != mutated_operand &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        argument->semantic_operand = saved_operand;
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetValueRepRecord *saved_rows =
            xr_malloc((size_t) partition->value_reps_count * sizeof(*saved_rows));
        uint32_t removed = UINT32_MAX;
        for (uint32_t row = 0; row < partition->value_reps_count; row++)
            if (target->value_reps[partition->value_reps_begin + row].slot ==
                XR_SEMANTIC_INDEX_NONE) {
                removed = row;
                break;
            }
        bool shape_exact = saved_rows && removed != UINT32_MAX &&
                           partition->value_reps_begin +
                                   partition->value_reps_count ==
                               target->value_reps_count;
        if (shape_exact) {
            memcpy(saved_rows, &target->value_reps[partition->value_reps_begin],
                   (size_t) partition->value_reps_count * sizeof(*saved_rows));
            memmove(&target->value_reps[partition->value_reps_begin + removed],
                    &target->value_reps[partition->value_reps_begin + removed + 1u],
                    (size_t) (partition->value_reps_count - removed - 1u) *
                        sizeof(*saved_rows));
            partition->value_reps_count--;
            target->value_reps_count--;
        }
        bool rejected = shape_exact &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        if (shape_exact) {
            target->value_reps_count++;
            partition->value_reps_count++;
            memcpy(&target->value_reps[partition->value_reps_begin], saved_rows,
                   (size_t) partition->value_reps_count * sizeof(*saved_rows));
        }
        xr_free(saved_rows);
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        XrTargetLayoutRecord *removed_layout =
            partition->layouts_count
                ? &target->layouts[partition->layouts_begin +
                                   partition->layouts_count - 1u]
                : NULL;
        XrTargetDebugFactRecord *saved_debug =
            xr_malloc((size_t) partition->debug_facts_count * sizeof(*saved_debug));
        bool shape_exact =
            removed_layout && saved_debug && partition->extents_count &&
            partition->layouts_begin + partition->layouts_count ==
                target->layouts_count &&
            partition->extents_begin + partition->extents_count ==
                target->extents_count &&
            removed_layout->extent == target->extents_count - 1u;
        if (shape_exact) {
            memcpy(saved_debug, &target->debug_facts[partition->debug_facts_begin],
                   (size_t) partition->debug_facts_count * sizeof(*saved_debug));
            for (uint32_t row = 0; row < partition->debug_facts_count; row++) {
                XrTargetDebugFactRecord *fact =
                    &target->debug_facts[partition->debug_facts_begin + row];
                const XrSemanticOperationRecord *operation =
                    xr_semantic_plan_operation(entry, fact->semantic_operation);
                if (operation && operation->result_type == removed_layout->semantic_type)
                    memset(&fact->layout_fingerprint, 0,
                           sizeof(fact->layout_fingerprint));
            }
            partition->layouts_count--;
            partition->extents_count--;
            target->layouts_count--;
            target->extents_count--;
        }
        bool rejected = shape_exact &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        if (shape_exact) {
            target->layouts_count++;
            target->extents_count++;
            partition->layouts_count++;
            partition->extents_count++;
            memcpy(&target->debug_facts[partition->debug_facts_begin], saved_debug,
                   (size_t) partition->debug_facts_count * sizeof(*saved_debug));
        }
        xr_free(saved_debug);
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact) {
        XrTargetModulePartitionRecord *partition =
            &target->module_partitions[target->program_graphs[0].entry_partition];
        bool shape_exact = partition->instructions_count &&
                           partition->debug_facts_count ==
                               partition->instructions_count &&
                           partition->instructions_begin +
                                   partition->instructions_count ==
                               target->instructions_count &&
                           partition->debug_facts_begin +
                                   partition->debug_facts_count ==
                               target->debug_facts_count;
        if (shape_exact) {
            partition->instructions_count--;
            partition->debug_facts_count--;
            target->instructions_count--;
            target->debug_facts_count--;
        }
        bool rejected = shape_exact &&
                        scalar_graph_target_rehashed_mutation_rejected(target);
        if (shape_exact) {
            target->instructions_count++;
            target->debug_facts_count++;
            partition->instructions_count++;
            partition->debug_facts_count++;
        }
        scalar_graph_target_refresh_fingerprints(target);
        exact = rejected;
    }
    if (exact)
        exact = xr_target_plan_verify(target, error, sizeof(error)) &&
                xr_target_plan_fingerprint_is_intact(target);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    return exact;
}

static void expect_strict_call_locator_rejection(bool match_caller_end, const char *namespace_id) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, namespace_id));

    FindCallContext found = {0};
    ASSERT_TRUE(find_call(fixture.spec->ast, &found));
    ASSERT_NOT_NULL(found.call);
    ASSERT_EQ_UINT(fixture.spec->ast->type, AST_PROGRAM);
    AstNode *caller = fixture.spec->ast->as.program.statements[1];
    ASSERT_NOT_NULL(caller);
    ASSERT_EQ_UINT(caller->type, AST_FUNCTION_DECL);

    found.call->line = caller->line;
    found.call->column = caller->column;
    if (match_caller_end) {
        found.call->end_line = caller->end_line;
        found.call->end_column = caller->end_column;
    }

    ASSERT_TRUE(fixture_publish(&fixture));
    const XaScalarProgramAuthority *authority = xa_typed_program_scalar_authority(fixture.typed);
    ASSERT_NOT_NULL(authority);
    char error[256] = {0};
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));

    XrProgramSemanticClosure *closure = NULL;
    ASSERT_FALSE(
        xa_typed_program_build_scalar_closure(fixture.typed, &closure, error, sizeof(error)));
    ASSERT_NULL(closure);
    fixture_cleanup(&fixture);
}

TEST(source_backed_scalar_snapshot_builds_verified_closure) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-positive"));
    ASSERT_TRUE(fixture_publish(&fixture));
    const XaScalarProgramAuthority *authority = xa_typed_program_scalar_authority(fixture.typed);
    ASSERT_NOT_NULL(authority);
    char error[256] = {0};
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    ASSERT_TRUE(fingerprint_equal(xa_scalar_program_authority_module(authority)->source_fingerprint,
                                  fixture.spec->source_content_fingerprint));

    XrProgramSemanticClosure *closure = NULL;
    ASSERT_TRUE(
        xa_typed_program_build_scalar_closure(fixture.typed, &closure, error, sizeof(error)));
    ASSERT_NOT_NULL(closure);
    ASSERT_TRUE(xr_program_semantic_closure_is_verified(closure));
    ASSERT_EQ_UINT(xr_program_semantic_closure_module_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_dependency_count(closure), 0);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_count(closure), 0);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_call_count(closure), 1);
    XrScalarI64FunctionContract nullary;
    XrScalarI64FunctionContract unary;
    XrFingerprint direct_call;
    ASSERT_TRUE(xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_NULLARY, &nullary));
    ASSERT_TRUE(xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY, &unary));
    ASSERT_TRUE(xr_scalar_i64_call_contract(&unary, &direct_call));
    uint32_t roots = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        bool entry = (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0;
        const XrScalarI64FunctionContract *expected = entry ? &nullary : &unary;
        roots += entry;
        const XaScalarFunctionAuthority *source_function = NULL;
        for (uint32_t j = 0; j < XA_SCALAR_PROGRAM_FUNCTION_COUNT; j++) {
            const XaScalarFunctionAuthority *candidate =
                xa_scalar_program_authority_function(authority, j);
            if (candidate &&
                memcmp(candidate->declaration_identity.bytes, row->declaration_identity.bytes,
                       sizeof(row->declaration_identity.bytes)) == 0)
                source_function = candidate;
        }
        ASSERT_NOT_NULL(source_function);
        ASSERT_EQ_UINT(row->declaration_locator.kind, source_function->declaration_span.kind);
        ASSERT_EQ_UINT(row->declaration_locator.start_line,
                       source_function->declaration_span.start_line);
        ASSERT_EQ_UINT(row->declaration_locator.start_column,
                       source_function->declaration_span.start_column);
        ASSERT_EQ_UINT(row->declaration_locator.end_line,
                       source_function->declaration_span.end_line);
        ASSERT_EQ_UINT(row->declaration_locator.end_column,
                       source_function->declaration_span.end_column);
        ASSERT_EQ_UINT(row->capability_mask, 0);
        ASSERT_TRUE(fingerprint_equal(row->signature_fingerprint, expected->signature_fingerprint));
        ASSERT_TRUE(fingerprint_equal(row->effect_fingerprint, expected->effect_fingerprint));
    }
    ASSERT_EQ_UINT(roots, 1);
    ASSERT_TRUE(fingerprint_equal(
        xr_program_semantic_closure_call(closure, 0)->contract_fingerprint, direct_call));
    const XaScalarCallAuthority *source_call = xa_scalar_program_authority_call(authority);
    const XrProgramSemanticCallRecord *projected_call =
        xr_program_semantic_closure_call(closure, 0);
    ASSERT_NOT_NULL(source_call);
    ASSERT_EQ_UINT(projected_call->locator.kind, source_call->callsite_span.kind);
    ASSERT_EQ_UINT(projected_call->locator.start_line, source_call->callsite_span.start_line);
    ASSERT_EQ_UINT(projected_call->locator.start_column, source_call->callsite_span.start_column);
    ASSERT_EQ_UINT(projected_call->locator.end_line, source_call->callsite_span.end_line);
    ASSERT_EQ_UINT(projected_call->locator.end_column, source_call->callsite_span.end_column);
    ASSERT_TRUE(projected_call->locator.end_line > projected_call->locator.start_line ||
                (projected_call->locator.end_line == projected_call->locator.start_line &&
                 projected_call->locator.end_column > projected_call->locator.start_column));
    xr_program_semantic_closure_free(closure);
    fixture_cleanup(&fixture);
}

TEST(source_backed_leaf_aggregate_publishes_typed_psc) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kLeafAggregateSource, "psc-leaf-aggregate"));
    ASSERT_TRUE(fixture_publish(&fixture));
    ASSERT_NULL(xa_typed_program_scalar_authority(fixture.typed));
    const XrProgramSemanticClosure *closure =
        xa_typed_program_program_semantic_closure(fixture.typed);
    ASSERT_NOT_NULL(closure);
    char error[256] = {0};
    ASSERT_TRUE(xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    ASSERT_EQ_UINT(xr_program_semantic_closure_family(closure),
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_field_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_parameter_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_call_count(closure), 1);
    uint32_t scalar_rows = 0;
    uint32_t aggregate_rows = 0;
    XrStableId aggregate_id = {{0}};
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        ASSERT_NOT_NULL(row);
        if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
            scalar_rows++;
            ASSERT_EQ_UINT(row->exact_scalar, XR_EXACT_SCALAR_I64);
            ASSERT_EQ_UINT(row->field_count, 0);
        } else {
            aggregate_rows++;
            aggregate_id = row->id;
            ASSERT_EQ_UINT(row->kind, XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE);
            ASSERT_EQ_UINT(row->field_count, 2);
        }
    }
    ASSERT_EQ_UINT(scalar_rows, 1);
    ASSERT_EQ_UINT(aggregate_rows, 1);
    uint32_t entries = 0;
    uint32_t callees = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        ASSERT_NOT_NULL(row);
        ASSERT_TRUE(
            memcmp(row->return_type.bytes, aggregate_id.bytes, sizeof(aggregate_id.bytes)) == 0);
        if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            entries++;
            ASSERT_EQ_UINT(row->parameter_count, 0);
        } else {
            callees++;
            ASSERT_EQ_UINT(row->flags, 0);
            ASSERT_EQ_UINT(row->parameter_count, 1);
            const XrProgramSemanticFunctionParameterRecord *parameter =
                xr_program_semantic_closure_function_parameter(closure, row->parameter_begin);
            ASSERT_NOT_NULL(parameter);
            ASSERT_EQ_UINT(parameter->mode, XR_PARAM_READ);
            ASSERT_TRUE(
                memcmp(parameter->type.bytes, aggregate_id.bytes, sizeof(aggregate_id.bytes)) == 0);
        }
    }
    ASSERT_EQ_UINT(entries, 1);
    ASSERT_EQ_UINT(callees, 1);
    fixture_cleanup(&fixture);
}

TEST(two_source_module_scalar_graph_publishes_complete_authority) {
    ScalarGraphFixture fixture;
    ASSERT_TRUE(scalar_graph_fixture_build(&fixture, "41"));
    char error[256] = {0};
    XrModuleSpec *entry_spec = &fixture.graph->specs[fixture.graph->entry_index];
    XrModuleSpec *producer_spec = &fixture.graph->specs[entry_spec->dep_indices[0]];
    AstNode *import_node = entry_spec->ast->as.program.statements[0];
    AstNode *export_node = producer_spec->ast->as.program.statements[0];
    const char *export_name = import_node->as.import_stmt.members[0].name;
    XaSymbol *original_export =
        (XaSymbol *) xr_hashmap_get(producer_spec->export_symbols, export_name);
    ASSERT_NOT_NULL(original_export);
    XaSymbol cloned_export = *original_export;
    cloned_export.id += UINT32_C(1000000);
    ASSERT_TRUE(xr_hashmap_set(producer_spec->export_symbols, export_name, &cloned_export));
    XrProgramSemanticClosure *cloned_symbol_closure = NULL;
    ASSERT_EQ_INT(
        xa_program_semantic_closure_publish_scalar_module_graph(
            fixture.analyzer, fixture.graph, &cloned_symbol_closure, error, sizeof(error)),
        XA_PROGRAM_SEMANTIC_CLOSURE_READY);
    xr_program_semantic_closure_free(cloned_symbol_closure);
    AstNode *entry_node = entry_spec->ast->as.program.statements[1];
    XaSymbol *entry_symbol =
        xa_analyzer_symbol_by_id(fixture.analyzer, entry_node->as.function_decl.symbol_id);
    ASSERT_NOT_NULL(entry_symbol);
    XrType *export_type = cloned_export.links.type;
    cloned_export.links.type = entry_symbol->links.type;
    XrProgramSemanticClosure *wrong_export_type_closure = NULL;
    ASSERT_EQ_INT(
        xa_program_semantic_closure_publish_scalar_module_graph(
            fixture.analyzer, fixture.graph, &wrong_export_type_closure, error, sizeof(error)),
        XA_PROGRAM_SEMANTIC_CLOSURE_INVALID);
    ASSERT_NULL(wrong_export_type_closure);
    cloned_export.links.type = export_type;
    AstNode foreign_declaration = *export_node;
    foreign_declaration.column++;
    cloned_export.links.function_decl_node = &foreign_declaration;
    XrProgramSemanticClosure *foreign_symbol_closure = NULL;
    ASSERT_EQ_INT(
        xa_program_semantic_closure_publish_scalar_module_graph(
            fixture.analyzer, fixture.graph, &foreign_symbol_closure, error, sizeof(error)),
        XA_PROGRAM_SEMANTIC_CLOSURE_INVALID);
    ASSERT_NULL(foreign_symbol_closure);
    ASSERT_TRUE(xr_hashmap_set(producer_spec->export_symbols, export_name, original_export));

    XrProgramSemanticClosure *closure = NULL;
    XaProgramSemanticClosurePublishStatus status =
        xa_program_semantic_closure_publish_scalar_module_graph(
            fixture.analyzer, fixture.graph, &closure, error, sizeof(error));
    if (status != XA_PROGRAM_SEMANTIC_CLOSURE_READY) {
        fprintf(stderr, "  scalar graph publication failed: %s\n", error);
    }
    ASSERT_EQ_INT(status, XA_PROGRAM_SEMANTIC_CLOSURE_READY);
    ASSERT_NOT_NULL(closure);
    ASSERT_TRUE(xr_program_semantic_closure_is_verified(closure));
    ASSERT_EQ_UINT(xr_program_semantic_closure_schema(closure),
                   XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    ASSERT_EQ_UINT(xr_program_semantic_closure_family(closure),
                   XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL);
    ASSERT_EQ_UINT(xr_program_semantic_closure_module_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_dependency_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_type_field_count(closure), 0);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_count(closure), 2);
    ASSERT_EQ_UINT(xr_program_semantic_closure_function_parameter_count(closure), 1);
    ASSERT_EQ_UINT(xr_program_semantic_closure_call_count(closure), 1);
    const XrProgramSemanticTypeRecord *i64_type = xr_program_semantic_closure_type(closure, 0);
    ASSERT_NOT_NULL(i64_type);
    ASSERT_EQ_UINT(i64_type->kind, XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR);
    ASSERT_EQ_UINT(i64_type->exact_scalar, XR_EXACT_SCALAR_I64);
    uint32_t entry_count = 0;
    uint32_t exported_count = 0;
    const XrProgramSemanticFunctionRecord *entry_function = NULL;
    const XrProgramSemanticFunctionRecord *exported_function = NULL;
    for (uint32_t i = 0; i < 2; i++) {
        const XrProgramSemanticFunctionRecord *function =
            xr_program_semantic_closure_function(closure, i);
        ASSERT_NOT_NULL(function);
        ASSERT_TRUE(memcmp(function->return_type.bytes, i64_type->id.bytes,
                           sizeof(i64_type->id.bytes)) == 0);
        ASSERT_EQ_UINT(function->capability_mask, 0);
        entry_count += (function->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0;
        exported_count += (function->flags & XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED) != 0;
        if (function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            entry_function = function;
            ASSERT_EQ_UINT(function->parameter_count, 0);
        } else {
            exported_function = function;
            ASSERT_EQ_UINT(function->flags, XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED);
            ASSERT_EQ_UINT(function->parameter_count, 1);
            const XrProgramSemanticFunctionParameterRecord *parameter =
                xr_program_semantic_closure_function_parameter(closure, function->parameter_begin);
            ASSERT_NOT_NULL(parameter);
            ASSERT_EQ_UINT(parameter->mode, XR_PARAM_READ);
            ASSERT_TRUE(
                memcmp(parameter->type.bytes, i64_type->id.bytes, sizeof(i64_type->id.bytes)) == 0);
        }
    }
    ASSERT_EQ_UINT(entry_count, 1);
    ASSERT_EQ_UINT(exported_count, 1);
    ASSERT_NOT_NULL(entry_function);
    ASSERT_NOT_NULL(exported_function);
    const XrProgramSemanticDependencyRecord *dependency =
        xr_program_semantic_closure_dependency(closure, 0);
    const XrProgramSemanticCallRecord *call =
        xr_program_semantic_closure_call(closure, 0);
    ASSERT_NOT_NULL(dependency);
    ASSERT_NOT_NULL(call);
    ASSERT_EQ_UINT(dependency->kind, XR_PROGRAM_SEMANTIC_DEPENDENCY_SELECTIVE_FUNCTION_IMPORT);
    ASSERT_EQ_UINT(dependency->import_locator.kind, AST_IMPORT_STMT);
    ASSERT_TRUE(memcmp(dependency->exported_declaration.bytes,
                       exported_function->declaration_identity.bytes,
                       sizeof(dependency->exported_declaration.bytes)) == 0);
    ASSERT_TRUE(memcmp(dependency->exported_function.bytes, exported_function->id.bytes,
                       sizeof(dependency->exported_function.bytes)) == 0);
    ASSERT_TRUE(memcmp(dependency->resolver_binding.bytes, call->resolver_binding.bytes,
                       sizeof(dependency->resolver_binding.bytes)) == 0);
    ASSERT_TRUE(memcmp(call->caller_function.bytes, entry_function->id.bytes,
                       sizeof(call->caller_function.bytes)) == 0);
    ASSERT_TRUE(memcmp(call->callee_function.bytes, exported_function->id.bytes,
                       sizeof(call->callee_function.bytes)) == 0);
    ASSERT_FALSE(memcmp(dependency->source_module.bytes,
                        dependency->dependency_module.bytes,
                        sizeof(dependency->source_module.bytes)) == 0);
    ASSERT_FALSE(memcmp(call->caller_function.bytes, call->callee_function.bytes,
                        sizeof(call->caller_function.bytes)) == 0);
    ASSERT_TRUE(
        scalar_graph_outer_resign_rejects(closure, SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_ZERO));
    ASSERT_TRUE(scalar_graph_outer_resign_rejects(closure,
                                                  SCALAR_GRAPH_OUTER_RESIGN_CALL_BINDING_MISMATCH));
    ASSERT_TRUE(
        scalar_graph_outer_resign_rejects(closure, SCALAR_GRAPH_OUTER_RESIGN_RESOLVER_MUTATION));
    ASSERT_TRUE(
        scalar_graph_outer_resign_rejects(closure, SCALAR_GRAPH_OUTER_RESIGN_FOREIGN_EXPORT));
    ASSERT_TRUE(
        scalar_graph_outer_resign_rejects(closure, SCALAR_GRAPH_OUTER_RESIGN_WRONG_IMPORT_LOCATOR));

    static const uint8_t expected_resolver_binding[16] = {
        0x5e, 0x67, 0x57, 0x46, 0x54, 0xf5, 0x51, 0x28,
        0x14, 0x26, 0xbf, 0x7c, 0x2a, 0x9a, 0x16, 0x83,
    };
    static const uint8_t expected_fingerprint[32] = {
        0xc1, 0xb2, 0x50, 0x3c, 0xcf, 0xf1, 0x19, 0x54, 0x45, 0x47, 0x9a,
        0xf2, 0x37, 0x46, 0x57, 0x10, 0xe3, 0x4d, 0x31, 0x31, 0x90, 0x81,
        0x6e, 0x21, 0x98, 0x04, 0xde, 0xd5, 0xfc, 0x32, 0x14, 0x99,
    };
    static const uint8_t expected_generation_id[16] = {
        0xea, 0x39, 0x98, 0x2c, 0x91, 0x2f, 0xdd, 0xd5,
        0x3e, 0x01, 0x00, 0xd5, 0x3c, 0x25, 0xa9, 0xcc,
    };
    XrFingerprint fingerprint = xr_program_semantic_closure_fingerprint(closure);
    XrGenerationClosureId generation_id =
        xr_program_semantic_closure_generation_id(closure);
    ASSERT_TRUE(memcmp(dependency->resolver_binding.bytes, expected_resolver_binding,
                       sizeof(expected_resolver_binding)) == 0);
    ASSERT_TRUE(memcmp(fingerprint.bytes, expected_fingerprint, sizeof(expected_fingerprint)) == 0);
    ASSERT_TRUE(
        memcmp(generation_id.bytes, expected_generation_id, sizeof(expected_generation_id)) == 0);

    ScalarGraphPlanFixture plan_fixture;
    ASSERT_MSG(scalar_graph_plan_fixture_build(&plan_fixture, &fixture, closure, error,
                                               sizeof(error)),
               error);
    ASSERT_TRUE(scalar_graph_xsm_roundtrip(&plan_fixture));
    ASSERT_TRUE(scalar_graph_export_carrier_mutations(&plan_fixture));
    ASSERT_TRUE(scalar_graph_program_row_mutations(&plan_fixture));
    ASSERT_TRUE(scalar_graph_external_join_mutations(&plan_fixture));
    ASSERT_TRUE(scalar_graph_module_and_plan_mutations(&plan_fixture));
    ASSERT_TRUE(scalar_graph_target_plan_is_exact(&plan_fixture));
    scalar_graph_plan_fixture_cleanup(&plan_fixture);

    int first = fixture.graph->topo_order[0];
    fixture.graph->topo_order[0] = fixture.graph->topo_order[1];
    fixture.graph->topo_order[1] = first;
    XrProgramSemanticClosure *reordered = NULL;
    ASSERT_EQ_INT(xa_program_semantic_closure_publish_scalar_module_graph(
                      fixture.analyzer, fixture.graph, &reordered, error, sizeof(error)),
                  XA_PROGRAM_SEMANTIC_CLOSURE_READY);
    ASSERT_TRUE(fingerprint_equal(
        fingerprint, xr_program_semantic_closure_fingerprint(reordered)));
    ASSERT_TRUE(generation_id_equal(
        generation_id, xr_program_semantic_closure_generation_id(reordered)));
    fixture.graph->topo_order[1] = fixture.graph->topo_order[0];
    fixture.graph->topo_order[0] = first;
    xr_program_semantic_closure_free(reordered);

    XrModuleSpec *entry = &fixture.graph->specs[fixture.graph->entry_index];
    int dependency_index = entry->dep_indices[0];
    entry->dep_count = 0;
    XrProgramSemanticClosure *rejected = NULL;
    ASSERT_EQ_INT(xa_program_semantic_closure_publish_scalar_module_graph(
                      fixture.analyzer, fixture.graph, &rejected, error, sizeof(error)),
                  XA_PROGRAM_SEMANTIC_CLOSURE_INVALID);
    ASSERT_NULL(rejected);
    entry->dep_count = 1;
    entry->dep_indices[0] = fixture.graph->entry_index;
    ASSERT_EQ_INT(xa_program_semantic_closure_publish_scalar_module_graph(
                      fixture.analyzer, fixture.graph, &rejected, error, sizeof(error)),
                  XA_PROGRAM_SEMANTIC_CLOSURE_INVALID);
    ASSERT_NULL(rejected);
    entry->dep_indices[0] = dependency_index;

    closure->dependencies[0].contract_fingerprint.bytes[0] ^= UINT8_C(0x80);
    ASSERT_FALSE(xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    xr_program_semantic_closure_free(closure);
    scalar_graph_fixture_cleanup(&fixture);
}

TEST(strict_call_locator_boundaries_fail_after_source_republication) {
    expect_strict_call_locator_rejection(false, "psc-scalar-call-shares-caller-start");
    expect_strict_call_locator_rejection(true, "psc-scalar-call-equals-caller-span");
}

TEST(published_snapshot_is_pointer_free_and_ignores_node_ids) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-owned"));
    ASSERT_TRUE(fixture_publish(&fixture));
    char error[256] = {0};
    XrProgramSemanticClosure *first = NULL;
    ASSERT_TRUE(xa_typed_program_build_scalar_closure(fixture.typed, &first, error, sizeof(error)));
    XrFingerprint first_fingerprint = xr_program_semantic_closure_fingerprint(first);

    FindCallContext found = {0};
    ASSERT_TRUE(find_call(fixture.spec->ast, &found));
    ASSERT_NOT_NULL(found.call);
    uint32_t original_node_id = found.call->node_id;
    int original_line = found.call->line;
    found.call->node_id += 1000;
    found.call->line += 100;
    const XaResolvedCall *resolved = xa_analyzer_get_resolved_call(fixture.analyzer, found.call);
    ASSERT_NULL(resolved);

    XrProgramSemanticClosure *second = NULL;
    ASSERT_TRUE(
        xa_typed_program_build_scalar_closure(fixture.typed, &second, error, sizeof(error)));
    ASSERT_TRUE(
        fingerprint_equal(first_fingerprint, xr_program_semantic_closure_fingerprint(second)));
    found.call->node_id = original_node_id;
    found.call->line = original_line;

    xr_program_semantic_closure_free(second);
    xr_program_semantic_closure_free(first);
    fixture_cleanup(&fixture);
}

TEST(snapshot_projects_after_analyzer_and_graph_teardown) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-lifetime"));
    ASSERT_TRUE(fixture_publish(&fixture));
    XaTypedProgram *typed = fixture.typed;
    fixture.typed = NULL;
    xa_analyzer_free(fixture.analyzer);
    fixture.analyzer = NULL;
    xr_module_graph_free(fixture.graph);
    fixture.graph = NULL;
    fixture.spec = NULL;

    char error[256] = {0};
    XrProgramSemanticClosure *closure = NULL;
    ASSERT_TRUE(xa_typed_program_build_scalar_closure(typed, &closure, error, sizeof(error)));
    ASSERT_TRUE(xr_program_semantic_closure_is_verified(closure));
    xr_program_semantic_closure_free(closure);
    xa_typed_program_free(typed);
}

TEST(compiler_only_graph_refuses_runtime_preload) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-no-runtime"));
    XrModule **modules = NULL;
    ASSERT_FALSE(xr_module_graph_preload(NULL, fixture.graph, &modules));
    ASSERT_NULL(modules);
    fixture_cleanup(&fixture);
}

TEST(call_authority_mutations_fail_independent_verification) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-mutation"));
    ASSERT_TRUE(fixture_publish(&fixture));
    XaScalarProgramAuthority *authority =
        (XaScalarProgramAuthority *) xa_typed_program_scalar_authority(fixture.typed);
    ASSERT_NOT_NULL(authority);
    char error[256] = {0};

    authority->call.caller_declaration_identity.bytes[0] ^= UINT8_C(0x40);
    ASSERT_FALSE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    authority->call.caller_declaration_identity.bytes[0] ^= UINT8_C(0x40);
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));

    authority->call.callsite_span.start_column++;
    ASSERT_FALSE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    authority->call.callsite_span.start_column--;
    ASSERT_TRUE(xa_scalar_program_authority_verify(authority, error, sizeof(error)));
    fixture_cleanup(&fixture);
}

TEST(incomplete_or_ambiguous_source_authority_fails_closed) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-negative"));
    AstNode *first = fixture.spec->ast->as.program.statements[0];
    AstNode *second = fixture.spec->ast->as.program.statements[1];
    XaScalarSourceSpan first_span = {
        .kind = first->type,
        .start_line = (uint32_t) first->line,
        .start_column = (uint32_t) first->column,
        .end_line = (uint32_t) first->end_line,
        .end_column = (uint32_t) first->end_column,
    };
    second->line = (int) first_span.start_line;
    second->column = (int) first_span.start_column;
    second->end_line = (int) first_span.end_line;
    second->end_column = (int) first_span.end_column;
    XaTypedProgramPublishResult duplicate =
        xa_typed_program_publish(fixture.analyzer, fixture.spec->ast, NULL, 1);
    ASSERT_NULL(duplicate.program);
    ASSERT_EQ_INT(duplicate.reason, XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY);
    XrProgramSemanticClosure *closure = NULL;
    char error[256] = {0};
    ASSERT_NULL(closure);
    fixture_cleanup(&fixture);

    XaAnalyzer *graphless = xa_analyzer_new(g_session);
    AstNode *program = xr_parse(g_session, kScalarSource);
    ASSERT_NOT_NULL(program);
    xa_analyzer_analyze(graphless, "graphless.xr", program);
    XaTypedProgramPublishResult result = xa_typed_program_publish(graphless, program, NULL, 0);
    ASSERT_NULL(result.program);
    ASSERT_EQ_INT(result.reason, XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY);
    xa_analyzer_free(graphless);
    xr_program_destroy(program);

    static const char outside_family[] = "fn echo(value: string) -> string { return value }\n"
                                         "fn root() -> string { return echo(\"x\") }\n";
    ASSERT_TRUE(fixture_analyze(&fixture, outside_family, "psc-nonscalar-family"));
    ASSERT_TRUE(fixture_publish(&fixture));
    ASSERT_NULL(xa_typed_program_scalar_authority(fixture.typed));
    ASSERT_FALSE(
        xa_typed_program_build_scalar_closure(fixture.typed, &closure, error, sizeof(error)));
    fixture_cleanup(&fixture);
}

TEST(resolved_target_mismatch_does_not_publish_scalar_authority) {
    ScalarFixture fixture;
    ASSERT_TRUE(fixture_analyze(&fixture, kScalarSource, "psc-scalar-target"));
    FindCallContext found = {0};
    ASSERT_TRUE(find_call(fixture.spec->ast, &found));
    XaResolvedCall *resolved =
        (XaResolvedCall *) xa_analyzer_get_resolved_call(fixture.analyzer, found.call);
    ASSERT_NOT_NULL(resolved);
    resolved->target_symbol_id++;
    XaTypedProgramPublishResult result =
        xa_typed_program_publish(fixture.analyzer, fixture.spec->ast, NULL, 1);
    ASSERT_NULL(result.program);
    ASSERT_EQ_INT(result.reason, XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY);
    fixture_cleanup(&fixture);
}

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("source-backed program semantic closure");
RUN_TEST(source_backed_scalar_snapshot_builds_verified_closure);
RUN_TEST(source_backed_leaf_aggregate_publishes_typed_psc);
RUN_TEST(two_source_module_scalar_graph_publishes_complete_authority);
RUN_TEST(strict_call_locator_boundaries_fail_after_source_republication);
RUN_TEST(published_snapshot_is_pointer_free_and_ignores_node_ids);
RUN_TEST(snapshot_projects_after_analyzer_and_graph_teardown);
RUN_TEST(compiler_only_graph_refuses_runtime_preload);
RUN_TEST(call_authority_mutations_fail_independent_verification);
RUN_TEST(incomplete_or_ambiguous_source_authority_fails_closed);
RUN_TEST(resolved_target_mismatch_does_not_publish_scalar_authority);
teardown();
TEST_MAIN_END()
