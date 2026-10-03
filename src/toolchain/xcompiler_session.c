/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_session.c - One target-neutral parser resource owner
 */
#include "xcompiler_session.h"
#include "xcompiler_arena_backing.h"
#include "../base/xarena.h"
#include "../frontend/parser/xstring_pool.h"

struct XrCompilerSession {
    XrCompileState *state;
    XrArena *arena;
    XrCompileStringPool *pool;
    XrCompilerSessionScope *scope;
    uint32_t next_ast_id;
    uint64_t next_scope_token;
};

static XrCompilerSessionStatus session_status(XrCompileResourceStatus status) {
    switch (status) {
        case XR_COMPILE_RESOURCE_OK: return XR_COMPILER_SESSION_OK;
        case XR_COMPILE_RESOURCE_BUDGET: return XR_COMPILER_SESSION_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_COMPILER_SESSION_OUT_OF_MEMORY;
        default: return XR_COMPILER_SESSION_BAD_ARGUMENT;
    }
}

XrCompilerSessionStatus xr_compile_session_new(XrCompileResources *resources, XrCompilerSession **output) {
    if (!resources || !output || *output) return XR_COMPILER_SESSION_BAD_ARGUMENT;
    XrCompileState *state = NULL;
    XrCompileResourceStatus status = xr_compile_state_new(resources, &state);
    if (status != XR_COMPILE_RESOURCE_OK) return session_status(status);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XrCompilerSession), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_release(state);
        return session_status(status);
    }
    XrCompilerSession *session = memory;
    session->state = state;
    *output = session;
    return XR_COMPILER_SESSION_OK;
}

void xr_compile_session_free(XrCompilerSession *session) {
    if (!session) return;
    XrCompileState *state = session->state;
    if (session->scope) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    while (session->scope) {
        XrCompilerSessionScope *scope = session->scope;
        session->scope = scope->parent;
        *scope = (XrCompilerSessionScope) {0};
    }
    xr_compile_state_free(session);
    xr_compile_state_release(state);
}

XrCompileState *xr_compile_session_compile_state(const XrCompilerSession *session) {
    return session ? session->state : NULL;
}
XrCompileResources *xr_compile_session_resources(const XrCompilerSession *session) {
    return xr_compile_state_resources(xr_compile_session_compile_state(session));
}
XrCompileResourceStatus xr_compile_session_resource_status(const XrCompilerSession *session) {
    return xr_compile_state_status(xr_compile_session_compile_state(session));
}
XrArena *xr_compile_session_current_arena(const XrCompilerSession *session) {
    return session ? session->arena : NULL;
}
XrCompileStringPool *xr_compile_session_string_pool(const XrCompilerSession *session) {
    return session ? session->pool : NULL;
}
uint32_t xr_compile_session_next_ast_node_id(XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return 0;
    if (session->next_ast_id == UINT32_MAX) {
        xr_compile_state_fail(session->state, XR_COMPILE_RESOURCE_BUDGET);
        return 0;
    }
    return ++session->next_ast_id;
}

XrCompileResourceStatus xr_compile_session_push_arena(XrCompilerSession *session,
    XrArena *arena, XrCompilerSessionScope *scope) {
    XrCompileState *state = xr_compile_session_compile_state(session);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!scope || scope->active || !xr_compiler_arena_matches_state(arena, state))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compiler_arena_capture_status(arena, state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (session->next_scope_token == UINT64_MAX)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    XrCompileStringPool *pool = session->pool;
    if (!xr_string_pool_matches(pool, state, arena)) {
        pool = NULL;
        status = xr_compile_string_pool_open(state, arena, &pool);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
    }
    XrCompilerSessionScope next = {session, session->arena, session->pool,
                                    session->scope, session->next_scope_token + 1, true};
    status = xr_compile_state_copy(state, scope, &next, sizeof(next));
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    session->scope = scope;
    session->next_scope_token = next.token;
    session->arena = arena;
    session->pool = pool;
    return XR_COMPILE_RESOURCE_OK;
}

XrCompileResourceStatus xr_compile_session_pop_arena(XrCompilerSessionScope *scope) {
    if (!scope || !scope->active || !scope->session) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompilerSession *session = scope->session;
    if (session->scope != scope || !scope->token)
        return xr_compile_state_fail(session->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    session->arena = scope->saved_arena;
    session->pool = scope->saved_pool;
    session->scope = scope->parent;
    *scope = (XrCompilerSessionScope) {0};
    return xr_compile_state_status(session->state);
}
