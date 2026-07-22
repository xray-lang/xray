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
#include "../runtime/xisolate_internal.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_internal.h"
#include "../runtime/value/xtype_pool.h"
#include <string.h>

struct XrCompilerSession {
    XrVMRuntime *vm_host;
    const char *project_root;
    const char *source_file;
    bool repl_mode;
    bool emit_aot;
    XrTargetDataLayout target_data_layout;
    const struct XrNativePackagePlan *native_package_plan;

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

XrCompilerSession *xr_compiler_session_new(const XrCompilerSessionConfig *cfg) {
    XrCompilerSession *session = (XrCompilerSession *) xr_calloc(1, sizeof(XrCompilerSession));
    if (!session)
        return NULL;
    if (!xr_target_data_layout_init_native(&session->target_data_layout)) {
        xr_free(session);
        return NULL;
    }
    if (cfg) {
        session->vm_host = cfg->vm_host;
        session->project_root = cfg->project_root;
        session->source_file = cfg->source_file;
        session->repl_mode = cfg->repl_mode;
        session->emit_aot = cfg->emit_aot;
        session->native_package_plan = cfg->native_package_plan;
        if (cfg->target_data_layout &&
            !xr_compiler_session_set_target_data_layout(session, cfg->target_data_layout)) {
            xr_free(session);
            return NULL;
        }
    }
    return session;
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
    xr_free(session);
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
    session->target_data_layout = *layout;
    return true;
}

void xr_compiler_session_set_native_package_plan(XrCompilerSession *session,
                                                 const struct XrNativePackagePlan *plan) {
    if (session)
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

    xr_compiler_session_set_current_arena(scope->session, scope->saved_arena);
    xr_compiler_session_set_string_pool(scope->session, scope->saved_pool);

    memset(scope, 0, sizeof(*scope));
}
