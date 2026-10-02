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
#include "xstdlib_embedded.h"
#include "xlockfile.h"
#include "xsemver.h"
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

/* Probe the two source spellings and publish an owned path only on success. */
static XrModuleStatus probe_file_import(const char *base_dir, const char *rel_path, char **output) {
    char path[XR_PATH_MAX];
    const char *formats[] = {"%s/%s.xr", "%s/%s/index.xr"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        int length = snprintf(path, sizeof(path), formats[i], base_dir, rel_path);
        if (length < 0) return XR_MODULE_INVALID;
        if ((size_t) length >= sizeof(path)) return XR_MODULE_BUDGET;
        if (xr_fs_exists(path)) {
            XrPathStatus status;
            *output = xr_realpath(path, &status);
            return xr_module_status_from_path(status);
        }
    }
    return XR_MODULE_NOT_FOUND;
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
    if (!r->cache) {
        xr_free(r);
        return NULL;
    }
    return r;
}

bool xr_module_resolver_set_lockfile(XrModuleResolver *r, XrLockfile *lockfile) {
    if (!r || xr_hashmap_count(r->cache) != 0)
        return false;
    r->config.lockfile = lockfile;
    return true;
}

/* Frees one cached entry during teardown. The map never copies or owns a key,
 * so the resolver that built this one is the only owner left to release it. */
static void free_cached_entry(const char *key, void *value, void *userdata) {
    (void) userdata;
    xr_free((char *) key);
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
    id->resource = NULL; id->representation = XR_MODULE_SOURCE;
}

/* ========== Cache Key ========== */

/* Build a cache key from the durable importer identity, never its physical path. */
static XrModuleStatus make_cache_key(const char *specifier, const char *importer_path,
    const XrModuleIdentityAuthority *authority, char **output) {
    char *identity = NULL, *logical = NULL;
    if (is_relative_specifier(specifier) && !importer_path) return XR_MODULE_OK;
    if (is_relative_specifier(specifier)) {
        XrModuleStatus status = xr_module_identity_from_source(authority, importer_path, &identity, &logical);
        if (status != XR_MODULE_OK) return status;
    }
    const char *importer = identity ? identity : "named-module-v1";
    size_t importer_length = strlen(importer), length = strlen(specifier);
    XrModuleStatus status = XR_MODULE_BUDGET;
    if (length <= SIZE_MAX - 2 && importer_length <= SIZE_MAX - length - 2) {
        char *key = xr_malloc(importer_length + length + 2);
        status = key ? XR_MODULE_OK : XR_MODULE_OUT_OF_MEMORY;
        if (key) {
            memcpy(key, importer, importer_length); key[importer_length] = '|';
            memcpy(key + importer_length + 1, specifier, length + 1); *output = key;
        }
    }
    xr_free(identity); xr_free(logical); return status;
}

static XrModuleStatus copy_module_id(XrModuleId *output, const XrModuleId *source) {
    XrModuleId owned = {0};
    owned.kind = source->kind;
    owned.canonical = source->canonical ? xr_strdup(source->canonical) : NULL;
    owned.logical_path = source->logical_path ? xr_strdup(source->logical_path) : NULL;
    owned.source_path = source->source_path ? xr_strdup(source->source_path) : NULL;
    owned.authority.kind = source->authority.kind;
    owned.authority.namespace_id = source->authority.namespace_id ? xr_strdup(source->authority.namespace_id) : NULL;
    owned.authority.physical_root = source->authority.physical_root ? xr_strdup(source->authority.physical_root) : NULL;
    owned.representation = source->representation; owned.resource = source->resource;
    if ((source->canonical && !owned.canonical) || (source->logical_path && !owned.logical_path) ||
        (source->source_path && !owned.source_path) ||
        (source->authority.namespace_id && !owned.authority.namespace_id) ||
        (source->authority.physical_root && !owned.authority.physical_root)) {
        xr_module_id_cleanup(&owned); return XR_MODULE_OUT_OF_MEMORY;
    }
    *output = owned; return XR_MODULE_OK;
}

/* ========== Resolution: stdlib ========== */

static XrModuleStatus resolve_stdlib(XrModuleResolver *r, const char *name, XrModuleId *out_id,
    char **err_buf) {
    if (!xr_stdlib_module_descriptor(name)) {
        if (err_buf) *err_buf = make_error("module '%s' not found in stdlib", name);
        return XR_MODULE_NOT_FOUND;
    }
    char logical[XR_PATH_MAX];
    int length = snprintf(logical, sizeof(logical), "%s/%s.xr", name, name);
    if (length < 0) return XR_MODULE_INVALID;
    if ((size_t) length >= sizeof(logical)) return XR_MODULE_BUDGET;
    XrModuleStatus status = XR_MODULE_OK;
    out_id->kind = XR_MOD_STDLIB;
    out_id->authority.kind = XR_MODULE_IDENTITY_STDLIB;
    out_id->authority.namespace_id = xr_strdup(name);
    if (!out_id->authority.namespace_id) { status = XR_MODULE_OUT_OF_MEMORY; goto failed; }
    if (r->config.stdlib_path) {
        char path[XR_PATH_MAX];
        length = snprintf(path, sizeof(path), "%s/%s", r->config.stdlib_path, logical);
        if (length < 0) { status = XR_MODULE_INVALID; goto failed; }
        if ((size_t) length >= sizeof(path)) { status = XR_MODULE_BUDGET; goto failed; }
        if (xr_fs_exists(path)) {
            XrPathStatus path_status;
            out_id->source_path = xr_realpath(path, &path_status);
            if (!out_id->source_path) { status = xr_module_status_from_path(path_status); goto failed; }
            out_id->authority.physical_root = xr_realpath(r->config.stdlib_path, &path_status);
            if (!out_id->authority.physical_root) { status = xr_module_status_from_path(path_status); goto failed; }
        }
    }
    status = xr_module_identity_from_logical(&out_id->authority, logical, &out_id->canonical);
    if (status != XR_MODULE_OK) goto failed;
    out_id->logical_path = xr_strdup(logical);
    if (out_id->logical_path) return XR_MODULE_OK;
    status = XR_MODULE_OUT_OF_MEMORY;
failed:
    xr_module_id_cleanup(out_id); return status;
}

/* ========== Resolution: relative file/directory ========== */
static bool stdlib_submodule_path(const char *path, char *name, size_t capacity) {
    const char *slash = strchr(path, '/');
    if (!slash || slash == path || (size_t) (slash - path) >= capacity) return false;
    memcpy(name, path, (size_t) (slash - path)); name[slash - path] = '\0';
    bool first = true;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') { if (first) return false; first = true; continue; }
        bool letter = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_';
        if (!letter && (first || *p < '0' || *p > '9')) return false;
        first = false;
    }
    return !first && strcmp(slash + 1, name) && xr_stdlib_module_descriptor(name);
}

static XrModuleStatus resolve_stdlib_submodule(XrModuleResolver *r, const char *specifier,
    XrModuleId *out_id, char **err_buf) {
    const char *relative = specifier + 4;
    char name[256], logical[XR_PATH_MAX], path[XR_PATH_MAX];
    XrModuleStatus status = XR_MODULE_INVALID;
    if (!stdlib_submodule_path(relative, name, sizeof(name))) goto failed;
    if (!r->config.stdlib_path) { status = XR_MODULE_NOT_FOUND; goto failed; }
    int length = snprintf(logical, sizeof(logical), "%s.xr", relative);
    if (length < 0) goto failed;
    if ((size_t) length >= sizeof(logical)) { status = XR_MODULE_BUDGET; goto failed; }
    length = snprintf(path, sizeof(path), "%s/%s", r->config.stdlib_path, logical);
    if (length < 0) goto failed;
    if ((size_t) length >= sizeof(path)) { status = XR_MODULE_BUDGET; goto failed; }
    if (!xr_fs_exists(path)) { status = XR_MODULE_NOT_FOUND; goto failed; }
    XrPathStatus path_status;
    out_id->kind = XR_MOD_STDLIB;
    out_id->source_path = xr_realpath(path, &path_status);
    if (!out_id->source_path) { status = xr_module_status_from_path(path_status); goto failed; }
    out_id->authority.kind = XR_MODULE_IDENTITY_STDLIB;
    out_id->authority.namespace_id = xr_strdup(name);
    if (!out_id->authority.namespace_id) { status = XR_MODULE_OUT_OF_MEMORY; goto failed; }
    out_id->authority.physical_root = xr_realpath(r->config.stdlib_path, &path_status);
    if (!out_id->authority.physical_root) { status = xr_module_status_from_path(path_status); goto failed; }
    status = xr_module_identity_from_source(&out_id->authority, out_id->source_path,
        &out_id->canonical, &out_id->logical_path);
    if (status == XR_MODULE_OK && strcmp(out_id->logical_path, logical)) status = XR_MODULE_INVALID;
    if (status == XR_MODULE_OK) return status;
failed:
    xr_module_id_cleanup(out_id);
    if (err_buf) *err_buf = make_error("stdlib source submodule '%s' lacks an exact authorized source", specifier);
    return status;
}

static XrModuleStatus resolve_relative(const char *specifier, const char *importer_path,
                            const XrModuleIdentityAuthority *importer_authority, XrModuleId *out_id,
                            char **err_buf) {
    const XrModuleIdentityAuthority *authority = importer_authority;
    if (!xr_module_identity_authority_valid(authority) ||
        (authority->kind != XR_MODULE_IDENTITY_PROJECT &&
         authority->kind != XR_MODULE_IDENTITY_SCRIPT &&
         authority->kind != XR_MODULE_IDENTITY_PACKAGE)) {
        if (err_buf)
            *err_buf =
                make_error("relative import '%s' requires explicit source authority", specifier);
        return -1;
    }
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
        return importer_path ? XR_MODULE_OUT_OF_MEMORY : XR_MODULE_IO;
    }

    char *resolved = NULL;
    XrModuleStatus status = probe_file_import(base_dir, specifier, &resolved);
    xr_free(base_dir);

    if (!resolved) {
        if (err_buf)
            *err_buf = make_error("module '%s' not found (tried .xr and /index.xr)", specifier);
        return status;
    }

    out_id->kind = authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
    status = xr_module_identity_from_source(authority, resolved, &out_id->canonical,
        &out_id->logical_path);
    if (status != XR_MODULE_OK) {
        if (err_buf)
            *err_buf = make_error("module '%s' escapes or lacks its identity authority", specifier);
        xr_free(resolved);
        return status;
    }
    out_id->source_path = resolved;
    out_id->authority.kind = authority->kind;
    out_id->authority.namespace_id =
        authority->namespace_id ? xr_strdup(authority->namespace_id) : NULL;
    out_id->authority.physical_root = xr_strdup(authority->physical_root);
    if (!out_id->source_path || !out_id->authority.physical_root ||
        (authority->namespace_id && !out_id->authority.namespace_id)) {
        xr_module_id_cleanup(out_id);
        if (err_buf)
            *err_buf = make_error("out of memory resolving module '%s'", specifier);
        return XR_MODULE_OUT_OF_MEMORY;
    }
    return 0;
}

/* ========== Resolution: third-party package ========== */

static XrModuleStatus resolve_package_source(const XrModuleIdentityAuthority *authority,
    const char *path, XrModuleId *out_id) {
    XrPathStatus path_status;
    out_id->source_path = xr_realpath(path, &path_status);
    if (!out_id->source_path) return xr_module_status_from_path(path_status);
    out_id->kind = XR_MOD_PACKAGE;
    XrModuleStatus status = xr_module_identity_from_source(authority, out_id->source_path,
        &out_id->canonical, &out_id->logical_path);
    if (status == XR_MODULE_OK) {
        out_id->authority.kind = authority->kind;
        out_id->authority.namespace_id = xr_strdup(authority->namespace_id);
        out_id->authority.physical_root = xr_strdup(authority->physical_root);
        if (!out_id->authority.namespace_id || !out_id->authority.physical_root)
            status = XR_MODULE_OUT_OF_MEMORY;
    }
    if (status != XR_MODULE_OK) xr_module_id_cleanup(out_id);
    return status;
}

static XrModuleStatus resolve_package(XrModuleResolver *r, const char *specifier, XrModuleId *out_id,
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
    if (!locked || !locked->version || !xr_semver_is_valid(locked->version) || !checksum_valid) {
        if (err_buf)
            *err_buf =
                make_error("package '%s' requires an exact checksummed xray.lock entry", specifier);
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

    char archive_path[XR_PATH_MAX];
    int archive_length = snprintf(archive_path, sizeof(archive_path),
                                  "%s/.xray/cache/%s-%s-%s.tar.gz", home, owner, name, version);
    if (archive_length < 0 || (size_t) archive_length >= sizeof(archive_path)) {
        if (err_buf)
            *err_buf = make_error("package '%s' checksum authority path is too long", specifier);
        return -1;
    }
    if (!xr_fs_is_file(archive_path)) {
        if (err_buf)
            *err_buf = make_error(
                "package '%s' exact archive is unavailable for checksum verification", specifier);
        return -1;
    }
    if (!xr_lockfile_verify_checksum(archive_path, locked->checksum)) {
        if (err_buf)
            *err_buf =
                make_error("package '%s' checksum does not match its cached archive", specifier);
        return -1;
    }

    char path[XR_PATH_MAX];

    char package_root[XR_PATH_MAX];
    int root_length = snprintf(package_root, sizeof(package_root), "%s/.xray/packages/%s/%s/%s",
                               home, owner, name, version);
    /* Every source path below is canonicalized before the identity authority
     * sees it, and containment is a byte comparison, so a root reached through
     * a symlinked home would make the package escape a root that is its own.
     * Canonicalization failures retain their typed cause rather than falling
     * back to an unverified physical path. */
    if (root_length > 0 && (size_t) root_length < sizeof(package_root)) {
        XrPathStatus path_status;
        char *canonical_root = xr_realpath(package_root, &path_status);
        if (!canonical_root) return xr_module_status_from_path(path_status);
        root_length = snprintf(package_root, sizeof(package_root), "%s", canonical_root);
        xr_free(canonical_root);
    }
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
        int length = snprintf(path, sizeof(path), "%s/%s", package_root, entries[i]);
        if (length < 0) return XR_MODULE_INVALID;
        if ((size_t) length >= sizeof(path)) return XR_MODULE_BUDGET;
        if (xr_fs_exists(path)) return resolve_package_source(&authority, path, out_id);
    }
    int length = snprintf(path, sizeof(path), "%s/%s.xr", package_root, name);
    if (length < 0) return XR_MODULE_INVALID;
    if ((size_t) length >= sizeof(path)) return XR_MODULE_BUDGET;
    if (xr_fs_exists(path)) return resolve_package_source(&authority, path, out_id);

    if (err_buf)
        *err_buf =
            make_error("package '%s' not found; run 'xray pkg add %s'", specifier, specifier);
    return XR_MODULE_NOT_FOUND;
}

/* ========== Resolution: project-relative path ========== */

#include "xmodule_checked_resource.inc.c"

/* ========== Main Resolution Entry ========== */

XrModuleStatus xr_module_resolver_resolve(XrModuleResolver *r, const char *specifier,
                               const char *importer_path,
                               const XrModuleIdentityAuthority *importer_authority,
                               XrModuleId *out_id, char **err_buf) {
    XR_DCHECK(r != NULL, "xr_module_resolver_resolve: NULL resolver");
    XR_DCHECK(specifier != NULL, "xr_module_resolver_resolve: NULL specifier");
    XR_DCHECK(out_id != NULL, "xr_module_resolver_resolve: NULL out_id");

    memset(out_id, 0, sizeof(*out_id));
    if (err_buf)
        *err_buf = NULL;

    bool matched = false;
    XrModuleStatus resource_status = resolve_checked_resource(r, specifier, importer_path,
        importer_authority, out_id, err_buf, &matched);
    if (resource_status != XR_MODULE_OK || matched) return resource_status;
    /* Check cache */
    char *cache_key = NULL;
    XrModuleStatus key_status = make_cache_key(specifier, importer_path, importer_authority, &cache_key);
    if (key_status != XR_MODULE_OK) return key_status;
    if (cache_key) {
        XrModuleId *cached = (XrModuleId *) xr_hashmap_get(r->cache, cache_key);
        if (cached) {
            XrModuleStatus status = copy_module_id(out_id, cached);
            xr_free(cache_key);
            return status;
        }
    }

    XrModuleStatus rc;

    /* The specifier's shape decides what it is, and the four shapes do not
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
        rc = resolve_relative(specifier, importer_path, importer_authority, out_id, err_buf);
    } else if (strncmp(specifier, "std/", 4) == 0) {
        rc = resolve_stdlib_submodule(r, specifier, out_id, err_buf);
    } else if (strchr(specifier, '/') != NULL) {
        rc = resolve_package(r, specifier, out_id, err_buf);
    } else {
        /* A named module: the standard library and .xrd-declared native
         * modules share this one namespace. */
        rc = resolve_stdlib(r, specifier, out_id, err_buf);
    }

    if (rc == XR_MODULE_OK && cache_key) {
        XrModuleId *cached = xr_calloc(1, sizeof(*cached));
        rc = cached ? copy_module_id(cached, out_id) : XR_MODULE_OUT_OF_MEMORY;
        if (rc == XR_MODULE_OK && !xr_hashmap_set(r->cache, cache_key, cached))
            rc = XR_MODULE_OUT_OF_MEMORY;
        if (rc == XR_MODULE_OK) return rc;
        if (cached) { xr_module_id_cleanup(cached); xr_free(cached); }
        xr_module_id_cleanup(out_id);
    }
    xr_free(cache_key);
    return rc;
}
