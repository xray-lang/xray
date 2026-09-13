/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xr_program_imported_callable.c - Source-backed imported callable KAT
 */

#include "../test_framework.h"

#include "analysis/xglobal_producer.h"
#include "aot/program/xr_backend_ir.h"
#include "base/xmalloc.h"
#include "execution/xr_execution.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_mono.h"
#include "frontend/canonical/xcanon.h"
#include "frontend/parser/xast.h"
#include "frontend/parser/xast_nodes.h"
#include "ir/xi_import_resolve.h"
#include "ir/xi_pipeline.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "plan/target/xr_target_profile.h"
#include "program/xr_program_from_xi.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"
#include "program/xr_validated_program_internal.h"
#include "runtime/abi/xr_runtime_target_profile.h"
#include "runtime/value/xchunk.h"
#include "runtime/xisolate_api.h"
#include "toolchain/xcompiler_session.h"
#include "vm/xr_program_vm.h"
#include "xray_vm.h"

#include <stdio.h>
#include <string.h>

typedef struct ImportedCallableProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    size_t count;
} ImportedCallableProviderBindings;

typedef struct ImportedCallableFixture {
    char directory[XR_TEST_PATH_MAX];
    char library_path[XR_TEST_PATH_MAX];
    char consumer_path[XR_TEST_PATH_MAX];
    XrModuleResolver *resolver;
    XrModuleGraph *graph;
    XaAnalyzer *analyzer;
    XiPipelineResult pipelines[2];
    XiModule *modules[2];
    uint32_t library_index;
    uint32_t consumer_index;
    XgGlobalEvidence evidence;
} ImportedCallableFixture;

static const char *g_generated_c_path;
static ImportedCallableFixture g_active_fixture;

static void destroy_imported_callable_fixture(ImportedCallableFixture *fixture);

static bool imported_callable_build_provider_bindings(
    const XrTargetProfile *profile, ImportedCallableProviderBindings *bindings) {
    (void) profile;
    memset(bindings, 0, sizeof(*bindings));
    return true;
}

static bool write_source_file(const char *path, const char *source) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    size_t size = strlen(source);
    bool written = fwrite(source, 1u, size, file) == size;
    return fclose(file) == 0 && written;
}

static bool analyze_all_modules(ImportedCallableFixture *fixture) {
    for (int topo = 0; topo < fixture->graph->topo_count; ++topo) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[topo]];
        xa_analyzer_analyze(fixture->analyzer, spec->source_path, spec->ast);
        int diagnostic_count = 0;
        for (XaDiagnostic *diagnostic =
                 xa_analyzer_get_diagnostics(fixture->analyzer, &diagnostic_count);
             diagnostic; diagnostic = diagnostic->next) {
            if (diagnostic->severity == XR_DIAG_SEV_ERROR) {
                fprintf(stderr, "imported callable analysis failed: %s\n", diagnostic->message);
                return false;
            }
        }
        if (spec->export_symbols)
            xr_hashmap_free(spec->export_symbols);
        spec->export_symbols = NULL;
        if (!xa_analyzer_collect_export_symbols_checked(fixture->analyzer, spec->ast,
                                                        &spec->export_symbols))
            return false;
        spec->status = XR_MODSPEC_ANALYZED;
        xa_analyzer_clear_diagnostics(fixture->analyzer);
    }
    return true;
}

/* Write the two module sources into a fresh directory, build and sort their
 * graph, and analyze every module so its exports are published. Leaves the
 * fixture ready for either the Xi pipeline or the compile driver. */
static bool prepare_imported_callable_graph(ImportedCallableFixture *fixture,
                                            XrCompilerSession *session,
                                            bool fail_after_source_write,
                                            const char *selected_library_source,
                                            const char *selected_consumer_source,
                                            char *created_directory,
                                            size_t created_directory_size) {
    static unsigned int serial;
    if (!fixture)
        return false;
    if (created_directory && created_directory_size != 0u)
        created_directory[0] = '\0';
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->directory, sizeof(fixture->directory),
             "xr_program_imported_callable_%u_XXXXXX", serial++);
    if (!xr_test_mkdtemp(fixture->directory))
        goto fail;
    char absolute_directory[XR_TEST_PATH_MAX];
    if (!xr_test_realpath_buf(fixture->directory, absolute_directory,
                              sizeof(absolute_directory)))
        goto fail;
    memcpy(fixture->directory, absolute_directory, strlen(absolute_directory) + 1u);
    if (created_directory && created_directory_size != 0u)
        snprintf(created_directory, created_directory_size, "%s", fixture->directory);
    int library_path_length =
        snprintf(fixture->library_path, sizeof(fixture->library_path), "%s/library.xr",
                 fixture->directory);
    int consumer_path_length =
        snprintf(fixture->consumer_path, sizeof(fixture->consumer_path), "%s/consumer.xr",
                 fixture->directory);
    if (library_path_length < 0 ||
        (size_t) library_path_length >= sizeof(fixture->library_path) ||
        consumer_path_length < 0 ||
        (size_t) consumer_path_length >= sizeof(fixture->consumer_path))
        goto fail;
    if (!write_source_file(fixture->library_path, selected_library_source) ||
        !write_source_file(fixture->consumer_path, selected_consumer_source))
        goto fail;
    if (fail_after_source_write)
        goto fail;

    XrModuleResolverConfig resolver_config = {0};
    fixture->resolver = xr_module_resolver_new(&resolver_config);
    fixture->graph = xr_module_graph_new(session, fixture->resolver);
    if (!fixture->resolver || !fixture->graph)
        goto fail;
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = fixture->directory,
    };
    char *graph_error = NULL;
    if (xr_module_graph_build(fixture->graph, fixture->consumer_path, &authority, &graph_error) !=
        0) {
        fprintf(stderr, "imported callable graph failed: %s\n",
                graph_error ? graph_error : "unknown graph error");
        xr_free(graph_error);
        goto fail;
    }
    xr_free(graph_error);
    if (xr_module_graph_topological_sort(fixture->graph) != 0 || fixture->graph->has_cycle ||
        fixture->graph->spec_count != 2 || fixture->graph->topo_count != 2 ||
        fixture->graph->entry_index < 0)
        goto fail;

    fixture->analyzer = xa_analyzer_new(session);
    if (!fixture->analyzer)
        goto fail;
    xa_analyzer_set_build_profile(fixture->analyzer, XA_ANALYZER_BUILD_PROFILE_HOSTED);
    xa_analyzer_set_graph(fixture->analyzer, fixture->graph);
    if (!analyze_all_modules(fixture))
        goto fail;
    return true;

fail:
    destroy_imported_callable_fixture(fixture);
    return false;
}

static bool build_imported_callable_fixture(ImportedCallableFixture *fixture,
                                            XrCompilerSession *session, XrVMRuntime *isolate,
                                            bool fail_after_source_write,
                                            const char *library_source_override,
                                            const char *consumer_source_override,
                                            char *created_directory, size_t created_directory_size) {
    static const char library_source[] =
        "fn bump(value: i64) -> i64 { return value + 1 }\n"
        "export fn identity(value: i64) -> i64 { return value }\n"
        "export fn increment(value: i64) -> i64 { return bump(value) }\n";
    static const char consumer_source[] =
        "import { identity, increment } from \"./library\"\n"
        "fn apply(flag: bool, value: i64) -> i64 {\n"
        "  var action = identity\n"
        "  if (flag) { action = increment }\n"
        "  return action(value)\n"
        "}\n"
        "fn root() -> i64 { return apply(true, 41) }\n";
    if (!prepare_imported_callable_graph(
            fixture, session, fail_after_source_write,
            library_source_override ? library_source_override : library_source,
            consumer_source_override ? consumer_source_override : consumer_source,
            created_directory, created_directory_size))
        return false;

    AstNode *roots[2] = {0};
    for (int topo = 0; topo < fixture->graph->topo_count; ++topo)
        roots[topo] = fixture->graph->specs[fixture->graph->topo_order[topo]].ast;
    XaMonoBudget mono_budget = xa_mono_default_budget();
    XaMonoUsage mono_usage = {0};
    if (!xa_mono_graph_pass(roots, fixture->graph->topo_count, isolate, &mono_budget, &mono_usage,
                            fixture->analyzer))
        goto fail;
    for (int topo = 0; topo < fixture->graph->topo_count; ++topo) {
        XrModuleSpec *spec = &fixture->graph->specs[fixture->graph->topo_order[topo]];
        XrCompilerSessionScope scope;
        bool has_scope = spec->ast->type == AST_PROGRAM && spec->ast->as.program.arena &&
                         xr_compiler_session_push_arena(session, spec->ast->as.program.arena,
                                                        spec->source_path, &scope);
        XrCanonStatus canon = xr_canon_program(spec->ast, fixture->analyzer, session);
        if (has_scope)
            xr_compiler_session_pop_arena(&scope);
        if (canon != XR_CANON_OK)
            goto fail;
    }
    if (!analyze_all_modules(fixture) ||
        !xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
            &fixture->evidence, fixture->graph, XG_BUILD_NATIVE_RELEASE, 0u, NULL, 0u,
            fixture->analyzer))
        goto fail;

    XiPipelineConfig config = xi_pipeline_program_input_config();
    config.run_canonicalize = false;
    config.module_graph = fixture->graph;
    config.graph_modules = fixture->modules;
    config.graph_module_count = 2;
    config.global_evidence = &fixture->evidence;
    for (uint32_t topo = 0u; topo < 2u; ++topo) {
        int spec_index = fixture->graph->topo_order[topo];
        XrModuleSpec *spec = &fixture->graph->specs[spec_index];
        config.source_file = spec->source_path;
        config.module_identity = spec->canonical;
        config.module_name = spec_index == fixture->graph->entry_index
                                 ? "imported_callable_consumer"
                                 : "imported_callable_library";
        config.global_evidence_module_id = topo + 1u;
        fixture->pipelines[topo] =
            xi_pipeline_compile_program(spec->ast, fixture->analyzer, isolate, &config);
        if (fixture->pipelines[topo].status != XI_PIPE_OK || !fixture->pipelines[topo].ir ||
            !fixture->pipelines[topo].ir->module) {
            fprintf(stderr, "imported callable Xi failed at %s: %s\n",
                    xi_pipeline_stage_str(fixture->pipelines[topo].error.stage),
                    fixture->pipelines[topo].error.detail);
            goto fail;
        }
        fixture->modules[topo] = fixture->pipelines[topo].ir->module;
        if (spec_index == fixture->graph->entry_index)
            fixture->consumer_index = topo;
    }
    fixture->library_index = fixture->consumer_index == 0u ? 1u : 0u;
    for (uint32_t topo = 0u; topo < 2u; ++topo) {
        int spec_index = fixture->graph->topo_order[topo];
        xi_resolve_imports(fixture->pipelines[topo].ir, fixture->graph,
                           fixture->graph->specs[spec_index].source_path, fixture->modules, 2);
    }
    return true;

fail:
    destroy_imported_callable_fixture(fixture);
    return false;
}

static void destroy_imported_callable_fixture(ImportedCallableFixture *fixture) {
    if (!fixture)
        return;
    for (uint32_t topo = 0u; topo < 2u; ++topo)
        xi_pipeline_result_free(&fixture->pipelines[topo]);
    xg_global_evidence_free(&fixture->evidence);
    xa_analyzer_free(fixture->analyzer);
    xr_module_graph_free(fixture->graph);
    xr_module_resolver_free(fixture->resolver);
    if (fixture->library_path[0])
        xr_test_unlink(fixture->library_path);
    if (fixture->consumer_path[0])
        xr_test_unlink(fixture->consumer_path);
    if (fixture->directory[0])
        xr_test_rmdir(fixture->directory);
    memset(fixture, 0, sizeof(*fixture));
}

static bool imported_callable_path_exists(const char *path) {
    struct stat status;
    return path && path[0] && stat(path, &status) == 0;
}

TEST(imported_callable_fixture_failure_cleans_directory) {
    char created_directory[XR_TEST_PATH_MAX] = {0};
    ASSERT_FALSE(build_imported_callable_fixture(&g_active_fixture, NULL, NULL, true,
                                                 NULL, NULL,
                                                 created_directory,
                                                 sizeof(created_directory)));
    ASSERT_TRUE(created_directory[0] != '\0');
    ASSERT_FALSE(imported_callable_path_exists(created_directory));
}

static XiFunc *find_module_function(const XiModule *module, const char *name) {
    for (uint16_t index = 0u; module && index < module->nfuncs; ++index) {
        XiFunc *function = module->functions[index];
        if (function && function->name && strcmp(function->name, name) == 0)
            return function;
    }
    return NULL;
}

static XiValue *find_indirect_call(const XiFunc *function, const XgGlobalEvidence *evidence,
                                    const XgCallsiteSummary **summary_out) {
    if (summary_out)
        *summary_out = NULL;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (!value || value->op != XI_CALL || value->xg_callsite_id == XG_NO_ID)
                continue;
            const XgCallsiteSummary *summary = xg_global_evidence_find_callsite(
                evidence, (XgCallsiteId) value->xg_callsite_id);
            if (!summary || summary->kind != XG_CALL_CLOSURE)
                continue;
            if (summary_out)
                *summary_out = summary;
            return value;
        }
    }
    return NULL;
}

static XiValue *find_direct_call(const XiFunc *function, const XgGlobalEvidence *evidence,
                                 const XgCallsiteSummary **summary_out) {
    if (summary_out)
        *summary_out = NULL;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (!value || value->op != XI_CALL || value->xg_callsite_id == XG_NO_ID)
                continue;
            const XgCallsiteSummary *summary = xg_global_evidence_find_callsite(
                evidence, (XgCallsiteId) value->xg_callsite_id);
            if (!summary || summary->kind != XG_CALL_DIRECT_FUNC)
                continue;
            if (summary_out)
                *summary_out = summary;
            return value;
        }
    }
    return NULL;
}

static const XrValidatedInstruction *find_validated_operation(
    const XrValidatedProgram *program, uint16_t operation_id,
    const XrValidatedFunction **owner_out) {
    if (owner_out)
        *owner_out = NULL;
    for (uint32_t function_index = 0u; function_index < program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction =
                    &block->instructions[instruction_index];
                if (instruction->operation_id != operation_id)
                    continue;
                if (owner_out)
                    *owner_out = function;
                return instruction;
            }
        }
    }
    return NULL;
}

static uint32_t collect_callable_pack_targets(const XrValidatedProgram *program,
                                              uint32_t targets[2]) {
    uint32_t count = 0u;
    for (uint32_t function_index = 0u; function_index < program->function_count;
         ++function_index) {
        const XrValidatedFunction *function = &program->functions[function_index];
        for (uint32_t block_index = 0u; block_index < function->block_count; ++block_index) {
            const XrValidatedBlock *block = &function->blocks[block_index];
            for (uint32_t instruction_index = 0u; instruction_index < block->instruction_count;
                 ++instruction_index) {
                const XrValidatedInstruction *instruction =
                    &block->instructions[instruction_index];
                if (instruction->operation_id != XR_CORE_OP_CORE_CALLABLE_PACK ||
                    instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION)
                    continue;
                uint32_t target = instruction->immediate.function_id;
                bool duplicate = false;
                for (uint32_t index = 0u; index < count; ++index)
                    duplicate |= targets[index] == target;
                if (!duplicate && count < 2u)
                    targets[count++] = target;
            }
        }
    }
    return count;
}

static uint32_t collect_import_checktypes(const XiFunc *function, XiValue *checks[2]) {
    uint32_t count = 0u;
    for (uint32_t block_index = 0u; function && block_index < function->nblocks; ++block_index) {
        XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *value = block->values[value_index];
            if (!value || value->op != XI_CHECKTYPE || value->nargs != 1u || !value->args ||
                !value->args[0] || value->args[0]->op != XI_GET_SHARED)
                continue;
            if (count < 2u)
                checks[count] = value;
            ++count;
        }
    }
    return count;
}

static bool imported_callable_producer_rejects(const XrProgramFromXiInput *input,
                                               char *diagnostic, size_t diagnostic_size) {
    XrProgramArtifact rejected = {0};
    XrProgramBuildStatus status =
        xr_program_write_from_xi(input, &rejected, diagnostic, diagnostic_size);
    bool failed_closed = status != XR_PROGRAM_BUILD_OK && rejected.bytes == NULL;
    xr_program_artifact_free(&rejected);
    return failed_closed;
}

TEST(test_imported_callable_multi_target_program) {
    XrVMConfig vm_config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&vm_config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *original_session = xr_compiler_session_current_for_isolate(isolate);
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ASSERT_EQ_PTR(xr_compiler_session_attach_isolate(isolate, session), original_session);
    char diagnostic[512] = {0};
    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, diagnostic,
                                                              sizeof(diagnostic)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(session, profile));

    ImportedCallableFixture *fixture = &g_active_fixture;
    ASSERT_TRUE(build_imported_callable_fixture(fixture, session, isolate, false, NULL, NULL,
                                                NULL, 0u));
    XiModule *consumer = fixture->modules[fixture->consumer_index];
    XiModule *library = fixture->modules[fixture->library_index];
    ASSERT_NOT_NULL(consumer);
    ASSERT_NOT_NULL(library);
    XiFunc *entry = find_module_function(consumer, "root");
    XiFunc *apply = find_module_function(consumer, "apply");
    XiFunc *increment = find_module_function(library, "increment");
    ASSERT_NOT_NULL(entry);
    ASSERT_NOT_NULL(apply);
    ASSERT_NOT_NULL(increment);

    XiImportRef *imports[2] = {0};
    uint32_t import_count = 0u;
    for (uint16_t slot = 0u; slot < consumer->nslots; ++slot) {
        XiImportRef *ref = consumer->slot_imports ? consumer->slot_imports[slot] : NULL;
        if (ref && import_count < 2u)
            imports[import_count++] = ref;
    }
    ASSERT_EQ_UINT(import_count, 2u);
    ASSERT_NE(imports[0]->resolved_func, imports[1]->resolved_func);
    for (uint32_t index = 0u; index < import_count; ++index) {
        ASSERT_TRUE(imports[index]->resolution_attempted);
        ASSERT_EQ_INT(imports[index]->resolved_mod_index, fixture->library_index);
        ASSERT_EQ_PTR(imports[index]->resolved_module, library);
        ASSERT_NOT_NULL(imports[index]->resolved_func);
        ASSERT_GE(imports[index]->resolved_shared_slot, 0);
        ASSERT_LT((uint32_t) imports[index]->resolved_shared_slot, library->nslots);
        ASSERT_EQ_PTR(library->slot_funcs[imports[index]->resolved_shared_slot],
                      imports[index]->resolved_func);
    }

    const XgCallsiteSummary *callsite = NULL;
    XiValue *indirect_call = find_indirect_call(apply, &fixture->evidence, &callsite);
    ASSERT_NOT_NULL(indirect_call);
    ASSERT_NOT_NULL(callsite);
    bool callable_target_count_is_exact = callsite->callable_target_count == 2u;
    ASSERT_TRUE(callable_target_count_is_exact);
    ASSERT_TRUE((callsite->flags & XG_CALL_TARGET_SET_VERIFIED) != 0u);
    ASSERT_TRUE((callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) != 0u);
    ASSERT_EQ_UINT(callsite->static_target_func_id, XG_NO_ID);
    ASSERT_NE(callsite->callable_signature_key, 0u);
    ASSERT_EQ_UINT(indirect_call->xg_callable_target_start, callsite->callable_target_start);
    ASSERT_EQ_UINT(indirect_call->xg_callable_target_count, callsite->callable_target_count);
    ASSERT_EQ_UINT(indirect_call->xg_callable_signature_key, callsite->callable_signature_key);
    ASSERT_EQ_UINT(indirect_call->xg_callable_effect_union, callsite->callable_effect_union);
    ASSERT_EQ_UINT(indirect_call->xg_callable_capability_union,
                   callsite->callable_capability_union);
    const XgCallableTargetSummary *targets = NULL;
    uint32_t target_count = 0u;
    ASSERT_TRUE(xg_global_evidence_callable_targets(&fixture->evidence, callsite, &targets,
                                                    &target_count));
    ASSERT_EQ_UINT(target_count, 2u);
    ASSERT_NE(targets[0].target_func_id, targets[1].target_func_id);
    uint32_t xg_effect_union = targets[0].effect_bits | targets[1].effect_bits;
    uint32_t xg_capability_union = targets[0].capability_bits | targets[1].capability_bits;
    ASSERT_EQ_UINT(callsite->callable_effect_union, xg_effect_union);
    ASSERT_EQ_UINT(callsite->callable_capability_union, xg_capability_union);
    ASSERT_TRUE((xg_effect_union & XG_BODY_MAY_CALL) == 0u);
    ASSERT_TRUE((callsite->flags & (XG_CALL_MAY_ERROR | XG_CALL_MAY_PANIC)) == 0u);

    const XgCallsiteSummary *library_callsite = NULL;
    XiValue *library_call = find_direct_call(increment, &fixture->evidence, &library_callsite);
    ASSERT_NOT_NULL(library_call);
    ASSERT_NOT_NULL(library_callsite);
    ASSERT_NE(library_callsite->static_target_func_id, XG_NO_ID);
    ASSERT_TRUE((library_callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) != 0u);
    ASSERT_TRUE((library_callsite->flags & (XG_CALL_MAY_ERROR | XG_CALL_MAY_PANIC)) == 0u);

    XrCoreIrKey semantic_profile = xr_core_ir_key(
        "source-imported-callable-profile", strlen("source-imported-callable-profile"));
    const XiFunc *module_roots[2] = {fixture->pipelines[0].ir, fixture->pipelines[1].ir};
    XrProgramFromXiInput producer_input = {
        .module_roots = module_roots,
        .module_count = 2u,
        .entry_function = entry,
        .global_evidence = &fixture->evidence,
        .semantic_profile_fingerprint = semantic_profile.bytes,
    };
    XiValue *import_checks[2] = {0};
    ASSERT_EQ_UINT(collect_import_checktypes(apply, import_checks), 2u);
    for (uint32_t index = 0u; index < 2u; ++index) {
        ASSERT_NOT_NULL(import_checks[index]);
        ASSERT_NOT_NULL(import_checks[index]->type);
        ASSERT_EQ_INT(import_checks[index]->type->kind, XR_KIND_FUNCTION);
        ASSERT_TRUE(xr_type_function_is_no_throw(import_checks[index]->type));
        ASSERT_FALSE(import_checks[index]->type->is_nullable);
        ASSERT_EQ_INT(import_checks[index]->aux_int,
                      (int64_t) xr_type_to_tid(import_checks[index]->type) << 1);
    }
    XrProgramArtifact artifact = {0};
    XrProgramBuildStatus producer_status = xr_program_write_from_xi(
        &producer_input, &artifact, diagnostic, sizeof(diagnostic));
    if (producer_status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "imported callable producer failed: %s: %s\n",
                xr_program_build_status_name(producer_status), diagnostic);
    ASSERT_EQ_INT(producer_status, XR_PROGRAM_BUILD_OK);

    XgCallsiteSummary *mutable_library_callsite = (XgCallsiteSummary *) library_callsite;
    uint32_t saved_library_callsite_flags = mutable_library_callsite->flags;
    mutable_library_callsite->flags &= ~XG_CALL_ERROR_EFFECT_VERIFIED;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    mutable_library_callsite->flags = saved_library_callsite_flags;

    XiImportRef *hostile_import = imports[0];
    XiFunc *hostile_target = hostile_import->resolved_func;
    ASSERT_NOT_NULL(hostile_target);
    ASSERT_EQ_UINT(hostile_target->nparams, 1u);
    ASSERT_NOT_NULL(hostile_target->params);
    ASSERT_NOT_NULL(hostile_target->params[0]);
    ASSERT_NOT_NULL(apply->params);
    ASSERT_NOT_NULL(apply->params[0]);

    XiModule *saved_resolved_module = hostile_import->resolved_module;
    hostile_import->resolved_module = NULL;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_import->resolved_module = saved_resolved_module;

    int saved_resolved_module_index = hostile_import->resolved_mod_index;
    hostile_import->resolved_mod_index = (int) fixture->consumer_index;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_import->resolved_mod_index = saved_resolved_module_index;

    ASSERT_GE(hostile_import->resolved_export_slot, 0);
    uint32_t hostile_export_slot = (uint32_t) hostile_import->resolved_export_slot;
    ASSERT_LT(hostile_export_slot, library->nexports);
    XiFunc *saved_export_function = library->exports[hostile_export_slot].function;
    library->exports[hostile_export_slot].function = NULL;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    library->exports[hostile_export_slot].function = saved_export_function;

    XgBodySummary *hostile_body = NULL;
    for (uint32_t index = 0u; index < fixture->evidence.nbodies; ++index) {
        if (fixture->evidence.bodies[index].func_id == hostile_target->xg_body_func_id) {
            hostile_body = &fixture->evidence.bodies[index];
            break;
        }
    }
    ASSERT_NOT_NULL(hostile_body);
    XgModuleId saved_body_module_id = hostile_body->module_id;
    hostile_body->module_id = (XgModuleId) (fixture->consumer_index + 1u);
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_body->module_id = saved_body_module_id;

    uint32_t saved_body_signature = hostile_body->signature_key;
    hostile_body->signature_key ^= UINT32_C(0x80000000);
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_body->signature_key = saved_body_signature;

    XgCallsiteSummary *mutable_callsite = (XgCallsiteSummary *) callsite;
    uint32_t saved_target_start = mutable_callsite->callable_target_start;
    mutable_callsite->callable_target_start = 0u;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    mutable_callsite->callable_target_start = saved_target_start;

    uint64_t saved_callsite_signature = mutable_callsite->callable_signature_key;
    mutable_callsite->callable_signature_key ^= UINT64_C(0x8000000000000000);
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    mutable_callsite->callable_signature_key = saved_callsite_signature;

    XrParamMode saved_target_mode = (XrParamMode) hostile_target->params[0]->param_mode;
    hostile_target->params[0]->param_mode = XR_PARAM_MOVE;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_target->params[0]->param_mode = saved_target_mode;

    XrType *saved_target_parameter_type = hostile_target->params[0]->type;
    hostile_target->params[0]->type = apply->params[0]->type;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_target->params[0]->type = saved_target_parameter_type;

    XrType *saved_target_result_type = hostile_target->return_type;
    hostile_target->return_type = apply->params[0]->type;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_target->return_type = saved_target_result_type;

    int64_t saved_checktype_aux = import_checks[0]->aux_int;
    import_checks[0]->aux_int ^= INT64_C(2);
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    import_checks[0]->aux_int = saved_checktype_aux;

    XrFnThrowEffect saved_checktype_effect = import_checks[0]->type->function.throw_effect;
    import_checks[0]->type->function.throw_effect = XR_FN_EFFECT_MAY_THROW;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    import_checks[0]->type->function.throw_effect = saved_checktype_effect;

    bool saved_checktype_nullable = import_checks[0]->type->is_nullable;
    import_checks[0]->type->is_nullable = true;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    import_checks[0]->type->is_nullable = saved_checktype_nullable;

    uint32_t target_start = mutable_callsite->callable_target_start - 1u;
    ASSERT_LT(target_start, fixture->evidence.ncallable_targets);
    XgCallableTargetSummary *hostile_target_row =
        &fixture->evidence.callable_targets[target_start];
    uint32_t saved_target_effects = hostile_target_row->effect_bits;
    uint32_t saved_effect_union = mutable_callsite->callable_effect_union;
    uint32_t saved_callsite_flags = mutable_callsite->flags;
    uint32_t saved_xi_effect_union = indirect_call->xg_callable_effect_union;

    hostile_target_row->effect_bits |= XG_BODY_MAY_ERROR;
    mutable_callsite->callable_effect_union |= XG_BODY_MAY_ERROR;
    mutable_callsite->flags |= XG_CALL_MAY_ERROR;
    indirect_call->xg_callable_effect_union = mutable_callsite->callable_effect_union;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_target_row->effect_bits = saved_target_effects;
    mutable_callsite->callable_effect_union = saved_effect_union;
    mutable_callsite->flags = saved_callsite_flags;
    indirect_call->xg_callable_effect_union = saved_xi_effect_union;

    hostile_target_row->effect_bits |= XG_BODY_MAY_PANIC;
    mutable_callsite->callable_effect_union |= XG_BODY_MAY_PANIC;
    mutable_callsite->flags |= XG_CALL_MAY_PANIC;
    indirect_call->xg_callable_effect_union = mutable_callsite->callable_effect_union;
    ASSERT_TRUE(imported_callable_producer_rejects(&producer_input, diagnostic,
                                                   sizeof(diagnostic)));
    hostile_target_row->effect_bits = saved_target_effects;
    mutable_callsite->callable_effect_union = saved_effect_union;
    mutable_callsite->flags = saved_callsite_flags;
    indirect_call->xg_callable_effect_union = saved_xi_effect_union;

    const char *saved_consumer_name = consumer->name;
    const char *saved_library_name = library->name;
    const char *saved_export_name = library->exports[0].name;
    const char *saved_member_name = imports[0]->member_name;
    const char *saved_module_path = imports[0]->module_path;
    const char *saved_function_name = imports[0]->resolved_func->name;
    consumer->name = "renamed_consumer_debug_only";
    library->name = "renamed_library_debug_only";
    library->exports[0].name = "renamed_export_debug_only";
    imports[0]->member_name = "renamed_member_debug_only";
    imports[0]->module_path = "renamed_path_debug_only";
    imports[0]->resolved_func->name = "renamed_function_debug_only";
    XrProgramArtifact renamed_artifact = {0};
    ASSERT_EQ_INT(xr_program_write_from_xi(&producer_input, &renamed_artifact, diagnostic,
                                           sizeof(diagnostic)),
                  XR_PROGRAM_BUILD_OK);
    ASSERT_EQ_UINT(renamed_artifact.size, artifact.size);
    ASSERT_EQ_INT(memcmp(renamed_artifact.bytes, artifact.bytes, artifact.size), 0);
    consumer->name = saved_consumer_name;
    library->name = saved_library_name;
    library->exports[0].name = saved_export_name;
    imports[0]->member_name = saved_member_name;
    imports[0]->module_path = saved_module_path;
    imports[0]->resolved_func->name = saved_function_name;
    xr_program_artifact_free(&renamed_artifact);

    XiFunc *saved_target = imports[0]->resolved_func;
    imports[0]->resolved_func = imports[1]->resolved_func;
    XrProgramArtifact forged_artifact = {0};
    ASSERT_EQ_INT(xr_program_write_from_xi(&producer_input, &forged_artifact, diagnostic,
                                           sizeof(diagnostic)),
                  XR_PROGRAM_BUILD_INVALID_INPUT);
    ASSERT_NULL(forged_artifact.bytes);
    imports[0]->resolved_func = saved_target;

    XrValidatedProgram *validated = NULL;
    XrProgramDiagnostic verify_diagnostic;
    ASSERT_EQ_INT(xr_program_validate(artifact.bytes, artifact.size, NULL, &validated,
                                      &verify_diagnostic),
                  XR_PROGRAM_VERIFY_OK);
    ASSERT_NOT_NULL(validated);
    const XrValidatedFunction *indirect_owner = NULL;
    const XrValidatedInstruction *canonical_indirect = find_validated_operation(
        validated, XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT, &indirect_owner);
    ASSERT_NOT_NULL(canonical_indirect);
    ASSERT_NOT_NULL(indirect_owner);
    ASSERT_GT(canonical_indirect->operand_count, 0u);
    uint32_t callee_value = canonical_indirect->operands[0];
    ASSERT_LT(callee_value, indirect_owner->value_count);
    uint16_t callable_type_id = indirect_owner->value_types[callee_value];
    const XrValidatedType *callable_type =
        xr_validated_program_type(validated, callable_type_id);
    ASSERT_NOT_NULL(callable_type);
    ASSERT_EQ_INT(callable_type->kind, XR_CORE_IR_TYPE_CALLABLE);
    ASSERT_LT(callable_type->signature_id, validated->signature_count);
    const XrValidatedSignature *signature =
        &validated->signatures[callable_type->signature_id];
    ASSERT_EQ_UINT(signature->parameter_count, 1u);
    ASSERT_EQ_UINT(signature->parameter_types[0], XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(signature->result_type_id, XR_CORE_TYPE_I64);
    ASSERT_EQ_UINT(signature->error_type_id, XR_CORE_TYPE_VOID);
    ASSERT_EQ_UINT(signature->panic_type_id, XR_CORE_TYPE_VOID);

    uint32_t pack_targets[2] = {0};
    ASSERT_EQ_UINT(collect_callable_pack_targets(validated, pack_targets), 2u);
    ASSERT_LT(pack_targets[0], validated->function_count);
    ASSERT_LT(pack_targets[1], validated->function_count);
    uint32_t core_effect_union = validated->functions[pack_targets[0]].effect_mask |
                                 validated->functions[pack_targets[1]].effect_mask;
    uint32_t core_capability_union = validated->functions[pack_targets[0]].capability_mask |
                                     validated->functions[pack_targets[1]].capability_mask;
    ASSERT_EQ_UINT(signature->effect_mask, core_effect_union);
    ASSERT_EQ_UINT(signature->capability_mask, core_capability_union);
    ASSERT_TRUE((signature->effect_mask & XR_CORE_EFFECT_CALL) != 0u);
    ASSERT_TRUE((signature->effect_mask & XR_CORE_EFFECT_TRAP) != 0u);

    XrReferenceOutcome reference = xr_reference_evaluate(
        validated, xr_validated_program_entry_function(validated), NULL, 0u, NULL, NULL);
    ASSERT_EQ_INT(reference.kind, XR_REFERENCE_OUTCOME_RETURN);
    ASSERT_EQ_INT(reference.value.kind, XR_REFERENCE_VALUE_I64);
    ASSERT_EQ_INT(reference.value.as.i64, 42);

    ImportedCallableProviderBindings bindings;
    ASSERT_TRUE(imported_callable_build_provider_bindings(profile, &bindings));
    XrExecutionBindingInput execution_input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = validated,
        .profile = profile,
        .providers = bindings.count ? bindings.providers : NULL,
        .provider_count = bindings.count,
        .generation = 1u,
    };
    XrExecutionDiagnostic execution_diagnostic;
    XrInstance *instance = NULL;
    ASSERT_EQ_INT(xr_execution_instance_create(&execution_input, &instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_NOT_NULL(instance);
    XrVmCode *vm_code = NULL;
    XrVmCodeDiagnostic vm_diagnostic;
    ASSERT_EQ_INT(xr_vm_code_build(instance, NULL, &vm_code, &vm_diagnostic), XR_VM_CODE_OK);
    XrVmOutcome vm = xr_vm_code_execute(vm_code, instance,
                                        xr_validated_program_entry_function(validated), NULL, 0u);
    ASSERT_EQ_INT(vm.kind, XR_VM_OUTCOME_RETURN);
    ASSERT_EQ_INT(vm.value.kind, XR_VM_VALUE_I64);
    ASSERT_EQ_INT(vm.value.as.i64, reference.value.as.i64);
    xr_vm_code_free(vm_code);

    XrBackendIR *backend_ir = NULL;
    XrBackendDiagnostic backend_diagnostic;
    XrBackendOptions backend_options = xr_backend_default_options();
    ASSERT_EQ_INT(xr_backend_ir_build(validated, profile, &backend_options, &backend_ir,
                                     &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_TRUE(xr_backend_ir_verify(backend_ir, &backend_diagnostic));
    ASSERT_TRUE(xr_backend_ir_translation_validate(backend_ir, &backend_diagnostic));
    XrGeneratedC generated = {0};
    ASSERT_EQ_INT(xr_backend_ir_emit_c(backend_ir, true, &generated, &backend_diagnostic),
                  XR_BACKEND_OK);
    ASSERT_NOT_NULL(generated.bytes);
    ASSERT_GT(generated.size, 0u);
    ASSERT_NOT_NULL(strstr(generated.bytes, "int main(void)"));
    ASSERT_NULL(strstr(generated.bytes, "selector"));
    ASSERT_NULL(strstr(generated.bytes, "identity"));
    ASSERT_NULL(strstr(generated.bytes, "increment"));
    ASSERT_NULL(strstr(generated.bytes, "imported_callable_library"));
    ASSERT_NULL(strstr(generated.bytes, "xr_aot_alloc"));
    if (g_generated_c_path) {
        FILE *generated_file = fopen(g_generated_c_path, "wb");
        ASSERT_NOT_NULL(generated_file);
        ASSERT_EQ_UINT(fwrite(generated.bytes, 1u, generated.size, generated_file),
                       generated.size);
        ASSERT_EQ_INT(fclose(generated_file), 0);
    }

    xr_generated_c_free(&generated);
    xr_backend_ir_free(backend_ir);
    ASSERT_EQ_INT(xr_execution_instance_begin_drain(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_retire(instance, &execution_diagnostic),
                  XR_EXECUTION_OK);
    ASSERT_EQ_INT(xr_execution_instance_free(&instance, &execution_diagnostic), XR_EXECUTION_OK);
    xr_validated_program_free(validated);
    xr_program_artifact_free(&artifact);
    destroy_imported_callable_fixture(fixture);
    xr_target_profile_free(profile);
    ASSERT_EQ_PTR(xr_compiler_session_attach_isolate(isolate, original_session), session);
    xr_compiler_session_delete(session);
    xray_vm_delete(isolate);
}

TEST(imported_static_method_uses_exact_cross_module_evidence) {
    static const char library_source[] =
        "export class Worker {\n"
        "  static child(value: i64) -> i64 {\n"
        "    Coro.yield()\n"
        "    return value\n"
        "  }\n"
        "}\n";
    static const char consumer_source[] =
        "import \"./library\" as library\n"
        "fn answer() -> i64 { return library.Worker.child(7) }\n";
    XrVMConfig vm_config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&vm_config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *original_session = xr_compiler_session_current_for_isolate(isolate);
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ASSERT_EQ_PTR(xr_compiler_session_attach_isolate(isolate, session), original_session);
    char diagnostic[512] = {0};
    XrTargetProfile *profile = NULL;
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&profile, diagnostic,
                                                              sizeof(diagnostic)));
    ASSERT_TRUE(xr_compiler_session_set_target_profile(session, profile));

    ImportedCallableFixture *fixture = &g_active_fixture;
    ASSERT_TRUE(build_imported_callable_fixture(
        fixture, session, isolate, false, library_source, consumer_source, NULL, 0u));
    XiFunc *answer = find_module_function(fixture->modules[fixture->consumer_index], "answer");
    ASSERT_NOT_NULL(answer);
    XiValue *method_call = NULL;
    for (uint32_t block_index = 0u; block_index < answer->nblocks; ++block_index) {
        XiBlock *block = answer->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            XiValue *candidate = block->values[value_index];
            if (!candidate || (candidate->op != XI_CALL_METHOD &&
                               candidate->op != XI_CALL_METHOD_DIRECT))
                continue;
            ASSERT_NULL(method_call);
            method_call = candidate;
        }
    }
    ASSERT_NOT_NULL(method_call);
    ASSERT_NE(method_call->xg_callsite_id, XG_NO_ID);
    const XgCallsiteSummary *callsite = xg_global_evidence_find_callsite(
        &fixture->evidence, (XgCallsiteId) method_call->xg_callsite_id);
    ASSERT_NOT_NULL(callsite);
    ASSERT_EQ_UINT(callsite->kind, XG_CALL_METHOD);
    ASSERT_NE(callsite->receiver_static_class_id, XG_NO_ID);
    ASSERT_NE(callsite->method_id, XG_NO_ID);
    ASSERT_NE(callsite->method_id, callsite->method_name_id);
    ASSERT_EQ_UINT(method_call->xg_method_id, callsite->method_id);

    const XgMethodSummary *method = NULL;
    for (uint32_t index = 0u; index < fixture->evidence.nmethods; ++index) {
        if (fixture->evidence.methods[index].method_id != callsite->method_id)
            continue;
        ASSERT_NULL(method);
        method = &fixture->evidence.methods[index];
    }
    ASSERT_NOT_NULL(method);
    ASSERT_EQ_UINT(method->owner_class_id, callsite->receiver_static_class_id);
    ASSERT_TRUE((method->flags & XG_METHOD_STATIC) != 0u);
    ASSERT_TRUE((method->flags & (XG_METHOD_NATIVE | XG_METHOD_GENERIC_TEMPLATE)) == 0u);
    ASSERT_EQ_UINT(method->signature_key, callsite->method_signature_key);

    const XgClassSummary *owner_class = NULL;
    for (uint32_t index = 0u; index < fixture->evidence.nclasses; ++index) {
        if (fixture->evidence.classes[index].class_id != method->owner_class_id)
            continue;
        ASSERT_NULL(owner_class);
        owner_class = &fixture->evidence.classes[index];
    }
    ASSERT_NOT_NULL(owner_class);

    const XgBodySummary *body = NULL;
    for (uint32_t index = 0u; index < fixture->evidence.nbodies; ++index) {
        if (fixture->evidence.bodies[index].owner_method_id != method->method_id)
            continue;
        ASSERT_NULL(body);
        body = &fixture->evidence.bodies[index];
    }
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_UINT(body->module_id, owner_class->module_id);
    ASSERT_EQ_UINT(body->signature_key, method->signature_key);

    destroy_imported_callable_fixture(fixture);
    xr_target_profile_free(profile);
    ASSERT_EQ_PTR(xr_compiler_session_attach_isolate(isolate, original_session), session);
    xr_compiler_session_delete(session);
    xray_vm_delete(isolate);
}

/* The compile driver emits a dependency before its dependants are compiled,
 * which detaches the dependency's Xi children from its module init. A static
 * method of an imported value struct must still be classified for the
 * consumer's coroutine lowering, from the dependency's frozen semantic plan,
 * exactly as an instance method is. */
TEST(imported_static_method_resolves_after_dependency_emission) {
    static const char library_source[] =
        "export struct Pair {\n"
        "  lanes: [i64; 2]\n"
        "  static make(lanes: [i64; 2]) -> Pair {\n"
        "    return Pair{lanes: copy(lanes)}\n"
        "  }\n"
        "  first() -> i64 { return this.lanes[0] }\n"
        "}\n";
    /* The call sits in the module initializer: unlike a named function it
     * carries no closed analyzer suspension summary, so classification has
     * to come from the dependency's plan. */
    static const char consumer_source[] =
        "import \"./library\" as library\n"
        "var first = library.Pair.make([3, 4]).first()\n";
    XrVMConfig vm_config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&vm_config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *original_session = xr_compiler_session_current_for_isolate(isolate);
    XrCompilerSessionConfig session_config = {0};
    XrCompilerSession *session = xr_compiler_session_new(&session_config);
    ASSERT_NOT_NULL(session);
    ASSERT_EQ_PTR(xr_compiler_session_attach_isolate(isolate, session), original_session);

    ImportedCallableFixture *fixture = &g_active_fixture;
    ASSERT_TRUE(prepare_imported_callable_graph(fixture, session, false, library_source,
                                                consumer_source, NULL, 0u));
    XrCompilerSessionOperationScope operation = {0};
    ASSERT_TRUE(xr_compiler_session_operation_begin(session, &operation));
    xr_compiler_session_set_module_graph(session, fixture->graph);
    XrCompiledModuleGraph compilation = {0};
    ASSERT_TRUE(xr_compile_module_graph_dependencies(session, fixture->analyzer, fixture->graph,
                                                     &compilation));
    XrModuleSpec *entry = &fixture->graph->specs[fixture->graph->entry_index];
    XiModule *entry_module = NULL;
    XrProto *proto = xr_compile_ast_in_graph(session, fixture->analyzer, entry->ast,
                                             entry->source_path, fixture->graph,
                                             compilation.modules, compilation.count,
                                             &entry_module, &entry->authority);
    ASSERT_NOT_NULL(proto);
    ASSERT_NOT_NULL(entry_module);

    xr_instruction_unit_free(proto);
    xr_compiled_module_graph_dispose(&compilation);
    xr_compiler_session_set_module_graph(session, NULL);
    ASSERT_TRUE(xr_compiler_session_operation_succeed(&operation));
    destroy_imported_callable_fixture(fixture);
    ASSERT_EQ_PTR(xr_compiler_session_attach_isolate(isolate, original_session), session);
    xr_compiler_session_delete(session);
    xray_vm_delete(isolate);
}

TEST_MAIN_BEGIN()
g_generated_c_path = argc == 2 ? argv[1] : NULL;
if (argc > 2)
    return 2;
RUN_TEST(imported_callable_fixture_failure_cleans_directory);
destroy_imported_callable_fixture(&g_active_fixture);
RUN_TEST(test_imported_callable_multi_target_program);
destroy_imported_callable_fixture(&g_active_fixture);
RUN_TEST(imported_static_method_uses_exact_cross_module_evidence);
RUN_TEST(imported_static_method_resolves_after_dependency_emission);
destroy_imported_callable_fixture(&g_active_fixture);
TEST_MAIN_END()
