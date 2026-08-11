/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_summary.h - Deterministic module summary identity and facets
 *
 * KEY CONCEPT:
 *   A summary owns its canonical module key and contains only stable IDs and
 *   opaque fingerprints. It never retains compiler graph or plan pointers.
 */

#ifndef XR_MODULE_SUMMARY_H
#define XR_MODULE_SUMMARY_H

#include "../base/xdefs.h"
#include "../plan/semantic/xr_semantic_ids.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XrModuleSummaryFacet {
    XR_MODULE_FACET_BODY_EVIDENCE = 0,
    XR_MODULE_FACET_PUBLIC_SIGNATURE,
    XR_MODULE_FACET_PUBLIC_TYPE,
    XR_MODULE_FACET_LAYOUT,
    XR_MODULE_FACET_EFFECT,
    XR_MODULE_FACET_OWNERSHIP,
    XR_MODULE_FACET_CAPABILITY,
    XR_MODULE_FACET_GENERIC,
    XR_MODULE_FACET_DEBUG_MAPPING,
    XR_MODULE_FACET_TARGET,
    XR_MODULE_FACET_PROVIDER,
    XR_MODULE_FACET_RUNTIME_ABI,
    XR_MODULE_FACET_COMPILER,
    XR_MODULE_FACET_SCHEMA,
    XR_MODULE_FACET_CONFIG,
    XR_MODULE_FACET_COUNT
} XrModuleSummaryFacet;

typedef uint64_t XrModuleFacetMask;

#define XR_MODULE_FACET_BIT(facet) (UINT64_C(1) << (unsigned) (facet))
#define XR_MODULE_FACET_ALL ((UINT64_C(1) << XR_MODULE_FACET_COUNT) - UINT64_C(1))

typedef struct XrModuleSummary {
    XrStableId module_id;
    char *canonical_key;
    XrFingerprint facets[XR_MODULE_FACET_COUNT];
    XrModuleFacetMask present_facets;
} XrModuleSummary;

XR_FUNC bool xr_module_summary_init(XrModuleSummary *summary, const char *canonical_key);
XR_FUNC bool xr_module_summary_copy(XrModuleSummary *out, const XrModuleSummary *source);
XR_FUNC void xr_module_summary_finalize(XrModuleSummary *summary);
XR_FUNC bool xr_module_summary_set_fingerprint(XrModuleSummary *summary,
                                               XrModuleSummaryFacet facet,
                                               XrFingerprint fingerprint);
XR_FUNC bool xr_module_summary_get_fingerprint(const XrModuleSummary *summary,
                                               XrModuleSummaryFacet facet,
                                               XrFingerprint *out);
XR_FUNC XrModuleFacetMask xr_module_summary_changed_facets(const XrModuleSummary *old_summary,
                                                           const XrModuleSummary *new_summary);
XR_FUNC bool xr_module_summary_fingerprint(const XrModuleSummary *summary,
                                           XrFingerprint *out);
XR_FUNC bool xr_module_summary_validate(const XrModuleSummary *summary);

#endif  // XR_MODULE_SUMMARY_H
