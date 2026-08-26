/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_summary_build.c - Module summary derivation from verified facts
 */

#include "xr_module_summary_build.h"

#include <string.h>

/* The semantic layer publishes one fingerprint for a whole verified plan and
 * no per-facet digest. Every module-local facet therefore carries that same
 * identity: a body edit invalidates more than it strictly must, which loses
 * reuse but can never let a stale consumer facet survive a real change. */
static const XrModuleSummaryFacet MODULE_LOCAL_FACETS[] = {
    XR_MODULE_FACET_BODY_EVIDENCE, XR_MODULE_FACET_PUBLIC_SIGNATURE,
    XR_MODULE_FACET_PUBLIC_TYPE,   XR_MODULE_FACET_LAYOUT,
    XR_MODULE_FACET_EFFECT,        XR_MODULE_FACET_OWNERSHIP,
    XR_MODULE_FACET_CAPABILITY,    XR_MODULE_FACET_GENERIC,
    XR_MODULE_FACET_DEBUG_MAPPING,
};

/* Provider and runtime-ABI identities are owned by the TargetProfile, so the
 * profile fingerprint is their exact authority rather than an approximation. */
static const XrModuleSummaryFacet TARGET_OWNED_FACETS[] = {
    XR_MODULE_FACET_TARGET,
    XR_MODULE_FACET_PROVIDER,
    XR_MODULE_FACET_RUNTIME_ABI,
};

static void copy_cache_fingerprint(XrFingerprint source, XrCacheFingerprint *target) {
    _Static_assert(sizeof(source.bytes) == sizeof(target->bytes),
                   "cache and semantic fingerprints must share one width");
    memcpy(target->bytes, source.bytes, sizeof(source.bytes));
}

static void schema_fingerprint(uint32_t semantic_schema, XrFingerprint *out) {
    uint8_t descriptor[8];
    descriptor[0] = (uint8_t) (semantic_schema & 0xFFu);
    descriptor[1] = (uint8_t) ((semantic_schema >> 8) & 0xFFu);
    descriptor[2] = (uint8_t) ((semantic_schema >> 16) & 0xFFu);
    descriptor[3] = (uint8_t) ((semantic_schema >> 24) & 0xFFu);
    descriptor[4] = (uint8_t) (XR_SEMANTIC_SCHEMA_VERSION & 0xFFu);
    descriptor[5] = (uint8_t) ((XR_SEMANTIC_SCHEMA_VERSION >> 8) & 0xFFu);
    descriptor[6] = (uint8_t) ((XR_SEMANTIC_SCHEMA_VERSION >> 16) & 0xFFu);
    descriptor[7] = (uint8_t) ((XR_SEMANTIC_SCHEMA_VERSION >> 24) & 0xFFu);
    xr_semantic_fingerprint(descriptor, sizeof(descriptor), out);
}

static bool set_facets(XrModuleSummary *summary, const XrModuleSummaryFacet *facets,
                       size_t count, XrFingerprint fingerprint) {
    for (size_t i = 0; i < count; i++) {
        if (!xr_module_summary_set_fingerprint(summary, facets[i], fingerprint))
            return false;
    }
    return true;
}

bool xr_module_summary_build(XrModuleSummary *out_summary, XrCacheKey *out_key,
                             const char *canonical_key, const XrModuleSummaryFacts *facts) {
    if (!out_summary || !out_key || !canonical_key || canonical_key[0] == '\0' || !facts)
        return false;

    memset(out_key, 0, sizeof(*out_key));
    if (!xr_module_summary_init(out_summary, canonical_key))
        return false;

    XrFingerprint schema;
    schema_fingerprint(facts->semantic_schema, &schema);

    if (!set_facets(out_summary, MODULE_LOCAL_FACETS,
                    sizeof(MODULE_LOCAL_FACETS) / sizeof(MODULE_LOCAL_FACETS[0]),
                    facts->semantics) ||
        !set_facets(out_summary, TARGET_OWNED_FACETS,
                    sizeof(TARGET_OWNED_FACETS) / sizeof(TARGET_OWNED_FACETS[0]),
                    facts->target) ||
        !xr_module_summary_set_fingerprint(out_summary, XR_MODULE_FACET_COMPILER,
                                           facts->toolchain) ||
        !xr_module_summary_set_fingerprint(out_summary, XR_MODULE_FACET_SCHEMA, schema) ||
        !xr_module_summary_set_fingerprint(out_summary, XR_MODULE_FACET_CONFIG,
                                           facts->configuration) ||
        !xr_module_summary_validate(out_summary)) {
        xr_module_summary_finalize(out_summary);
        return false;
    }

    /* Every summary sets every facet, so an edge relation can never observe a
     * facet whose absence would silently stop invalidation propagation. */
    if (out_summary->present_facets != XR_MODULE_FACET_ALL) {
        xr_module_summary_finalize(out_summary);
        return false;
    }

    XrSemanticCacheKeyInput input;
    memset(&input, 0, sizeof(input));
    copy_cache_fingerprint(facts->semantics, &input.normalized_source);
    copy_cache_fingerprint(facts->program_semantics,
                           &input.program_semantic_closure);
    xr_cache_fingerprint_bytes(facts->generation.bytes,
                               sizeof(facts->generation.bytes),
                               &input.generation_closure);
    copy_cache_fingerprint(facts->toolchain, &input.compiler);
    copy_cache_fingerprint(schema, &input.semantic_schema);
    copy_cache_fingerprint(facts->target, &input.contract);
    copy_cache_fingerprint(facts->configuration, &input.language_configuration);
    copy_cache_fingerprint(facts->dependencies, &input.semantic_dependencies);
    copy_cache_fingerprint(facts->declarations, &input.declaration_identities);
    xr_cache_key_semantic(&input, out_key);
    return true;
}

void xr_module_summary_full_relation(XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    if (!relation)
        return;
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
        relation[facet] = XR_MODULE_FACET_ALL;
}
