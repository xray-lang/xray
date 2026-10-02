/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_embedded.c - Embedded stdlib script lookup
 *
 * KEY CONCEPT:
 *   One generated source authority table serves compiler and runtime queries.
 *   This translation unit has no runtime binders or bytecode dependencies.
 */

#include "xstdlib_embedded.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <stdlib_embedded_sources.inc>

XR_FUNC XrOsIoStatus xr_stdlib_source_descriptor_work(void *owner,
    XrOsIoStatus (*work)(void *, uint64_t), const char *name,
    const XrStdlibSourceDescriptor **output) {
    if (!owner || !work || !name || !output) return XR_OS_IO_BAD_ARGUMENT;
    const XrStdlibSourceDescriptor *found = NULL;
    for (size_t i = 0; i < xr_stdlib_source_descriptor_count; ++i) {
        XrOsIoStatus status = work(owner,1);
        if (status != XR_OS_IO_OK) return status;
        const XrStdlibSourceDescriptor *entry = &xr_stdlib_source_descriptors[i];
        if (!entry->name) { *output = NULL; return XR_OS_IO_OK; }
        bool equal = false;
        for (size_t n = 0;; ++n) {
            status = work(owner,2);
            if (status != XR_OS_IO_OK) return status;
            char expected = entry->name[n], actual = name[n];
            if (expected != actual) break;
            if (!actual) { equal = true; break; }
        }
        if (!equal) continue;
        if (found) { *output = NULL; return XR_OS_IO_OK; }
        found = entry;
    }
    *output = found; return XR_OS_IO_OK;
}
static XrOsIoStatus runtime_query_work(void *owner, uint64_t units) {
    (void)owner; (void)units; return XR_OS_IO_OK;
}
XR_FUNC const XrStdlibSourceDescriptor *xr_stdlib_source_descriptor(const char *name) {
    const XrStdlibSourceDescriptor *result = NULL;
    if (name) (void)xr_stdlib_source_descriptor_work(&result,runtime_query_work,name,&result);
    return result;
}
XR_FUNC XrOsIoStatus xr_get_embedded_stdlib_work(void *owner,
    XrOsIoStatus (*work)(void *, uint64_t), const char *name, const char **output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    const XrStdlibSourceDescriptor *entry = NULL;
    XrOsIoStatus status = xr_stdlib_source_descriptor_work(owner,work,name,&entry);
    if (status != XR_OS_IO_OK) return status;
    if (entry && (status = work(owner,1)) != XR_OS_IO_OK) return status;
    *output = entry ? entry->source : NULL; return XR_OS_IO_OK;
}

XR_FUNC const char *xr_get_embedded_stdlib(const char *module_name) {
    const XrStdlibSourceDescriptor *entry = xr_stdlib_source_descriptor(module_name);
    return entry ? entry->source : NULL;
}
