/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_stdlib_catalog.c - Owned Catalog input from authenticated Checked bytes
 *
 * KEY CONCEPT: Checked library admission has no native code dependency.
 */
#include "xxir_stdlib_internal.h"
#include "xxir_compile_memory.h"
XR_FUNC XrXirStatus xir_stdlib_library_catalog_new(const XrXirCompileContext *context,
    const char *physical_root, XrXirLibraryCatalog **output) {
    if (!xir_compile_context_valid(context) || !physical_root || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    size_t count = xir_stdlib_checked_component_count;
    if (!count || count > SIZE_MAX / sizeof(XrXirLibraryInput) ||
        count > SIZE_MAX / sizeof(XrXirLibraryModuleInput)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    XrXirLibraryInput *inputs = xir_compile_calloc(context,count,sizeof(*inputs),&status);
    if (!inputs) return status;
    XrXirLibraryModuleInput *modules = xir_compile_calloc(context,count,sizeof(*modules),&status);
    if (!modules) { xr_compile_resources_free(inputs); return status; }
    for (size_t i = 0; i < count; ++i) {
        const XirStdlibCheckedComponent *component = &xir_stdlib_checked_components[i];
        if (!xir_compile_work(context,1+sizeof(inputs[i].sha256))) { status = XR_XIR_BUDGET; break; }
        modules[i] = (XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_STDLIB,
            component->namespace_id,physical_root},component->logical_path};
        inputs[i] = (XrXirLibraryInput){component->bytes,component->length,{0},&modules[i],1};
        memcpy(inputs[i].sha256,component->sha256,sizeof(inputs[i].sha256));
    }
    // Each body and its canonical module are independently verified on the same ledger.
    if (status == XR_XIR_OK)
        status = xr_xir_compile_library_catalog_new_v2(context,inputs,count,output);
    xr_compile_resources_free(modules);
    xr_compile_resources_free(inputs);
    return status;
}
