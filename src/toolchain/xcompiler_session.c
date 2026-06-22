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

#include "../base/xmalloc.h"
#include "../runtime/xisolate_internal.h"
#include <string.h>

struct XrCompilerSession {
    XrayIsolate *vm_host;
    const char *project_root;
    const char *source_file;
    bool repl_mode;
    bool emit_aot;

    struct XrArena *current_arena;
    struct XrCompileStringPool *compile_string_pool;
    uint32_t next_ast_node_id;
};

XrCompilerSession *xr_compiler_session_new(const XrCompilerSessionConfig *cfg) {
    XrCompilerSession *session = (XrCompilerSession *) xr_calloc(1, sizeof(XrCompilerSession));
    if (!session)
        return NULL;
    if (cfg) {
        session->vm_host = cfg->vm_host;
        session->project_root = cfg->project_root;
        session->source_file = cfg->source_file;
        session->repl_mode = cfg->repl_mode;
        session->emit_aot = cfg->emit_aot;
        if (cfg->vm_host)
            session->next_ast_node_id = cfg->vm_host->next_ast_node_id;
    }
    return session;
}

void xr_compiler_session_delete(XrCompilerSession *session) {
    xr_free(session);
}

XrayIsolate *xr_compiler_session_vm_host(const XrCompilerSession *session) {
    return session ? session->vm_host : NULL;
}

XrCompilerSession *xr_compiler_session_current_for_isolate(XrayIsolate *isolate) {
    return isolate ? isolate->compiler_session : NULL;
}

XrCompilerSession *xr_compiler_session_attach_isolate(XrayIsolate *isolate,
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

void xr_compiler_session_commit_legacy_isolate_state(const XrCompilerSession *session) {
    if (!session || !session->vm_host)
        return;
    if (session->vm_host->next_ast_node_id < session->next_ast_node_id)
        session->vm_host->next_ast_node_id = session->next_ast_node_id;
}

struct XrCompileStringPool *xr_compiler_session_string_pool(const XrCompilerSession *session) {
    return session ? session->compile_string_pool : NULL;
}

void xr_compiler_session_set_string_pool(XrCompilerSession *session,
                                         struct XrCompileStringPool *pool) {
    if (session)
        session->compile_string_pool = pool;
}
