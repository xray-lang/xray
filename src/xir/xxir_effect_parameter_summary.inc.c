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

/* Both allocations are private until every row has been copied. A failure
 * preserves the caller's empty output and releases all provisional storage. */

/* The opaque result owner has already admitted these arrays. Scalar reads
 * need no function body, descriptor pool, allocation or reconstruction. */

/* Raw receiving checks validate owned storage and correspondence to the
 * freshly derived contracts. They never replay a second role analysis. */
