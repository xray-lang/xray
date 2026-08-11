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
#include "../base/xsha256.h"

#include <string.h>

static bool facet_is_valid(XrModuleSummaryFacet facet) {
    return (unsigned) facet < XR_MODULE_FACET_COUNT;
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
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

bool xr_module_summary_fingerprint(const XrModuleSummary *summary, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-module-summary-v1\0";
    if (!out || !xr_module_summary_validate(summary))
        return false;

    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, summary->module_id.bytes, sizeof(summary->module_id.bytes));
    size_t key_size = strlen(summary->canonical_key);
    hash_u64(&ctx, (uint64_t) key_size);
    xr_sha256_update(&ctx, (const uint8_t *) summary->canonical_key, key_size);
    hash_u64(&ctx, summary->present_facets);
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
        XrModuleFacetMask bit = XR_MODULE_FACET_BIT(facet);
        if ((summary->present_facets & bit) == 0)
            continue;
        hash_u64(&ctx, facet);
        xr_sha256_update(&ctx, summary->facets[facet].bytes,
                         sizeof(summary->facets[facet].bytes));
    }
    xr_sha256_final(&ctx, out->bytes);
    return true;
}

bool xr_module_summary_validate(const XrModuleSummary *summary) {
    if (!summary || !summary->canonical_key || summary->canonical_key[0] == '\0' ||
        (summary->present_facets & ~XR_MODULE_FACET_ALL) != 0) {
        return false;
    }
    static const XrFingerprint zero = {{0}};
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
        if ((summary->present_facets & XR_MODULE_FACET_BIT(facet)) == 0 &&
            !xr_fingerprint_equal(summary->facets[facet], zero)) {
            return false;
        }
    }

    XrStableId expected_id;
    XrFingerprint key_digest;
    return xr_stable_id_from_key(summary->canonical_key, &expected_id, &key_digest) &&
           xr_stable_id_equal(summary->module_id, expected_id);
}
