/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_parameter_summary.inc.c - Owned callable parameter summaries
 *
 * KEY CONCEPT:
 *   The direct SSA collector is the sole role producer. Formula construction and
 *   published readers retain only these numeric results, never SSA proofs.
 */

typedef struct EffectParameterSummaryRow { uint32_t first, count; } EffectParameterSummaryRow;

typedef struct EffectParameterSummary {
    XrCompileResources *resources;
    uint32_t count, total;
    EffectParameterSummaryRow *rows;
    XrXirEffectParameter *values;
} EffectParameterSummary;

static void effect_parameter_summary_clear(EffectParameterSummary *summary) {
    xr_compile_resources_free(summary->rows);
    xr_compile_resources_free(summary->values);
    *summary = (EffectParameterSummary){0};
}

static bool effect_parameter_summary_value_valid(XrXirEffectParameter value) {
    return value.kind <= XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC &&
        !(value.uses & ~(XR_XIR_EFFECT_USE_INVOKE | XR_XIR_EFFECT_USE_FORWARD |
            XR_XIR_EFFECT_USE_COPY | XR_XIR_EFFECT_USE_FIXED | XR_XIR_EFFECT_USE_CONST_CAPTURE |
            XR_XIR_EFFECT_USE_STORE | XR_XIR_EFFECT_USE_RETURN | XR_XIR_EFFECT_USE_CHILD |
            XR_XIR_EFFECT_USE_OTHER));
}

/* Both allocations are private until every row has been copied. A failure
 * preserves the caller's empty output and releases all provisional storage. */

/* The opaque result owner has already admitted these arrays. Scalar reads
 * need no function body, descriptor pool, allocation or reconstruction. */
static XrXirStatus effect_parameter_summary_get(const EffectParameterSummary *summary,
    uint32_t function, uint32_t parameter, uint32_t *output) {
    if (!summary || !summary->resources ||
        !output || function >= summary->count || !summary->rows) return XR_XIR_BAD_STRUCTURE;
    const EffectParameterSummaryRow *row = &summary->rows[function];
    if (parameter >= row->count || row->first > summary->total ||
        row->count > summary->total - row->first || !summary->values) return XR_XIR_BAD_STRUCTURE;
    XrXirEffectParameter value = summary->values[row->first + parameter];
    if (!effect_parameter_summary_value_valid(value)) return XR_XIR_BAD_STRUCTURE;
    *output = value.kind;
    return XR_XIR_OK;
}

/* Raw receiving checks validate owned storage and correspondence to the
 * freshly derived contracts. They never replay a second role analysis. */
