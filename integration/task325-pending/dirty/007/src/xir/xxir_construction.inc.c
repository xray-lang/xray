/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_construction.inc.c - Receiving-owner construction allocation and cloning
 *
 * KEY CONCEPT:
 *   A live immutable input may use another ledger; every receiving copy is charged.
 */
/* Dense immutable facts share the same receiving Compile owner as the graph. */
static XrXirStatus construction_rows_shape(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstructionRow *rows, uint32_t count) {
    if (!xir_compile_context_valid(context) || (!!rows != !!count)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    const XrXirNominalTable *table = types ? types->nominals : NULL;
    uint32_t expected = table && table->declarations ? table->count : 0;
    if (count != expected) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n = 0; n < count; ++n) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (rows[n].field_count != table->declarations[n].field_count ||
            (!!rows[n].field_initializers != !!rows[n].field_count)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC void xr_xir_compile_construction_free(XrXirConstruction *construction) {
    if (!construction) return;
    for (uint32_t n = 0; construction->rows && n < construction->count; ++n)
        xr_compile_resources_free((void *)construction->rows[n].field_initializers);
    xr_compile_resources_free(construction->rows);
    xr_compile_resources_free(construction->kinds);
    xr_compile_resources_free(construction);
}
XR_FUNC XrXirStatus xr_xir_compile_construction_new(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstructionRow *rows, uint32_t count,
    XrXirConstruction **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = construction_rows_shape(context, types, rows, count);
    if (status != XR_XIR_OK) return status;
    XrXirConstruction *owned = xir_compile_calloc(context, 1, sizeof(*owned), &status);
    if (!owned) return status;
    owned->context = *context;
    owned->rows = count ? xir_compile_calloc(context, count, sizeof(*owned->rows), &status) : NULL;
    owned->count = count;
    if (status == XR_XIR_OK && count)
        owned->kinds = xir_compile_calloc(context, count, sizeof(*owned->kinds), &status);
    if (status != XR_XIR_OK) goto failed;
    for (uint32_t n = 0; n < count; ++n) {
        if (!xir_compile_work(context, 2)) { status = XR_XIR_BUDGET; goto failed; }
        XrXirConstructionRow *to = &owned->rows[n];
        owned->kinds[n] = types->nominals->declarations[n].kind;
        to->default_initializer = rows[n].default_initializer;
        to->field_count = rows[n].field_count;
        uint64_t bytes = (uint64_t)to->field_count * sizeof(uint32_t);
        if (bytes > SIZE_MAX) { status = XR_XIR_BUDGET; goto failed; }
        if (bytes) {
            to->field_initializers = xir_compile_copy(context, rows[n].field_initializers,
                (size_t)bytes, &status);
            if (!to->field_initializers) goto failed;
        }
    }
    *output = owned;
    return XR_XIR_OK;
failed:
    xr_xir_compile_construction_free(owned);
    return status;
}
XR_FUNC uint32_t xr_xir_compile_construction_count(const XrXirConstruction *construction) {
    return construction ? construction->count : 0;
}
XR_FUNC const XrXirConstructionRow *xr_xir_compile_construction_row(
    const XrXirConstruction *construction, uint32_t ordinal) {
    return construction && ordinal < construction->count ? &construction->rows[ordinal] : NULL;
}
XR_FUNC const XrXirConstruction *xr_xir_compile_artifact_construction(const XrXirArtifact *artifact) {
    return artifact ? artifact->construction : NULL;
}
XR_FUNC XrXirStatus xir_construction_shape(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *construction) {
    if (!xir_compile_context_valid(context) || !construction ||
        !xir_compile_context_valid(&construction->context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = construction_rows_shape(context, types, construction->rows, construction->count);
    if (status != XR_XIR_OK) return status;
    if (!!construction->kinds != !!construction->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n = 0; n < construction->count; ++n) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (construction->kinds[n] != types->nominals->declarations[n].kind)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_construction_clone(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *source, XrXirConstruction **output) {
    XrXirStatus status = xir_construction_shape(context, types, source);
    return status == XR_XIR_OK ? xr_xir_compile_construction_new(context, types,
        source->rows, source->count, output) : status;
}
/* Only a projection may synthesize all-zero facts, before independent recheck.
 * Public admission never substitutes this for a missing owner. */
XR_FUNC XrXirStatus xir_construction_empty(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirConstruction **output) {
    if (!xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    const XrXirNominalTable *table = types ? types->nominals : NULL;
    uint32_t count = table && table->declarations ? table->count : 0;
    XrXirStatus status = XR_XIR_OK;
    XrXirConstruction *owned = xir_compile_calloc(context, 1, sizeof(*owned), &status);
    if (!owned) return status;
    owned->context = *context;
    owned->count = count;
    owned->rows = count ? xir_compile_calloc(context, count, sizeof(*owned->rows), &status) : NULL;
    if (status == XR_XIR_OK && count)
        owned->kinds = xir_compile_calloc(context, count, sizeof(*owned->kinds), &status);
    if (status != XR_XIR_OK) goto failed;
    for (uint32_t n = 0; n < count; ++n) {
        if (!xir_compile_work(context, 1)) { status = XR_XIR_BUDGET; goto failed; }
        XrXirConstructionRow *row = &owned->rows[n];
        owned->kinds[n] = table->declarations[n].kind;
        row->field_count = table->declarations[n].field_count;
        if (row->field_count) {
            row->field_initializers = xir_compile_calloc(context, row->field_count, sizeof(uint32_t), &status);
            if (!row->field_initializers) goto failed;
        }
    }
    *output = owned; return XR_XIR_OK;
failed:
    xr_xir_compile_construction_free(owned); return status;
}
