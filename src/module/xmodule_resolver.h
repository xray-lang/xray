/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_resolver.h - Unified import specifier → source path resolver
 *
 * KEY CONCEPT:
 *   Resolve source and immutable Checked imports under one compiler ledger.
 *   Owning results remain valid after their producing resolver is destroyed.
 *
 * RESOLUTION RULES:
 *   Bare name         -> stdlib native factory registry lookup
 *   "std/name/path"   -> exact stdlib source submodule under an explicit root
 *   "./" or "../"     → relative file (.xr) or directory (index.xr)
 *   "owner/name"      → third-party package under ~/.xray/packages/
 */

#ifndef XMODULE_RESOLVER_H
#define XMODULE_RESOLVER_H

#include <stdbool.h>
#include "../base/xdefs.h"
#include "../base/xhashmap.h"
#include "xmodule_identity.h"
#include "xmodule_overlay.h"

/* Forward declarations */
struct XrXirLibraryCatalog;
struct XrProject;
struct XrLockfile;

/* Explicit representations preserve the existing typed authority domain. */
typedef enum XrModuleRepresentation { XR_MODULE_SOURCE, XR_MODULE_CHECKED_LIBRARY } XrModuleRepresentation;
typedef struct XrModuleResourceBinding {
    const char *canonical, *logical_path, *source_locator;
    XrModuleIdentityAuthority authority;
    const void *checked; /* Borrowed from the immutable compiler-owned catalog. */
    uint32_t checked_module;
    const struct XrModuleResourceBinding *const *dependencies;
    uint32_t dependency_count;
} XrModuleResourceBinding;

/* ========== Module ID ========== */

/*
 * Canonical durable identifier for a resolved module.
 * Every kind uses a typed, versioned, length-framed identity.
 */
typedef enum {
    XR_MOD_STDLIB,
    XR_MOD_FILE,
    XR_MOD_PACKAGE,
    XR_MOD_MEMORY,
} XrModuleKind;

typedef struct {
    XrModuleKind kind;
    char *canonical;   /* Owned string; caller releases through xr_compile_module_id_cleanup */
    char *logical_path; /* Root-relative source path (owned) */
    char *source_path; /* Absolute .xr path, or NULL for native stdlib.
                          Owned string; caller releases through xr_compile_module_id_cleanup */
    XrModuleIdentityAuthority authority; /* Owned authority coordinate and I/O root */
    XrModuleRepresentation representation;
    const XrModuleResourceBinding *resource;
} XrModuleId;

/* Free contents of an XrModuleId (does NOT free the struct itself). */
XR_FUNC void xr_compile_module_id_cleanup(XrModuleId *id);

/* ========== Resolver Configuration ========== */

typedef struct {
    /*
     * Optional stdlib source directory (e.g. "stdlib/").
     * When non-NULL the resolver probes for a development source path.
     * Embedded canonical sources remain valid when this is NULL.
     */
    const char *stdlib_path;

    /*
     * Optional lockfile for pinning third-party package versions.
     * Borrowed pointer; may be NULL. It must use the same ledger policy and
     * remain alive until the resolver is destroyed.
     */
    struct XrLockfile *lockfile;

    /* Borrowed immutable catalog; must share resources and outlive the resolver. */
    const struct XrXirLibraryCatalog *catalog;
} XrModuleResolverConfig;

/* ========== Resolver Instance ========== */

typedef struct XrModuleResolver {
    XrCompileResources *resources;
    XrModuleResolverConfig config;
    XrHashMap *cache; /* specifier+importer → XrModuleId (owned) */
    XrModuleOverlay *overlay; /* Owned immutable text selection shared with graph. */
    bool resolution_started;
} XrModuleResolver;

/* ========== Lifecycle ========== */

/* Failure preserves output. stdlib_path is copied; lockfile and catalog are borrowed. */
XR_FUNC XrModuleStatus xr_compile_module_resolver_new(XrCompileResources *resources,
    const XrModuleResolverConfig *cfg, XrModuleResolver **output);
XR_FUNC void xr_compile_module_resolver_free(XrModuleResolver *r);
/* Install exactly once before any resolution. Copies inputs on the same ledger;
 * a failure leaves the resolver unchanged. Checked catalog collisions reject. */
XR_FUNC XrModuleStatus xr_compile_module_resolver_set_overlay(XrModuleResolver *r,
    const XrModuleOverlayInput *inputs, size_t count);

XR_FUNC XrModuleStatus xr_compile_module_resolver_set_lockfile(XrModuleResolver *r, struct XrLockfile *lockfile);

/* ========== Resolution API ========== */

/*
 * Resolve an import specifier to a canonical module id.
 *
 * @param r             Resolver instance
 * @param specifier     The import path string (without quotes)
 * @param importer_path Absolute path of the importing file, or NULL for
 *                      entry scripts (uses cwd as base)
 * @param out_id        On success, filled with the resolved module info.
 *                      Failure preserves its prior value.
 *                      Caller must call xr_compile_module_id_cleanup() when done.
 * @param err_buf       On failure, a human-readable error message is
 *                      written here (ledger-owned). Caller uses xr_compile_resources_free.
 *                      May be NULL if the caller doesn't need the message.
 * @return              XR_MODULE_OK on success, otherwise a typed failure
 */
/* Resolve with the importing module's exact authority. Package-internal relative
 * imports must use their package authority rather than the entry project root. */
XR_FUNC XrModuleStatus xr_compile_module_resolver_resolve(XrModuleResolver *r, const char *specifier,
                                       const char *importer_path,
                                       const XrModuleIdentityAuthority *importer_authority,
                                       XrModuleId *out_id, char **err_buf);

#endif  // XMODULE_RESOLVER_H
