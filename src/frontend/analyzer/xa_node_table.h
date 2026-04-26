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
 *   nodes (literals, calls, member access, ...). Earlier each AstNode
 *   carried that type inline as `node->compile_type`, which forced the
 *   formatter (which never runs the analyzer) to allocate a field it
 *   never reads, and forced the analyzer / codegen to share a mutable
 *   field on a struct that is otherwise immutable parse output.
 *
 *   This module provides a side table -- a hash map keyed by
 *   `AstNode *` -- so the type metadata lives next to the analyzer
 *   that produced it, not next to the AST that the parser owns.
 *
 *   Ownership: one XaNodeTable per XaAnalyzer; freed with the analyzer.
 *   Keys are bare AstNode pointers. Entries are valid only while the
 *   analyzer's owning AST is alive (the analyzer is destroyed before
 *   the AST in every release path that exists).
 *
 *   Lookup is O(1) amortised; sets in the same bucket as an existing
 *   entry overwrite. Returns NULL for unknown nodes -- treated by
 *   callers as "type not yet inferred / unknown".
 */

#ifndef XA_NODE_TABLE_H
#define XA_NODE_TABLE_H

#include "../../base/xdefs.h"

struct AstNode;
struct XrType;

typedef struct XaNodeTable XaNodeTable;

XR_FUNC XaNodeTable *xa_node_table_new(void);
XR_FUNC void xa_node_table_free(XaNodeTable *t);

// Insert / overwrite the inferred type for `node`. Passing NULL for
// `type` clears any existing entry.
XR_FUNC void xa_node_table_set_type(XaNodeTable *t, struct AstNode *node, struct XrType *type);

// Returns the previously set type, or NULL if no entry exists for
// `node`. Callers MUST treat NULL as "unknown" (the field used to
// degrade to NULL in the same way as a missing entry).
XR_FUNC struct XrType *xa_node_table_get_type(const XaNodeTable *t, const struct AstNode *node);

// Drop all entries, keep the bucket array allocated. Used between
// analyses of the same file when the analyzer reuses its scratch state.
XR_FUNC void xa_node_table_clear(XaNodeTable *t);

// Number of live entries (mostly for invariant checks in tests).
XR_FUNC int xa_node_table_size(const XaNodeTable *t);

#endif  // XA_NODE_TABLE_H
