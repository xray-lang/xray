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
#include "xxir_metadata_domain_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_receive_control.h"
#include "xxir_types.h"
#include "xxir_view_types_internal.h"
#include "xxir_constraints.h"
#include "xxir_constraint_proof.h"
#include "xxir_internal.h"
#include "../base/xmalloc.h"

#include "xxir_result_binders.inc.c"

bool xr_xir_type_in_context(const XrXirModule *module, uint32_t function, XrXirType type) {
    if (!module || function >= module->function_count) return false;
    if (type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO || type == XR_XIR_BUFFER_STORAGE) return true;
    uint32_t count = module->generics ? module->generics[function].parameter_count : 0;
    const XrXirTypeNode *node = xr_xir_type_node(module->types, type);
    if (node) return (node->kind == XR_XIR_TYPE_CALLABLE || node->kind == XR_XIR_TYPE_ARRAY ||
        node->kind == XR_XIR_TYPE_MAP || node->kind == XR_XIR_TYPE_SET ||
        node->kind == XR_XIR_TYPE_CELL || node->kind == XR_XIR_TYPE_NOMINAL ||
        node->kind == XR_XIR_TYPE_NULLABLE || node->kind == XR_XIR_TYPE_TUPLE || node->kind == XR_XIR_TYPE_ATOMIC ||
        node->kind == XR_XIR_TYPE_TASK || node->kind == XR_XIR_TYPE_SLICE ||
        (node->kind == XR_XIR_TYPE_MUT_SLICE || node->kind == XR_XIR_TYPE_ITERATOR || node->kind == XR_XIR_TYPE_CHANNEL)) && node->parameter_span <= count;
    return (uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type < XR_XIR_TYPE_PARAMETER_LIMIT &&
        (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE < count;
}
XrXirStatus xr_xir_compile_type_constraints(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, XrXirType type, XrXirConstraint constraints) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!(remaining)) return XR_XIR_BUDGET;

    if (!module || function >= module->function_count || constraints.interface_count || constraints.interfaces ||
        constraints.markers & ~XR_XIR_CONSTRAINT_MASK ||
        xr_xir_type_is_cell(module->types, type)) return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *expression=xr_xir_type_node(module->types,type);
    bool error_metadata=type==XR_XIR_NEVER || (expression && expression->kind==XR_XIR_TYPE_UNION);
    if (error_metadata ? !(constraints.markers & XR_XIR_CONSTRAINT_ERROR) :
        !xr_xir_type_in_context(module, function, type)) return XR_XIR_BAD_TYPE;
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_FUNCTION,function,0}};
    uint32_t borrowed=0;
    XrXirStatus status=xr_xir_compile_view_type_flags(remaining,module->types,type,&borrowed);
    if (status!=XR_XIR_OK) return status;
    if (borrowed) return XR_XIR_BAD_TYPE;
    return xr_xir_compile_type_markers_prove(remaining, &context, type, constraints.markers);
}
XrXirStatus xr_xir_compile_type_satisfies(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, XrXirType type, XrXirConstraint constraints) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    XrXirStatus status = xr_xir_compile_type_constraints(remaining, module, function, type, constraints);
    return status == XR_XIR_OK ? xr_xir_compile_type_access(remaining, module, function, type) : status;
}
XrXirStatus xr_xir_compile_generics_structure_verify(const XrXirCompileContext *compile_context, const XrXirModule *module) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!module) return XR_XIR_BAD_STRUCTURE;
    if (!module->generics) return XR_XIR_OK;
    if (module->stage == XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    uint64_t table_bytes = (uint64_t) module->function_count * sizeof(*module->generics);
    if (table_bytes > SIZE_MAX) return XR_XIR_BUDGET;

    bool templates = false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirGeneric *g = &module->generics[f];
        if (g->parameter_count > 65536 || g->parameter_count > remaining->limits.parameters) return XR_XIR_BUDGET;
        remaining->limits.parameters -= g->parameter_count;
        uint64_t bytes = (uint64_t) g->parameter_count * sizeof(*g->constraints) +
            (uint64_t) g->argument_count * sizeof(*g->arguments) +
            (g->parameter_kinds ? (uint64_t)g->parameter_count * sizeof(*g->parameter_kinds) : 0);
        if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

        if ((g->parameter_count != 0) != (g->constraints != NULL) ||
            (g->argument_count != 0) != (g->arguments != NULL)) return XR_XIR_BAD_STRUCTURE;
        templates |= g->parameter_count != 0;
        bool result_binder = false;
        if (g->parameter_kinds && !g->parameter_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < g->parameter_count; ++p) {
            uint32_t kind = xr_xir_binder_kind(g,p);
            if (kind > XR_XIR_BINDER_RESULT_VARIABLE) return XR_XIR_BAD_STRUCTURE;
            result_binder |= kind == XR_XIR_BINDER_RESULT_VARIABLE;
            XrXirStatus status = xr_xir_compile_constraint_structure(remaining, module->types, g->constraints[p], g->parameter_count);
            if (status != XR_XIR_OK) return status;
        }
        if (g->parameter_kinds && !result_binder) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a = 0; a < g->argument_count; ++a) {
            if (g->arguments[a] == XR_XIR_UNIT) {
                XrXirStatus status = xr_xir_compile_result_unit_use(remaining, module, f, a);
                if (status != XR_XIR_OK) return status;
            }
            if (xr_xir_type_is_cell(module->types,g->arguments[a])) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xr_xir_compile_type_expression_shape(remaining, module->types, g->arguments[a], g->parameter_count);
            if (status != XR_XIR_OK) return status;
            uint32_t borrowed=0;
            status=xr_xir_compile_view_type_flags(remaining,module->types,g->arguments[a],&borrowed);
            if (status!=XR_XIR_OK) return status;
            if (borrowed) return XR_XIR_BAD_TYPE;
        }
    }
    return templates ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
void xr_xir_compile_generics_free(XrXirGeneric *generics, uint32_t functions) {
    if (!generics) return;
    for (uint32_t f = 0; f < functions; ++f) {
        xr_xir_compile_constraint_array_free((XrXirConstraint *)generics[f].constraints, generics[f].parameter_count);
        xr_compile_resources_free((void *) generics[f].arguments);
        xr_compile_resources_free((void *) generics[f].parameter_kinds);
    }
    xr_compile_resources_free(generics);
}
XrXirStatus xr_xir_compile_generics_clone(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirGeneric **output, XrCompileRequestControl *control) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;

    if (!module || !output || *output) return XR_XIR_BAD_STRUCTURE;
    allocation_status=xir_receive_poll(control);
    if (allocation_status!=XR_XIR_OK) return allocation_status;
    if (!module->generics) { *output = NULL; return XR_XIR_OK; }

    XrXirGeneric *copy = xir_type_copy_zero(compile_context, module->function_count, sizeof(*copy), &allocation_status,control);
    if (!copy) return allocation_status;
    uint32_t progress=0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        allocation_status=xir_receive_step(control,&progress);
        if (allocation_status!=XR_XIR_OK) break;
        const XrXirGeneric *from = &module->generics[f];
        copy[f].parameter_count = from->parameter_count; copy[f].argument_count = from->argument_count;
        XrXirConstraint *constraints = NULL;
        XrXirStatus status = xr_xir_compile_constraint_array_copy_verified(compile_context, from->constraints, from->parameter_count, control, &constraints);
        copy[f].constraints = constraints;
        /* Constraint copying has its own failure status. Propagate it before
         * sibling copies can allocate through the still-successful status. */
        if (status != XR_XIR_OK) {
            xr_xir_compile_generics_free(copy, module->function_count);
            return status;
        }
        XrXirType *arguments = xir_type_copy_bytes(compile_context, from->arguments, from->argument_count, sizeof(*arguments), &allocation_status,control);
        copy[f].arguments = arguments;
        uint32_t *kinds = from->parameter_kinds ? xir_type_copy_bytes(compile_context, from->parameter_kinds, from->parameter_count, sizeof(*kinds), &allocation_status,control) : NULL;
        copy[f].parameter_kinds = kinds;
        if (status != XR_XIR_OK || allocation_status != XR_XIR_OK || (from->argument_count && !arguments) || (from->parameter_kinds && !kinds)) {
            xr_xir_compile_generics_free(copy, module->function_count);
            return status != XR_XIR_OK ? status : allocation_status;
        }


    }
    if (allocation_status==XR_XIR_OK) allocation_status=xir_receive_poll(control);
    if (allocation_status!=XR_XIR_OK) {xr_xir_compile_generics_free(copy,module->function_count);return allocation_status;}
    *output = copy; return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_generic_call(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t caller, const XrXirInstruction *call) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!module->generics) return call->type_arguments[0] || call->type_arguments[1] ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
    const XrXirGeneric *from = &module->generics[caller], *to = &module->generics[call->immediate];
    uint32_t first = call->type_arguments[0], count = call->type_arguments[1];
    if (count != to->parameter_count || first > from->argument_count ||
        count > from->argument_count - first || (!count && first)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < count; ++a) {
        XrXirType argument = from->arguments[first+a];
        XrXirStatus status = xr_xir_binder_kind(to,a) == XR_XIR_BINDER_RESULT_VARIABLE ?
            xr_xir_compile_result_argument(remaining, module, caller, argument) :
            xr_xir_compile_type_satisfies(remaining, module, caller, argument,
                (XrXirConstraint){.markers=to->constraints[a].markers & XR_XIR_CONSTRAINT_ERROR});
        if (xr_xir_binder_kind(to,a) == XR_XIR_BINDER_TYPE && result_symbol(from,argument)) return XR_XIR_BAD_TYPE;
        if (status != XR_XIR_OK) return status;
        XrXirProofContext context = {module, {XR_XIR_CONTEXT_FUNCTION,caller,0}};
        XrXirConstraintUse use = {module,{XR_XIR_CONTEXT_FUNCTION,(uint32_t)call->immediate,0}, a,
            from->arguments + first, count};
        status = xr_xir_compile_constraints_prove(remaining, &context, &use);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_metadata_call_type_matches(const XrXirCompileContext *compile_context,
    const XirMetadataDomain *domain,uint32_t caller,const XrXirInstruction *call,
    XrXirType type,XrXirType actual) {
    if (!xir_compile_context_valid(compile_context) || !domain || !call ||
        caller>=domain->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state=*compile_context;
    uint32_t count=call->type_arguments[1],first=call->type_arguments[0];
    const XrXirGeneric *generic=domain->generics ? &domain->generics[caller] : NULL;
    if (count && (!generic || !generic->arguments || first>generic->argument_count ||
        count>generic->argument_count-first)) return XR_XIR_BAD_STRUCTURE;
    const XrXirType *arguments=count ? generic->arguments+first : NULL;
    return xr_xir_compile_type_substitution_matches(&compile_state,domain->types,arguments,count,type,actual);
}
XR_FUNC XrXirStatus xr_xir_compile_call_type_matches(const XrXirCompileContext *compile_context,
    const XrXirModule *module,uint32_t caller,const XrXirInstruction *call,
    XrXirType type,XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XirMetadataDomain domain=xir_metadata_domain_from_module(module);
    return xir_metadata_call_type_matches(compile_context,module ? &domain : NULL,caller,call,type,actual);
}
