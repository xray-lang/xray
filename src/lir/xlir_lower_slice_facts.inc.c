/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_slice_facts.inc.c - Relocate the completed Artifact's loan summary
 */
static bool lower_slice_effect_kind(XirModuleEffectTermKind source, XrLirSliceEffectKind *output) {
    switch (source) {
    case XIR_MODULE_EFFECT_SLOT: *output=XR_LIR_SLICE_EFFECT_SLOT; return true;
    case XIR_MODULE_EFFECT_CALLBACK: *output=XR_LIR_SLICE_EFFECT_CALLBACK; return true;
    case XIR_MODULE_EFFECT_CAPTURE: *output=XR_LIR_SLICE_EFFECT_CAPTURE; return true;
    case XIR_MODULE_EFFECT_UNKNOWN: *output=XR_LIR_SLICE_EFFECT_UNKNOWN; return true;
    case XIR_MODULE_EFFECT_FIELD: *output=XR_LIR_SLICE_EFFECT_FIELD; return true;
    case XIR_MODULE_EFFECT_PARAMETER: *output=XR_LIR_SLICE_EFFECT_PARAMETER; return true;
    default: return false;
    }
}
static bool lower_slice_effect_span(Lower *c, XirModuleEffectExportView source,
    XirModuleEffectSpan span, XrLirSliceEffectSpan *output, XrLirSliceEffect *terms, size_t *used) {
    if (span.first > source.term_count || span.count > source.term_count - span.first)
        return lower_unsupported(c,"Slice effect span has no completed source");
    output->first = span.count ? *used : 0; output->count = span.count;
    for (size_t n = 0; n < span.count; ++n) {
        XirModuleEffectTerm from = source.terms[span.first + n]; XrLirSliceEffect term;
        if (!lower_slice_effect_kind(from.kind,&term.kind))
            return lower_unsupported(c,"Slice effect lost its symbolic kind");
        term.identity = from.identity;
        if (term.kind == XR_LIR_SLICE_EFFECT_SLOT) {
            if (from.identity >= source.slot_count || !c->state_slots || c->state_slots[from.identity] == UINT32_MAX ||
                source.slots[from.identity].module != c->source->declarations->slots[from.identity].module)
                return lower_unsupported(c,"Slice effect slot has no receiving storage");
            term.identity = c->state_slots[from.identity];
        }
        terms[(*used)++] = term;
    }
    return true;
}
static bool lower_slice_effects_receive(Lower *c, XirModuleEffectExportView source) {
    if (source.function_count != c->source->function_count || source.slot_count != c->source->declarations->slot_count)
        return lower_unsupported(c,"Slice effects differ from the lowered Artifact");
    size_t extent = (size_t)c->module->function_count + 1, count = 0;
    if (extent <= c->module->function_count) return lower_unsupported(c,"Slice function summary extent");
    for (uint32_t n = 0; n < source.function_count; ++n) if (c->active[n]) {
        const XirModuleEffectFunction *row = &source.functions[n];
        const XirModuleEffectSpan spans[] = {row->access,row->write,row->result_origin};
        for (size_t s = 0; s < sizeof(spans)/sizeof(*spans); ++s) {
            if (spans[s].count > SIZE_MAX - count) return lower_unsupported(c,"Slice summary term extent");
            count += spans[s].count;
        }
    }
    XrLirSliceFunctionFact *rows = lower_alloc(c,extent,sizeof(*rows));
    XrLirSliceEffect *terms = count ? lower_alloc(c,count,sizeof(*terms)) : NULL;
    if (!rows || (count && !terms)) { xr_free(rows); xr_free(terms); return false; }
    size_t used = 0; bool good = true;
    for (uint32_t n = 0; n < source.function_count && good; ++n) if (c->active[n]) {
        c->function = n; const XrXirFunction *function = &c->source->functions[n];
        const XirModuleEffectFunction *from = &source.functions[n];
        uint32_t target = c->functions[n].id;
        if (!target || from->parameter_count != function->parameter_count ||
            from->module != c->source->declarations->functions[n].module ||
            from->capture_count != c->source->declarations->functions[n].capture_count) {
            good = lower_unsupported(c,"Slice summary parameter relocation differs"); break;
        }
        XrLirSliceFunctionFact row = {.function=target,
            .parameter_count=c->module->functions[target - 1].parameter_count,
            .result_parameter=UINT32_MAX,.available=source.module_access_complete};
        good = lower_slice_effect_span(c,source,from->access,&row.access,terms,&used) &&
            lower_slice_effect_span(c,source,from->write,&row.write,terms,&used) &&
            lower_slice_effect_span(c,source,from->result_origin,&row.result_origin,terms,&used);
        if (good && xr_lir_internal_slice_type(c->module,lower_type(c,function->result))) {
            /* Exported parameter identities already include physical captures.
             * Hidden State is appended by Lower and never shifts these ids. */
            for (size_t t = 0; t < row.result_origin.count && good; ++t) {
                XrLirSliceEffect origin = terms[row.result_origin.first + t];
                if (origin.kind != XR_LIR_SLICE_EFFECT_PARAMETER || origin.identity < from->capture_count ||
                    origin.identity >= function->parameter_count ||
                    lower_type(c,function->parameters[origin.identity]) != lower_type(c,function->result) ||
                    (row.result_parameter != UINT32_MAX && row.result_parameter != origin.identity))
                    good = lower_unsupported(c,"Slice result has no unique relocated borrowed input");
                else row.result_parameter = origin.identity;
            }
            if (good && (!row.available || row.result_parameter == UINT32_MAX))
                good = lower_unsupported(c,"Slice result origin is unavailable");
        }
        rows[target] = row;
    }
    if (good) good = used == count && lower_build(c,xr_lir_internal_slice_effects_set(c->module,
        (XrLirSliceFactsView){.functions=rows,.terms=terms,.function_count=c->module->function_count,.term_count=used}));
    xr_free(rows); xr_free(terms); return good;
}
static bool lower_slice_effects(Lower *c) {
    if (!c->views) return true;
    XirModuleEffectExport *owned = NULL;
    XrXirStatus status = xir_effects_module_export_copy(c->request->closed,&owned);
    bool good = false;
    if (status == XR_XIR_OK) {
        XirModuleEffectExportView view = {0};
        status = xir_effects_module_export_view(owned,&view);
        if (status == XR_XIR_OK) good = lower_slice_effects_receive(c,view);
    }
    xir_effects_module_export_free(owned);
    if (status != XR_XIR_OK) lower_error(c,status == XR_XIR_OUT_OF_MEMORY ? XR_LIR_OUT_OF_MEMORY : XR_LIR_UNSUPPORTED,
        "Slice calls require completed module WRITE and ORIGIN facts");
    return good;
}
