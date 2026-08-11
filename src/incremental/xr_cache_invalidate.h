/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_invalidate.h - Deterministic facet invalidation and reason chains
 *
 * KEY CONCEPT:
 *   Invalidation is a fixed-point computation over explicit edge contracts.
 *   Results are stable-ID ordered and retain enough identity to walk from an
 *   affected module back to the root change without compiler pointers.
 */

#ifndef XR_CACHE_INVALIDATE_H
#define XR_CACHE_INVALIDATE_H

#include "xr_dependency_graph.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum XrInvalidationReason {
    XR_INVALIDATION_SUMMARY_CHANGED = 0,
    XR_INVALIDATION_MODULE_ADDED,
    XR_INVALIDATION_MODULE_DELETED,
    XR_INVALIDATION_MODULE_RENAMED,
    XR_INVALIDATION_GRAPH_CHANGED,
    XR_INVALIDATION_DEPENDENCY
} XrInvalidationReason;

typedef struct XrInvalidationEvent {
    XrInvalidationReason reason;
    XrStableId root_id;
    XrModuleFacetMask changed_facets;
    XrFingerprint old_fingerprint;
    XrFingerprint new_fingerprint;
    /* Copied during apply; never retained by the graph or result. */
    const XrModuleSummary *replacement_summary;
} XrInvalidationEvent;

typedef struct XrInvalidationRecord {
    XrStableId module_id;
    XrModuleFacetMask invalidated_facets;
    XrModuleFacetMask observed_facets;
    XrInvalidationReason direct_reason;
    bool has_parent;
    XrStableId parent_module_id;
} XrInvalidationRecord;

typedef struct XrInvalidationResult {
    XrStableId root_id;
    XrFingerprint root_old_fingerprint;
    XrFingerprint root_new_fingerprint;
    XrInvalidationRecord *records;
    size_t record_count;
} XrInvalidationResult;

XR_FUNC bool xr_cache_invalidate_apply(XrDependencyGraph *graph,
                                       const XrInvalidationEvent *event,
                                       XrInvalidationResult *out_result);
XR_FUNC void xr_invalidation_result_finalize(XrInvalidationResult *result);
XR_FUNC const XrInvalidationRecord *
xr_invalidation_result_find(const XrInvalidationResult *result, XrStableId module_id);
XR_FUNC const XrInvalidationRecord *
xr_invalidation_result_at(const XrInvalidationResult *result, size_t index);

#endif  // XR_CACHE_INVALIDATE_H
