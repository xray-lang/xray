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
        analyzer->type_pool)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrArena *consteval = NULL, *callable = NULL;
    XrTypePool *pool = NULL;
    status = open_arena(state, &consteval);
    if (status == XR_COMPILE_RESOURCE_OK) status = open_arena(state, &callable);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_type_pool_open(state, &pool);
    if (status != XR_COMPILE_RESOURCE_OK) {
        close_arena(callable);
        close_arena(consteval);
        xr_type_pool_free(pool);
        return status;
    }
    analyzer->consteval_arena = consteval;
    analyzer->callable_target_arena = callable;
    analyzer->type_pool = pool;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_storage_close(XaAnalyzer *analyzer) {
    if (!analyzer) return;
    XrArena *consteval = analyzer->consteval_arena;
    XrArena *callable = analyzer->callable_target_arena;
    XrTypePool *pool = analyzer->type_pool;
    analyzer->consteval_arena = NULL;
    analyzer->callable_target_arena = NULL;
    analyzer->type_pool = NULL;
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
