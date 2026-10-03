/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_embedded_runtime.c - Runtime bytecode and native entry installation
 *
 * KEY CONCEPT: Source metadata does not retain runtime function addresses.
 */
#include "xstdlib_embedded.h"
#include "xmodule.h"
#include <string.h>

typedef struct XrEmbeddedStdlibBytecode {
    const char *name;
    const uint8_t *bytecode;
    size_t size;
} XrEmbeddedStdlibBytecode;

typedef struct XrEmbeddedStdlibNativeBinding {
    const char *name;
    bool (*install)(struct XrVMRuntime *, struct XrModule *);
} XrEmbeddedStdlibNativeBinding;

#include <stdlib_embedded_bytecodes.inc>
#include <stdlib_embedded_runtime.inc>

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

XR_FUNC bool xr_stdlib_module_install_native_entries(XrVMRuntime *isolate, XrModule *module,
                                                     const char *requested_module_name) {
    if (!isolate || !module || !module->name || !requested_module_name ||
        strcmp(module->name, requested_module_name) != 0)
        return false;
    const XrStdlibSourceDescriptor *entry = xr_stdlib_source_descriptor(requested_module_name);
    if (!entry || xr_module_state(module) != XR_MODULE_NEW || module->export_count != 0)
        return false;
    const XrEmbeddedStdlibNativeBinding *binding = NULL;
    for (size_t i = 0; i < xr_stdlib_native_binding_count; ++i) {
        const XrEmbeddedStdlibNativeBinding *candidate = &xr_stdlib_native_bindings[i];
        if (!candidate->name || strcmp(candidate->name, requested_module_name)) continue;
        if (binding || !candidate->install) return false;
        binding = candidate;
    }
    if (!entry->has_native_entries) return binding == NULL;
    return binding && binding->install(isolate, module);
}
