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
#include "../base/xtarget_data_layout.h"

struct XrArena;
struct XrCompileStringPool;
struct XrReplSymbolTable;
struct XrSourceCache;
struct XrTypePool;
struct XaAnalyzer;
struct XrModuleGraph;
struct XrNativePackagePlan;
struct XrTargetProfile;

typedef struct XrCompilerSession XrCompilerSession;

typedef enum XrCompileUnitKind {
    XR_COMPILE_UNIT_USER = 0,
    XR_COMPILE_UNIT_STDLIB,
} XrCompileUnitKind;

typedef struct XrCompileUnitIdentity {
    XrCompileUnitKind kind;
    const char *canonical_module;
} XrCompileUnitIdentity;

typedef struct XrCompilerSessionScope {
    XrCompilerSession *session;
    struct XrArena *saved_arena;
    struct XrCompileStringPool *saved_pool;
    bool active;
} XrCompilerSessionScope;

typedef struct XrCompilerSessionConfig {
    XrVMRuntime *vm_host;
    const char *project_root;
    const char *source_file;
    bool repl_mode;
    bool emit_aot;
    const XrTargetDataLayout *target_data_layout;
    struct XrTargetProfile *target_profile;
    const struct XrNativePackagePlan *native_package_plan; /* borrowed */
} XrCompilerSessionConfig;

XR_FUNC XrCompilerSession *xr_compiler_session_new(const XrCompilerSessionConfig *cfg);
XR_FUNC void xr_compiler_session_delete(XrCompilerSession *session);

XR_FUNC XrVMRuntime *xr_compiler_session_vm_host(const XrCompilerSession *session);
XR_FUNC const XrTargetDataLayout *
xr_compiler_session_target_data_layout(const XrCompilerSession *session);
XR_FUNC bool xr_compiler_session_set_target_data_layout(XrCompilerSession *session,
                                                        const XrTargetDataLayout *layout);
XR_FUNC bool xr_compiler_session_set_target_profile(
    XrCompilerSession *session, struct XrTargetProfile *profile);
XR_FUNC const struct XrTargetProfile *xr_compiler_session_target_profile(
    const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_native_package_plan(XrCompilerSession *session,
                                                         const struct XrNativePackagePlan *plan);
XR_FUNC const struct XrNativePackagePlan *
xr_compiler_session_native_package_plan(const XrCompilerSession *session);
XR_FUNC XrCompilerSession *xr_compiler_session_current_for_isolate(XrVMRuntime *isolate);
XR_FUNC XrCompilerSession *xr_compiler_session_attach_isolate(XrVMRuntime *isolate,
                                                              XrCompilerSession *session);

XR_FUNC struct XrArena *xr_compiler_session_current_arena(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_current_arena(XrCompilerSession *session,
                                                   struct XrArena *arena);

XR_FUNC uint32_t xr_compiler_session_next_ast_node_id(XrCompilerSession *session);
XR_FUNC uint32_t xr_compiler_session_ast_node_id(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_ast_node_id(XrCompilerSession *session, uint32_t next_id);

XR_FUNC struct XrCompileStringPool *
xr_compiler_session_string_pool(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_set_string_pool(XrCompilerSession *session,
                                                 struct XrCompileStringPool *pool);

XR_FUNC struct XrTypePool *xr_compiler_session_ensure_analyzer_pool(XrCompilerSession *session);
XR_FUNC struct XrTypePool *xr_compiler_session_analyzer_pool(const XrCompilerSession *session);
XR_FUNC void xr_compiler_session_install_analyzer_pool(XrCompilerSession *session);

XR_FUNC struct XrSourceCache *xr_compiler_session_ensure_source_cache(XrCompilerSession *session);
XR_FUNC struct XrSourceCache *xr_compiler_session_source_cache(const XrCompilerSession *session);

XR_FUNC struct XrReplSymbolTable *
xr_compiler_session_ensure_repl_symbols(XrCompilerSession *session);
XR_FUNC struct XrReplSymbolTable *
xr_compiler_session_repl_symbols(const XrCompilerSession *session);
XR_FUNC struct XaAnalyzer *xr_compiler_session_ensure_repl_analyzer(XrCompilerSession *session);
XR_FUNC struct XaAnalyzer *xr_compiler_session_repl_analyzer(const XrCompilerSession *session);
/* A persistent analyzer retains symbols and type references into every REPL
 * input AST.  Transfer the program arena to the session until analyzer
 * teardown; destroying an input immediately would leave those references
 * dangling on the next incremental compile. */
XR_FUNC bool xr_compiler_session_retain_repl_program(XrCompilerSession *session, AstNode *program);

XR_FUNC void xr_compiler_session_set_module_graph(XrCompilerSession *session,
                                                  struct XrModuleGraph *graph);
XR_FUNC struct XrModuleGraph *xr_compiler_session_module_graph(const XrCompilerSession *session);

/* Explicit trust identity for graph-less compilation units such as the
 * build-time stdlib bytecode bootstrap.  Callers must clear it after the
 * compilation operation; strings are borrowed for the scoped operation. */
XR_FUNC void xr_compiler_session_set_compile_unit_identity(XrCompilerSession *session,
                                                           const XrCompileUnitIdentity *identity);
XR_FUNC XrCompileUnitIdentity
xr_compiler_session_compile_unit_identity(const XrCompilerSession *session);

XR_FUNC bool xr_compiler_session_push_arena(XrCompilerSession *session, struct XrArena *arena,
                                            const char *source_file, XrCompilerSessionScope *scope);
XR_FUNC void xr_compiler_session_pop_arena(XrCompilerSessionScope *scope);

#endif  // XCOMPILER_SESSION_H
