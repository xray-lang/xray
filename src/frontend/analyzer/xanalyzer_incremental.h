/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_incremental.h - Incremental analysis support
 *
 * KEY CONCEPT:
 *   Tracks dependencies between symbols and supports incremental
 *   re-analysis when source code changes.
 *
 * WHY THIS DESIGN:
 *   - Dependency graph enables targeted re-analysis
 *   - Function-level granularity balances precision and overhead
 *   - Hash-based change detection avoids unnecessary work
 */

#ifndef XANALYZER_INCREMENTAL_H
#define XANALYZER_INCREMENTAL_H

#include "../../runtime/value/xtype.h"
#include "xanalyzer_symbol.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

// Forward declarations
typedef struct XaAnalyzer XaAnalyzer;
typedef struct AstNode AstNode;

// ============================================================================
// Dependency Graph
// ============================================================================

// Dependency kind
typedef enum XaDepKind {
    XA_DEP_REFERENCE,       // Symbol is referenced
    XA_DEP_CALL,            // Function is called
    XA_DEP_INHERITANCE,     // Class inherits from another
    XA_DEP_TYPE_USE,        // Type is used in annotation
} XaDepKind;

// Single dependency edge
typedef struct XaDependency {
    uint32_t from_id;           // Symbol that depends
    uint32_t to_id;             // Symbol being depended on
    XaDepKind kind;
    struct XaDependency *next;  // Next in bucket
} XaDependency;

// Dependency graph
typedef struct XaDependencyGraph {
    XaDependency **forward;     // from_id -> list of dependencies
    XaDependency **reverse;     // to_id -> list of dependents
    int bucket_count;
    int edge_count;
} XaDependencyGraph;

// ============================================================================
// Function/Block Cache
// ============================================================================

// Cached analysis result for a function/block
typedef struct XaBlockCache {
    uint32_t symbol_id;         // Owning function symbol ID (0 for top-level)
    uint64_t content_hash;      // Hash of the block's source content
    uint32_t start_line;        // Start line of the block
    uint32_t end_line;          // End line of the block
    XrType *inferred_type;      // Cached inferred type (for functions)
    struct XaBlockCache *next;
} XaBlockCache;

// File-level cache
typedef struct XaFileCache {
    const char *path;
    uint64_t file_hash;         // Hash of entire file content
    XaBlockCache *blocks;       // Cached blocks in this file
    int block_count;
    struct XaFileCache *next;
} XaFileCache;

// ============================================================================
// Incremental Analysis Context
// ============================================================================

typedef struct XaIncrementalCtx {
    XaDependencyGraph *deps;    // Dependency graph
    XaFileCache *file_caches;   // Per-file caches
    int file_count;

    // Working set for current update
    uint32_t *dirty_symbols;    // Symbols that need re-analysis
    int dirty_count;
    int dirty_capacity;

    // Statistics
    int full_analyses;          // Count of full re-analyses
    int incremental_updates;    // Count of incremental updates
    int skipped_functions;      // Functions skipped due to cache hit
} XaIncrementalCtx;

// ============================================================================
// API Functions
// ============================================================================

// Lifecycle
XR_FUNC XaIncrementalCtx *xa_incremental_new(void);
XR_FUNC void xa_incremental_free(XaIncrementalCtx *ctx);

// Dependency graph operations.
// The per-symbol delete API was removed (no caller, half-broken). Use
// xa_dep_remove_symbols() to drop every edge that touches any of `ids[]`;
// it is the only sanctioned cleanup path and is called from
// xa_analyzer_remove_file() in xanalyzer.c.
XR_FUNC void xa_dep_add(XaIncrementalCtx *ctx, uint32_t from, uint32_t to, XaDepKind kind);
XR_FUNC void xa_dep_remove_symbols(XaIncrementalCtx *ctx,
                                   const uint32_t *symbol_ids, int count);
XR_FUNC void xa_dep_get_dependents(XaIncrementalCtx *ctx, uint32_t symbol_id,
                           uint32_t **out_ids, int *out_count);

// Cache operations
XR_FUNC XaFileCache *xa_cache_get_file(XaIncrementalCtx *ctx, const char *path);
XR_FUNC XaBlockCache *xa_cache_get_block(XaFileCache *file, uint32_t symbol_id);
XR_FUNC void xa_cache_update_block(XaIncrementalCtx *ctx, const char *path,
                           uint32_t symbol_id, uint64_t hash,
                           uint32_t start_line, uint32_t end_line,
                           XrType *inferred_type);
XR_FUNC void xa_cache_invalidate_file(XaIncrementalCtx *ctx, const char *path);

// Change detection
XR_FUNC uint64_t xa_hash_content(const char *content, int length);
XR_FUNC uint64_t xa_hash_ast_block(AstNode *block);

// Incremental analysis
typedef struct XaChangeSet {
    uint32_t *added_symbols;
    int added_count;
    uint32_t *removed_symbols;
    int removed_count;
    uint32_t *modified_symbols;
    int modified_count;
} XaChangeSet;

XR_FUNC XaChangeSet *xa_detect_changes(XaIncrementalCtx *ctx, XaAnalyzer *analyzer,
                               const char *file, AstNode *old_ast, AstNode *new_ast);
XR_FUNC void xa_changeset_free(XaChangeSet *cs);

// Propagate changes through dependency graph
XR_FUNC void xa_propagate_dirty(XaIncrementalCtx *ctx, XaChangeSet *changes);

// xa_incremental_update() was deleted. The visible analyzer entry
// points are now xa_analyzer_refresh_file() (full-file rebuild + dirty
// propagation) and xa_analyzer_invalidate_range() (block-level invalidate
// stub) -- both declared in xanalyzer.h.

#endif // XANALYZER_INCREMENTAL_H
