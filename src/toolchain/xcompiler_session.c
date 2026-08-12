/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_session.c - Toolchain-owned compiler session state
 */

#include "xcompiler_session.h"

#include "../api/xrepl.h"
#include "../base/xarena.h"
#include "../base/xsource_cache.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xast_api.h"
#include "../frontend/parser/xstring_pool.h"
#include "../incremental/xr_cache_invalidate.h"
#include "../incremental/xr_cache_store.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_internal.h"
#include "../runtime/value/xtype_pool.h"
#include <string.h>

struct XrCompilerSession {
    XrVMRuntime *vm_host;
    char *project_root;
    char *source_file;
    bool repl_mode;
    bool emit_aot;
    XrTargetDataLayout target_data_layout;
    XrTargetProfile *target_profile;
    const struct XrNativePackagePlan *native_package_plan;
    XrCompilerSessionGenerationSnapshot generations;

    XrDependencyGraph dependency_graph;
    XrCacheStore *cache_store;
    XrInvalidationResult invalidation_history[XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT];
    size_t invalidation_history_count;
    size_t peak_incremental_logical_bytes;
    uint64_t completed_operations;
    uint64_t cancelled_operations;
    uint64_t fatal_operations;
    XrCompilerSessionOperationOutcome last_operation_outcome;
    bool incremental_operation_active;

    struct XrArena *current_arena;
    struct XrCompileStringPool *compile_string_pool;
    uint32_t next_ast_node_id;

    struct XrTypePool *analyzer_pool;
    struct XrSourceCache *source_cache;

    struct XrReplSymbolTable *repl_symbols;
    struct XaAnalyzer *repl_analyzer;
    AstNode **repl_programs;
    size_t repl_program_count;
    size_t repl_program_capacity;

    struct XrModuleGraph *module_graph;
    XrCompileUnitIdentity compile_unit_identity;
};

static const XrCompilerSessionGenerationSnapshot XR_INITIAL_GENERATIONS = {
    .session_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
    .workspace_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
    .configuration_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
    .target_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
    .provider_generation = XR_COMPILER_SESSION_INITIAL_GENERATION,
};

static char *copy_optional_string(const char *text) {
    if (!text)
        return NULL;
    size_t length = strlen(text);
    if (length == SIZE_MAX)
        return NULL;
    char *copy = (char *) xr_malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

static bool generation_would_overflow(uint64_t generation, uint32_t change_mask,
                                      uint32_t change) {
    return (change_mask & change) != 0 && generation == UINT64_MAX;
}

static void clear_transient_operation_state(XrCompilerSession *session) {
    session->current_arena = NULL;
    session->compile_string_pool = NULL;
    session->next_ast_node_id = 0;
    session->module_graph = NULL;
    session->compile_unit_identity = (XrCompileUnitIdentity) {0};
}

static void clear_invalidation_history(XrCompilerSession *session) {
    for (size_t i = 0; i < session->invalidation_history_count; i++)
        xr_invalidation_result_finalize(&session->invalidation_history[i]);
    session->invalidation_history_count = 0;
}

static bool checked_add_size(size_t *total, size_t value) {
    if (value > SIZE_MAX - *total)
        return false;
    *total += value;
    return true;
}

static bool checked_array_size(size_t count, size_t element_size, size_t *out) {
    if (element_size != 0 && count > SIZE_MAX / element_size)
        return false;
    *out = count * element_size;
    return true;
}

static size_t incremental_logical_bytes(const XrCompilerSession *session) {
    size_t node_bytes;
    size_t edge_bytes;
    size_t total = 0;
    if (!checked_array_size(session->dependency_graph.node_capacity,
                            sizeof(XrModuleSummary), &node_bytes) ||
        !checked_array_size(session->dependency_graph.edge_capacity,
                            sizeof(XrDependencyEdge), &edge_bytes) ||
        !checked_add_size(&total, node_bytes) ||
        !checked_add_size(&total, edge_bytes)) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < session->dependency_graph.node_count; i++) {
        const char *key = session->dependency_graph.nodes[i].canonical_key;
        if (key) {
            size_t key_length = strlen(key);
            if (key_length == SIZE_MAX || !checked_add_size(&total, key_length + 1u))
                return SIZE_MAX;
        }
    }
    for (size_t i = 0; i < session->invalidation_history_count; i++) {
        const XrInvalidationResult *result = &session->invalidation_history[i];
        size_t record_bytes;
        size_t evidence_bytes;
        if (!checked_array_size(result->record_count, sizeof(*result->records),
                                &record_bytes) ||
            !checked_array_size(result->evidence_count, sizeof(*result->evidence),
                                &evidence_bytes) ||
            !checked_add_size(&total, record_bytes) ||
            !checked_add_size(&total, evidence_bytes)) {
            return SIZE_MAX;
        }
    }
    return total;
}

static void refresh_incremental_watermark(XrCompilerSession *session) {
    size_t current = incremental_logical_bytes(session);
    if (current > session->peak_incremental_logical_bytes)
        session->peak_incremental_logical_bytes = current;
}

static bool copy_dependency_graph(XrDependencyGraph *out, const XrDependencyGraph *source) {
    xr_dependency_graph_init(out);
    if (!xr_dependency_graph_validate(source))
        return false;
    for (size_t i = 0; i < source->node_count; i++) {
        if (!xr_dependency_graph_add_node(out, &source->nodes[i]))
            goto failure;
    }
    for (size_t i = 0; i < source->edge_count; i++) {
        const XrDependencyEdge *edge = &source->edges[i];
        if (!xr_dependency_graph_add_edge(out, edge->consumer, edge->dependency,
                                          edge->relation)) {
            goto failure;
        }
    }
    if (xr_dependency_graph_validate(out))
        return true;
failure:
    xr_dependency_graph_finalize(out);
    return false;
}

XrCompilerSession *xr_compiler_session_new(const XrCompilerSessionConfig *cfg) {
    XrCompilerSession *session = (XrCompilerSession *) xr_calloc(1, sizeof(XrCompilerSession));
    if (!session)
        return NULL;
    session->generations = XR_INITIAL_GENERATIONS;
    xr_dependency_graph_init(&session->dependency_graph);
    if (!xr_target_data_layout_init_native(&session->target_data_layout)) {
        xr_free(session);
        return NULL;
    }
    if (cfg) {
        if (cfg->target_data_layout && cfg->target_profile) {
            const XrTargetMachineFacts *machine =
                xr_target_profile_machine_facts(cfg->target_profile);
            if (!xr_target_profile_verify(cfg->target_profile, NULL, 0) ||
                !machine ||
                memcmp(cfg->target_data_layout, &machine->data_layout,
                       sizeof(*cfg->target_data_layout)) != 0) {
                goto failure;
            }
        }
        session->project_root = copy_optional_string(cfg->project_root);
        if (cfg->project_root && !session->project_root)
            goto failure;
        session->source_file = copy_optional_string(cfg->source_file);
        if (cfg->source_file && !session->source_file)
            goto failure;
        session->vm_host = cfg->vm_host;
        session->repl_mode = cfg->repl_mode;
        session->emit_aot = cfg->emit_aot;
        session->native_package_plan = cfg->native_package_plan;
        if (cfg->incremental_cache) {
            session->cache_store = xr_cache_store_open(cfg->incremental_cache);
            if (!session->cache_store)
                goto failure;
        }
        if (cfg->target_profile) {
            if (!xr_compiler_session_set_target_profile(session, cfg->target_profile))
                goto failure;
        } else if (cfg->target_data_layout &&
                   !xr_compiler_session_set_target_data_layout(
                       session, cfg->target_data_layout)) {
            goto failure;
        }
    }
    return session;

failure:
    xr_cache_store_close(session->cache_store);
    xr_dependency_graph_finalize(&session->dependency_graph);
    xr_target_profile_free(session->target_profile);
    xr_free(session->source_file);
    xr_free(session->project_root);
    xr_free(session);
    return NULL;
}

void xr_compiler_session_delete(XrCompilerSession *session) {
    if (!session)
        return;
    if (session->vm_host) {
        if (session->vm_host->vm.debug_source_cache == session->source_cache)
            session->vm_host->vm.debug_source_cache = NULL;
        if (session->vm_host->compiler_session == session)
            session->vm_host->compiler_session = NULL;
    }
    if (session->repl_analyzer) {
        if (xr_type_get_current_pool() == session->repl_analyzer->type_pool)
            xr_type_set_current_pool(NULL, NULL);
        xa_analyzer_free(session->repl_analyzer);
    }
    for (size_t i = 0; i < session->repl_program_count; i++)
        xr_program_destroy(session->repl_programs[i]);
    xr_free(session->repl_programs);
    if (session->repl_symbols)
        xr_repl_symbols_free(session->repl_symbols);
    if (xr_type_get_current_pool() == session->analyzer_pool)
        xr_type_set_current_pool(NULL, NULL);
    if (session->source_cache)
        xr_source_cache_free(session->source_cache);
    if (session->analyzer_pool)
        xr_type_pool_free(session->analyzer_pool);
    clear_invalidation_history(session);
    xr_cache_store_close(session->cache_store);
    xr_dependency_graph_finalize(&session->dependency_graph);
    xr_target_profile_free(session->target_profile);
    xr_free(session->source_file);
    xr_free(session->project_root);
    xr_free(session);
}

const char *xr_compiler_session_project_root(const XrCompilerSession *session) {
    return session ? session->project_root : NULL;
}

const char *xr_compiler_session_source_file(const XrCompilerSession *session) {
    return session ? session->source_file : NULL;
}

XrCompilerSessionGenerationSnapshot
xr_compiler_session_generation_snapshot(const XrCompilerSession *session) {
    return session ? session->generations : (XrCompilerSessionGenerationSnapshot) {0};
}

bool xr_compiler_session_apply_generation_change(XrCompilerSession *session,
                                                 uint32_t change_mask) {
    if (!session || (change_mask & ~((uint32_t) XR_COMPILER_SESSION_CHANGE_ALL)) != 0)
        return false;
    if (change_mask == XR_COMPILER_SESSION_CHANGE_NONE)
        return true;

    XrCompilerSessionGenerationSnapshot next = session->generations;
    if (generation_would_overflow(next.session_generation, change_mask,
                                  XR_COMPILER_SESSION_CHANGE_SESSION) ||
        generation_would_overflow(next.workspace_generation, change_mask,
                                  XR_COMPILER_SESSION_CHANGE_WORKSPACE) ||
        generation_would_overflow(next.configuration_generation, change_mask,
                                  XR_COMPILER_SESSION_CHANGE_CONFIGURATION) ||
        generation_would_overflow(next.target_generation, change_mask,
                                  XR_COMPILER_SESSION_CHANGE_TARGET) ||
        generation_would_overflow(next.provider_generation, change_mask,
                                  XR_COMPILER_SESSION_CHANGE_PROVIDER))
        return false;

    if (change_mask & XR_COMPILER_SESSION_CHANGE_SESSION)
        next.session_generation++;
    if (change_mask & XR_COMPILER_SESSION_CHANGE_WORKSPACE)
        next.workspace_generation++;
    if (change_mask & XR_COMPILER_SESSION_CHANGE_CONFIGURATION)
        next.configuration_generation++;
    if (change_mask & XR_COMPILER_SESSION_CHANGE_TARGET)
        next.target_generation++;
    if (change_mask & XR_COMPILER_SESSION_CHANGE_PROVIDER)
        next.provider_generation++;
    session->generations = next;
    return true;
}

bool xr_compiler_session_reset_incremental(XrCompilerSession *session) {
    if (!session ||
        !xr_compiler_session_apply_generation_change(
            session, XR_COMPILER_SESSION_CHANGE_SESSION | XR_COMPILER_SESSION_CHANGE_WORKSPACE))
        return false;

    clear_transient_operation_state(session);
    session->incremental_operation_active = false;
    session->last_operation_outcome = XR_COMPILER_SESSION_OPERATION_NONE;
    session->completed_operations = 0;
    session->cancelled_operations = 0;
    session->fatal_operations = 0;
    clear_invalidation_history(session);
    xr_dependency_graph_finalize(&session->dependency_graph);
    xr_dependency_graph_init(&session->dependency_graph);
    session->peak_incremental_logical_bytes = 0;
    return true;
}

bool xr_compiler_session_begin_incremental_operation(XrCompilerSession *session) {
    if (!session || session->incremental_operation_active || session->current_arena ||
        session->compile_string_pool || session->module_graph ||
        session->compile_unit_identity.canonical_module)
        return false;
    session->incremental_operation_active = true;
    session->last_operation_outcome = XR_COMPILER_SESSION_OPERATION_NONE;
    return true;
}

bool xr_compiler_session_finish_incremental_operation(XrCompilerSession *session) {
    if (!session || !session->incremental_operation_active || session->current_arena ||
        session->compile_string_pool)
        return false;
    clear_transient_operation_state(session);
    session->incremental_operation_active = false;
    session->last_operation_outcome = XR_COMPILER_SESSION_OPERATION_SUCCEEDED;
    session->completed_operations++;
    return true;
}

bool xr_compiler_session_abort_incremental_operation(
    XrCompilerSession *session, XrCompilerSessionOperationOutcome outcome) {
    if (!session || !session->incremental_operation_active ||
        (outcome != XR_COMPILER_SESSION_OPERATION_CANCELLED &&
         outcome != XR_COMPILER_SESSION_OPERATION_FATAL) ||
        session->generations.session_generation == UINT64_MAX) {
        return false;
    }
    clear_transient_operation_state(session);
    session->generations.session_generation++;
    session->incremental_operation_active = false;
    session->last_operation_outcome = outcome;
    if (outcome == XR_COMPILER_SESSION_OPERATION_CANCELLED)
        session->cancelled_operations++;
    else
        session->fatal_operations++;
    return true;
}

bool xr_compiler_session_publish_dependency_graph(XrCompilerSession *session,
                                                  const XrDependencyGraph *graph) {
    if (!session || session->incremental_operation_active ||
        session->generations.workspace_generation == UINT64_MAX)
        return false;
    XrDependencyGraph copy;
    if (!copy_dependency_graph(&copy, graph))
        return false;
    XrDependencyGraph previous = session->dependency_graph;
    session->dependency_graph = copy;
    session->generations.workspace_generation++;
    clear_invalidation_history(session);
    xr_dependency_graph_finalize(&previous);
    refresh_incremental_watermark(session);
    return true;
}

bool xr_compiler_session_apply_invalidation(XrCompilerSession *session,
                                            const XrInvalidationEvent *event) {
    if (!session || session->incremental_operation_active || !event ||
        session->generations.workspace_generation == UINT64_MAX)
        return false;
    XrInvalidationResult result;
    if (!xr_cache_invalidate_apply(&session->dependency_graph, event, &result))
        return false;
    if (session->invalidation_history_count ==
        XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT) {
        xr_invalidation_result_finalize(&session->invalidation_history[0]);
        memmove(&session->invalidation_history[0], &session->invalidation_history[1],
                (XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT - 1u) *
                    sizeof(session->invalidation_history[0]));
        session->invalidation_history_count--;
    }
    session->invalidation_history[session->invalidation_history_count++] = result;
    session->generations.workspace_generation++;
    refresh_incremental_watermark(session);
    return true;
}

const XrDependencyGraph *xr_compiler_session_dependency_graph(
    const XrCompilerSession *session) {
    return session ? &session->dependency_graph : NULL;
}

const XrInvalidationResult *xr_compiler_session_invalidation_at(
    const XrCompilerSession *session, size_t index) {
    return session && index < session->invalidation_history_count
               ? &session->invalidation_history[index]
               : NULL;
}

XrCacheStore *xr_compiler_session_cache_store(const XrCompilerSession *session) {
    return session ? session->cache_store : NULL;
}

XrCompilerSessionIncrementalStats xr_compiler_session_incremental_stats(
    const XrCompilerSession *session) {
    if (!session)
        return (XrCompilerSessionIncrementalStats) {0};
    return (XrCompilerSessionIncrementalStats) {
        .module_count = session->dependency_graph.node_count,
        .dependency_count = session->dependency_graph.edge_count,
        .invalidation_history_count = session->invalidation_history_count,
        .invalidation_history_limit = XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT,
        .logical_bytes = incremental_logical_bytes(session),
        .peak_logical_bytes = session->peak_incremental_logical_bytes,
        .completed_operations = session->completed_operations,
        .cancelled_operations = session->cancelled_operations,
        .fatal_operations = session->fatal_operations,
        .last_outcome = session->last_operation_outcome,
        .operation_active = session->incremental_operation_active,
        .cache_store_open = session->cache_store != NULL,
    };
}

bool xr_compiler_session_incremental_idle_cleanup(XrCompilerSession *session,
                                                  size_t retained_history) {
    if (!session || session->incremental_operation_active ||
        retained_history > XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT)
        return false;
    while (session->invalidation_history_count > retained_history) {
        xr_invalidation_result_finalize(&session->invalidation_history[0]);
        if (session->invalidation_history_count > 1u) {
            memmove(&session->invalidation_history[0], &session->invalidation_history[1],
                    (session->invalidation_history_count - 1u) *
                        sizeof(session->invalidation_history[0]));
        }
        session->invalidation_history_count--;
    }
    refresh_incremental_watermark(session);
    return true;
}

XrVMRuntime *xr_compiler_session_vm_host(const XrCompilerSession *session) {
    return session ? session->vm_host : NULL;
}

const XrTargetDataLayout *xr_compiler_session_target_data_layout(const XrCompilerSession *session) {
    return session && xr_target_data_layout_validate(&session->target_data_layout)
               ? &session->target_data_layout
               : NULL;
}

bool xr_compiler_session_set_target_data_layout(XrCompilerSession *session,
                                                const XrTargetDataLayout *layout) {
    if (!session || !xr_target_data_layout_validate(layout))
        return false;
    if (session->target_profile) {
        const XrTargetMachineFacts *machine =
            xr_target_profile_machine_facts(session->target_profile);
        if (!machine || memcmp(layout, &machine->data_layout, sizeof(*layout)) != 0)
            return false;
    }
    if (!xr_compiler_session_apply_generation_change(
            session, XR_COMPILER_SESSION_CHANGE_TARGET))
        return false;
    session->target_data_layout = *layout;
    return true;
}

bool xr_compiler_session_set_target_profile(XrCompilerSession *session,
                                            XrTargetProfile *profile) {
    if (!session || !profile || !xr_target_profile_verify(profile, NULL, 0))
        return false;
    if (session->target_profile)
        return xr_target_profile_require_exact(session->target_profile, profile,
                                               NULL, 0);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    if (!machine || !xr_target_data_layout_validate(&machine->data_layout))
        return false;
    XrTargetProfile *retained = xr_target_profile_retain(profile);
    if (!retained)
        return false;
    if (!xr_compiler_session_apply_generation_change(
            session, XR_COMPILER_SESSION_CHANGE_TARGET |
                         XR_COMPILER_SESSION_CHANGE_PROVIDER)) {
        xr_target_profile_free(retained);
        return false;
    }
    session->target_data_layout = machine->data_layout;
    session->target_profile = retained;
    return true;
}

const XrTargetProfile *xr_compiler_session_target_profile(
    const XrCompilerSession *session) {
    return session ? session->target_profile : NULL;
}

void xr_compiler_session_set_native_package_plan(XrCompilerSession *session,
                                                 const struct XrNativePackagePlan *plan) {
    if (!session || !xr_compiler_session_apply_generation_change(
                        session, XR_COMPILER_SESSION_CHANGE_PROVIDER))
        return;
    session->native_package_plan = plan;
}

const struct XrNativePackagePlan *
xr_compiler_session_native_package_plan(const XrCompilerSession *session) {
    return session ? session->native_package_plan : NULL;
}

XrCompilerSession *xr_compiler_session_current_for_isolate(XrVMRuntime *isolate) {
    return isolate ? isolate->compiler_session : NULL;
}

XrCompilerSession *xr_compiler_session_attach_isolate(XrVMRuntime *isolate,
                                                      XrCompilerSession *session) {
    if (!isolate)
        return NULL;
    XrCompilerSession *previous = isolate->compiler_session;
    isolate->compiler_session = session;
    if (session && !session->vm_host)
        session->vm_host = isolate;
    return previous;
}

struct XrArena *xr_compiler_session_current_arena(const XrCompilerSession *session) {
    return session ? session->current_arena : NULL;
}

void xr_compiler_session_set_current_arena(XrCompilerSession *session, struct XrArena *arena) {
    if (session)
        session->current_arena = arena;
}

uint32_t xr_compiler_session_next_ast_node_id(XrCompilerSession *session) {
    if (!session)
        return 0;
    return ++session->next_ast_node_id;
}

uint32_t xr_compiler_session_ast_node_id(const XrCompilerSession *session) {
    return session ? session->next_ast_node_id : 0;
}

void xr_compiler_session_set_ast_node_id(XrCompilerSession *session, uint32_t next_id) {
    if (session)
        session->next_ast_node_id = next_id;
}

struct XrCompileStringPool *xr_compiler_session_string_pool(const XrCompilerSession *session) {
    return session ? session->compile_string_pool : NULL;
}

void xr_compiler_session_set_string_pool(XrCompilerSession *session,
                                         struct XrCompileStringPool *pool) {
    if (session)
        session->compile_string_pool = pool;
}

struct XrTypePool *xr_compiler_session_ensure_analyzer_pool(XrCompilerSession *session) {
    if (!session)
        return NULL;
    if (!session->analyzer_pool)
        session->analyzer_pool = xr_type_pool_new();
    return session->analyzer_pool;
}

struct XrTypePool *xr_compiler_session_analyzer_pool(const XrCompilerSession *session) {
    return session ? session->analyzer_pool : NULL;
}

void xr_compiler_session_install_analyzer_pool(XrCompilerSession *session) {
    XrTypePool *pool = xr_compiler_session_ensure_analyzer_pool(session);
    if (!session || !pool)
        return;
    xr_type_set_current_pool(pool, &pool->next_type_id);
}

struct XrSourceCache *xr_compiler_session_ensure_source_cache(XrCompilerSession *session) {
    if (!session)
        return NULL;
    if (!session->source_cache)
        session->source_cache = xr_source_cache_new();
    if (session->vm_host)
        session->vm_host->vm.debug_source_cache = session->source_cache;
    return session->source_cache;
}

struct XrSourceCache *xr_compiler_session_source_cache(const XrCompilerSession *session) {
    return session ? session->source_cache : NULL;
}

struct XrReplSymbolTable *xr_compiler_session_ensure_repl_symbols(XrCompilerSession *session) {
    if (!session)
        return NULL;
    if (!session->repl_symbols)
        session->repl_symbols = xr_repl_symbols_new();
    return session->repl_symbols;
}

struct XrReplSymbolTable *xr_compiler_session_repl_symbols(const XrCompilerSession *session) {
    return session ? session->repl_symbols : NULL;
}

struct XaAnalyzer *xr_compiler_session_ensure_repl_analyzer(XrCompilerSession *session) {
    if (!session || !session->vm_host)
        return NULL;
    if (!session->repl_analyzer)
        session->repl_analyzer = xa_analyzer_new(session);
    return session->repl_analyzer;
}

struct XaAnalyzer *xr_compiler_session_repl_analyzer(const XrCompilerSession *session) {
    return session ? session->repl_analyzer : NULL;
}

bool xr_compiler_session_retain_repl_program(XrCompilerSession *session, AstNode *program) {
    if (!session || !program)
        return false;
    if (session->repl_program_count == session->repl_program_capacity) {
        size_t next_capacity =
            session->repl_program_capacity ? session->repl_program_capacity * 2 : 8;
        AstNode **next = (AstNode **) xr_realloc(session->repl_programs,
                                                 next_capacity * sizeof(*session->repl_programs));
        if (!next)
            return false;
        session->repl_programs = next;
        session->repl_program_capacity = next_capacity;
    }
    session->repl_programs[session->repl_program_count++] = program;
    return true;
}

void xr_compiler_session_set_module_graph(XrCompilerSession *session, struct XrModuleGraph *graph) {
    if (session)
        session->module_graph = graph;
}

struct XrModuleGraph *xr_compiler_session_module_graph(const XrCompilerSession *session) {
    return session ? session->module_graph : NULL;
}

void xr_compiler_session_set_compile_unit_identity(XrCompilerSession *session,
                                                   const XrCompileUnitIdentity *identity) {
    if (!session)
        return;
    session->compile_unit_identity = identity ? *identity : (XrCompileUnitIdentity) {0};
}

XrCompileUnitIdentity xr_compiler_session_compile_unit_identity(const XrCompilerSession *session) {
    return session ? session->compile_unit_identity : (XrCompileUnitIdentity) {0};
}

bool xr_compiler_session_push_arena(XrCompilerSession *session, struct XrArena *arena,
                                    const char *source_file, XrCompilerSessionScope *scope) {
    (void) source_file;
    if (!scope)
        return false;
    memset(scope, 0, sizeof(*scope));
    if (!session || !arena)
        return false;

    scope->session = session;
    scope->saved_arena = xr_compiler_session_current_arena(session);
    scope->saved_pool = xr_compiler_session_string_pool(session);
    scope->session_generation = session->generations.session_generation;

    xr_compiler_session_set_current_arena(session, arena);
    if (!xr_compiler_session_string_pool(session) || scope->saved_arena != arena) {
        XrCompileStringPool *pool = xr_string_pool_new(arena);
        xr_compiler_session_set_string_pool(session, pool);
    }
    scope->active = true;
    return true;
}

void xr_compiler_session_pop_arena(XrCompilerSessionScope *scope) {
    if (!scope || !scope->active)
        return;

    if (scope->session &&
        scope->session_generation == scope->session->generations.session_generation) {
        xr_compiler_session_set_current_arena(scope->session, scope->saved_arena);
        xr_compiler_session_set_string_pool(scope->session, scope->saved_pool);
    }

    memset(scope, 0, sizeof(*scope));
}
