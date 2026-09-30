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
#include "xxir_types.h"
#include "xxir_constraints.h"
#include "xxir_constraint_proof.h"
#include "../base/xmalloc.h"

bool xr_xir_type_in_context(const XrXirModule *module, uint32_t function, XrXirType type) {
    if (!module || function >= module->function_count) return false;
    if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING || type == XR_XIR_ATOMIC_I64 ||
        type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO) return true;
    uint32_t count = module->generics ? module->generics[function].parameter_count : 0;
    const XrXirTypeNode *node = xr_xir_type_node(module->types, type);
    if (node) return (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY ||
        node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_NOMINAL) && node->parameter_span <= count;
    return (uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type < XR_XIR_TYPE_PARAMETER_LIMIT &&
        (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE < count;
}
XrXirStatus xr_xir_type_constraints(const XrXirModule *module, uint32_t function,
    XrXirType type, XrXirConstraint constraints, XrXirBudget *remaining) {
    if (!remaining || !remaining->work) return XR_XIR_BUDGET;
    --remaining->work;
    if (!module || constraints.interface_count || constraints.interfaces ||
        constraints.markers & ~XR_XIR_CONSTRAINT_MASK || !xr_xir_type_in_context(module, function, type) ||
        xr_xir_type_is_cell(module->types, type)) return XR_XIR_BAD_TYPE;
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_FUNCTION,function,0}};
    return xr_xir_type_markers_prove(&context,type,constraints.markers,remaining);
}
XrXirStatus xr_xir_type_satisfies(const XrXirModule *module, uint32_t function,
    XrXirType type, XrXirConstraint constraints, XrXirBudget *remaining) {
    XrXirStatus status = xr_xir_type_constraints(module, function, type, constraints, remaining);
    return status == XR_XIR_OK ? xr_xir_type_access(module, function, type, remaining) : status;
}
XrXirStatus xr_xir_generics_structure_verify(const XrXirModule *module, XrXirBudget *remaining) {
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
        for (uint32_t p = 0; p < g->parameter_count; ++p) {
            XrXirStatus status = xr_xir_constraint_structure(module->types, g->constraints[p], g->parameter_count, remaining);
            if (status != XR_XIR_OK) return status;
        }
        for (uint32_t a = 0; a < g->argument_count; ++a) {
            if (g->arguments[a] == XR_XIR_UNIT || xr_xir_type_is_cell(module->types,g->arguments[a])) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xr_xir_type_expression_shape(module->types,g->arguments[a],g->parameter_count,remaining);
            if (status != XR_XIR_OK) return status;
        }
    }
    return templates ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
void xr_xir_generics_free(XrXirGeneric *generics, uint32_t functions) {
    if (!generics) return;
    for (uint32_t f = 0; f < functions; ++f) {
        xr_xir_constraint_array_free((XrXirConstraint *)generics[f].constraints, generics[f].parameter_count);
        xr_free((void *) generics[f].arguments);
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
        XrXirConstraint *constraints = NULL;
        XrXirStatus status = xr_xir_constraint_array_copy_verified(from->constraints, from->parameter_count, &constraints);
        XrXirType *arguments = from->argument_count ? xr_malloc((size_t) from->argument_count * sizeof(*arguments)) : NULL;
        copy[f].constraints = constraints; copy[f].arguments = arguments;
        if (status != XR_XIR_OK || (from->argument_count && !arguments)) {
            xr_xir_generics_free(copy, module->function_count);
            return status != XR_XIR_OK ? status : XR_XIR_OUT_OF_MEMORY;
        }
        if (from->argument_count) memcpy(arguments, from->arguments, (size_t) from->argument_count * sizeof(*arguments));
    }
    *output = copy; return XR_XIR_OK;
}
XrXirStatus xr_xir_generic_call(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *call, XrXirBudget *remaining) {
    if (!module->generics) return call->type_arguments[0] || call->type_arguments[1] ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
    const XrXirGeneric *from = &module->generics[caller], *to = &module->generics[call->immediate];
    uint32_t first = call->type_arguments[0], count = call->type_arguments[1];
    if (count != to->parameter_count || first > from->argument_count ||
        count > from->argument_count - first || (!count && first)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < count; ++a) {
        XrXirStatus status = xr_xir_type_satisfies(module, caller, from->arguments[first + a], (XrXirConstraint){0}, remaining);
        if (status != XR_XIR_OK) return status;
        XrXirProofContext context = {module, {XR_XIR_CONTEXT_FUNCTION,caller,0}};
        XrXirConstraintUse use = {module,{XR_XIR_CONTEXT_FUNCTION,(uint32_t)call->immediate,0}, a,
            from->arguments + first, count};
        status = xr_xir_constraints_prove(&context, &use, remaining);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_call_type_matches(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *call, XrXirType type, XrXirType actual, XrXirBudget *remaining) {
    uint32_t count = call->type_arguments[1];
    const XrXirType *arguments = count ? module->generics[caller].arguments + call->type_arguments[0] : NULL;
    return xr_xir_type_substitution_matches(module->types, arguments, count, type, actual, remaining);
}
