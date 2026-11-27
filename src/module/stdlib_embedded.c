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

typedef struct {
    const char *logical_path;
    const char *text;
} XrStdlibLogicalSource;

#include <stdlib_embedded_sources.inc>

XR_FUNC const XrStdlibSourceDescriptor *xr_stdlib_source_descriptor(const char *name) {
    if (!name) return NULL;
    const XrStdlibSourceDescriptor *found = NULL;
    for (size_t i = 0; i < xr_stdlib_source_descriptor_count; ++i) {
        const XrStdlibSourceDescriptor *entry = &xr_stdlib_source_descriptors[i];
        if (!entry->name) return NULL;
        if (strcmp(entry->name,name)) continue;
        if (found) return NULL;
        found = entry;
    }
    return found;
}

XR_FUNC const char *xr_get_embedded_stdlib_source(const char *logical_path) {
    if (!logical_path) return NULL;
    for (size_t i = 0; i < sizeof(xr_stdlib_logical_sources) / sizeof(xr_stdlib_logical_sources[0]); ++i) {
        const XrStdlibLogicalSource *source = &xr_stdlib_logical_sources[i];
        if (!strcmp(source->logical_path, logical_path)) return source->text;
    }
    return NULL;
}

XR_FUNC const char *xr_get_embedded_stdlib(const char *module_name) {
    const XrStdlibSourceDescriptor *entry = xr_stdlib_source_descriptor(module_name);
    return entry ? entry->source : NULL;
}
