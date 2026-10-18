/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_storage.c - Atomic owned analyzer storage publication
 */
#include "xanalyzer_storage.h"
#include "xanalyzer.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include "../../toolchain/xcompiler_session.h"
#include "../parser/xast_nodes.h"

static void close_arena(XrArena *arena) {
    if (!arena) return;
    xr_arena_destroy(arena);
    xr_compile_state_free(arena);
}

static XrCompileResourceStatus open_arena(XrCompileState *state, XrArena **output) {
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_state_calloc(state, 1, sizeof(XrArena), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrArena *arena = memory;
    XrArenaBacking backing = {0};
    XrArenaStatus bound = xr_compiler_arena_state_backing(state, &backing);
    if (bound != XR_ARENA_OK) {
        xr_compile_state_free(arena);
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    }
    (void) xr_arena_open(arena, 4096, &backing);
    status = xr_compiler_arena_capture_status(arena, state);
    if (status != XR_COMPILE_RESOURCE_OK) {
        close_arena(arena);
        return status;
    }
    *output = arena;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_storage_open(
    XrCompileState *state, XaAnalyzer *analyzer) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!analyzer || analyzer->consteval_arena || analyzer->callable_target_arena ||
        analyzer->type_pool || analyzer->symbol_registry || analyzer->global_scope ||
        analyzer->current_scope)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrArena *consteval = NULL, *callable = NULL;
    XrTypePool *pool = NULL;
    XaSymbolRegistry *registry = NULL;
    XaScope *global = NULL;
    status = open_arena(state, &consteval);
    if (status == XR_COMPILE_RESOURCE_OK) status = open_arena(state, &callable);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_type_pool_open(state, &pool);
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_symbol_registry_new(state, pool, &registry);
    if (status == XR_COMPILE_RESOURCE_OK) {
        global = xa_scope_new(registry, XA_SCOPE_GLOBAL, NULL);
        if (!global) status = xr_compile_state_status(state);
    }
    if (status != XR_COMPILE_RESOURCE_OK) {
        xa_scope_free(global);
        xa_symbol_registry_release(registry);
        close_arena(callable);
        close_arena(consteval);
        xr_type_pool_free(pool);
        return status;
    }
    analyzer->consteval_arena = consteval;
    analyzer->callable_target_arena = callable;
    analyzer->type_pool = pool;
    analyzer->symbol_registry = registry;
    analyzer->global_scope = global;
    analyzer->current_scope = global;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_storage_close(XaAnalyzer *analyzer) {
    if (!analyzer) return;
    XrArena *consteval = analyzer->consteval_arena;
    XrArena *callable = analyzer->callable_target_arena;
    XrTypePool *pool = analyzer->type_pool;
    analyzer->consteval_arena = NULL;
    analyzer->callable_target_arena = NULL;
    XaScope *global = analyzer->global_scope;
    XaSymbolRegistry *registry = analyzer->symbol_registry;
    analyzer->type_pool = NULL;
    analyzer->global_scope = NULL;
    analyzer->current_scope = NULL;
    analyzer->symbol_registry = NULL;
    xa_scope_free(global);
    xa_symbol_registry_release(registry);
    close_arena(callable);
    close_arena(consteval);
    xr_type_pool_free(pool);
}

XR_FUNC XrCompileResourceStatus xa_analyzer_ast_scope_open(XaAnalyzer *analyzer,
    const struct AstNode *program, XrCompilerSessionScope *scope) {
    if (!analyzer || !analyzer->compiler_session)
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompilerSession *session = analyzer->compiler_session;
    XrCompileResourceStatus status = xr_compile_session_resource_status(session);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!program || !scope || scope->active)
        return xr_compile_state_fail(xr_compile_session_compile_state(session),
            XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (program->type != AST_PROGRAM || !program->as.program.arena)
        return XR_COMPILE_RESOURCE_OK;
    return xr_compile_session_push_arena(session, program->as.program.arena, scope);
}

XR_FUNC XaSymbol *xa_analyzer_symbol_by_id(XaAnalyzer *analyzer, uint32_t symbol_id) {
    if (!analyzer || !analyzer->symbol_registry || symbol_id == 0)
        return NULL;
    return xa_symbol_registry_lookup(analyzer->symbol_registry, symbol_id);
}

XR_FUNC XrCompileResourceStatus xa_analyzer_enter_scope(
    XaAnalyzer *analyzer, XaScopeKind kind, void *ast_node) {
    XrCompileState *state = xa_symbol_registry_state(analyzer ? analyzer->symbol_registry : NULL);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!analyzer->current_scope ||
        analyzer->current_scope->registry != analyzer->symbol_registry ||
        kind < XA_SCOPE_GLOBAL || kind > XA_SCOPE_LOOP)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (ast_node) {
        for (int i = 0; i < analyzer->current_scope->child_count; i++) {
            status = xr_compile_state_work(state, 1);
            if (status != XR_COMPILE_RESOURCE_OK) return status;
            XaScope *child = analyzer->current_scope->children[i];
            if (child->ast_node != ast_node) continue;
            if (child->kind != kind)
                return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            status = xr_compile_state_work(state, 1);
            if (status != XR_COMPILE_RESOURCE_OK) return status;
            analyzer->current_scope = child;
            return XR_COMPILE_RESOURCE_OK;
        }
    }
    XaScope *scope = xa_scope_new(analyzer->symbol_registry, kind, analyzer->current_scope);
    if (!scope) return xr_compile_state_status(state);
    scope->ast_node = ast_node;
    analyzer->current_scope = scope;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_exit_scope(XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->current_scope) return;
    XR_DCHECK(analyzer->current_scope->parent != NULL,
        "exit_scope: already at root scope (unbalanced enter/exit)");
    if (analyzer->current_scope->parent)
        analyzer->current_scope = analyzer->current_scope->parent;
}
