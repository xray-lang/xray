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
    XrXirLibraryInput input = {xir_stdlib_output_checked,xir_stdlib_output_checked_length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",physical_root},"io/output.xr"}},1};
    if (!xir_compile_work(context,sizeof(input.sha256))) return XR_XIR_BUDGET;
    memcpy(input.sha256,xir_stdlib_output_checked_sha256,sizeof(input.sha256));
    // The ordinary reader verifies the complete body and canonical owner.
    return xr_xir_compile_library_catalog_new_v2(context,&input,1,output);
}
