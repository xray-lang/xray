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
    XR_INVALIDATION_MODULE_RESOLUTION_CHANGED,
    XR_INVALIDATION_DEPENDENCY
} XrInvalidationReason;

#define XR_INVALIDATION_MAX_DELTA_ROWS 4096u
#define XR_INVALIDATION_MAX_EVIDENCE_ROWS \
    (XR_DEPENDENCY_GRAPH_MAX_EDGES * XR_MODULE_FACET_COUNT)

typedef struct XrDependencyGraphDeltaRow {
    XrStableId consumer;
    XrStableId dependency;
    /* An all-zero relation represents an absent edge. */
    XrModuleFacetMask old_relation[XR_MODULE_FACET_COUNT];
    XrModuleFacetMask new_relation[XR_MODULE_FACET_COUNT];
} XrDependencyGraphDeltaRow;

typedef struct XrDependencyGraphDelta {
    const XrDependencyGraphDeltaRow *rows;
    size_t row_count;
} XrDependencyGraphDelta;

typedef struct XrModuleResolutionChange {
    /* Canonical rows for exactly root_id's outgoing dependency set. */
    const XrDependencyGraphDelta *delta;
    /* Exact consumer facets derived from delta; mismatches fail closed. */
    XrModuleFacetMask changed_facets;
    /* Compare-and-swap authority for the old and replacement module set. */
    XrFingerprint old_fingerprint;
    XrFingerprint new_fingerprint;
} XrModuleResolutionChange;

typedef struct XrInvalidationEvent {
    XrInvalidationReason reason;
    XrStableId root_id;
    /* Copied during apply; never retained by the graph or result. */
    const XrModuleSummary *replacement_summary;
    /* Required only for MODULE_RESOLUTION_CHANGED. */
    const XrModuleResolutionChange *module_resolution;
} XrInvalidationEvent;

typedef struct XrInvalidationEvidence {
    XrStableId module_id;
    XrStableId parent_module_id;
    /* Exactly one dependency facet bit. */
    XrModuleFacetMask observed_facet;
    XrModuleFacetMask invalidated_facets;
} XrInvalidationEvidence;

typedef struct XrInvalidationRecord {
    XrStableId module_id;
    XrModuleFacetMask invalidated_facets;
    XrModuleFacetMask observed_facets;
    XrInvalidationReason direct_reason;
    size_t evidence_start;
    size_t evidence_count;
} XrInvalidationRecord;

typedef struct XrInvalidationResult {
    XrStableId root_id;
    XrFingerprint root_old_fingerprint;
    XrFingerprint root_new_fingerprint;
    XrInvalidationRecord *records;
    size_t record_count;
    XrInvalidationEvidence *evidence;
    size_t evidence_count;
} XrInvalidationResult;

typedef struct XrInvalidationExplanationStep {
    XrStableId module_id;
    XrStableId parent_module_id;
    /* Both masks contain exactly one facet bit for this selected path. */
    XrModuleFacetMask invalidated_facet;
    XrModuleFacetMask observed_facet;
} XrInvalidationExplanationStep;

typedef struct XrInvalidationExplanation {
    XrStableId root_id;
    XrStableId subject_id;
    XrFingerprint root_old_fingerprint;
    XrFingerprint root_new_fingerprint;
    XrModuleFacetMask subject_facet;
    XrInvalidationReason root_reason;
    /* Ordered from the affected subject back to the direct root change. */
    XrInvalidationExplanationStep *steps;
    size_t step_count;
} XrInvalidationExplanation;

XR_FUNC bool xr_cache_invalidate_apply(XrDependencyGraph *graph,
                                       const XrInvalidationEvent *event,
                                       XrInvalidationResult *out_result);
XR_FUNC bool xr_cache_invalidation_verify(const XrDependencyGraph *before,
                                          const XrInvalidationEvent *event,
                                          const XrDependencyGraph *after,
                                          const XrInvalidationResult *result);
XR_FUNC void xr_invalidation_result_finalize(XrInvalidationResult *result);
XR_FUNC const XrInvalidationRecord *
xr_invalidation_result_find(const XrInvalidationResult *result, XrStableId module_id);
XR_FUNC const XrInvalidationRecord *
xr_invalidation_result_at(const XrInvalidationResult *result, size_t index);
XR_FUNC const XrInvalidationEvidence *
xr_invalidation_evidence_at(const XrInvalidationResult *result, size_t index);
XR_FUNC bool xr_invalidation_explain(const XrInvalidationResult *result,
                                     XrStableId subject_id,
                                     XrModuleFacetMask subject_facet,
                                     XrInvalidationExplanation *out);
XR_FUNC void xr_invalidation_explanation_finalize(
    XrInvalidationExplanation *explanation);

#endif  // XR_CACHE_INVALIDATE_H
