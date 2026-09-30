/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraints.c - Owned constraint arrays and allocation rollback
 */
#include "xxir_constraints.h"
#include "xxir_types.h"
#include "../base/xmalloc.h"
#include <string.h>

void xr_xir_interface_applications_free(XrXirInterfaceApplication *applications, uint32_t count) {
    if (!applications) return;
    for (uint32_t i = 0; i < count; ++i) xr_free((void *)applications[i].arguments);
    xr_free(applications);
}
XrXirStatus xr_xir_interface_applications_copy_verified(
    const XrXirInterfaceApplication *source, uint32_t count, XrXirInterfaceApplication **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!!source != !!count) return XR_XIR_BAD_STRUCTURE;
    if (!count) return XR_XIR_OK;
    if ((uint64_t)count * sizeof(*source) > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirInterfaceApplication *copy = xr_calloc(count, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirInterfaceApplication *from = &source[i];
        if (!!from->arguments != !!from->argument_count) { status = XR_XIR_BAD_STRUCTURE; break; }
        uint64_t bytes = (uint64_t)from->argument_count * sizeof(*from->arguments);
        if (bytes > SIZE_MAX) { status = XR_XIR_BUDGET; break; }
        XrXirType *arguments = bytes ? xr_malloc((size_t)bytes) : NULL;
        if (bytes && !arguments) { status = XR_XIR_OUT_OF_MEMORY; break; }
        if (bytes) memcpy(arguments, from->arguments, (size_t)bytes);
        copy[i] = (XrXirInterfaceApplication){from->declaration, arguments, from->argument_count};
    }
    if (status != XR_XIR_OK) { xr_xir_interface_applications_free(copy, count); return status; }
    *output = copy; return XR_XIR_OK;
}
void xr_xir_constraint_array_free(XrXirConstraint *constraints, uint32_t count) {
    if (!constraints) return;
    for (uint32_t i = 0; i < count; ++i)
        xr_xir_interface_applications_free((XrXirInterfaceApplication *)constraints[i].interfaces,
            constraints[i].interface_count);
    xr_free(constraints);
}
XrXirStatus xr_xir_constraint_array_copy_verified(
    const XrXirConstraint *source, uint32_t count, XrXirConstraint **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!!source != !!count) return XR_XIR_BAD_STRUCTURE;
    if (!count) return XR_XIR_OK;
    if ((uint64_t)count * sizeof(*source) > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirConstraint *copy = xr_calloc(count, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirInterfaceApplication *applications = NULL;
        XrXirStatus status = xr_xir_interface_applications_copy_verified(source[i].interfaces,
            source[i].interface_count, &applications);
        if (status != XR_XIR_OK) { xr_xir_constraint_array_free(copy, count); return status; }
        copy[i] = (XrXirConstraint){source[i].markers, applications, source[i].interface_count};
    }
    *output = copy; return XR_XIR_OK;
}
static XrXirStatus constraint_record_name(XrXirLiteral from, XrXirLiteral to, XrXirBudget *budget) {
    uint64_t work = from.length == to.length ? (uint64_t)from.length + 1 : 1;
    if (work > budget->work) return XR_XIR_BUDGET;
    budget->work -= work;
    if (!from.bytes || !to.bytes || !from.length || !to.length) return XR_XIR_BAD_STRUCTURE;
    return from.length == to.length && !memcmp(from.bytes,to.bytes,from.length) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
static XrXirStatus constraint_record_identity(const XrXirTypes *from_types,
    const XrXirInterfaceApplication *from, const XrXirTypes *to_types,
    const XrXirInterfaceApplication *to, XrXirBudget *budget) {
    if (!from_types || !to_types || !from_types->interfaces || !to_types->interfaces ||
        !from_types->interfaces->declarations || !to_types->interfaces->declarations ||
        from->declaration >= from_types->interfaces->count || to->declaration >= to_types->interfaces->count ||
        (!!from->arguments != !!from->argument_count) || (!!to->arguments != !!to->argument_count))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *a = &from_types->interfaces->declarations[from->declaration];
    const XrXirInterfaceDeclaration *b = &to_types->interfaces->declarations[to->declaration];
    if (a->parameter_count != b->parameter_count || from->argument_count != a->parameter_count ||
        to->argument_count != b->parameter_count) return XR_XIR_BAD_TYPE;
    XrXirStatus status = constraint_record_name(a->module,b->module,budget);
    return status == XR_XIR_OK ? constraint_record_name(a->name,b->name,budget) : status;
}
XrXirStatus xr_xir_constraint_records_match(const XrXirTypes *from_types,
    XrXirConstraint from, const XrXirTypes *to_types, XrXirConstraint to,
    uint32_t parameter_count, XrXirBudget *budget) {
    if (!budget || (!!from.interfaces != !!from.interface_count) || (!!to.interfaces != !!to.interface_count))
        return XR_XIR_BAD_STRUCTURE;
    if (!budget->work) return XR_XIR_BUDGET;
    --budget->work;
    if (from.markers != to.markers || from.interface_count != to.interface_count) return XR_XIR_BAD_TYPE;
    if (!from.interface_count) return XR_XIR_OK;
    if (parameter_count > XR_XIR_TYPE_PARAMETER_LIMIT - XR_XIR_TYPE_PARAMETER_BASE) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)parameter_count * sizeof(XrXirType);
    if (bytes > SIZE_MAX || bytes > budget->scratch_bytes || parameter_count > budget->work) return XR_XIR_BUDGET;
    budget->scratch_bytes -= bytes; budget->work -= parameter_count;
    XrXirType *parameters = parameter_count ? xr_malloc((size_t)bytes) : NULL;
    if (parameter_count && !parameters) return XR_XIR_OUT_OF_MEMORY;
    for (uint32_t p = 0; p < parameter_count; ++p) parameters[p] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + p);
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t i = 0; i < from.interface_count && status == XR_XIR_OK; ++i) {
        if (!budget->work) { status = XR_XIR_BUDGET; break; }
        --budget->work;
        const XrXirInterfaceApplication *a = &from.interfaces[i], *b = &to.interfaces[i];
        status = constraint_record_identity(from_types,a,to_types,b,budget);
        for (uint32_t p = 0; p < a->argument_count && status == XR_XIR_OK; ++p)
            status = xr_xir_type_substitution_matches_between(from_types,to_types,parameters,
                parameter_count,a->arguments[p],b->arguments[p],budget);
    }
    xr_free(parameters); return status;
}
