/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_provenance.inc.c - Independently owned specialization evidence
 *
 * KEY CONCEPT:
 *   Copying evidence preserves source lifetime, not its authority. Consumers
 *   must verify the complete output correspondence before using substitutions.
 */

XR_FUNC void xr_xir_compile_provenance_free(XrXirProvenance *provenance) {
    if (!provenance) return;
    for (uint32_t i = 0; provenance->origins && i < provenance->count; ++i) {
        xr_compile_resources_free((void *)provenance->origins[i].arguments);
        xr_compile_resources_free((void *)provenance->origins[i].effect_arguments);
    }
    for (uint32_t i = 0; provenance->contracts && i < provenance->contract_count; ++i) {
        const XrXirFunctionEffectContract *contract = &provenance->contracts[i];
        xr_compile_resources_free((void *)contract->parameters);
        xr_compile_resources_free((void *)contract->formula.terms);
        xr_compile_resources_free((void *)contract->values);
        xr_compile_resources_free((void *)contract->bindings);
    }
    xr_compile_resources_free(provenance->origins);
    xr_compile_resources_free(provenance->contracts);
    xr_compile_resources_free(provenance->bindings);
    xr_xir_compile_artifact_free(provenance->source);
    xr_compile_resources_free(provenance);
}

static XrXirStatus provenance_vector_shape(const XrXirCompileContext *context,
    const void *data, uint32_t count, size_t element_size) {
    if ((uint64_t)count > SIZE_MAX / element_size) return XR_XIR_BUDGET;
    if (!xir_compile_work(context, 3)) return XR_XIR_BUDGET;
    if (!!data != !!count) return XR_XIR_BAD_STRUCTURE;
    if (!count) return XR_XIR_OK;
    /* Every vector is copied into a new block. Check its physical lower bound
     * before inspecting a resource-impossible borrowed record array. */
    size_t bytes = (size_t)count * element_size;
    return xir_compile_resource_status(xr_compile_resources_admit(context->resources,bytes,bytes));
}

static XrXirStatus provenance_contract_storage_shape(const XrXirCompileContext *context,
    const XrXirFunctionEffectContract *contract) {
    XrXirStatus status = provenance_vector_shape(context, contract->parameters,
        contract->parameter_count, sizeof(*contract->parameters));
    if (status == XR_XIR_OK) status = provenance_vector_shape(context, contract->formula.terms,
        contract->formula.term_count, sizeof(*contract->formula.terms));
    if (status == XR_XIR_OK) status = provenance_vector_shape(context, contract->values,
        contract->value_count, sizeof(*contract->values));
    if (status == XR_XIR_OK) status = provenance_vector_shape(context, contract->bindings,
        contract->binding_count, sizeof(*contract->bindings));
    return status;
}

/* Storage shape is necessary for copying; it proves no formula or origin. */
static XrXirStatus provenance_storage_shape(const XrXirCompileContext *context,
    const XrXirProvenance *source) {
    if (!source) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, 8)) return XR_XIR_BUDGET;
    XrXirStatus status;
    if (source->kind == XR_XIR_EVIDENCE_TEMPLATE) {
        if (source->source || source->origins || source->count || source->bindings ||
            source->binding_count || !source->contract_count) return XR_XIR_BAD_STRUCTURE;
        status = provenance_vector_shape(context, source->contracts, source->contract_count,
            sizeof(*source->contracts));
        if (status != XR_XIR_OK) return status;
        for (uint32_t f = 0; f < source->contract_count; ++f) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            status = provenance_contract_storage_shape(context, &source->contracts[f]);
            if (status != XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    if (source->kind != XR_XIR_EVIDENCE_INSTANCE || !source->source ||
        source->contracts || source->contract_count || !source->count) return XR_XIR_BAD_STRUCTURE;
    status = provenance_vector_shape(context, source->origins, source->count,
        sizeof(*source->origins));
    if (status == XR_XIR_OK) status = provenance_vector_shape(context, source->bindings,
        source->binding_count, sizeof(*source->bindings));
    if (status != XR_XIR_OK) return status;
    const XrXirModule *module = &source->source->module;
    if (module->stage != XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    if (module->provenance && module->provenance->kind != XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < source->count; ++f) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirOrigin *origin = &source->origins[f];
        if (origin->function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
        status = provenance_vector_shape(context, origin->arguments, origin->argument_count,
            sizeof(*origin->arguments));
        if (status == XR_XIR_OK) status = provenance_vector_shape(context, origin->effect_arguments,
            origin->effect_argument_count, sizeof(*origin->effect_arguments));
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static bool provenance_contract_clone(const XrXirCompileContext *context,
    const XrXirFunctionEffectContract *from, XrXirFunctionEffectContract *to,
    XrXirStatus *allocation_status) {
    if (!xir_compile_work(context, sizeof(*from))) {
        *allocation_status = XR_XIR_BUDGET; return false;
    }
    to->parameter_count = from->parameter_count;
    to->formula.constant_mask = from->formula.constant_mask;
    to->formula.term_count = from->formula.term_count;
    to->value_count = from->value_count;
    to->binding_count = from->binding_count;
    to->parameters = copy_bytes(context, from->parameters,
        (size_t)from->parameter_count * sizeof(*from->parameters), allocation_status);
    if (from->parameter_count && !to->parameters) return false;
    to->formula.terms = copy_bytes(context, from->formula.terms,
        (size_t)from->formula.term_count * sizeof(*from->formula.terms), allocation_status);
    if (from->formula.term_count && !to->formula.terms) return false;
    to->values = copy_bytes(context, from->values,
        (size_t)from->value_count * sizeof(*from->values), allocation_status);
    if (from->value_count && !to->values) return false;
    to->bindings = copy_bytes(context, from->bindings,
        (size_t)from->binding_count * sizeof(*from->bindings), allocation_status);
    return !from->binding_count || to->bindings;
}

static bool provenance_origins_clone(const XrXirCompileContext *context,
    const XrXirProvenance *from, XrXirProvenance *to, XrXirStatus *allocation_status) {
    to->origins = xir_compile_calloc(context, from->count, sizeof(*to->origins), allocation_status);
    if (!to->origins) return false;
    to->count = from->count;
    for (uint32_t f = 0; f < from->count; ++f) {
        if (!xir_compile_work(context, sizeof(*from->origins))) {
            *allocation_status = XR_XIR_BUDGET; return false;
        }
        const XrXirOrigin *origin = &from->origins[f];
        XrXirOrigin *copy = &to->origins[f];
        copy->function = origin->function;
        copy->argument_count = origin->argument_count;
        copy->effect_argument_count = origin->effect_argument_count;
        copy->arguments = copy_bytes(context, origin->arguments,
            (size_t)origin->argument_count * sizeof(*origin->arguments), allocation_status);
        if (origin->argument_count && !copy->arguments) return false;
        copy->effect_arguments = copy_bytes(context, origin->effect_arguments,
            (size_t)origin->effect_argument_count * sizeof(*origin->effect_arguments), allocation_status);
        if (origin->effect_argument_count && !copy->effect_arguments) return false;
    }
    return true;
}

static XrXirProvenance *provenance_clone(const XrXirCompileContext *context,
    const XrXirProvenance *source, XrXirStatus *allocation_status) {
    *allocation_status = provenance_storage_shape(context, source);
    if (*allocation_status != XR_XIR_OK) return NULL;
    XrXirProvenance *copy = xir_compile_calloc(context, 1, sizeof(*copy), allocation_status);
    if (!copy) return NULL;
    if (!xir_compile_work(context, 1)) { *allocation_status = XR_XIR_BUDGET; goto failed; }
    copy->kind = source->kind;
    if (source->kind == XR_XIR_EVIDENCE_TEMPLATE) {
        copy->contracts = xir_compile_calloc(context, source->contract_count,
            sizeof(*copy->contracts), allocation_status);
        if (!copy->contracts) goto failed;
        copy->contract_count = source->contract_count;
        for (uint32_t f = 0; f < source->contract_count; ++f)
            if (!provenance_contract_clone(context, &source->contracts[f], &copy->contracts[f],
                allocation_status)) goto failed;
    } else {
        copy->source = clone_module(context, &source->source->module, source->source->construction, allocation_status);
        if (!copy->source || !provenance_origins_clone(context, source, copy, allocation_status)) goto failed;
        copy->bindings = copy_bytes(context, source->bindings,
            (size_t)source->binding_count * sizeof(*source->bindings), allocation_status);
        if (source->binding_count && !copy->bindings) goto failed;
        copy->binding_count = source->binding_count;
    }
    return copy;
failed:
    xr_xir_compile_provenance_free(copy);
    return NULL;
}

XR_FUNC XrXirStatus xr_xir_compile_provenance_copy(const XrXirCompileContext *context,
    const XrXirProvenance *source, XrXirProvenance **output) {
    if (!xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    if (!output || *output || !source) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = provenance_storage_shape(context, source);
    if (status != XR_XIR_OK) return status;
    if (source->kind == XR_XIR_EVIDENCE_INSTANCE) {
        status = xr_xir_compile_verify_v2(context, &source->source->module, source->source->construction, NULL);
        if (status != XR_XIR_OK) return status;
        for (uint32_t f = 0; f < source->count; ++f) {
            if (!xir_compile_work(context, 2)) return XR_XIR_BUDGET;
            uint32_t declaration = source->origins[f].function;
            uint32_t arity = source->source->module.generics ?
                source->source->module.generics[declaration].parameter_count : 0;
            if (source->origins[f].argument_count != arity) return XR_XIR_BAD_STRUCTURE;
        }
    }
    XrXirProvenance *copy = provenance_clone(context, source, &status);
    if (!copy) return status;
    *output = copy;
    return XR_XIR_OK;
}
