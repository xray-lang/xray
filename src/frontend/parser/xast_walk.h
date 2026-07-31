/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_walk.h - Generic AST traversal and canonical node signatures
 *
 * KEY CONCEPT:
 *   One structure-aware description of the AST, usable by any consumer that
 *   needs to walk nodes without knowing their payload layout: structural
 *   comparison, digests, tooling, tests. Before this existed, every traversal
 *   (analyzer, lowering, formatter) re-encoded the shape of all ~90 node types
 *   independently, and a new node type could be silently missed by any of them.
 *
 *   Both entry points are FAIL-CLOSED: an AST node type that has not been
 *   taught to this file reports failure rather than reporting "no children" or
 *   "empty payload". A caller that treats failure as an error therefore cannot
 *   be silently under-informed by a newly added node type.
 *
 *   Deliberately excluded from both the child walk and the signature:
 *     - source positions (line/column) — formatting changes them by design;
 *     - analyzer side-table data (symbol_id, monomorphization metadata) —
 *       not present on a freshly parsed AST;
 *     - comment trivia — carried separately and compared by the formatter's
 *       own comment tests.
 */

#ifndef XAST_WALK_H
#define XAST_WALK_H

#include <stdbool.h>
#include <stddef.h>

#include "xast_nodes.h"
#include "../../base/xdefs.h"

// Visitor invoked once per child slot, in source order. `child` may be NULL:
// absent optional children (an `if` without `else`, a `return` with no value)
// are reported as NULL slots so that structure comparison can tell "absent"
// apart from "different". Return false to stop the walk early.
typedef bool (*XrAstChildFn)(AstNode *child, void *user_data);

// Visit every direct AST child of `node`. Returns false if `node`'s type is not
// covered by this file (fail-closed) or if the visitor stopped the walk; use
// xr_ast_node_is_known() to tell those two apart.
XR_FUNC bool xr_ast_for_each_child(const AstNode *node, XrAstChildFn fn, void *user_data);

// True when this file knows how to walk and describe `node`.
XR_FUNC bool xr_ast_node_is_known(const AstNode *node);

// Write a canonical, position-independent description of `node`'s own payload
// (its type plus every field that is not an AST child) into `buf`. Two nodes
// are structurally identical iff their signatures match and their children
// match pairwise. Returns false if the node type is unknown or `buf` is too
// small; on success `buf` is NUL-terminated.
XR_FUNC bool xr_ast_node_signature(const AstNode *node, char *buf, size_t buf_size);

#endif  // XAST_WALK_H
