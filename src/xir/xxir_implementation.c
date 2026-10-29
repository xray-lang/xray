/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_implementation.c - Independent implementation ownership and rollback
 *
 * KEY CONCEPT:
 *   Initialized prefixes release every nested allocation on all failure paths.
 */
#include "xxir_implementation.h"
#include "xxir_compile_memory.h"
#include "xxir_lower_control.h"
#include "../base/xmalloc.h"
#include <string.h>

void xr_xir_compile_implementations_free(XrXirImplementationTable *table) {
    if (!table) return;
    if (table->records) for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirImplementation *record = &table->records[i];
        xr_compile_resources_free((void *)record->interface.arguments);
        if (record->bindings) for (uint32_t b = 0; b < record->binding_count; ++b)
            xr_compile_resources_free((void *)record->bindings[b].requirement.arguments);
        xr_compile_resources_free((void *)record->bindings);
    }
    xr_compile_resources_free((void *)table->records); xr_compile_resources_free(table);
}
static XrXirStatus implementation_poll(XrCompileRequestControl *control) {
    return control ? xir_compile_resource_status(xr_compile_request_control_poll(control)) : XR_XIR_OK;
}
static XrXirStatus implementation_application_copy(const XrXirCompileContext *compile_context, const XrXirInterfaceApplication *source,
    XrCompileRequestControl *control, XrXirInterfaceApplication *output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!!source->arguments != !!source->argument_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)source->argument_count * sizeof(*source->arguments);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirType *arguments = bytes ? xir_compile_alloc(compile_context, (size_t)bytes, &allocation_status) : NULL;
    if (bytes && !arguments) return allocation_status;
    XrXirStatus status=xir_compile_resource_status(xir_lower_copy_span(control,arguments,source->arguments,(size_t)bytes));
    if (status!=XR_XIR_OK) {xr_compile_resources_free(arguments);return status;}
    *output = (XrXirInterfaceApplication){source->declaration, arguments, source->argument_count};
    return XR_XIR_OK;
}
static XrXirStatus implementation_record_copy(const XrXirCompileContext *compile_context, const XrXirImplementation *source,
    XrCompileRequestControl *control, XrXirImplementation *output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!!source->bindings != !!source->binding_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)source->binding_count * sizeof(*source->bindings);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    output->nominal_declaration = source->nominal_declaration;
    XrXirStatus status = implementation_application_copy(compile_context, &source->interface, control, &output->interface);
    if (status != XR_XIR_OK) return status;

    XrXirImplementationBinding *bindings = bytes ? xir_compile_calloc(compile_context, source->binding_count, sizeof(*bindings), &allocation_status) : NULL;
    if (bytes && !bindings) return allocation_status;
    output->bindings = bindings; output->binding_count = source->binding_count;
    for (uint32_t b = 0; b < source->binding_count; ++b) {
        status=implementation_poll(control);if (status!=XR_XIR_OK) return status;
        status = implementation_application_copy(compile_context, &source->bindings[b].requirement, control, &bindings[b].requirement);
        if (status != XR_XIR_OK) return status;
        bindings[b].member = source->bindings[b].member; bindings[b].function = source->bindings[b].function;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_implementations_copy_verified(const XrXirCompileContext *compile_context, const XrXirImplementationTable *source, XrCompileRequestControl *control, XrXirImplementationTable **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=implementation_poll(control);if (status!=XR_XIR_OK) return status;

    if (!source) { *output = NULL; return XR_XIR_OK; }
    if (!source->records || !source->count) return XR_XIR_BAD_STRUCTURE;
    if ((uint64_t)source->count * sizeof(*source->records) > SIZE_MAX) return XR_XIR_BUDGET;

    XrXirImplementationTable *table = xir_compile_calloc(compile_context, 1, sizeof(*table), &allocation_status);
    if (!table) return allocation_status;
    XrXirImplementation *records = xir_compile_calloc(compile_context, source->count, sizeof(*records), &allocation_status);
    table->records = records; table->count = source->count;
    if (!records) { xr_xir_compile_implementations_free(table); return allocation_status; }
    for (uint32_t i = 0; i < source->count; ++i) {
        status=implementation_poll(control);
        if (status==XR_XIR_OK) status = implementation_record_copy(compile_context, &source->records[i], control, &records[i]);
        if (status != XR_XIR_OK) { xr_xir_compile_implementations_free(table); return status; }
    }
    status=implementation_poll(control);
    if (status!=XR_XIR_OK) {xr_xir_compile_implementations_free(table);return status;}
    *output = table; return XR_XIR_OK;
}
