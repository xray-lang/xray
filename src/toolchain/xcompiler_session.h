/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_session.h - Toolchain-owned compiler session state
 */

#ifndef XCOMPILER_SESSION_H
#define XCOMPILER_SESSION_H

#include "../base/xdefs.h"
#include "../base/xforward_decl.h"

struct XrArena;
struct XrCompileStringPool;

typedef struct XrCompilerSession XrCompilerSession;

typedef struct XrCompilerSessionConfig {
    XrayIsolate *vm_host;
    const char *project_root;
    const char *source_file;
    bool repl_mode;
    bool emit_aot;
} XrCompilerSessionConfig;

XR_FUNC XrCompilerSession *xr_compiler_session_new(const XrCompilerSessionConfig *cfg);
XR_FUNC void xr_compiler_session_delete(XrCompilerSession *session);

XR_FUNC XrayIsolate *xr_compiler_session_vm_host(const XrCompilerSession *session);
XR_FUNC XrCompilerSession *xr_compiler_session_current_for_isolate(XrayIsolate *isolate);
XR_FUNC XrCompilerSession *xr_compiler_session_attach_isolate(XrayIsolate *isolate,
                                                              XrCompilerSession *session);

XR_FUNC struct XrArena *xr_compiler_session_current_arena(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_current_arena(XrCompilerSession *session,
                                                   struct XrArena *arena);

XR_FUNC uint32_t xr_compiler_session_next_ast_node_id(XrCompilerSession *session);
XR_FUNC uint32_t xr_compiler_session_ast_node_id(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_ast_node_id(XrCompilerSession *session, uint32_t next_id);
XR_FUNC void xr_compiler_session_commit_legacy_isolate_state(const XrCompilerSession *session);

XR_FUNC struct XrCompileStringPool *
xr_compiler_session_string_pool(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_string_pool(XrCompilerSession *session,
                                                 struct XrCompileStringPool *pool);

#endif  // XCOMPILER_SESSION_H
