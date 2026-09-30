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
#include "../base/xmalloc.h"
#include <string.h>

void xr_xir_implementations_free(XrXirImplementationTable *table) {
    if (!table) return;
    if (table->records) for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirImplementation *record = &table->records[i];
        xr_free((void *)record->interface.arguments);
        if (record->bindings) for (uint32_t b = 0; b < record->binding_count; ++b)
            xr_free((void *)record->bindings[b].requirement.arguments);
        xr_free((void *)record->bindings);
    }
    xr_free((void *)table->records); xr_free(table);
}
static XrXirStatus implementation_application_copy(const XrXirInterfaceApplication *source,
    XrXirInterfaceApplication *output) {
    if (!!source->arguments != !!source->argument_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)source->argument_count * sizeof(*source->arguments);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirType *arguments = bytes ? xr_malloc((size_t)bytes) : NULL;
    if (bytes && !arguments) return XR_XIR_OUT_OF_MEMORY;
    if (bytes) memcpy(arguments, source->arguments, (size_t)bytes);
    *output = (XrXirInterfaceApplication){source->declaration, arguments, source->argument_count};
    return XR_XIR_OK;
}
static XrXirStatus implementation_record_copy(const XrXirImplementation *source,
    XrXirImplementation *output) {
    if (!!source->bindings != !!source->binding_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)source->binding_count * sizeof(*source->bindings);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    output->nominal_declaration = source->nominal_declaration;
    XrXirStatus status = implementation_application_copy(&source->interface, &output->interface);
    if (status != XR_XIR_OK) return status;
    XrXirImplementationBinding *bindings = bytes ? xr_calloc(source->binding_count, sizeof(*bindings)) : NULL;
    if (bytes && !bindings) return XR_XIR_OUT_OF_MEMORY;
    output->bindings = bindings; output->binding_count = source->binding_count;
    for (uint32_t b = 0; b < source->binding_count; ++b) {
        status = implementation_application_copy(&source->bindings[b].requirement, &bindings[b].requirement);
        if (status != XR_XIR_OK) return status;
        bindings[b].member = source->bindings[b].member; bindings[b].function = source->bindings[b].function;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_implementations_copy_verified(const XrXirImplementationTable *source,
    XrXirImplementationTable **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!source) return XR_XIR_OK;
    if (!source->records || !source->count) return XR_XIR_BAD_STRUCTURE;
    if ((uint64_t)source->count * sizeof(*source->records) > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirImplementationTable *table = xr_calloc(1, sizeof(*table));
    if (!table) return XR_XIR_OUT_OF_MEMORY;
    XrXirImplementation *records = xr_calloc(source->count, sizeof(*records));
    table->records = records; table->count = source->count;
    if (!records) { xr_xir_implementations_free(table); return XR_XIR_OUT_OF_MEMORY; }
    for (uint32_t i = 0; i < source->count; ++i) {
        XrXirStatus status = implementation_record_copy(&source->records[i], &records[i]);
        if (status != XR_XIR_OK) { xr_xir_implementations_free(table); return status; }
    }
    *output = table; return XR_XIR_OK;
}
