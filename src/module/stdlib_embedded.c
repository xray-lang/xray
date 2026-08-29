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
 *   Pure-Xray stdlib modules must be loadable without relying on cwd or an
 *   installed stdlib directory. Bytecode is the preferred runtime artifact;
 *   embedded source is retained for AOT/export analysis and bootstrap builds.
 */

#include "xstdlib_embedded.h"

#include "xmodule.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *name;
    const uint8_t *bytecode;
    size_t size;
} XrEmbeddedStdlibBytecode;

#include <stdlib_embedded_sources.inc>
#include <stdlib_embedded_bytecodes.inc>

XR_FUNC const XrStdlibModuleDescriptor *xr_stdlib_module_descriptor(const char *module_name) {
    if (!module_name)
        return NULL;
    const XrStdlibModuleDescriptor *found = NULL;
    for (size_t i = 0; i < xr_stdlib_module_descriptor_count; i++) {
        const XrStdlibModuleDescriptor *entry = &xr_stdlib_module_descriptors[i];
        if (!entry->name)
            return NULL;
        if (strcmp(entry->name, module_name) != 0)
            continue;
        /* A duplicate name would make "which module is this" ambiguous, so
         * refuse the lookup rather than pick one of the two. */
        if (found)
            return NULL;
        found = entry;
    }
    return found;
}

XR_FUNC const uint8_t *xr_get_embedded_stdlib_bytecode(const char *module_name, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!module_name)
        return NULL;
    for (size_t i = 0; i < xr_embedded_stdlib_bytecode_count; i++) {
        const XrEmbeddedStdlibBytecode *entry = &xr_embedded_stdlib_bytecodes[i];
        if (entry->name && strcmp(entry->name, module_name) == 0) {
            if (out_size)
                *out_size = entry->size;
            return entry->bytecode;
        }
    }
    return NULL;
}

XR_FUNC const char *xr_get_embedded_stdlib(const char *module_name) {
    const XrStdlibModuleDescriptor *entry = xr_stdlib_module_descriptor(module_name);
    return entry ? entry->source : NULL;
}

XR_FUNC bool xr_stdlib_module_install_native_entries(XrVMRuntime *isolate, XrModule *module,
                                                     const char *requested_module_name) {
    if (!isolate || !module || !module->name || !requested_module_name ||
        strcmp(module->name, requested_module_name) != 0)
        return false;
    const XrStdlibModuleDescriptor *entry = xr_stdlib_module_descriptor(requested_module_name);
    if (!entry)
        return false;
    if (xr_module_state(module) != XR_MODULE_NEW || module->export_count != 0)
        return false;
    if (!entry->bind_native_entries)
        return true;
    return entry->bind_native_entries(isolate, module);
}
