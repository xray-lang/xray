/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbundle.h - Multi-file bundling support
 *
 * KEY CONCEPT:
 *   Collects all dependencies of an entry file (recursive import analysis),
 *   compiles and bundles multiple modules into a single bytecode package,
 *   and loads modules from embedded bytecode at runtime.
 */

#ifndef XBUNDLE_H
#define XBUNDLE_H

#include <stdint.h>
#include <stddef.h>

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"

// Bundled module entry
typedef struct {
    const char *path;
    const uint8_t *bc;
    size_t bc_size;
} XrBundleEntry;

// External dependencies (stdlib or third-party packages)
typedef struct {
    char **deps;
    int count;
    int capacity;
} XrExternalDeps;

// Bundle result
typedef struct {
    XrBundleEntry *entries;
    int count;
    int capacity;
    const char *entry_path;
    XrExternalDeps stdlib;
    XrExternalDeps packages;
} XrBundle;

typedef enum {
    XR_BUNDLE_DEFAULT = 0,
    XR_BUNDLE_STATIC_PACKAGES = 1
} XrBundleFlags;

// Create bundle from entry file, returns NULL on failure
XR_FUNC XrBundle* xr_bundle_create(XrayIsolate *X, const char *entry_file);

// Create bundle with options
XR_FUNC XrBundle* xr_bundle_create_ex(XrayIsolate *X, const char *entry_file, XrBundleFlags flags);

XR_FUNC void xr_bundle_free(XrBundle *bundle);

// Generate C source code, caller must free returned string
XR_FUNC char* xr_bundle_to_c_source(XrBundle *bundle, const char *var_prefix);

#define XR_BUNDLE_MAGIC     "XRPK"
#define XR_BUNDLE_VERSION   1

#endif // XBUNDLE_H
