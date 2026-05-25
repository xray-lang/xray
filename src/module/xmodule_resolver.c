/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_resolver.c - Unified import specifier → source path resolver
 */

#include "xmodule_resolver.h"
#include "xlockfile.h"
#include "../base/xchecks.h"
#include "../base/xdefs.h"
#include "../base/xfileio.h"
#include "../base/xhashmap.h"
#include "../base/xmalloc.h"
#include "../os/os_dir.h"
#include "../os/os_fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

/* ========== Internal Helpers ========== */

/*
 * Build an error message from a printf-style format.
 * Returns xr_malloc'd string; caller must xr_free().
 */
static char *make_error(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return xr_strdup(buf);
}

/*
 * Probe for a file import: try <base>/<rel>.xr, then <base>/<rel>/index.xr.
 * Returns xr_malloc'd absolute path on success, NULL on failure.
 */
static char *probe_file_import(const char *base_dir, const char *rel_path) {
    XR_DCHECK(base_dir != NULL, "probe_file_import: NULL base_dir");
    XR_DCHECK(rel_path != NULL, "probe_file_import: NULL rel_path");

    char path[XR_PATH_MAX];

    /* Try <base>/<rel>.xr */
    snprintf(path, sizeof(path), "%s/%s.xr", base_dir, rel_path);
    if (xr_fs_exists(path)) {
        char *real = xr_realpath(path);
        return real ? real : xr_strdup(path);
    }

    /* Try <base>/<rel>/index.xr (directory entry) */
    snprintf(path, sizeof(path), "%s/%s/index.xr", base_dir, rel_path);
    if (xr_fs_exists(path)) {
        char *real = xr_realpath(path);
        return real ? real : xr_strdup(path);
    }

    return NULL;
}

/*
 * Check whether a specifier is a relative path (starts with ./ or ../).
 */
static bool is_relative_specifier(const char *spec) {
    return strncmp(spec, "./", 2) == 0 || strncmp(spec, "../", 3) == 0;
}

/* ========== Lifecycle ========== */

XrModuleResolver *xr_module_resolver_new(const XrModuleResolverConfig *cfg) {
    XR_DCHECK(cfg != NULL, "xr_module_resolver_new: NULL config");

    XrModuleResolver *r = xr_calloc(1, sizeof(XrModuleResolver));
    if (!r)
        return NULL;

    r->config = *cfg;
    r->cache = xr_hashmap_new();
    XR_DCHECK(r->cache != NULL, "xr_module_resolver_new: cache alloc failed");
    return r;
}

/* Callback to free cached XrModuleId entries during teardown. */
static void free_cached_entry(const char *key, void *value, void *userdata) {
    (void) key;
    (void) userdata;
    if (!value)
        return;
    XrModuleId *id = (XrModuleId *) value;
    xr_module_id_cleanup(id);
    xr_free(id);
}

void xr_module_resolver_free(XrModuleResolver *r) {
    if (!r)
        return;
    if (r->cache) {
        xr_hashmap_foreach(r->cache, free_cached_entry, NULL);
        xr_hashmap_free(r->cache);
    }
    xr_free(r);
}

void xr_module_id_cleanup(XrModuleId *id) {
    if (!id)
        return;
    if (id->canonical) {
        xr_free(id->canonical);
        id->canonical = NULL;
    }
    if (id->source_path) {
        xr_free(id->source_path);
        id->source_path = NULL;
    }
}

/* ========== Cache Key ========== */

/*
 * Build a cache key from specifier + importer path.
 * Format: "<importer_path>\0<specifier>" concatenation via separator '|'.
 * Returns xr_malloc'd string; caller must xr_free().
 */
static char *make_cache_key(const char *specifier, const char *importer_path) {
    const char *imp = importer_path ? importer_path : "<entry>";
    size_t imp_len = strlen(imp);
    size_t spec_len = strlen(specifier);
    char *key = xr_malloc(imp_len + 1 + spec_len + 1);
    if (!key)
        return NULL;
    memcpy(key, imp, imp_len);
    key[imp_len] = '|';
    memcpy(key + imp_len + 1, specifier, spec_len);
    key[imp_len + 1 + spec_len] = '\0';
    return key;
}

/* Copy an XrModuleId into a fresh heap allocation for caching. */
static XrModuleId *clone_module_id(const XrModuleId *src) {
    XrModuleId *dst = xr_malloc(sizeof(XrModuleId));
    if (!dst)
        return NULL;
    dst->kind = src->kind;
    dst->canonical = src->canonical ? xr_strdup(src->canonical) : NULL;
    dst->source_path = src->source_path ? xr_strdup(src->source_path) : NULL;
    return dst;
}

/* Copy a cached XrModuleId into the caller's out_id (which caller will cleanup). */
static void copy_module_id(XrModuleId *dst, const XrModuleId *src) {
    dst->kind = src->kind;
    dst->canonical = src->canonical ? xr_strdup(src->canonical) : NULL;
    dst->source_path = src->source_path ? xr_strdup(src->source_path) : NULL;
}

/* ========== Resolution: stdlib ========== */

static int resolve_stdlib(XrModuleResolver *r, const char *name, XrModuleId *out_id,
                          char **err_buf) {
    /* Check native loader registry */
    if (r->config.native_loaders && xr_hashmap_has(r->config.native_loaders, name)) {
        out_id->kind = XR_MOD_STDLIB;
        out_id->canonical = xr_strdup(name);
        out_id->source_path = NULL;

        /* Also check for script extension: stdlib/<name>/<name>.xr */
        if (r->config.stdlib_path) {
            char path[XR_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s/%s.xr", r->config.stdlib_path, name, name);
            if (xr_fs_exists(path)) {
                char *real = xr_realpath(path);
                out_id->source_path = real ? real : xr_strdup(path);
            }
        }
        return 0;
    }

    /* Not a known stdlib module */
    if (err_buf)
        *err_buf = make_error("module '%s' not found in stdlib", name);
    return -1;
}

/* ========== Resolution: relative file/directory ========== */

static int resolve_relative(XrModuleResolver *r, const char *specifier, const char *importer_path,
                            XrModuleId *out_id, char **err_buf) {
    (void) r;

    /* Determine base directory from importer path */
    char *base_dir = NULL;
    if (importer_path) {
        base_dir = xr_path_dirname(importer_path);
    } else {
        /* Entry script: use cwd */
        char cwd[XR_PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            base_dir = xr_strdup(cwd);
        }
    }

    if (!base_dir) {
        if (err_buf)
            *err_buf =
                make_error("cannot determine base directory for relative import '%s'", specifier);
        return -1;
    }

    char *resolved = probe_file_import(base_dir, specifier);
    xr_free(base_dir);

    if (!resolved) {
        if (err_buf)
            *err_buf = make_error("module '%s' not found (tried .xr and /index.xr)", specifier);
        return -1;
    }

    out_id->kind = XR_MOD_FILE;
    out_id->canonical = resolved; /* Already absolute (realpath) */
    out_id->source_path = xr_strdup(resolved);
    return 0;
}

/* ========== Resolution: third-party package ========== */

static int resolve_package(XrModuleResolver *r, const char *specifier, XrModuleId *out_id,
                           char **err_buf) {
    /* Parse owner/name */
    char owner[64], name[64];
    if (sscanf(specifier, "%63[^/]/%63s", owner, name) != 2) {
        if (err_buf)
            *err_buf = make_error("invalid package specifier '%s'", specifier);
        return -1;
    }

    const char *home = getenv("HOME");
    if (!home) {
        if (err_buf)
            *err_buf = make_error("HOME not set; cannot locate package '%s'", specifier);
        return -1;
    }

    /* Try lockfile version first */
    const char *version = NULL;
    if (r->config.lockfile) {
        const XrLockedPackage *lp = xr_lockfile_find(r->config.lockfile, specifier);
        if (lp && lp->version)
            version = lp->version;
    }

    char path[XR_PATH_MAX];

    if (version) {
        /* Exact version from lockfile */
        const char *entries[] = {"src/main.xr", "main.xr"};
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/%s/%s", home, owner, name,
                     version, entries[i]);
            if (xr_fs_exists(path)) {
                char *real = xr_realpath(path);
                out_id->kind = XR_MOD_PACKAGE;
                out_id->canonical = xr_strdup(specifier);
                out_id->source_path = real ? real : xr_strdup(path);
                return 0;
            }
        }
        /* Try <name>.xr entry */
        snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/%s/%s.xr", home, owner, name, version,
                 name);
        if (xr_fs_exists(path)) {
            char *real = xr_realpath(path);
            out_id->kind = XR_MOD_PACKAGE;
            out_id->canonical = xr_strdup(specifier);
            out_id->source_path = real ? real : xr_strdup(path);
            return 0;
        }
    }

    /* Fallback: scan version directories under ~/.xray/packages/owner/name/ */
    char pkg_base[XR_PATH_MAX];
    snprintf(pkg_base, sizeof(pkg_base), "%s/.xray/packages/%s/%s", home, owner, name);
    XrDirIter *vdir = xr_dir_open(pkg_base);
    if (vdir) {
        XrDirEntry ve;
        while (xr_dir_next(vdir, &ve)) {
            if (ve.name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s/src/main.xr", pkg_base, ve.name);
            if (xr_fs_exists(path)) {
                xr_dir_close(vdir);
                char *real = xr_realpath(path);
                out_id->kind = XR_MOD_PACKAGE;
                out_id->canonical = xr_strdup(specifier);
                out_id->source_path = real ? real : xr_strdup(path);
                return 0;
            }
            snprintf(path, sizeof(path), "%s/%s/main.xr", pkg_base, ve.name);
            if (xr_fs_exists(path)) {
                xr_dir_close(vdir);
                char *real = xr_realpath(path);
                out_id->kind = XR_MOD_PACKAGE;
                out_id->canonical = xr_strdup(specifier);
                out_id->source_path = real ? real : xr_strdup(path);
                return 0;
            }
        }
        xr_dir_close(vdir);
    }

    if (err_buf)
        *err_buf =
            make_error("package '%s' not found; run 'xray pkg add %s'", specifier, specifier);
    return -1;
}

/* ========== Resolution: project-relative path ========== */

/*
 * Non-relative quoted paths that are not packages (no slash or only one
 * segment) are resolved relative to the project root or cwd.
 */
static int resolve_project_relative(XrModuleResolver *r, const char *specifier,
                                    const char *importer_path, XrModuleId *out_id, char **err_buf) {
    (void) r;

    /* Use importer's directory as fallback project root */
    char *base_dir = NULL;
    if (importer_path) {
        base_dir = xr_path_dirname(importer_path);
    } else {
        char cwd[XR_PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)))
            base_dir = xr_strdup(cwd);
    }

    if (!base_dir) {
        if (err_buf)
            *err_buf = make_error("cannot determine base directory for import '%s'", specifier);
        return -1;
    }

    char *resolved = probe_file_import(base_dir, specifier);
    xr_free(base_dir);

    if (resolved) {
        out_id->kind = XR_MOD_FILE;
        out_id->canonical = resolved;
        out_id->source_path = xr_strdup(resolved);
        return 0;
    }

    if (err_buf)
        *err_buf = make_error("module '%s' not found", specifier);
    return -1;
}

/* ========== Main Resolution Entry ========== */

int xr_module_resolver_resolve(XrModuleResolver *r, const char *specifier, bool is_bare_name,
                               const char *importer_path, XrModuleId *out_id, char **err_buf) {
    XR_DCHECK(r != NULL, "xr_module_resolver_resolve: NULL resolver");
    XR_DCHECK(specifier != NULL, "xr_module_resolver_resolve: NULL specifier");
    XR_DCHECK(out_id != NULL, "xr_module_resolver_resolve: NULL out_id");

    memset(out_id, 0, sizeof(*out_id));
    if (err_buf)
        *err_buf = NULL;

    /* Check cache */
    char *cache_key = make_cache_key(specifier, importer_path);
    if (cache_key) {
        XrModuleId *cached = (XrModuleId *) xr_hashmap_get(r->cache, cache_key);
        if (cached) {
            copy_module_id(out_id, cached);
            xr_free(cache_key);
            return 0;
        }
    }

    int rc;

    if (is_bare_name) {
        /* Bare identifier: stdlib only */
        rc = resolve_stdlib(r, specifier, out_id, err_buf);
    } else if (is_relative_specifier(specifier)) {
        /* "./" or "../" prefix: file/directory import */
        rc = resolve_relative(r, specifier, importer_path, out_id, err_buf);
    } else if (strchr(specifier, '/') != NULL) {
        /* Contains slash but not relative: try package first, then project-relative */
        rc = resolve_package(r, specifier, out_id, NULL);
        if (rc != 0)
            rc = resolve_project_relative(r, specifier, importer_path, out_id, err_buf);
    } else {
        /* Single segment, quoted: try as project-relative path */
        rc = resolve_project_relative(r, specifier, importer_path, out_id, err_buf);
    }

    /* Cache on success */
    if (rc == 0 && cache_key) {
        XrModuleId *to_cache = clone_module_id(out_id);
        if (to_cache) {
            xr_hashmap_set(r->cache, cache_key, to_cache);
            /* cache_key now owned by hashmap */
            return 0;
        }
    }

    if (cache_key)
        xr_free(cache_key);

    return rc;
}
