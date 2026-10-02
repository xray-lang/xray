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

void xr_xir_compile_provenance_free(XrXirProvenance *provenance) {
    if (!provenance) return;
    for (uint32_t i = 0; i < provenance->count; ++i)
        xr_compile_resources_free((void *)provenance->origins[i].arguments);
    xr_compile_resources_free(provenance->origins);
    xr_xir_compile_artifact_free(provenance->source);
    xr_compile_resources_free(provenance);
}

XrXirStatus xr_xir_compile_provenance_copy(const XrXirCompileContext *compile_context, const XrXirModule *source, const XrXirOrigin *origins, uint32_t count, XrXirProvenance **output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!remaining || !source || !origins || !count) return XR_XIR_BAD_STRUCTURE;
    if (source->stage != XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    if (source->provenance) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext source_limits = *remaining;
    XrXirStatus status = xr_xir_compile_verify(remaining, source, NULL);
    if (status != XR_XIR_OK) return status;
    uint64_t bytes = sizeof(XrXirProvenance) + (uint64_t)count * sizeof(*origins);
    if (bytes > SIZE_MAX ||!xir_compile_work(remaining, count))
        return XR_XIR_BUDGET;


    for (uint32_t i = 0; i < count; ++i) {
        const XrXirOrigin *origin = &origins[i];
        if (origin->function >= source->function_count ||
            (!!origin->arguments != !!origin->argument_count)) return XR_XIR_BAD_STRUCTURE;
        uint32_t arity = source->generics ? source->generics[origin->function].parameter_count : 0;
        if (origin->argument_count != arity) return XR_XIR_BAD_STRUCTURE;
        bytes = (uint64_t)origin->argument_count * sizeof(*origin->arguments);
        if (bytes > SIZE_MAX ||!xir_compile_work(remaining, origin->argument_count))
            return XR_XIR_BUDGET;


    }
    XrXirProvenance *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    copy->origins = xir_compile_calloc(compile_context, count, sizeof(*copy->origins), &allocation_status);
    if (!copy->origins) { xr_xir_compile_provenance_free(copy); return allocation_status; }
    copy->count = count;
    copy->source = clone_module(compile_context, source, &allocation_status);
    if (!copy->source) { xr_xir_compile_provenance_free(copy); return allocation_status; }
    copy->source->context = source_limits;
    for (uint32_t i = 0; i < count; ++i) {
        copy->origins[i] = origins[i];
        copy->origins[i].arguments = copy_bytes(compile_context, origins[i].arguments,
            (size_t)origins[i].argument_count * sizeof(*origins[i].arguments), &allocation_status);
        if (origins[i].argument_count && !copy->origins[i].arguments) {
            xr_xir_compile_provenance_free(copy); return allocation_status;
        }
    }
    *output = copy;
    return XR_XIR_OK;
}
