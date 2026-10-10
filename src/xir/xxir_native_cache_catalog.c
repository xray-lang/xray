/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_cache_catalog.c - Owned Catalog input from authenticated Checked bytes
 *
 * KEY CONCEPT: Source Catalog admission is independent of runtime cache lookup.
 */
#include "xxir_native_cache_internal.h"
#include "xxir_native_cache_registry_internal.h"
#include "xxir_compile_memory.h"
XR_FUNC XrXirStatus xir_native_cache_library_catalog_new(const XrXirCompileContext *context,
    const char *physical_root, XrXirLibraryCatalog **output) {
    if (!xir_compile_context_valid(context) || !physical_root || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    const XirNativeCacheComponent *checked = &xir_native_cache_registry.checked;
    XrXirLibraryInput input = {checked->bytes,checked->length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",physical_root},"io/output.xr"}},1};
    if (!xir_compile_work(context,sizeof(input.sha256))) return XR_XIR_BUDGET;
    memcpy(input.sha256,checked->digest,sizeof(input.sha256));
    /* The ordinary reader verifies the complete Checked body and canonical owner.
     * The AOT consumer has no dependency on a native cache variant's applicability. */
    return xr_xir_compile_library_catalog_new_v2(context,&input,1,output);
}
