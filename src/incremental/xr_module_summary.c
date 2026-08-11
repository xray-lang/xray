/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_summary.c - Deterministic module summary identity and facets
 */

#include "xr_module_summary.h"

#include "../base/xmalloc.h"

#include <string.h>

static bool facet_is_valid(XrModuleSummaryFacet facet) {
    return (unsigned) facet < XR_MODULE_FACET_COUNT;
}

bool xr_module_summary_init(XrModuleSummary *summary, const char *canonical_key) {
    if (!summary || !canonical_key || canonical_key[0] == '\0')
        return false;

    memset(summary, 0, sizeof(*summary));
    size_t key_size = strlen(canonical_key) + 1u;
    char *owned_key = (char *) xr_malloc(key_size);
    if (!owned_key)
        return false;
    memcpy(owned_key, canonical_key, key_size);

    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(canonical_key, &summary->module_id, &key_digest)) {
        xr_free(owned_key);
        return false;
    }
    summary->canonical_key = owned_key;
    return true;
}

bool xr_module_summary_copy(XrModuleSummary *out, const XrModuleSummary *source) {
    if (!out || !xr_module_summary_validate(source))
        return false;
    if (!xr_module_summary_init(out, source->canonical_key))
        return false;
    memcpy(out->facets, source->facets, sizeof(out->facets));
    out->present_facets = source->present_facets;
    return true;
}

void xr_module_summary_finalize(XrModuleSummary *summary) {
    if (!summary)
        return;
    xr_free(summary->canonical_key);
    memset(summary, 0, sizeof(*summary));
}

bool xr_module_summary_set_fingerprint(XrModuleSummary *summary, XrModuleSummaryFacet facet,
                                       XrFingerprint fingerprint) {
    if (!summary || !summary->canonical_key || !facet_is_valid(facet))
        return false;
    summary->facets[facet] = fingerprint;
    summary->present_facets |= XR_MODULE_FACET_BIT(facet);
    return true;
}

bool xr_module_summary_get_fingerprint(const XrModuleSummary *summary, XrModuleSummaryFacet facet,
                                       XrFingerprint *out) {
    if (!summary || !out || !facet_is_valid(facet) ||
        !(summary->present_facets & XR_MODULE_FACET_BIT(facet))) {
        return false;
    }
    *out = summary->facets[facet];
    return true;
}

XrModuleFacetMask xr_module_summary_changed_facets(const XrModuleSummary *old_summary,
                                                   const XrModuleSummary *new_summary) {
    if (!xr_module_summary_validate(old_summary) || !xr_module_summary_validate(new_summary) ||
        !xr_stable_id_equal(old_summary->module_id, new_summary->module_id)) {
        return XR_MODULE_FACET_ALL;
    }

    XrModuleFacetMask changed = old_summary->present_facets ^ new_summary->present_facets;
    XrModuleFacetMask shared = old_summary->present_facets & new_summary->present_facets;
    for (unsigned i = 0; i < XR_MODULE_FACET_COUNT; i++) {
        XrModuleFacetMask bit = XR_MODULE_FACET_BIT(i);
        if ((shared & bit) &&
            !xr_fingerprint_equal(old_summary->facets[i], new_summary->facets[i])) {
            changed |= bit;
        }
    }
    return changed;
}

bool xr_module_summary_validate(const XrModuleSummary *summary) {
    if (!summary || !summary->canonical_key || summary->canonical_key[0] == '\0' ||
        (summary->present_facets & ~XR_MODULE_FACET_ALL) != 0) {
        return false;
    }
    XrStableId expected_id;
    XrFingerprint key_digest;
    return xr_stable_id_from_key(summary->canonical_key, &expected_id, &key_digest) &&
           xr_stable_id_equal(summary->module_id, expected_id);
}
