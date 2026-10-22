/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_embedded_runtime.c - Runtime native entry installation
 *
 * KEY CONCEPT: Source metadata does not retain runtime function addresses.
 */
#include "xstdlib_embedded.h"
#include "xmodule.h"
#include <string.h>

typedef struct XrEmbeddedStdlibNativeBinding {
    const char *name;
    bool (*install)(struct XrVMRuntime *, struct XrModule *);
} XrEmbeddedStdlibNativeBinding;

#include <stdlib_embedded_runtime.inc>

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
