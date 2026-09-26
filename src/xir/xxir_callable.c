/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_callable.c - Bounded structural callable signature admission
 *
 * KEY CONCEPT:
 *   Earlier type references form a finite graph; exact contracts have one identity.
 */
#include "xxir_callable.h"
#include "../base/xmalloc.h"

const XrXirCallableSignature *xr_xir_callable_signature(const XrXirCallableTypes *types, XrXirType type) {
    uint32_t id = (uint32_t) type;
    return types && types->signatures && id >= XR_XIR_CALLABLE_TYPE_BASE && id < XR_XIR_TYPE_PARAMETER_BASE &&
        id - XR_XIR_CALLABLE_TYPE_BASE < types->count ? &types->signatures[id - XR_XIR_CALLABLE_TYPE_BASE] : NULL;
}
static bool callable_component(XrXirType type, uint32_t earlier) {
    return type == XR_XIR_BOOL || type == XR_XIR_I64 || type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 ||
        ((uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE < 65536) ||
        ((uint32_t) type >= XR_XIR_CALLABLE_TYPE_BASE && (uint32_t) type - XR_XIR_CALLABLE_TYPE_BASE < earlier);
}
uint32_t xr_xir_callable_span(const XrXirCallableTypes *types, XrXirType type) {
    if ((uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE) return (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE + 1;
    const XrXirCallableSignature *s = xr_xir_callable_signature(types, type);
    return s ? s->parameter_span : 0;
}
XrXirStatus xr_xir_callable_types_verify(const XrXirCallableTypes *types, XrXirBudget *remaining) {
    if (!types) return XR_XIR_OK;
    if (!remaining || !types->count || !types->signatures) return XR_XIR_BAD_STRUCTURE;
    if (types->count > XR_XIR_TYPE_PARAMETER_BASE - XR_XIR_CALLABLE_TYPE_BASE) return XR_XIR_BUDGET;
    uint64_t bytes = sizeof(*types) + (uint64_t) types->count * sizeof(*types->signatures);
    if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || types->count > remaining->work) return XR_XIR_BUDGET;
    remaining->metadata_bytes -= bytes; remaining->work -= types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        const XrXirCallableSignature *s = &types->signatures[i];
        if (s->parameter_count > 65536 || s->parameter_count > remaining->parameters) return XR_XIR_BUDGET;
        bytes = (uint64_t) s->parameter_count * sizeof(*s->parameters);
        if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || s->parameter_count > remaining->work) return XR_XIR_BUDGET;
        remaining->metadata_bytes -= bytes; remaining->work -= s->parameter_count;
        remaining->parameters -= s->parameter_count;
        if ((s->parameter_count != 0) != (s->parameters != NULL)) return XR_XIR_BAD_STRUCTURE;
        if (s->flags || (s->result != XR_XIR_UNIT && !callable_component(s->result, i))) return XR_XIR_BAD_TYPE;
        for (uint32_t p = 0; p < s->parameter_count; ++p)
            if (s->parameters[p].mode || !callable_component(s->parameters[p].type, i)) return XR_XIR_BAD_TYPE;
        uint32_t span = xr_xir_callable_span(types, s->result);
        for (uint32_t p = 0; p < s->parameter_count; ++p) {
            uint32_t component = xr_xir_callable_span(types, s->parameters[p].type);
            if (component > span) span = component;
        }
        if (s->parameter_span != span) return XR_XIR_BAD_TYPE;
        for (uint32_t j = 0; j < i; ++j) {
            if (!remaining->work) return XR_XIR_BUDGET;
            --remaining->work;
            const XrXirCallableSignature *previous = &types->signatures[j];
            if (previous->parameter_count != s->parameter_count || previous->result != s->result || previous->flags != s->flags) continue;
            if (s->parameter_count > remaining->work) return XR_XIR_BUDGET;
            remaining->work -= s->parameter_count;
            bool same = true;
            for (uint32_t p = 0; p < s->parameter_count; ++p)
                if (s->parameters[p].type != previous->parameters[p].type || s->parameters[p].mode != previous->parameters[p].mode) same = false;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
void xr_xir_callable_types_free(XrXirCallableTypes *types) {
    if (!types) return;
    if (types->signatures)
        for (uint32_t i = 0; i < types->count; ++i) xr_free((void *) types->signatures[i].parameters);
    xr_free((void *) types->signatures); xr_free(types);
}
XrXirStatus xr_xir_callable_types_clone(const XrXirCallableTypes *types, XrXirCallableTypes **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!types) return XR_XIR_OK;
    XrXirCallableTypes *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    XrXirCallableSignature *signatures = xr_calloc(types->count, sizeof(*signatures));
    if (!signatures) { xr_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->signatures = signatures; copy->count = types->count;
    for (uint32_t i = 0; i < types->count; ++i) {
        signatures[i] = types->signatures[i];
        uint32_t count = signatures[i].parameter_count;
        XrXirCallableParameter *parameters = count ? xr_malloc((size_t) count * sizeof(*parameters)) : NULL;
        signatures[i].parameters = parameters;
        if (count && !parameters) { xr_xir_callable_types_free(copy); return XR_XIR_OUT_OF_MEMORY; }
        if (count) memcpy(parameters, types->signatures[i].parameters, (size_t) count * sizeof(*parameters));
    }
    *output = copy; return XR_XIR_OK;
}

XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value) {
    if (value < function->parameter_count) return function->parameters[value];
    value -= function->parameter_count;
    return value < function->instruction_count ? function->instructions[value].type : XR_XIR_UNIT;
}
