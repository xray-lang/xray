/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_slice_facts.inc.c - Independent receiving storage for loan summaries
 */
struct XrLirSliceFacts { XrLirSliceFactsView view; };

XR_FUNC void xr_lir_slice_facts_destroy(XrLirSliceFacts *facts) {
    if (!facts) return;
    xr_free((void *)facts->view.functions); xr_free((void *)facts->view.terms);
    xr_free((void *)facts->view.callback_loans); xr_free(facts);
}
static bool lir_slice_span_valid(XrLirSliceEffectSpan span, size_t count) {
    return span.first <= count && span.count <= count - span.first;
}
static bool lir_slice_effects_valid(const XrLirModule *m, XrLirSliceFactsView view) {
    if (view.function_count != m->function_count || !view.functions ||
        (view.term_count && !view.terms) || (view.callback_loan_count && !view.callback_loans)) return false;
    for (size_t n = 0; n < view.term_count; ++n) {
        XrLirSliceEffect term = view.terms[n];
        if ((uint32_t)term.kind > XR_LIR_SLICE_EFFECT_PARAMETER ||
            (term.kind == XR_LIR_SLICE_EFFECT_SLOT && term.identity >= m->state_layout.slot_count)) return false;
    }
    for (uint32_t n = 0; n < view.function_count; ++n) {
        const XrLirSliceFunctionFact *row = &view.functions[n + 1];
        if (!row->function) continue;
        const XrLirFunctionData *f = &m->functions[n];
        if (row->function != n + 1 || row->parameter_count != f->parameter_count ||
            !lir_slice_span_valid(row->access, view.term_count) ||
            !lir_slice_span_valid(row->write, view.term_count) ||
            !lir_slice_span_valid(row->result_origin, view.term_count) ||
            !lir_slice_span_valid(row->callback_loans, view.callback_loan_count)) return false;
        if (row->callback_loans.count && !row->available) return false;
        for (size_t p=0;p<row->callback_loans.count;++p) {
            XrLirSliceCallbackLoan relation=view.callback_loans[row->callback_loans.first+p];
            if (relation.callback_parameter>=f->parameter_count || relation.loan_parameter>=f->parameter_count ||
                relation.callback_parameter<f->closure.capture_count ||
                (relation.loan_kind!=XR_LIR_SLICE_EFFECT_PARAMETER && relation.loan_kind!=XR_LIR_SLICE_EFFECT_CAPTURE) ||
                (relation.loan_kind==XR_LIR_SLICE_EFFECT_CAPTURE)!=(relation.loan_parameter<f->closure.capture_count) ||
                !xr_lir_internal_slice_type(m,f->parameter_types[relation.loan_parameter])) return false;
            XrLirCallableType signature;
            if (xr_lir_callable_type_view(m,f->parameter_types[relation.callback_parameter],&signature)!=XR_LIR_OK ||
                (signature.flags&XR_LIR_CALLABLE_C_ABI))
                return false;
            for (uint32_t c=0;c<f->callable_parameter_count;++c)
                if (f->callable_parameters[c].parameter==relation.callback_parameter &&
                    f->callable_parameters[c].contract.escaping) return false;
        }
        if (row->result_parameter != UINT32_MAX && (row->result_parameter >= f->parameter_count ||
            !xr_lir_internal_slice_type(m, f->result) || f->parameter_types[row->result_parameter] != f->result)) return false;
        if (row->result_parameter != UINT32_MAX) {
            if (!row->result_origin.count) return false;
            for (size_t t = 0; t < row->result_origin.count; ++t) {
                XrLirSliceEffect origin = view.terms[row->result_origin.first + t];
                if (origin.kind != XR_LIR_SLICE_EFFECT_PARAMETER || origin.identity != row->result_parameter) return false;
            }
        }
        const XrLirSliceEffectSpan spans[] = {row->access,row->write,row->result_origin};
        for (size_t s = 0; s < sizeof(spans)/sizeof(*spans); ++s)
            for (size_t t = 0; t < spans[s].count; ++t) {
                XrLirSliceEffect term = view.terms[spans[s].first + t];
                if ((term.kind == XR_LIR_SLICE_EFFECT_PARAMETER || term.kind == XR_LIR_SLICE_EFFECT_CAPTURE ||
                    term.kind == XR_LIR_SLICE_EFFECT_CALLBACK) && term.identity >= f->parameter_count) return false;
            }
    }
    return true;
}
static XrLirStatus lir_slice_facts_copy(XrLirSliceFactsView view, XrLirSliceFacts **empty) {
    size_t count = (size_t)view.function_count + 1;
    if (count <= view.function_count || count > SIZE_MAX / sizeof(*view.functions) ||
        view.term_count > SIZE_MAX / sizeof(*view.terms) ||
        view.callback_loan_count > SIZE_MAX / sizeof(*view.callback_loans)) return XR_LIR_OUT_OF_MEMORY;
    XrLirSliceFacts *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_LIR_OUT_OF_MEMORY;
    copy->view = view;
    copy->view.functions = xr_malloc(count * sizeof(*view.functions));
    copy->view.terms = view.term_count ? xr_malloc(view.term_count * sizeof(*view.terms)) : NULL;
    copy->view.callback_loans=view.callback_loan_count ?
        xr_malloc(view.callback_loan_count*sizeof(*view.callback_loans)) : NULL;
    if (!copy->view.functions || (view.term_count && !copy->view.terms) ||
        (view.callback_loan_count && !copy->view.callback_loans)) {
        xr_lir_slice_facts_destroy(copy); return XR_LIR_OUT_OF_MEMORY;
    }
    memcpy((void *)copy->view.functions, view.functions, count * sizeof(*view.functions));
    if (view.term_count) memcpy((void *)copy->view.terms, view.terms, view.term_count * sizeof(*view.terms));
    if (view.callback_loan_count) memcpy((void *)copy->view.callback_loans,view.callback_loans,
        view.callback_loan_count*sizeof(*view.callback_loans));
    *empty = copy; return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_internal_slice_effects_set(XrLirModule *m, XrLirSliceFactsView view) {
    if (!m || m->finished || m->slice_facts || !lir_slice_effects_valid(m, view)) return XR_LIR_INVALID_ARGUMENT;
    if (m->failure != XR_LIR_OK) return m->failure;
    return lir_slice_facts_copy(view, &m->slice_facts);
}
XR_FUNC XrLirStatus xr_lir_slice_facts_create(const XrLirModule *m, XrLirSliceFacts **empty) {
    if (!xr_lir_module_is_readable(m) || !empty || *empty) return XR_LIR_INVALID_ARGUMENT;
    return m->slice_facts ? lir_slice_facts_copy(m->slice_facts->view, empty) : XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_slice_facts_view(const XrLirSliceFacts *facts, XrLirSliceFactsView *view) {
    if (!facts || !view) return XR_LIR_INVALID_ARGUMENT;
    *view = facts->view; return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_internal_slice_function_fact(const XrLirModule *m, uint32_t function,
    XrLirSliceFunctionFact *output) {
    if (!m || !function || function > m->function_count || !output) return XR_LIR_INVALID_ARGUMENT;
    if (!m->slice_facts || function > m->slice_facts->view.function_count ||
        !m->slice_facts->view.functions[function].function) return XR_LIR_INVALID_STATE;
    *output = m->slice_facts->view.functions[function]; return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_slice_function_fact(const XrLirModule *m, uint32_t function,
    XrLirSliceFunctionFact *output) {
    if (!xr_lir_module_is_readable(m)) return XR_LIR_INVALID_ARGUMENT;
    return xr_lir_internal_slice_function_fact(m,function,output);
}
XR_FUNC XrLirStatus xr_lir_internal_slice_effects_copy(XrLirModule *output, const XrLirModule *input,
    const XrLirFunction *functions, uint32_t count) {
    if (!input || !output || count != input->function_count || !functions) return XR_LIR_INVALID_ARGUMENT;
    if (!input->slice_facts) return XR_LIR_OK;
    XrLirSliceFactsView view = input->slice_facts->view;
    if (output->function_count != count) return XR_LIR_INVALID_ARGUMENT;
    size_t extent = (size_t)count + 1;
    if (extent <= count || extent > SIZE_MAX / sizeof(*view.functions)) return XR_LIR_OUT_OF_MEMORY;
    XrLirSliceFunctionFact *rows = xr_calloc(extent, sizeof(*rows));
    if (!rows) return XR_LIR_OUT_OF_MEMORY;
    XrLirStatus status = XR_LIR_OK;
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t n = index + 1;
        XrLirFunction target = functions[n];
        if (target.module != output || target.id != n) {
            status = XR_LIR_INVALID_ARGUMENT; break;
        }
        rows[target.id] = view.functions[n];
        if (rows[target.id].function) rows[target.id].function = target.id;
    }
    if (status == XR_LIR_OK) {
        view.functions = rows; status = xr_lir_internal_slice_effects_set(output, view);
    }
    xr_free(rows); return status;
}
