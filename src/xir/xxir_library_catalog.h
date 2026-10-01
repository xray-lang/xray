/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_library_catalog.h - Owned authenticated Checked library input
 *
 * KEY CONCEPT: A catalog owns independently checked bodies, never source ASTs.
 */
#ifndef XXIR_LIBRARY_CATALOG_H
#define XXIR_LIBRARY_CATALOG_H
#include "xxir_checked.h"
#include "../module/xmodule_resolver.h"
typedef struct XrXirLibraryCatalog XrXirLibraryCatalog;
typedef struct XrXirLibraryInput {
    XrModuleIdentityAuthority authority;
    const char *logical_path;
    const void *packet;
    size_t length;
    uint8_t sha256[32];
} XrXirLibraryInput;
XR_FUNC XrXirStatus xr_xir_library_catalog_new(const XrXirLibraryInput *input,
    const XrXirBudget *budget, XrXirLibraryCatalog **output);
XR_FUNC void xr_xir_library_catalog_free(XrXirLibraryCatalog *catalog);
XR_FUNC const XrModuleResourceBinding *xr_xir_library_catalog_resource(const XrXirLibraryCatalog *catalog);
#endif // XXIR_LIBRARY_CATALOG_H
