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
struct XrCacheStore;
struct XrCacheStoreConfig;
struct XrDependencyGraph;
struct XrInvalidationEvent;
struct XrInvalidationResult;
struct XrReplSymbolTable;
struct XrSourceCache;
struct XrTypePool;
struct XaAnalyzer;
struct XrModuleGraph;
struct XrNativePackagePlan;
struct XrTargetProfile;

typedef struct XrCompilerSession XrCompilerSession;

#define XR_COMPILER_SESSION_INITIAL_GENERATION UINT64_C(1)
#define XR_COMPILER_SESSION_INVALIDATION_HISTORY_LIMIT 16u

typedef enum XrCompilerSessionGenerationChange {
    XR_COMPILER_SESSION_CHANGE_NONE = 0,
    XR_COMPILER_SESSION_CHANGE_SESSION = 1u << 0,
    XR_COMPILER_SESSION_CHANGE_WORKSPACE = 1u << 1,
    XR_COMPILER_SESSION_CHANGE_CONFIGURATION = 1u << 2,
    XR_COMPILER_SESSION_CHANGE_TARGET = 1u << 3,
    XR_COMPILER_SESSION_CHANGE_PROVIDER = 1u << 4,
    XR_COMPILER_SESSION_CHANGE_ALL = (1u << 5) - 1u,
} XrCompilerSessionGenerationChange;

/* A snapshot is detached from the mutable session and remains stable after
 * later change notifications or resets. */
typedef struct XrCompilerSessionGenerationSnapshot {
    uint64_t session_generation;
    uint64_t workspace_generation;
    uint64_t configuration_generation;
    uint64_t target_generation;
    uint64_t provider_generation;
} XrCompilerSessionGenerationSnapshot;

typedef enum XrCompilerSessionOperationOutcome {
    XR_COMPILER_SESSION_OPERATION_NONE = 0,
    XR_COMPILER_SESSION_OPERATION_SUCCEEDED,
    XR_COMPILER_SESSION_OPERATION_CANCELLED,
    XR_COMPILER_SESSION_OPERATION_FATAL,
} XrCompilerSessionOperationOutcome;

typedef struct XrCompilerSessionOperationScope {
    XrCompilerSession *session;
    uint64_t session_generation;
    bool owns_operation;
    bool active;
} XrCompilerSessionOperationScope;

typedef struct XrCompilerSessionIncrementalStats {
    size_t module_count;
    size_t dependency_count;
    size_t invalidation_history_count;
    size_t invalidation_history_limit;
    size_t logical_bytes;
    size_t peak_logical_bytes;
    uint64_t completed_operations;
    uint64_t cancelled_operations;
    uint64_t fatal_operations;
    XrCompilerSessionOperationOutcome last_outcome;
    bool operation_active;
    bool cache_store_open;
} XrCompilerSessionIncrementalStats;

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
    uint64_t session_generation;
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
    /* The session opens and owns the configured store. The store copies all
     * scalar and path configuration; verifier context lifetime remains the
     * caller's explicit responsibility. */
    const struct XrCacheStoreConfig *incremental_cache;
} XrCompilerSessionConfig;

XR_FUNC XrCompilerSession *xr_compiler_session_new(const XrCompilerSessionConfig *cfg);
XR_FUNC void xr_compiler_session_delete(XrCompilerSession *session);

XR_FUNC const char *xr_compiler_session_project_root(const XrCompilerSession *session);
XR_FUNC const char *xr_compiler_session_source_file(const XrCompilerSession *session);
XR_FUNC XrCompilerSessionGenerationSnapshot
xr_compiler_session_generation_snapshot(const XrCompilerSession *session);
XR_FUNC bool xr_compiler_session_apply_generation_change(XrCompilerSession *session,
                                                         uint32_t change_mask);
XR_FUNC bool xr_compiler_session_reset_incremental(XrCompilerSession *session);
/* The outermost scope owns the transaction. Nested compiler entry points
 * borrow it, so a module bundle is one operation rather than one operation per
 * dependency. Any nested failure aborts the owner and invalidates every scope
 * from the abandoned generation. */
XR_FUNC bool xr_compiler_session_operation_begin(
    XrCompilerSession *session, XrCompilerSessionOperationScope *scope);
XR_FUNC bool xr_compiler_session_operation_succeed(
    XrCompilerSessionOperationScope *scope);
XR_FUNC bool xr_compiler_session_operation_fail(
    XrCompilerSessionOperationScope *scope, XrCompilerSessionOperationOutcome outcome);
XR_FUNC bool xr_compiler_session_publish_dependency_graph(
    XrCompilerSession *session, const struct XrDependencyGraph *graph);
XR_FUNC bool xr_compiler_session_apply_invalidation(
    XrCompilerSession *session, const struct XrInvalidationEvent *event);
XR_FUNC const struct XrDependencyGraph *xr_compiler_session_dependency_graph(
    const XrCompilerSession *session);
XR_FUNC const struct XrInvalidationResult *xr_compiler_session_invalidation_at(
    const XrCompilerSession *session, size_t index);
XR_FUNC struct XrCacheStore *xr_compiler_session_cache_store(
    const XrCompilerSession *session);
XR_FUNC XrCompilerSessionIncrementalStats xr_compiler_session_incremental_stats(
    const XrCompilerSession *session);
XR_FUNC bool xr_compiler_session_incremental_idle_cleanup(
    XrCompilerSession *session, size_t retained_history);

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
