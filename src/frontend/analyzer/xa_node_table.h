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

#include "xconsteval.h"
#include "xa_effect_db.h"
#include "../../base/xdefs.h"
#include "../../shared/xr_conversion.h"
#include "../../runtime/value/xtype.h"
#include <stdbool.h>
#include <stdint.h>

struct AstNode;
struct XrType;
struct XaScope;
struct XaSymbol;

typedef struct XaNodeTable XaNodeTable;

/* Pointer-free conversion fact used when publishing an immutable
 * XaTypedProgram snapshot.  The snapshot owns the returned array; node_id is
 * the only syntax identity retained across the analyzer/publication boundary. */
typedef struct XaNodeConversionEntry {
    uint32_t node_id;
    XrConversionWitness witness;
} XaNodeConversionEntry;

/* Flow-sensitive error-channel conclusion for one call expression.  The
 * effect id names the exact analyzer-owned error set; throw_effect is the
 * constructive lowering decision, and incomplete facts remain fail-closed. */
typedef struct XaCallErrorEffectFact {
    XaEffectId effect_id;
    XrFnThrowEffect throw_effect;
    XaEffectCompleteness completeness;
    XaUnknownReasonSet unknown_reasons;
} XaCallErrorEffectFact;

typedef struct XaNodeCallErrorEffectEntry {
    uint32_t node_id;
    XaCallErrorEffectFact fact;
} XaNodeCallErrorEffectEntry;

XR_FUNC XaNodeTable *xa_node_table_new(void);
XR_FUNC void xa_node_table_free(XaNodeTable *t);

// Insert / overwrite the inferred type for `node`. Passing NULL for
// `type` clears any existing entry. Uses node->node_id as key.
XR_FUNC void xa_node_table_set_type(XaNodeTable *t, struct AstNode *node, struct XrType *type);

// Returns the previously set type, or NULL if no entry exists for
// `node`. Callers MUST treat NULL as "unknown".
XR_FUNC struct XrType *xa_node_table_get_type(const XaNodeTable *t, const struct AstNode *node);

// Set full binding facts for a node (type + scope + symbol).
XR_FUNC void xa_node_table_set(XaNodeTable *t, struct AstNode *node, struct XrType *type,
                               struct XaScope *scope, struct XaSymbol *symbol);

// Retrieve scope / symbol binding for a node.
XR_FUNC struct XaScope *xa_node_table_get_scope(const XaNodeTable *t, const struct AstNode *node);
XR_FUNC struct XaSymbol *xa_node_table_get_symbol(const XaNodeTable *t, const struct AstNode *node);

// Store / retrieve a compile-time value fact for `node`.
// Passing NULL for `value` clears only the ct-value fact.
XR_FUNC void xa_node_table_set_ct_value(XaNodeTable *t, const struct AstNode *node,
                                        const XrCtValue *value);
XR_FUNC bool xa_node_table_get_ct_value(const XaNodeTable *t, const struct AstNode *node,
                                        XrCtValue *out_value);

// Store / retrieve the analyzer's canonical conversion classification for an
// expression.  The witness is POD and owns no pointers across stage lifetimes.
XR_FUNC void xa_node_table_set_conversion(XaNodeTable *t, const struct AstNode *node,
                                          const XrConversionWitness *witness);
XR_FUNC bool xa_node_table_get_conversion(const XaNodeTable *t, const struct AstNode *node,
                                          XrConversionWitness *out_witness);

/* Copy every conversion fact into a deterministic node_id-sorted array.
 * The caller owns *out_entries and releases it with xr_free().  Empty tables
 * succeed with a NULL array and zero count; allocation failure returns false. */
XR_FUNC bool xa_node_table_snapshot_conversions(const XaNodeTable *t,
                                                XaNodeConversionEntry **out_entries,
                                                uint32_t *out_count);

/* Store and publish the flow-sensitive call effect computed by error-set
 * inference.  These facts are separate from the callee's declared function
 * type because a mutable local can have a precise target at one callsite. */
XR_FUNC bool xa_node_table_set_call_error_effect(XaNodeTable *t, const struct AstNode *node,
                                                 const XaCallErrorEffectFact *fact);
XR_FUNC bool xa_node_table_get_call_error_effect(const XaNodeTable *t, const struct AstNode *node,
                                                 XaCallErrorEffectFact *out_fact);
XR_FUNC bool xa_node_table_snapshot_call_error_effects(const XaNodeTable *t,
                                                       XaNodeCallErrorEffectEntry **out_entries,
                                                       uint32_t *out_count);
XR_FUNC void xa_node_table_clear_call_error_effects(XaNodeTable *t);

// Drop all entries, keep the bucket array allocated. Used between
// analyses of the same file when the analyzer reuses its scratch state.
XR_FUNC void xa_node_table_clear(XaNodeTable *t);

// Number of live entries (mostly for invariant checks in tests).
XR_FUNC int xa_node_table_size(const XaNodeTable *t);

#endif  // XA_NODE_TABLE_H
