/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_summary_build.h - Module summary derivation from verified facts
 *
 * KEY CONCEPT:
 *   A summary is derived only from authorities that were already verified by
 *   their owners. This boundary never re-infers semantics, target facts, or
 *   schema decisions; it maps exact fingerprints onto summary facets and the
 *   matching content-addressed cache identity.
 */

#ifndef XR_MODULE_SUMMARY_BUILD_H
#define XR_MODULE_SUMMARY_BUILD_H

#include "xr_cache_key.h"
#include "xr_dependency_graph.h"

#include <stdbool.h>
#include <stdint.h>

/* Exact identities one module summary is derived from. Every fingerprint is
 * produced and verified by the layer that owns the fact; a caller that cannot
 * supply an exact fingerprint must fail instead of substituting a default. */
typedef struct XrModuleSummaryFacts {
    XrFingerprint program_semantics; /* verified ProgramSemanticClosure identity */
    XrStableId generation;           /* exact GenerationClosureId */
    XrFingerprint semantics;     /* verified SemanticPlan identity */
    XrFingerprint dependencies;  /* ordered dependency module/fingerprint digest */
    XrFingerprint declarations;  /* ordered exported declaration identity digest */
    XrFingerprint target;        /* exact TargetProfile identity */
    XrFingerprint toolchain;     /* compiler build identity */
    XrFingerprint configuration; /* language and build configuration identity */
    uint32_t semantic_schema;    /* SemanticPlan schema version */
} XrModuleSummaryFacts;

/* Derives the summary and its semantic cache identity from one fact set, so a
 * published artifact and the summary that describes it can never disagree. */
XR_FUNC bool xr_module_summary_build(XrModuleSummary *out_summary, XrCacheKey *out_key,
                                     const char *canonical_key,
                                     const XrModuleSummaryFacts *facts);

/* Conservative observation relation: any change to any observed dependency
 * facet makes every consumer facet stale. Facet-precise relations require
 * facet-granular plan digests that the semantic layer does not publish yet, and
 * a narrower relation would under-invalidate rather than merely lose reuse. */
XR_FUNC void xr_module_summary_full_relation(XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]);

#endif  // XR_MODULE_SUMMARY_BUILD_H
