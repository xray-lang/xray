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
#include "../os/os_fs.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
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

static bool resolver_copy_authority(XrModuleResolver *r,
                                    const XrModuleIdentityAuthority *authority) {
    char *namespace_copy = NULL;
    char *root_copy = NULL;
    if (authority && authority->namespace_id) {
        namespace_copy = xr_strdup(authority->namespace_id);
        if (!namespace_copy)
            return false;
    }
    if (authority && authority->physical_root) {
        root_copy = xr_strdup(authority->physical_root);
        if (!root_copy) {
            xr_free(namespace_copy);
            return false;
        }
    }
    xr_free(r->authority_namespace);
    xr_free(r->authority_root);
    r->authority_namespace = namespace_copy;
    r->authority_root = root_copy;
    memset(&r->config.authority, 0, sizeof(r->config.authority));
    if (authority) {
        r->config.authority.kind = authority->kind;
        r->config.authority.namespace_id = r->authority_namespace;
        r->config.authority.physical_root = r->authority_root;
    }
    return true;
}

XrModuleResolver *xr_module_resolver_new(const XrModuleResolverConfig *cfg) {
    XR_DCHECK(cfg != NULL, "xr_module_resolver_new: NULL config");

    XrModuleResolver *r = xr_calloc(1, sizeof(XrModuleResolver));
    if (!r)
        return NULL;

    r->config = *cfg;
    memset(&r->config.authority, 0, sizeof(r->config.authority));
    if (!resolver_copy_authority(r, &cfg->authority)) {
        xr_free(r);
        return NULL;
    }
    r->cache = xr_hashmap_new();
    if (!r->cache) {
        xr_free(r->authority_namespace);
        xr_free(r->authority_root);
        xr_free(r);
        return NULL;
    }
    return r;
}

bool xr_module_resolver_set_authority(XrModuleResolver *r,
                                      const XrModuleIdentityAuthority *authority) {
    if (!r || !authority || !authority->physical_root || xr_hashmap_count(r->cache) != 0)
        return false;
    return resolver_copy_authority(r, authority);
}

bool xr_module_resolver_set_lockfile(XrModuleResolver *r, XrLockfile *lockfile) {
    if (!r || xr_hashmap_count(r->cache) != 0)
        return false;
    r->config.lockfile = lockfile;
    return true;
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
    xr_free(r->authority_namespace);
    xr_free(r->authority_root);
    xr_free(r);
}

void xr_module_id_cleanup(XrModuleId *id) {
    if (!id)
        return;
    if (id->canonical) {
        xr_free(id->canonical);
        id->canonical = NULL;
    }
    if (id->logical_path) {
        xr_free(id->logical_path);
        id->logical_path = NULL;
    }
    if (id->source_path) {
        xr_free(id->source_path);
        id->source_path = NULL;
    }
    xr_free((char *) id->authority.namespace_id);
    xr_free((char *) id->authority.physical_root);
    memset(&id->authority, 0, sizeof(id->authority));
}

/* ========== Cache Key ========== */

/* Build a cache key from the durable importer identity, never its physical path. */
static char *make_cache_key(const char *specifier, const char *importer_path,
                            const XrModuleIdentityAuthority *authority) {
    char *importer_identity = NULL;
    char *importer_logical = NULL;
    if (is_relative_specifier(specifier) &&
        (!importer_path || !authority ||
         !xr_module_identity_from_source(authority, importer_path, &importer_identity,
                                         &importer_logical)))
        return NULL;
    const char *imp = importer_identity ? importer_identity : "named-module-v1";
    size_t imp_len = strlen(imp);
    size_t spec_len = strlen(specifier);
    if (imp_len > SIZE_MAX - spec_len - 2) {
        xr_free(importer_identity);
        xr_free(importer_logical);
        return NULL;
    }
    char *key = xr_malloc(imp_len + 1 + spec_len + 1);
    if (!key) {
        xr_free(importer_identity);
        xr_free(importer_logical);
        return NULL;
    }
    memcpy(key, imp, imp_len);
    key[imp_len] = '|';
    memcpy(key + imp_len + 1, specifier, spec_len);
    key[imp_len + 1 + spec_len] = '\0';
    xr_free(importer_identity);
    xr_free(importer_logical);
    return key;
}

/* Copy an XrModuleId into a fresh heap allocation for caching. */
static XrModuleId *clone_module_id(const XrModuleId *src) {
    XrModuleId *dst = xr_malloc(sizeof(XrModuleId));
    if (!dst)
        return NULL;
    dst->kind = src->kind;
    dst->canonical = src->canonical ? xr_strdup(src->canonical) : NULL;
    dst->logical_path = src->logical_path ? xr_strdup(src->logical_path) : NULL;
    dst->source_path = src->source_path ? xr_strdup(src->source_path) : NULL;
    dst->authority.kind = src->authority.kind;
    dst->authority.namespace_id =
        src->authority.namespace_id ? xr_strdup(src->authority.namespace_id) : NULL;
    dst->authority.physical_root =
        src->authority.physical_root ? xr_strdup(src->authority.physical_root) : NULL;
    return dst;
}

/* Copy a cached XrModuleId into the caller's out_id (which caller will cleanup). */
static void copy_module_id(XrModuleId *dst, const XrModuleId *src) {
    dst->kind = src->kind;
    dst->canonical = src->canonical ? xr_strdup(src->canonical) : NULL;
    dst->logical_path = src->logical_path ? xr_strdup(src->logical_path) : NULL;
    dst->source_path = src->source_path ? xr_strdup(src->source_path) : NULL;
    dst->authority.kind = src->authority.kind;
    dst->authority.namespace_id =
        src->authority.namespace_id ? xr_strdup(src->authority.namespace_id) : NULL;
    dst->authority.physical_root =
        src->authority.physical_root ? xr_strdup(src->authority.physical_root) : NULL;
}

/* ========== Resolution: stdlib ========== */

static int resolve_stdlib(XrModuleResolver *r, const char *name, XrModuleId *out_id,
                          char **err_buf) {
    /* Check native factory registry. */
    if (r->config.native_factories && xr_hashmap_has(r->config.native_factories, name)) {
        out_id->kind = XR_MOD_STDLIB;
        out_id->canonical = xr_strdup(name);
        out_id->logical_path = xr_strdup(name);
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
                            const XrModuleIdentityAuthority *importer_authority,
                            XrModuleId *out_id, char **err_buf) {
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

    const XrModuleIdentityAuthority *authority =
        importer_authority ? importer_authority : &r->config.authority;
    out_id->kind = authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
    if (!xr_module_identity_from_source(authority, resolved, &out_id->canonical,
                                        &out_id->logical_path)) {
        if (err_buf)
            *err_buf = make_error("module '%s' escapes or lacks its identity authority",
                                  specifier);
        xr_free(resolved);
        return -1;
    }
    out_id->source_path = resolved;
    out_id->authority.kind = authority->kind;
    out_id->authority.namespace_id = authority->namespace_id
                                         ? xr_strdup(authority->namespace_id)
                                         : NULL;
    out_id->authority.physical_root = xr_strdup(authority->physical_root);
    if (!out_id->source_path || !out_id->authority.physical_root ||
        (authority->namespace_id && !out_id->authority.namespace_id)) {
        xr_module_id_cleanup(out_id);
        if (err_buf)
            *err_buf = make_error("out of memory resolving module '%s'", specifier);
        return -1;
    }
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
    if (strchr(name, '/')) {
        if (err_buf)
            *err_buf = make_error("invalid package specifier '%s'", specifier);
        return -1;
    }

    const XrLockedPackage *locked =
        r->config.lockfile ? xr_lockfile_find(r->config.lockfile, specifier) : NULL;
    bool checksum_valid = locked && locked->checksum && strlen(locked->checksum) == 71 &&
                          strncmp(locked->checksum, "sha256:", 7) == 0;
    for (size_t i = 7; checksum_valid && i < 71; i++)
        checksum_valid = isxdigit((unsigned char) locked->checksum[i]) != 0;
    if (!locked || !locked->version || !locked->version[0] || !checksum_valid) {
        if (err_buf)
            *err_buf = make_error("package '%s' requires an exact checksummed xray.lock entry",
                                  specifier);
        return -1;
    }
    const char *version = locked->version;

    const char *home = getenv("HOME");
#ifdef XR_OS_WINDOWS
    if (!home)
        home = getenv("USERPROFILE");
#endif
    if (!home) {
        if (err_buf)
            *err_buf = make_error("HOME not set; cannot locate package '%s'", specifier);
        return -1;
    }

    char path[XR_PATH_MAX];

    char package_root[XR_PATH_MAX];
    int root_length = snprintf(package_root, sizeof(package_root), "%s/.xray/packages/%s/%s/%s",
                               home, owner, name, version);
    char namespace_id[256];
    int namespace_length =
        snprintf(namespace_id, sizeof(namespace_id), "%s@%s", specifier, version);
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_PACKAGE,
        .namespace_id = namespace_id,
        .physical_root = package_root,
    };
    if (root_length < 0 || (size_t) root_length >= sizeof(package_root) || namespace_length < 0 ||
        (size_t) namespace_length >= sizeof(namespace_id) ||
        !xr_module_identity_authority_valid(&authority)) {
        if (err_buf)
            *err_buf = make_error("package '%s' has an invalid locked identity", specifier);
        return -1;
    }

    const char *entries[] = {"src/main.xr", "main.xr"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/%s/%s", home, owner, name, version,
                 entries[i]);
        if (xr_fs_exists(path)) {
            char *real = xr_realpath(path);
            out_id->kind = XR_MOD_PACKAGE;
            out_id->source_path = real ? real : xr_strdup(path);
            if (out_id->source_path &&
                xr_module_identity_from_source(&authority, out_id->source_path,
                                               &out_id->canonical, &out_id->logical_path)) {
                out_id->authority.kind = authority.kind;
                out_id->authority.namespace_id = xr_strdup(namespace_id);
                out_id->authority.physical_root = xr_strdup(package_root);
                if (out_id->authority.namespace_id && out_id->authority.physical_root)
                    return 0;
            }
            xr_module_id_cleanup(out_id);
            if (err_buf)
                *err_buf = make_error("package '%s' has an invalid identity root", specifier);
            return -1;
        }
    }
    snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/%s/%s.xr", home, owner, name, version,
             name);
    if (xr_fs_exists(path)) {
        char *real = xr_realpath(path);
        out_id->kind = XR_MOD_PACKAGE;
        out_id->source_path = real ? real : xr_strdup(path);
        if (out_id->source_path &&
            xr_module_identity_from_source(&authority, out_id->source_path, &out_id->canonical,
                                           &out_id->logical_path)) {
            out_id->authority.kind = authority.kind;
            out_id->authority.namespace_id = xr_strdup(namespace_id);
            out_id->authority.physical_root = xr_strdup(package_root);
            if (out_id->authority.namespace_id && out_id->authority.physical_root)
                return 0;
        }
        xr_module_id_cleanup(out_id);
        if (err_buf)
            *err_buf = make_error("package '%s' has an invalid identity root", specifier);
        return -1;
    }

    if (err_buf)
        *err_buf =
            make_error("package '%s' not found; run 'xray pkg add %s'", specifier, specifier);
    return -1;
}

/* ========== Resolution: project-relative path ========== */

/* ========== Main Resolution Entry ========== */

int xr_module_resolver_resolve(XrModuleResolver *r, const char *specifier,
                               const char *importer_path,
                               const XrModuleIdentityAuthority *importer_authority,
                               XrModuleId *out_id, char **err_buf) {
    XR_DCHECK(r != NULL, "xr_module_resolver_resolve: NULL resolver");
    XR_DCHECK(specifier != NULL, "xr_module_resolver_resolve: NULL specifier");
    XR_DCHECK(out_id != NULL, "xr_module_resolver_resolve: NULL out_id");

    memset(out_id, 0, sizeof(*out_id));
    if (err_buf)
        *err_buf = NULL;

    const XrModuleIdentityAuthority *effective_authority =
        importer_authority ? importer_authority : &r->config.authority;
    /* Check cache */
    char *cache_key = make_cache_key(specifier, importer_path, effective_authority);
    if (cache_key) {
        XrModuleId *cached = (XrModuleId *) xr_hashmap_get(r->cache, cache_key);
        if (cached) {
            copy_module_id(out_id, cached);
            xr_free(cache_key);
            return 0;
        }
    }

    int rc;

    /* The specifier's shape decides what it is, and the three shapes do not
     * overlap. `is_bare_name` used to carry that decision from the caller and
     * disagreed with the text often enough to matter -- `"math"` arrived as
     * bare and resolved to the standard library.
     *
     * The project-root form is gone with it. It shared its shape with a package
     * exactly, and the two were told apart by which resolver happened to
     * succeed first, so adding a directory to a project could take over a
     * package import and adding a dependency could take over a directory one.
     * Nothing in the tree used it. */
    if (is_relative_specifier(specifier)) {
        rc = resolve_relative(r, specifier, importer_path, importer_authority, out_id, err_buf);
    } else if (strchr(specifier, '/') != NULL) {
        rc = resolve_package(r, specifier, out_id, err_buf);
    } else {
        /* A named module: the standard library and .xrd-declared native
         * modules share this one namespace. */
        rc = resolve_stdlib(r, specifier, out_id, err_buf);
    }

    /* Cache on success. The cache is an optimization: on OOM just skip
     * caching, resolution itself already succeeded. */
    if (rc == 0 && cache_key) {
        XrModuleId *to_cache = clone_module_id(out_id);
        if (to_cache) {
            if (xr_hashmap_set(r->cache, cache_key, to_cache)) {
                /* cache_key now owned by hashmap */
                return 0;
            }
            xr_module_id_cleanup(to_cache);
            xr_free(to_cache);
        }
    }

    if (cache_key)
        xr_free(cache_key);

    return rc;
}
