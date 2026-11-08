/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_source_summary.inc.c - Numeric reference effects between refinement rounds
 *
 * KEY CONCEPT:
 *   A completed derivation publishes one immutable reference mask per site.
 *   Source type refinement consumes that round without rebuilding its producer.
 */

enum { EFFECT_SOURCE_REFERENCE_PRESENT = 128 };
_Static_assert(((XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED) &
    ~UINT32_C(127)) == 0, "Reference masks fit below the presence bit");

typedef struct EffectSourceSummaryRow { uint32_t first, count; } EffectSourceSummaryRow;

typedef struct EffectSourceSummary {
    XrCompileResources *resources;
    uint32_t count, total;
    EffectSourceSummaryRow *rows;
    uint8_t *masks;
} EffectSourceSummary;

static void effect_source_summary_clear(EffectSourceSummary *summary) {
    xr_compile_resources_free(summary->rows);
    xr_compile_resources_free(summary->masks);
    *summary = (EffectSourceSummary){0};
}

/* Only nongeneric declaration contexts participate in Source inference.
 * Generic definitions keep their declared bounds until specialization. */
static XrXirStatus effect_source_summary_begin(const XrXirCompileContext *work,
    const XrXirModule *module, EffectSourceSummary *output) {
    if (!xir_compile_context_valid(work) || !module || !module->function_count || !module->functions ||
        !output || output->resources || output->rows || output->masks || output->count || output->total)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t total = 0, bytes = (uint64_t)module->function_count * sizeof(*output->rows);
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (!module->generics || !module->generics[f].parameter_count)
            total += module->functions[f].instruction_count;
    if (total > UINT32_MAX || bytes > SIZE_MAX || total > SIZE_MAX) return XR_XIR_OVERFLOW;
    XrXirStatus status = XR_XIR_OK;
    EffectSourceSummary summary = {.resources = work->resources,
        .count = module->function_count, .total = (uint32_t)total};
    summary.rows = xir_compile_alloc(work, (size_t)bytes, &status);
    summary.masks = total ? xir_compile_calloc(work, (size_t)total, sizeof(*summary.masks), &status) : NULL;
    if (!summary.rows || (total && !summary.masks)) {
        effect_source_summary_clear(&summary);
        return status;
    }
    uint32_t first = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        uint32_t count = module->generics && module->generics[f].parameter_count ?
            0 : module->functions[f].instruction_count;
        summary.rows[f] = (EffectSourceSummaryRow){first, count};
        first += count;
    }
    XR_DCHECK(first == summary.total, "Each nongeneric instruction has one numeric result slot");
    *output = summary;
    return XR_XIR_OK;
}

/* A private constructor owns both arrays for the complete read round. No
 * source pointer, allocator callback or type descriptor enters this read. */
static XrXirStatus effect_source_summary_read(const EffectSourceSummary *summary,
    uint32_t function, uint32_t instruction, uint32_t *output) {
    if (!summary || !summary->resources || !output || function >= summary->count || !summary->rows)
        return XR_XIR_BAD_STRUCTURE;
    EffectSourceSummaryRow row = summary->rows[function];
    if (instruction >= row.count || row.first > summary->total ||
        row.count > summary->total - row.first || !summary->masks) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask = summary->masks[row.first + instruction];
    if (!(mask & EFFECT_SOURCE_REFERENCE_PRESENT) ||
        (mask & ~(EFFECT_SOURCE_REFERENCE_PRESENT | XR_XIR_CALLABLE_ROOT_REQUIRED |
            XR_XIR_CALLABLE_ROOT_UNRESOLVED))) return XR_XIR_BAD_STRUCTURE;
    *output = mask & ~EFFECT_SOURCE_REFERENCE_PRESENT;
    return XR_XIR_OK;
}
