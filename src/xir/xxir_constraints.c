/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraints.c - Owned constraint arrays and allocation rollback
 */
#include "xxir_constraints.h"
#include "xxir_compile_memory.h"
#include "xxir_types.h"
#include "../base/xmalloc.h"
#include <string.h>

void xr_xir_compile_interface_applications_free(XrXirInterfaceApplication *applications, uint32_t count) {
    if (!applications) return;
    for (uint32_t i = 0; i < count; ++i) xr_compile_resources_free((void *)applications[i].arguments);
    xr_compile_resources_free(applications);
}
XrXirStatus xr_xir_compile_interface_applications_copy_verified(const XrXirCompileContext *compile_context, const XrXirInterfaceApplication *source, uint32_t count, XrXirInterfaceApplication **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!!source != !!count) return XR_XIR_BAD_STRUCTURE;
    if (!count) { *output = NULL; return XR_XIR_OK; }
    if ((uint64_t)count * sizeof(*source) > SIZE_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(compile_context, count)) return XR_XIR_BUDGET;
    XrXirInterfaceApplication *copy = xir_compile_calloc(compile_context, count, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirInterfaceApplication *from = &source[i];
        if (!!from->arguments != !!from->argument_count) { status = XR_XIR_BAD_STRUCTURE; break; }
        uint64_t bytes = (uint64_t)from->argument_count * sizeof(*from->arguments);
        if (bytes > SIZE_MAX) { status = XR_XIR_BUDGET; break; }
        XrXirType *arguments = xir_compile_copy(compile_context, from->arguments, (size_t)bytes, &allocation_status);
        if (bytes && !arguments) { status = allocation_status; break; }

        copy[i] = (XrXirInterfaceApplication){from->declaration, arguments, from->argument_count};
    }
    if (status != XR_XIR_OK) { xr_xir_compile_interface_applications_free(copy, count); return status; }
    *output = copy; return XR_XIR_OK;
}
void xr_xir_compile_constraint_array_free(XrXirConstraint *constraints, uint32_t count) {
    if (!constraints) return;
    for (uint32_t i = 0; i < count; ++i)
        xr_xir_compile_interface_applications_free((XrXirInterfaceApplication *)constraints[i].interfaces, constraints[i].interface_count);
    xr_compile_resources_free(constraints);
}
XrXirStatus xr_xir_compile_constraint_array_copy_verified(const XrXirCompileContext *compile_context, const XrXirConstraint *source, uint32_t count, XrXirConstraint **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!!source != !!count) return XR_XIR_BAD_STRUCTURE;
    if (!count) { *output = NULL; return XR_XIR_OK; }
    if ((uint64_t)count * sizeof(*source) > SIZE_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(compile_context, count)) return XR_XIR_BUDGET;
    XrXirConstraint *copy = xir_compile_calloc(compile_context, count, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirInterfaceApplication *applications = NULL;
        XrXirStatus status = xr_xir_compile_interface_applications_copy_verified(compile_context, source[i].interfaces, source[i].interface_count, &applications);
        if (status != XR_XIR_OK) { xr_xir_compile_constraint_array_free(copy, count); return status; }
        copy[i] = (XrXirConstraint){source[i].markers, applications, source[i].interface_count};
    }
    *output = copy; return XR_XIR_OK;
}
static XrXirStatus constraint_record_name(XrXirLiteral from, XrXirLiteral to, XrXirCompileContext *budget) {
    uint64_t work = from.length == to.length ? (uint64_t)from.length + 1 : 1;
    if (!xir_compile_work(budget, work)) return XR_XIR_BUDGET;

    if (!from.bytes || !to.bytes || !from.length || !to.length) return XR_XIR_BAD_STRUCTURE;
    return from.length == to.length && !memcmp(from.bytes,to.bytes,from.length) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
static XrXirStatus constraint_record_identity(const XrXirTypes *from_types,
    const XrXirInterfaceApplication *from, const XrXirTypes *to_types,
    const XrXirInterfaceApplication *to, XrXirCompileContext *budget) {
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
XrXirStatus xr_xir_compile_constraint_records_match(const XrXirCompileContext *compile_context, const XrXirTypes *from_types, XrXirConstraint from, const XrXirTypes *to_types, XrXirConstraint to, uint32_t parameter_count) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!budget || (!!from.interfaces != !!from.interface_count) || (!!to.interfaces != !!to.interface_count))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(budget, 1)) return XR_XIR_BUDGET;

    if (from.markers != to.markers || from.interface_count != to.interface_count) return XR_XIR_BAD_TYPE;
    if (!from.interface_count) return XR_XIR_OK;
    if (parameter_count > XR_XIR_TYPE_PARAMETER_LIMIT - XR_XIR_TYPE_PARAMETER_BASE) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)parameter_count * sizeof(XrXirType);
    if (bytes > SIZE_MAX ||!xir_compile_work(budget, parameter_count)) return XR_XIR_BUDGET;

    XrXirType *parameters = parameter_count ? xir_compile_alloc(compile_context, (size_t)bytes, &allocation_status) : NULL;
    if (parameter_count && !parameters) {  return allocation_status; }
    for (uint32_t p = 0; p < parameter_count; ++p) parameters[p] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + p);
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t i = 0; i < from.interface_count && status == XR_XIR_OK; ++i) {
        if (!xir_compile_work(budget, 1)) { status = XR_XIR_BUDGET; break; }

        const XrXirInterfaceApplication *a = &from.interfaces[i], *b = &to.interfaces[i];
        status = constraint_record_identity(from_types,a,to_types,b,budget);
        for (uint32_t p = 0; p < a->argument_count && status == XR_XIR_OK; ++p)
            status = xr_xir_compile_type_substitution_matches_between(budget, from_types, to_types, parameters, parameter_count, a->arguments[p], b->arguments[p]);
    }
    xr_compile_resources_free(parameters);  return status;
}
