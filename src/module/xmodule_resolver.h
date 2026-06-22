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
 *   Single entry point for resolving any import specifier to an absolute
 *   source file path. Used by the bundler, runtime loader, and (future)
 *   module graph builder. Replaces ad-hoc resolution logic scattered
 *   across xmodule.c and xbundle.c.
 *
 * RESOLUTION RULES:
 *   Bare name         → stdlib native_loaders registry lookup
 *   "./" or "../"     → relative file (.xr) or directory (index.xr)
 *   "owner/name"      → third-party package under ~/.xray/packages/
 *   other quoted path → project-relative file or directory
 */

#ifndef XMODULE_RESOLVER_H
#define XMODULE_RESOLVER_H

#include <stdbool.h>
#include "../base/xdefs.h"
#include "../base/xhashmap.h"

/* Forward declarations */
struct XrVMRuntime;
struct XrProject;
struct XrLockfile;

/* ========== Module ID ========== */

/*
 * Canonical identifier for a resolved module.
 * - stdlib:  kind=STDLIB,  canonical="time" (bare name)
 * - file:    kind=FILE,    canonical="/abs/path/to/mod.xr"
 * - package: kind=PACKAGE, canonical="owner/name"
 */
typedef enum {
    XR_MOD_STDLIB,
    XR_MOD_FILE,
    XR_MOD_PACKAGE,
} XrModuleKind;

typedef struct {
    XrModuleKind kind;
    char *canonical;   /* Owned string — caller must xr_free() */
    char *source_path; /* Absolute .xr path, or NULL for native stdlib.
                          Owned string — caller must xr_free() */
} XrModuleId;

/* Free contents of an XrModuleId (does NOT free the struct itself). */
XR_FUNC void xr_module_id_cleanup(XrModuleId *id);

/* ========== Resolver Configuration ========== */

typedef struct {
    /*
     * Hashmap of stdlib module names registered as native loaders.
     * Keys are bare module names ("time", "io", …).  Values are
     * NativeModuleLoader function pointers but the resolver only
     * checks key existence — it never calls the loaders.
     * Borrowed pointer; caller must keep alive for resolver lifetime.
     */
    XrHashMap *native_loaders;

    /*
     * Optional stdlib source directory (e.g. "stdlib/").
     * When non-NULL the resolver also probes for stdlib/<name>/<name>.xr
     * script extensions. NULL means only native modules are valid.
     */
    const char *stdlib_path;

    /*
     * Optional lockfile for pinning third-party package versions.
     * Borrowed pointer; may be NULL.
     */
    struct XrLockfile *lockfile;
} XrModuleResolverConfig;

/* ========== Resolver Instance ========== */

typedef struct XrModuleResolver {
    XrModuleResolverConfig config;
    XrHashMap *cache; /* specifier+importer → XrModuleId (owned) */
} XrModuleResolver;

/* ========== Lifecycle ========== */

XR_FUNC XrModuleResolver *xr_module_resolver_new(const XrModuleResolverConfig *cfg);
XR_FUNC void xr_module_resolver_free(XrModuleResolver *r);

/* ========== Resolution API ========== */

/*
 * Resolve an import specifier to a canonical module id.
 *
 * @param r             Resolver instance
 * @param specifier     The import path string (without quotes)
 * @param is_bare_name  true if the specifier was an unquoted identifier
 * @param importer_path Absolute path of the importing file, or NULL for
 *                      entry scripts (uses cwd as base)
 * @param out_id        On success, filled with the resolved module info.
 *                      Caller must call xr_module_id_cleanup() when done.
 * @param err_buf       On failure, a human-readable error message is
 *                      written here (xr_malloc'd).  Caller must xr_free().
 *                      May be NULL if the caller doesn't need the message.
 * @return              0 on success, -1 on failure
 */
XR_FUNC int xr_module_resolver_resolve(XrModuleResolver *r, const char *specifier,
                                       bool is_bare_name, const char *importer_path,
                                       XrModuleId *out_id, char **err_buf);

#endif  // XMODULE_RESOLVER_H
