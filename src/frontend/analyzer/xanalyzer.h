/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer.h - Static type analyzer public interface
 *
 * KEY CONCEPT:
 *   The analyzer performs multi-pass analysis on AST:
 *   1. Symbol collection - gather all declarations
 *   2. Type inference - compute types for expressions
 *   3. Type checking - verify type compatibility
 *
 * WHY THIS DESIGN:
 *   - Unified API for compiler and LSP
 *   - Incremental analysis support for IDE responsiveness
 *   - Lazy type computation for performance
 */

#ifndef XANALYZER_H
#define XANALYZER_H

#include "../../runtime/value/xtype.h"
#include "../../base/xtarget_data_layout.h"
#include "xanalyzer_symbol.h"
#include "xa_parallel_call_plan.h"
#include "xa_memory_effect_db.h"
#include "xa_resolved_call.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../runtime/xerror.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

// Forward declarations
typedef struct XaAnalyzer XaAnalyzer;
typedef struct AstNode XrAstNode;
typedef struct XrArena XrArena;
typedef struct XrCompilerSession XrCompilerSession;
typedef struct XrVMRuntime XrVMRuntime;

/* The analyzer consumes the compiler session's target ABI.  Semantic layout
 * queries must never infer ABI facts from the machine running the compiler. */
XR_FUNC const XrTargetDataLayout *xa_analyzer_target_data_layout(const XaAnalyzer *analyzer);

/* A Task whose result carries a unique mutable root is a single-owner handle.
 * Await lowering consumes that handle even though the source language keeps
 * the uniform `await task` spelling. */
XR_FUNC bool xa_task_type_requires_consuming_await(const XrType *type);
XR_FUNC bool xa_task_result_requires_consuming_await(const XrType *result);
/* Copy-value aggregates can still be pointer-backed in the hosted runtimes.
 * Their Task result is compiler-planned as a shared copy so the source-level
 * multi-observer contract does not depend on backend representation. */
XR_FUNC bool xa_task_type_requires_shared_copy_publication(const XrType *type);
XR_FUNC bool xa_task_result_requires_shared_copy_publication(const XrType *result);
XR_FUNC bool xa_type_is_concurrency_handle(const XrType *type);

// Diagnostic severity (matches LSP XrLspDiagnosticSeverity values)
typedef enum XrDiagSeverity {
    XR_DIAG_SEV_ERROR = 1,
    XR_DIAG_SEV_WARNING = 2,
    XR_DIAG_SEV_INFO = 3,
    XR_DIAG_SEV_HINT = 4,
} XrDiagSeverity;

typedef enum XaAnalyzerBuildProfile {
    XA_ANALYZER_BUILD_PROFILE_HOSTED = 0,
    XA_ANALYZER_BUILD_PROFILE_FREESTANDING,
} XaAnalyzerBuildProfile;

// Diagnostic message
typedef struct XaDiagnostic {
    XrDiagSeverity severity;
    const char *message;
    int code;  // Error code (XrErrorCode from xerror.h)
    XrLocation location;
    bool reported;  // Set after xr_diag_print to avoid duplicates
    struct XaDiagnostic *next;
} XaDiagnostic;

// File entry for multi-file support
typedef struct XaFileEntry {
    char *path;             // Owned file path
    XaScope *file_scope;    // File's top-level scope
    uint64_t content_hash;  // Hash for change detection
    bool dirty;             // Needs re-analysis
    struct XaFileEntry *next;
} XaFileEntry;

// Scoped activation of one analyzed file. Multi-module compiler phases must
// resolve names from the file that owns the AST, not whichever file happened
// to be analyzed last by a shared analyzer.
typedef struct XaAnalyzerFileScope {
    XaScope *previous_scope;
    const char *previous_file;
    bool active;
} XaAnalyzerFileScope;

// Analyzer context
struct XaAnalyzer {
    // Owning compiler session (explicit, no TLS)
    XrCompilerSession *compiler_session;

    /* Monotonic semantic publication revision. TypedProgram snapshots bind
     * to this value and become invalid after any completed re-analysis. */
    uint64_t semantic_revision;

    // Borrowed VM host for the current bytecode/runtime type helpers.
    XrVMRuntime *isolate;

    // Type pool (per-analyzer, no global state)
    XrTypePool *type_pool;

    // Symbol ID counter (per-analyzer, thread-safe)
    uint32_t next_symbol_id;

    // Global scope
    XaScope *global_scope;

    // Current analysis scope
    XaScope *current_scope;

    // Current file being analyzed
    const char *current_file;

    // Symbol registry (id -> XaSymbol*) for O(1) ID lookup
    void *symbols_by_id;  // XrIntMap internally

    // Multi-file support
    XaFileEntry *files;  // Linked list for ordered traversal
    void *files_map;     // XrHashMap*: path -> XaFileEntry* for O(1) lookup
    int file_count;

    // Diagnostics list (tail pointer keeps append O(1) and preserves source order)
    XaDiagnostic *diagnostics;
    XaDiagnostic *diagnostics_tail;
    int diagnostic_count;

    // Analysis options
    bool strict_null_checks;  // Treat null as distinct type
    bool strict_mode;         // Enable all strict checks
    bool infer_return_types;  // Infer function return types
    XaAnalyzerBuildProfile build_profile;

    // Caches
    void *type_cache;  // Cache for computed types
    void *flow_cache;  // Cache for flow analysis

    // Incremental analysis support
    void *incremental;  // XaIncrementalCtx* (forward declared)

    // AST -> inferred XrType side table. Populated during Pass 2
    // (type inference) and read by codegen / LSP. Replaces the inline
    // AstNode::compile_type field that previously coupled the parser
    // with semantic state. Forward-declared as void* to keep the public
    // analyzer header free of frontend-internal types.
    void *node_table;  // XaNodeTable* (forward declared)

    // Arena-owned compile-time aggregate values cached in symbols/nodes.
    XrArena *consteval_arena;

    // AST -> selection facts table. Populated during Pass 2 for
    // member access, method call, index, and module export nodes.
    // Consumed by lowerer/backend instead of re-discovering member info.
    void *selection_table;  // XaSelectionTable* (forward declared)

    // AST call -> resolved stdlib parallel intrinsic identity. Populated
    // during Pass 2 for module calls, selective imports, and Plan<S> methods.
    // Consumed by lowerer/backend before falling back to textual rediscovery.
    void *parallel_call_plan_table;  // XaParallelCallPlanTable* (forward declared)

    // AST call -> immutable canonical call identity. Intrinsics are assigned
    // from declaration metadata and survive alias/re-export spelling changes.
    void *resolved_call_table;  // XaResolvedCallTable* (forward declared)

    // Canonical analyzer-owned typed-error effect database. Function symbols
    // hold only non-owning XaEffectId values into this session-local store.
    XaEffectDatabase *effect_db;

    // Canonical root-relative relocation/invalidation summaries. Function
    // symbols hold only non-owning XaMemoryEffectId values into this store.
    XaMemoryEffectDatabase *memory_effect_db;

    // Canonical analyzer-owned allocation effects. Function symbols hold only
    // non-owning XaAllocEffectId values into this session-local store.
    XaAllocationDatabase *allocation_db;

    // Type inference/recovery telemetry. Unknown tracks legacy unresolved inference;
    // ErrorType tracks compiler recovery poison that must not reach executable IR.
    int unresolved_inference_count;
    int recovery_poison_type_count;

    // Module dependency graph (optional, set for graph-driven analysis).
    // When set, cross-module import types are resolved from the graph's
    // export tables instead of falling back to unknown.
    struct XrModuleGraph *graph;

    /* Explicit identity of the module currently being analyzed.  This is
     * derived from graph ownership by AST identity, or from the scoped
     * compile-unit identity for graph-less bootstrap compilation. */
    bool current_module_is_stdlib;
    const char *current_module_canonical;

    /* Enum layouts replaced by the post-monomorphization re-analysis pass.
     * XrType copies (catch error types, function-signature error types, ...)
     * cache the raw layout pointer and are still read during the second pass,
     * so a replaced layout must outlive its rebuilt XaEnumInfo and is freed
     * only when the analyzer is destroyed. Stored as void* to keep this public
     * header independent of the runtime layout type. */
    void **retired_enum_layouts;
    size_t retired_enum_layout_count;
    size_t retired_enum_layout_cap;
};

// API: Analyzer lifecycle
XR_FUNC XaAnalyzer *xa_analyzer_new(XrCompilerSession *session);
XR_FUNC void xa_analyzer_free(XaAnalyzer *analyzer);

// Current retained bytes in the analyzer's type-pool arena.
//
// The pool grows monotonically under repeated xa_analyzer_refresh_file():
// re-analysis frees the old XaSymbol structs but their XrType objects were
// bump-allocated from this arena, which can only be freed wholesale. Long-lived
// hosts (the LSP) poll this to decide when a full analyzer rebuild is worth the
// cost to reclaim the accumulated type garbage.
XR_FUNC size_t xa_analyzer_type_pool_bytes(const XaAnalyzer *analyzer);

// API: Configuration
XR_FUNC void xa_analyzer_set_strict_null(XaAnalyzer *analyzer, bool enable);
XR_FUNC void xa_analyzer_set_strict_mode(XaAnalyzer *analyzer, bool enable);
XR_FUNC void xa_analyzer_set_build_profile(XaAnalyzer *analyzer, XaAnalyzerBuildProfile profile);
XR_FUNC bool xa_analyzer_is_freestanding(const XaAnalyzer *analyzer);

// Freestanding stdlib policy shared by analyzer diagnostics and graph-level
// AOT preflight.  The module-level predicate is intentionally profile-wide;
// member-level narrowing only applies to modules that are already allowed.
XR_FUNC bool xa_freestanding_stdlib_module_known(const char *module_name);
XR_FUNC bool xa_freestanding_stdlib_module_allowed(const char *module_name);

/* Stable nominal-type owner for a declaration file.  This is resolved from
 * the module graph, not from the mutable module currently driving a later
 * inference pass; imported declarations may be revisited while another
 * module is current. */
/* Returns an owned canonical owner string; caller frees with xr_free(). */
XR_FUNC bool xa_analyzer_path_is_stdlib(const char *file);
XR_FUNC char *xa_analyzer_nominal_owner_for_file(XaAnalyzer *analyzer, const char *file);
XR_FUNC bool xa_freestanding_stdlib_member_allowed(const char *module_name,
                                                   const char *member_name);
XR_FUNC const char *xa_freestanding_stdlib_member_reject_suggestion(const char *module_name);

// Set module graph for cross-module type resolution.
// The graph must outlive the analyzer. Pass NULL to disable.
struct XrModuleGraph;
XR_FUNC void xa_analyzer_set_graph(XaAnalyzer *analyzer, struct XrModuleGraph *graph);

// API: Analysis
XR_FUNC void xa_analyzer_analyze(XaAnalyzer *analyzer, const char *file, XrAstNode *ast);
XR_FUNC void xa_analyzer_update(XaAnalyzer *analyzer, const char *file, XrAstNode *ast);
XR_FUNC bool xa_analyzer_push_file_scope(XaAnalyzer *analyzer, const char *file,
                                         XaAnalyzerFileScope *scope);
XR_FUNC void xa_analyzer_pop_file_scope(XaAnalyzer *analyzer, XaAnalyzerFileScope *scope);

// Full-file rebuild + dependency-graph dirty propagation. Earlier
// versions named this xa_analyzer_update_incremental(), which lied about its
// granularity -- it always re-analysed the whole file. The new name
// matches what it really does, freeing the word "incremental" for a
// future true-incremental implementation.
XR_FUNC void xa_analyzer_refresh_file(XaAnalyzer *analyzer, const char *file, XrAstNode *ast,
                                      uint64_t content_hash);

// Block-level invalidation hook for the LSP edit path. Today this
// degrades to "mark the whole file dirty"; the next refresh_file call
// will re-analyse the file. The (start_line, end_line) range is recorded
// so a future incremental implementation can use it without changing the
// API surface its callers rely on.
XR_FUNC void xa_analyzer_invalidate_range(XaAnalyzer *analyzer, const char *file,
                                          uint32_t start_line, uint32_t end_line);

XR_FUNC void xa_analyzer_remove_file(XaAnalyzer *analyzer, const char *file);

// API: AST -> inferred type side table.
// Set during Pass 2,
// read by codegen / LSP / mono. Both functions return / accept NULL
// gracefully (NULL == "type unknown / not yet inferred", same semantics
// the field had).
XR_FUNC void xa_analyzer_set_node_type(XaAnalyzer *analyzer, struct AstNode *node,
                                       struct XrType *type);
XR_FUNC struct XrType *xa_analyzer_get_node_type(XaAnalyzer *analyzer, const struct AstNode *node);
XR_FUNC void xa_analyzer_set_node_conversion(XaAnalyzer *analyzer, const struct AstNode *node,
                                             const XrConversionWitness *witness);
XR_FUNC bool xa_analyzer_get_node_conversion(XaAnalyzer *analyzer, const struct AstNode *node,
                                             XrConversionWitness *out_witness);
XR_FUNC void xa_analyzer_set_node_ct_value(XaAnalyzer *analyzer, const struct AstNode *node,
                                           const XrCtValue *value);
XR_FUNC bool xa_analyzer_get_node_ct_value(XaAnalyzer *analyzer, const struct AstNode *node,
                                           XrCtValue *out_value);
XR_FUNC struct XrType *xa_analyzer_resolve_adt_payload_type(XaAnalyzer *analyzer,
                                                            struct XrType *subject_type,
                                                            const struct AstNode *variant,
                                                            int payload_index);

// API: Selection facts (member/method/index resolution).
// Recorded during Pass 2. Consumed by lowerer to avoid re-resolving members.
struct XaSelection;
XR_FUNC const struct XaSelection *xa_analyzer_get_selection(XaAnalyzer *analyzer,
                                                            const struct AstNode *node);
XR_FUNC const XaParallelCallPlan *xa_analyzer_get_parallel_call_plan(XaAnalyzer *analyzer,
                                                                     const struct AstNode *node);
XR_FUNC const XaResolvedCall *xa_analyzer_get_resolved_call(XaAnalyzer *analyzer,
                                                            const struct AstNode *node);

// API: Cross-file incremental analysis
XR_FUNC const char **xa_analyzer_get_dirty_files(XaAnalyzer *analyzer, int *count);
XR_FUNC void xa_analyzer_mark_file_dirty(XaAnalyzer *analyzer, const char *file);

// API: Symbol lookup
XR_FUNC XaSymbol *xa_analyzer_lookup(XaAnalyzer *analyzer, const char *name);
XR_FUNC XaSymbol *xa_analyzer_lookup_in_scope(XaAnalyzer *analyzer, const char *name,
                                              XaScope *scope);
XR_FUNC XaSymbol *xa_analyzer_lookup_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                        uint32_t column);
XR_FUNC XaSymbol *xa_analyzer_lookup_deep(XaAnalyzer *analyzer, const char *name);

/* Single answer to "does this symbol denote module `module_name`?", and its
 * built-in-only variant for VM intrinsics (`Coro.yield()`, `Coro.Local<T>()`,
 * `CoroPool.submit()`, ...).  Every pass that special-cases a module member must
 * route the question here: deciding it from the receiver's source spelling makes
 * the same semantic judgement in several places with several different answers,
 * and mistakes any user declaration of that name for the module. */
XR_FUNC bool xa_symbol_is_module(XaAnalyzer *analyzer, XaSymbol *symbol, const char *module_name);
XR_FUNC bool xa_symbol_is_builtin_module(XaAnalyzer *analyzer, XaSymbol *symbol,
                                         const char *module_name);

// API: Type queries
XR_FUNC XrType *xa_analyzer_get_type(XaAnalyzer *analyzer, XaSymbol *symbol);
XR_FUNC XrType *xa_analyzer_get_type_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                        uint32_t column);
XR_FUNC XrType *xa_analyzer_infer_expr_type(XaAnalyzer *analyzer, XrAstNode *expr);

// Symbol links are now embedded directly in XaSymbol; no intmap lookup required.
static inline XaSymbolLinks *xa_analyzer_get_links(XaAnalyzer *analyzer, XaSymbol *symbol) {
    if (!symbol)
        return NULL;
    if (analyzer && !symbol->links.summary_owner)
        symbol->links.summary_owner = analyzer;
    return &symbol->links;
}

// API: Class lookup
XR_FUNC XrClassInfo *xa_analyzer_get_class(XaAnalyzer *analyzer, const char *name);

// API: Member completions (for LSP)
XR_FUNC XaSymbol **xa_analyzer_get_members(XaAnalyzer *analyzer, XrType *type, int *count);
XR_FUNC XaSymbol **xa_analyzer_get_scope_symbols(XaAnalyzer *analyzer, XaScope *scope, int *count);

// API: Diagnostics
XR_FUNC XaDiagnostic *xa_analyzer_get_diagnostics(XaAnalyzer *analyzer, int *count);
XR_FUNC void xa_analyzer_clear_diagnostics(XaAnalyzer *analyzer);

// API: Type checking
XR_FUNC bool xa_analyzer_check_assignment(XaAnalyzer *analyzer, XrType *target, XrType *source,
                                          XrLocation *loc);
XR_FUNC bool xa_analyzer_check_call(XaAnalyzer *analyzer, XrType *func_type, XrType **arg_types,
                                    int arg_count, XrLocation *loc);
// Analyzer-only diagnostic recovery. Normal language assignability remains in
// xr_type_assignable(); this helper only suppresses cascades after ErrorType exists.
XR_FUNC bool xa_recovery_compatible(XrType *target, XrType *source);
XR_FUNC bool xa_typecheck_assignable(XrType *target, XrType *source);
// Call-boundary compatibility is mode-aware: a READ parameter borrows the
// argument without restoring mutable authority, so a const argument may be
// viewed through the parameter's declared non-const type. REF/MOVE remain
// strict because they can mutate or consume the caller's storage.
XR_FUNC bool xa_call_arg_type_assignable(XrType *target, XrType *source, XrParamMode mode);

// API: Iterable/Iterator structural type checking
// Check if type satisfies Iterator<T> (has hasNext(): bool and next(): T)
XR_FUNC bool xa_analyzer_is_iterator(XaAnalyzer *analyzer, XrType *type, XrType **out_element_type);
// Check if type satisfies Iterable<T> (built-in or has iterator() -> Iterator<T>)
XR_FUNC bool xa_analyzer_is_iterable(XaAnalyzer *analyzer, XrType *type, XrType **out_element_type);

// API: Cross-module exports collection
// After analyzing a file, collect exported semantic symbols into a hashmap.
// Returns a new hashmap (name -> XaSymbol*) owned by caller (free with xr_hashmap_free).
// Symbol pointers are borrowed from analyzer scopes and live until analyzer is freed.
// Returns NULL if no exports found.
XR_FUNC struct XrHashMap *xa_analyzer_collect_export_symbols(XaAnalyzer *analyzer, XrAstNode *ast);
// Checked variant used by module-graph drivers. Returns false when the module's
// export metadata is poisoned and the whole table must be treated as invalid.
XR_FUNC bool xa_analyzer_collect_export_symbols_checked(XaAnalyzer *analyzer, XrAstNode *ast,
                                                        struct XrHashMap **out_exports);

// Internal: Add diagnostic
XR_FUNC void xa_analyzer_add_diagnostic(XaAnalyzer *analyzer, XrDiagSeverity severity, int code,
                                        const char *message, XrLocation *loc);

// Internal: Enter/exit scope (ast_node can be NULL)
XR_FUNC void xa_analyzer_enter_scope(XaAnalyzer *analyzer, XaScopeKind kind, void *ast_node);
XR_FUNC void xa_analyzer_exit_scope(XaAnalyzer *analyzer);

// API: Variable operations (compatible with ct_infer)
XR_FUNC XrType *xa_analyzer_lookup_var(XaAnalyzer *analyzer, const char *name);
XR_FUNC void xa_analyzer_define_var(XaAnalyzer *analyzer, const char *name, XrType *type);

// ============================================================================
// LSP Support: Find References and Rename
// ============================================================================

// Symbol reference location
typedef struct XaSymbolRef {
    const char *file;     // File path
    uint32_t line;        // 1-indexed line
    uint32_t column;      // 1-indexed column
    uint32_t end_column;  // End column (for highlight range)
    bool is_definition;   // true if this is the definition
    bool is_write;        // true if this is a write access
    struct XaSymbolRef *next;
} XaSymbolRef;

// Find all references to a symbol by name (uses dependency graph)
XR_FUNC XaSymbolRef *xa_analyzer_find_references(XaAnalyzer *analyzer, const char *name,
                                                 bool include_definition, int *count);

// Find all references to symbol at position
XR_FUNC XaSymbolRef *xa_analyzer_find_references_at(XaAnalyzer *analyzer, const char *file,
                                                    uint32_t line, uint32_t column, int *count);

// Free reference list
XR_FUNC void xa_analyzer_free_references(XaSymbolRef *refs);

// Prepare rename: check if symbol at position can be renamed
XR_FUNC bool xa_analyzer_can_rename(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                    uint32_t column, char **out_symbol_name);

#endif  // XANALYZER_H
