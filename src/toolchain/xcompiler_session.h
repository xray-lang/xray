/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_session.h - One target-neutral parser resource owner
 */
#ifndef XCOMPILER_SESSION_H
#define XCOMPILER_SESSION_H
#include "../base/xdefs.h"
#include "../base/xcompile_state.h"
#include "../base/xcompile_request.h"

struct XrArena;
struct XrCompileStringPool;
typedef struct XrCompilerSession XrCompilerSession;
/* Zero-initialize before push. A live scope cannot move or be copied; only the
 * exact top scope may pop. The borrowed scope storage must stay alive until
 * pop or session destruction. Closing with open scopes invalidates them and
 * sets BAD_ARGUMENT on the shared state before releasing the session. */
typedef struct XrCompilerSessionScope {
    XrCompilerSession *session;
    struct XrArena *saved_arena;
    struct XrCompileStringPool *saved_pool;
    struct XrCompilerSessionScope *parent;
    uint64_t token;
    bool active;
} XrCompilerSessionScope;

/* The caller supplies the sole ledger; output must be empty. No VM, target,
 * analyzer, REPL, cache or dependency-graph ownership belongs to this session.
 * Completed AST arenas retain their own state and may outlive the session. */
XR_FUNC XrCompileResourceStatus xr_compile_session_new(XrCompileResources *resources, XrCompilerSession **output);
/* A retained AST owner can create a temporary parser context on its exact
 * state. It shares its arena memory owner without copying limits. */
XR_FUNC XrCompileResourceStatus xr_compile_session_new_with_state(XrCompileState *state, XrCompilerSession **output);
XR_FUNC void xr_compile_session_free(XrCompilerSession *session);
/* A parser session borrows one initialized request control. The control and
 * session remain live throughout parsing; AST arenas retain memory only.
 * Bind once before parsing. Destruction never invokes a borrowed callback. */
XR_FUNC XrCompileResourceStatus xr_compile_session_bind_request(XrCompilerSession *, XrCompileRequestControl *);
XR_FUNC XrCompileRequestControl *xr_compile_session_request(const XrCompilerSession *);
XR_FUNC XrCompileResourceStatus xr_compile_session_poll(XrCompilerSession *);

XR_FUNC XrCompileResources *xr_compile_session_resources(const XrCompilerSession *session);
XR_FUNC XrCompileState *xr_compile_session_compile_state(const XrCompilerSession *session);
XR_FUNC XrCompileResourceStatus xr_compile_session_resource_status(const XrCompilerSession *session);
XR_FUNC struct XrArena *xr_compile_session_current_arena(const XrCompilerSession *session);
XR_FUNC struct XrCompileStringPool *xr_compile_session_string_pool(const XrCompilerSession *session);
XR_FUNC uint32_t xr_compile_session_next_ast_node_id(XrCompilerSession *session);
XR_FUNC XrCompileResourceStatus xr_compile_session_push_arena(XrCompilerSession *session,
    struct XrArena *arena, XrCompilerSessionScope *scope);
/* Valid LIFO cleanup restores bindings even after resource failure; the return
 * still preserves that first failure. Mismatched pop leaves every binding live. */
XR_FUNC XrCompileResourceStatus xr_compile_session_pop_arena(XrCompilerSessionScope *scope);
#endif // XCOMPILER_SESSION_H
