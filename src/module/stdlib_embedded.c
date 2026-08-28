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

typedef bool (*XrEmbeddedStdlibPrivateLeafBinder)(struct XrVMRuntime *isolate,
                                                   struct XrModule *module);

typedef struct {
    const char *name;
    const char *source;
    XrEmbeddedStdlibPrivateLeafBinder bind_private_leaves;
} XrEmbeddedStdlibSource;

typedef struct {
    const char *name;
    const uint8_t *bytecode;
    size_t size;
} XrEmbeddedStdlibBytecode;

#include <stdlib_embedded_sources.inc>
#include <stdlib_embedded_bytecodes.inc>

static bool find_embedded_source(const char *module_name,
                                 const XrEmbeddedStdlibSource **out_entry) {
    if (out_entry)
        *out_entry = NULL;
    if (!module_name || !out_entry)
        return false;
    for (size_t i = 0; i < xr_embedded_stdlib_source_count; i++) {
        const XrEmbeddedStdlibSource *entry = &xr_embedded_stdlib_sources[i];
        if (!entry->name || !entry->source)
            return false;
        if (strcmp(entry->name, module_name) != 0)
            continue;
        if (*out_entry)
            return false;
        *out_entry = entry;
    }
    return true;
}

XR_FUNC const uint8_t *xr_get_embedded_stdlib_bytecode(const char *module_name,
                                                        size_t *out_size) {
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
    const XrEmbeddedStdlibSource *entry = NULL;
    if (!find_embedded_source(module_name, &entry) || !entry)
        return NULL;
    return entry->source;
}

XR_FUNC bool xr_stdlib_embedded_private_leaves_install(XrVMRuntime *isolate,
                                                        XrModule *module,
                                                        const char *requested_module_name) {
    if (!isolate || !module || !module->name || !requested_module_name ||
        strcmp(module->name, requested_module_name) != 0)
        return false;
    const XrEmbeddedStdlibSource *entry = NULL;
    if (!find_embedded_source(requested_module_name, &entry) || !entry)
        return false;
    if (xr_module_state(module) != XR_MODULE_NEW || module->export_count != 0)
        return false;
    if (!entry->bind_private_leaves)
        return true;
    return entry->bind_private_leaves(isolate, module);
}
