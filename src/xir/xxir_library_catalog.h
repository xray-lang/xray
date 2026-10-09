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
/* Compiler C interface revision 2 changes input and module-view layouts.
 * The unversioned constructor and resource-array symbols are retired.
 * This is independent of Checked wire and runtime ABI identities. */
#define XR_XIR_LIBRARY_C_INTERFACE_VERSION 2u

typedef struct XrXirLibraryCatalog XrXirLibraryCatalog;
/* Binding order is the exact Checked declarations.modules ordinal order. */
typedef struct XrXirLibraryModuleInput {
    XrModuleIdentityAuthority authority;
    const char *logical_path;
} XrXirLibraryModuleInput;
typedef struct XrXirLibraryInput {
    const void *packet;
    size_t length;
    uint8_t sha256[32];
    XrXirLibraryModuleInput *modules;
    size_t module_count;
} XrXirLibraryInput;
/* Output must be empty; occupied output rejects before resource work.
 * The catalog copies context and owns all strings and Checked bodies on its
 * ledger. Inputs are borrowed only during construction; failure preserves output.
 * The catalog pins resources after the caller releases its ledger reference. */
XR_FUNC XrXirStatus xr_xir_compile_library_catalog_new_v2(const XrXirCompileContext *context,
    const XrXirLibraryInput *inputs, size_t count, XrXirLibraryCatalog **output);
XR_FUNC void xr_xir_compile_library_catalog_free(XrXirLibraryCatalog *catalog);
XR_FUNC const XrModuleResourceBinding *xr_xir_compile_library_catalog_resources_v2(
    const XrXirLibraryCatalog *catalog, size_t *count);
XR_FUNC const XrXirCompileContext *xr_xir_compile_library_catalog_context(const XrXirLibraryCatalog *catalog);
#endif // XXIR_LIBRARY_CATALOG_H
