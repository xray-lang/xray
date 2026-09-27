/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_provenance.inc.c - Independently owned specialization evidence
 *
 * KEY CONCEPT:
 *   Copying evidence preserves source lifetime, not its authority. Consumers
 *   must verify the complete output correspondence before using substitutions.
 */

void xr_xir_provenance_free(XrXirProvenance *provenance) {
    if (!provenance) return;
    for (uint32_t i = 0; i < provenance->count; ++i)
        xr_free((void *)provenance->origins[i].arguments);
    xr_free(provenance->origins);
    xr_xir_artifact_free(provenance->source);
    xr_free(provenance);
}

XrXirStatus xr_xir_provenance_copy(const XrXirModule *source,
    const XrXirOrigin *origins, uint32_t count, XrXirBudget *remaining,
    XrXirProvenance **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!remaining || !source || !origins || !count) return XR_XIR_BAD_STRUCTURE;
    if (source->stage != XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    if (source->provenance) return XR_XIR_BAD_STRUCTURE;
    XrXirBudget source_limits = *remaining;
    XrXirStatus status = xr_xir_verify_remaining(source, remaining, NULL);
    if (status != XR_XIR_OK) return status;
    uint64_t bytes = sizeof(XrXirProvenance) + (uint64_t)count * sizeof(*origins);
    if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || count > remaining->work)
        return XR_XIR_BUDGET;
    remaining->metadata_bytes -= bytes;
    remaining->work -= count;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirOrigin *origin = &origins[i];
        if (origin->function >= source->function_count ||
            (!!origin->arguments != !!origin->argument_count)) return XR_XIR_BAD_STRUCTURE;
        uint32_t arity = source->generics ? source->generics[origin->function].parameter_count : 0;
        if (origin->argument_count != arity) return XR_XIR_BAD_STRUCTURE;
        bytes = (uint64_t)origin->argument_count * sizeof(*origin->arguments);
        if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || origin->argument_count > remaining->work)
            return XR_XIR_BUDGET;
        remaining->metadata_bytes -= bytes;
        remaining->work -= origin->argument_count;
    }
    XrXirProvenance *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    copy->origins = xr_calloc(count, sizeof(*copy->origins));
    if (!copy->origins) { xr_xir_provenance_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->count = count;
    copy->source = clone_module(source);
    if (!copy->source) { xr_xir_provenance_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->source->budget = source_limits;
    for (uint32_t i = 0; i < count; ++i) {
        copy->origins[i] = origins[i];
        copy->origins[i].arguments = copy_bytes(origins[i].arguments,
            (size_t)origins[i].argument_count * sizeof(*origins[i].arguments));
        if (origins[i].argument_count && !copy->origins[i].arguments) {
            xr_xir_provenance_free(copy); return XR_XIR_OUT_OF_MEMORY;
        }
    }
    *output = copy;
    return XR_XIR_OK;
}
