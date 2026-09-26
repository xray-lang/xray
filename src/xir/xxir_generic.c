/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generic.c - Owned generic metadata and declaration-based entailment
 *
 * KEY CONCEPT:
 *   Unknown type arguments retain only the capabilities their declarations prove.
 */
#include "xxir_generic.h"
#include "xxir_callable.h"
#include "../base/xmalloc.h"

bool xr_xir_type_in_context(const XrXirModule *module, uint32_t function, XrXirType type) {
    if (type == XR_XIR_BOOL || type == XR_XIR_I64 || type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64) return true;
    if (xr_xir_callable_signature(module->callables, type)) return true;
    return module->generics && (uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE &&
        (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE < module->generics[function].parameter_count;
}
bool xr_xir_type_satisfies(const XrXirModule *module, uint32_t function, XrXirType type, uint32_t constraints) {
    if (constraints & ~XR_XIR_CONSTRAINT_SENDABLE || !xr_xir_type_in_context(module, function, type)) return false;
    if ((uint32_t) type < XR_XIR_TYPE_PARAMETER_BASE)
        return !xr_xir_callable_signature(module->callables, type) || !constraints;
    uint32_t declared = module->generics[function].constraints[(uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE];
    return (declared & constraints) == constraints;
}
XrXirStatus xr_xir_generics_verify(const XrXirModule *module, XrXirBudget *remaining) {
    if (!module->generics) return XR_XIR_OK;
    if (module->stage == XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    uint64_t table_bytes = (uint64_t) module->function_count * sizeof(*module->generics);
    if (table_bytes > SIZE_MAX || table_bytes > remaining->metadata_bytes) return XR_XIR_BUDGET;
    remaining->metadata_bytes -= table_bytes;
    bool templates = false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirGeneric *g = &module->generics[f];
        if (g->parameter_count > 65536 || g->parameter_count > remaining->parameters) return XR_XIR_BUDGET;
        remaining->parameters -= g->parameter_count;
        uint64_t count = (uint64_t) g->parameter_count + g->argument_count;
        uint64_t bytes = (uint64_t) g->parameter_count * sizeof(*g->constraints) +
            (uint64_t) g->argument_count * sizeof(*g->arguments);
        if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes || count + 1 > remaining->work) return XR_XIR_BUDGET;
        remaining->metadata_bytes -= bytes; remaining->work -= count + 1;
        if ((g->parameter_count != 0) != (g->constraints != NULL) ||
            (g->argument_count != 0) != (g->arguments != NULL)) return XR_XIR_BAD_STRUCTURE;
        templates |= g->parameter_count != 0;
        for (uint32_t p = 0; p < g->parameter_count; ++p)
            if (g->constraints[p] & ~XR_XIR_CONSTRAINT_SENDABLE) return XR_XIR_BAD_TYPE;
        for (uint32_t a = 0; a < g->argument_count; ++a)
            if (!xr_xir_type_in_context(module, f, g->arguments[a])) return XR_XIR_BAD_TYPE;
    }
    return templates ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
void xr_xir_generics_free(XrXirGeneric *generics, uint32_t functions) {
    if (!generics) return;
    for (uint32_t f = 0; f < functions; ++f) {
        xr_free((void *) generics[f].constraints); xr_free((void *) generics[f].arguments);
    }
    xr_free(generics);
}
XrXirStatus xr_xir_generics_clone(const XrXirModule *module, XrXirGeneric **output) {
    *output = NULL;
    if (!module->generics) return XR_XIR_OK;
    XrXirGeneric *copy = xr_calloc(module->function_count, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirGeneric *from = &module->generics[f];
        copy[f].parameter_count = from->parameter_count; copy[f].argument_count = from->argument_count;
        uint32_t *constraints = from->parameter_count ? xr_malloc((size_t) from->parameter_count * sizeof(*constraints)) : NULL;
        XrXirType *arguments = from->argument_count ? xr_malloc((size_t) from->argument_count * sizeof(*arguments)) : NULL;
        copy[f].constraints = constraints; copy[f].arguments = arguments;
        if ((from->parameter_count && !constraints) || (from->argument_count && !arguments)) {
            xr_xir_generics_free(copy, module->function_count); return XR_XIR_OUT_OF_MEMORY;
        }
        if (from->parameter_count) memcpy(constraints, from->constraints, (size_t) from->parameter_count * sizeof(*constraints));
        if (from->argument_count) memcpy(arguments, from->arguments, (size_t) from->argument_count * sizeof(*arguments));
    }
    *output = copy; return XR_XIR_OK;
}
XrXirStatus xr_xir_generic_call(const XrXirModule *module, uint32_t caller, const XrXirInstruction *call) {
    if (!module->generics) return call->targets[0] || call->targets[1] ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
    const XrXirGeneric *from = &module->generics[caller], *to = &module->generics[call->immediate];
    uint32_t first = call->targets[0], count = call->targets[1];
    if (count != to->parameter_count || first > from->argument_count ||
        count > from->argument_count - first || (!count && first)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < count; ++a)
        if (!xr_xir_type_satisfies(module, caller, from->arguments[first + a], to->constraints[a])) return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
XrXirType xr_xir_call_type(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *call, XrXirType type) {
    if ((uint32_t) type < XR_XIR_TYPE_PARAMETER_BASE) return type;
    uint32_t index = (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE;
    if (!module->generics || index >= call->targets[1]) return (XrXirType) UINT32_MAX;
    return module->generics[caller].arguments[call->targets[0] + index];
}
