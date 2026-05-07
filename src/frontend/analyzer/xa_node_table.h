/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_node_table.h - AST -> semantic info side table
 *
 * KEY CONCEPT:
 *   The analyzer infers a compile-time XrType for many AST expression
 *   nodes (literals, calls, member access, ...). This module provides
 *   a side table keyed by stable `AstNode.node_id` (uint32_t) so the
 *   semantic metadata lives next to the analyzer that produced it.
 *
 *   Each entry carries: inferred XrType, enclosing XaScope, resolved
 *   XaSymbol. Together these form the "typed node facts" that the
 *   canonicalizer and lowerer consume.
 *
 *   Ownership: one XaNodeTable per XaAnalyzer; freed with the analyzer.
 *   Entries are valid while the analyzer's owning AST is alive.
 *
 *   Lookup is O(1) amortised; sets in the same bucket as an existing
 *   entry overwrite. Returns NULL for unknown nodes -- treated by
 *   callers as "type not yet inferred / unknown".
 */

#ifndef XA_NODE_TABLE_H
#define XA_NODE_TABLE_H

#include "../../base/xdefs.h"
#include <stdint.h>

struct AstNode;
struct XrType;
struct XaScope;
struct XaSymbol;

typedef struct XaNodeTable XaNodeTable;

XR_FUNC XaNodeTable *xa_node_table_new(void);
XR_FUNC void xa_node_table_free(XaNodeTable *t);

// Insert / overwrite the inferred type for `node`. Passing NULL for
// `type` clears any existing entry. Uses node->node_id as key.
XR_FUNC void xa_node_table_set_type(XaNodeTable *t, struct AstNode *node, struct XrType *type);

// Returns the previously set type, or NULL if no entry exists for
// `node`. Callers MUST treat NULL as "unknown".
XR_FUNC struct XrType *xa_node_table_get_type(const XaNodeTable *t, const struct AstNode *node);

// Set full binding facts for a node (type + scope + symbol).
XR_FUNC void xa_node_table_set(XaNodeTable *t, struct AstNode *node,
                               struct XrType *type, struct XaScope *scope,
                               struct XaSymbol *symbol);

// Retrieve scope / symbol binding for a node.
XR_FUNC struct XaScope *xa_node_table_get_scope(const XaNodeTable *t, const struct AstNode *node);
XR_FUNC struct XaSymbol *xa_node_table_get_symbol(const XaNodeTable *t, const struct AstNode *node);

// Drop all entries, keep the bucket array allocated. Used between
// analyses of the same file when the analyzer reuses its scratch state.
XR_FUNC void xa_node_table_clear(XaNodeTable *t);

// Number of live entries (mostly for invariant checks in tests).
XR_FUNC int xa_node_table_size(const XaNodeTable *t);

#endif  // XA_NODE_TABLE_H
