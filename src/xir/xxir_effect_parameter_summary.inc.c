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
static XrXirStatus effect_parameter_summary_copy(const XrXirCompileContext *work,
    const XrXirFunctionEffectContract *contracts, uint32_t count, EffectParameterSummary *output) {
    if (!xir_compile_context_valid(work) || !output || output->resources || output->count ||
        output->total || output->rows || output->values || !count || !contracts)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t total = 0;
    for (uint32_t f = 0; f < count; ++f) {
        if (!!contracts[f].parameters != !!contracts[f].parameter_count) return XR_XIR_BAD_STRUCTURE;
        total += contracts[f].parameter_count;
    }
    uint64_t row_bytes = (uint64_t)count * sizeof(*output->rows);
    if (total > UINT32_MAX) return XR_XIR_OVERFLOW;
    uint64_t value_bytes = total * sizeof(*output->values);
    if (row_bytes > SIZE_MAX || value_bytes > SIZE_MAX) return XR_XIR_OVERFLOW;
    XrXirStatus status = XR_XIR_OK;
    EffectParameterSummary summary = {.resources = work->resources, .count = count, .total = (uint32_t)total};
    summary.rows = xir_compile_alloc(work, (size_t)row_bytes, &status);
    summary.values = total ? xir_compile_alloc(work, (size_t)value_bytes, &status) : NULL;
    if (!summary.rows || (total && !summary.values)) goto failure;
    uint32_t first = 0;
    for (uint32_t f = 0; f < count; ++f) {
        const XrXirFunctionEffectContract *contract = &contracts[f];
        summary.rows[f] = (EffectParameterSummaryRow){first, contract->parameter_count};
        for (uint32_t p = 0; p < contract->parameter_count; ++p) {
            XrXirEffectParameter value = contract->parameters[p];
            if (!effect_parameter_summary_value_valid(value)) { status = XR_XIR_BAD_STRUCTURE; goto failure; }
            summary.values[first++] = value;
        }
    }
    XR_DCHECK(first == summary.total, "Every derived parameter has exactly one owned summary row");
    XR_DCHECK(summary.resources == work->resources, "The receiving summary uses its allocation owner");
    *output = summary;
    return XR_XIR_OK;

failure:
    effect_parameter_summary_clear(&summary);
    return status;
}

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
static XrXirStatus effect_parameter_summary_shape(const XrXirCompileContext *work,
    const EffectParameterSummary *summary, uint32_t count) {
    if (!xir_compile_context_valid(work) || !summary || summary->resources != work->resources ||
        !count || summary->count != count || !summary->rows || !!summary->values != !!summary->total)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t row_bytes = (uint64_t)summary->count * sizeof(*summary->rows);
    uint64_t value_bytes = (uint64_t)summary->total * sizeof(*summary->values);
    if (row_bytes > SIZE_MAX || value_bytes > SIZE_MAX ||
        xr_compile_resources_extent(work->resources, summary->rows, (size_t)row_bytes) != XR_COMPILE_RESOURCE_OK ||
        (value_bytes && xr_compile_resources_extent(work->resources, summary->values,
            (size_t)value_bytes) != XR_COMPILE_RESOURCE_OK)) return XR_XIR_BAD_STRUCTURE;
    uint64_t first = 0;
    for (uint32_t f = 0; f < count; ++f) {
        const EffectParameterSummaryRow *row = &summary->rows[f];
        if (row->first != first || row->count > summary->total - first) return XR_XIR_BAD_STRUCTURE;
        first += row->count;
    }
    if (first != summary->total) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < summary->total; ++p)
        if (!effect_parameter_summary_value_valid(summary->values[p])) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_summary_matches(const XrXirCompileContext *work,
    const EffectParameterSummary *summary, const XrXirFunctionEffectContract *contracts, uint32_t count) {
    XrXirStatus status = effect_parameter_summary_shape(work, summary, count);
    uint64_t bytes = (uint64_t)count * sizeof(*contracts);
    if (status != XR_XIR_OK) return status;
    if (!contracts || bytes > SIZE_MAX || xr_compile_resources_extent(work->resources, contracts,
        (size_t)bytes) != XR_COMPILE_RESOURCE_OK) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < count; ++f) {
        const XrXirFunctionEffectContract *contract = &contracts[f];
        const EffectParameterSummaryRow *row = &summary->rows[f];
        bytes = (uint64_t)row->count * sizeof(*contract->parameters);
        if (contract->parameter_count != row->count || !!contract->parameters != !!row->count ||
            bytes > SIZE_MAX || (bytes && xr_compile_resources_extent(work->resources,
                contract->parameters, (size_t)bytes) != XR_COMPILE_RESOURCE_OK)) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < row->count; ++p) {
            XrXirEffectParameter stored = summary->values[row->first + p], actual = contract->parameters[p];
            if (stored.kind != actual.kind || stored.uses != actual.uses) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
