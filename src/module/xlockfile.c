/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlockfile.c - xray.lock file support implementation
 *
 * KEY CONCEPT:
 *   Manages xray.lock file which records exact resolved versions of all
 *   dependencies. Ensures reproducible builds across different environments.
 */

#include "xlockfile.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
#include "../../stdlib/crypto/crypto.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LOCKFILE_VERSION 1
#define INITIAL_CAPACITY 16

static const char *skip_whitespace_and_comments(const char *s) {
    while (*s) {
        while (*s && isspace(*s))
            s++;
        if (*s == '#') {
            while (*s && *s != '\n')
                s++;
            continue;
        }

        break;
    }
    return s;
}

/*
 * Parse a quoted string value.
 * Returns a copied string (caller must free).
 */
static char *parse_quoted_string(const char **p) {
    const char *s = *p;

    // Skip whitespace
    while (*s && isspace(*s))
        s++;

    if (*s != '"')
        return NULL;
    s++;  // Skip opening quote

    const char *start = s;
    while (*s && *s != '"' && *s != '\n')
        s++;

    if (*s != '"')
        return NULL;

    size_t len = s - start;
    char *result = (char *) xr_malloc(len + 1);
    if (result) {
        memcpy(result, start, len);
        result[len] = '\0';
    }

    *p = s + 1;  // Skip closing quote
    return result;
}

static void free_dependencies(char **deps, int count);

/*
 * Parse array value ["a", "b", "c"].
 * Returns string array (caller must free each element and array itself).
 */
static char **parse_string_array(const char **p, int *count) {
    const char *s = *p;
    *count = 0;

    // Skip whitespace
    while (*s && isspace(*s))
        s++;

    if (*s != '[')
        return NULL;
    s++;  // Skip '['

    // Estimate capacity
    int capacity = 8;
    char **result = (char **) xr_malloc(capacity * sizeof(char *));
    if (!result)
        return NULL;

    while (*s) {
        // Skip whitespace
        while (*s && isspace(*s))
            s++;

        if (*s == ']') {
            s++;
            break;
        }

        if (*s == '"') {
            char *item = parse_quoted_string(&s);
            if (item) {
                if (*count >= capacity) {
                    capacity *= 2;
                    char **_new_result = (char **) xr_realloc(result, capacity * sizeof(char *));
                    if (!_new_result) {
                        xr_free(item);
                        free_dependencies(result, *count);
                        *count = 0;
                        return NULL;
                    }
                    result = _new_result;
                }
                result[(*count)++] = item;
            }
        }

        // Skip comma
        while (*s && isspace(*s))
            s++;
        if (*s == ',')
            s++;
    }

    *p = s;
    return result;
}

/*
 * Free locked package dependencies array.
 */
static void free_dependencies(char **deps, int count) {
    if (!deps)
        return;
    for (int i = 0; i < count; i++) {
        xr_free(deps[i]);
    }
    xr_free(deps);
}

/*
 * Free a single locked package.
 */
static void free_locked_package(XrLockedPackage *pkg) {
    if (!pkg)
        return;
    xr_free(pkg->name);
    xr_free(pkg->version);
    xr_free(pkg->resolved);
    xr_free(pkg->checksum);
    free_dependencies(pkg->dependencies, pkg->dep_count);
}

/* ========== Lockfile API Implementation ========== */

XrLockfile *xr_lockfile_new(void) {
    XrLockfile *lock = (XrLockfile *) xr_malloc(sizeof(XrLockfile));
    if (!lock)
        return NULL;
    memset(lock, 0, sizeof(XrLockfile));

    lock->version = LOCKFILE_VERSION;
    lock->package_capacity = INITIAL_CAPACITY;
    lock->packages =
        (XrLockedPackage *) xr_malloc(lock->package_capacity * sizeof(XrLockedPackage));

    if (!lock->packages) {
        xr_free(lock);
        return NULL;
    }
    memset(lock->packages, 0, lock->package_capacity * sizeof(XrLockedPackage));

    return lock;
}

XrLockfile *xr_lockfile_load(const char *path) {
    XR_DCHECK(path != NULL, "lockfile_load: NULL path");
    char *content = xr_file_read_all(path, "r", NULL);
    if (!content)
        return NULL;

    // Create lockfile
    XrLockfile *lock = xr_lockfile_new();
    if (!lock) {
        xr_free(content);
        return NULL;
    }

    // Parse content
    const char *p = content;
    char current_package[256] = {0};

    while (*p) {
        p = skip_whitespace_and_comments(p);
        if (!*p)
            break;

        // Parse section header [package.xxx]
        if (*p == '[') {
            p++;

            // Skip "package." prefix
            if (strncmp(p, "package.", 8) == 0) {
                p += 8;

                // Extract package name
                const char *start = p;
                while (*p && *p != ']' && *p != '\n')
                    p++;

                size_t len = p - start;
                if (len < sizeof(current_package)) {
                    memcpy(current_package, start, len);
                    current_package[len] = '\0';

                    // Add package
                    xr_lockfile_add_package(lock, current_package, "0.0.0", "", "");
                }

                if (*p == ']')
                    p++;
            } else {
                // Skip other sections
                while (*p && *p != ']')
                    p++;
                if (*p == ']')
                    p++;
                current_package[0] = '\0';
            }
            continue;
        }

        // Parse key-value pairs
        if (current_package[0] && isalpha(*p)) {
            const char *key_start = p;
            // Fix: allow alphanumeric and underscore in key names
            while (*p && (isalnum(*p) || *p == '_'))
                p++;

            size_t key_len = p - key_start;
            char key[64];
            if (key_len < sizeof(key)) {
                memcpy(key, key_start, key_len);
                key[key_len] = '\0';
            } else {
                key[0] = '\0';
            }

            // Skip =
            while (*p && isspace(*p))
                p++;
            if (*p == '=')
                p++;
            while (*p && isspace(*p))
                p++;

            // Find package
            XrLockedPackage *pkg = NULL;
            for (int i = 0; i < lock->package_count; i++) {
                if (strcmp(lock->packages[i].name, current_package) == 0) {
                    pkg = &lock->packages[i];
                    break;
                }
            }

            if (pkg) {
                if (strcmp(key, "version") == 0) {
                    xr_free(pkg->version);
                    pkg->version = parse_quoted_string(&p);
                } else if (strcmp(key, "resolved") == 0) {
                    xr_free(pkg->resolved);
                    pkg->resolved = parse_quoted_string(&p);
                } else if (strcmp(key, "checksum") == 0) {
                    xr_free(pkg->checksum);
                    pkg->checksum = parse_quoted_string(&p);
                } else if (strcmp(key, "dependencies") == 0) {
                    free_dependencies(pkg->dependencies, pkg->dep_count);
                    pkg->dependencies = parse_string_array(&p, &pkg->dep_count);
                }
            }
        }

        // Skip to next line
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    xr_free(content);
    return lock;
}

bool xr_lockfile_save(const XrLockfile *lock, const char *path) {
    if (!lock || !path)
        return false;

    FILE *f = fopen(path, "w");
    if (!f)
        return false;

    // Write header
    fprintf(f, "# xray.lock - Auto-generated, do not edit manually\n");
    fprintf(f, "# Format version: %d\n\n", lock->version);

    // Write each package
    for (int i = 0; i < lock->package_count; i++) {
        const XrLockedPackage *pkg = &lock->packages[i];

        fprintf(f, "[package.%s]\n", pkg->name);
        fprintf(f, "version = \"%s\"\n", pkg->version ? pkg->version : "0.0.0");

        if (pkg->resolved && pkg->resolved[0]) {
            fprintf(f, "resolved = \"%s\"\n", pkg->resolved);
        }

        if (pkg->checksum && pkg->checksum[0]) {
            fprintf(f, "checksum = \"%s\"\n", pkg->checksum);
        }

        // Write dependencies
        fprintf(f, "dependencies = [");
        for (int j = 0; j < pkg->dep_count; j++) {
            if (j > 0)
                fprintf(f, ", ");
            fprintf(f, "\"%s\"", pkg->dependencies[j]);
        }
        fprintf(f, "]\n\n");
    }

    fclose(f);
    return true;
}

void xr_lockfile_free(XrLockfile *lock) {
    if (!lock)
        return;

    for (int i = 0; i < lock->package_count; i++) {
        free_locked_package(&lock->packages[i]);
    }

    xr_free(lock->packages);
    xr_free(lock);
}

/* ========== Package Operations API Implementation ========== */

bool xr_lockfile_add_package(XrLockfile *lock, const char *name, const char *version,
                             const char *resolved, const char *checksum) {
    if (!lock || !name)
        return false;

    // Check if already exists
    for (int i = 0; i < lock->package_count; i++) {
        if (strcmp(lock->packages[i].name, name) == 0) {
            // Update existing package
            XrLockedPackage *pkg = &lock->packages[i];
            xr_free(pkg->version);
            xr_free(pkg->resolved);
            xr_free(pkg->checksum);
            pkg->version = xr_strdup(version);
            pkg->resolved = xr_strdup(resolved);
            pkg->checksum = xr_strdup(checksum);
            return true;
        }
    }

    // Check capacity
    if (lock->package_count >= lock->package_capacity) {
        int old_cap = lock->package_capacity;
        int new_cap = old_cap * 2;
        XrLockedPackage *new_pkgs =
            (XrLockedPackage *) xr_realloc(lock->packages, new_cap * sizeof(XrLockedPackage));
        if (!new_pkgs)
            return false;
        lock->packages = new_pkgs;
        lock->package_capacity = new_cap;
    }

    // Add new package
    XrLockedPackage *pkg = &lock->packages[lock->package_count++];
    memset(pkg, 0, sizeof(XrLockedPackage));
    pkg->name = xr_strdup(name);
    pkg->version = xr_strdup(version);
    pkg->resolved = xr_strdup(resolved);
    pkg->checksum = xr_strdup(checksum);

    return true;
}

bool xr_lockfile_add_dependency(XrLockfile *lock, const char *package_name, const char *dep_spec) {
    if (!lock || !package_name || !dep_spec)
        return false;

    // Find package
    XrLockedPackage *pkg = NULL;
    for (int i = 0; i < lock->package_count; i++) {
        if (strcmp(lock->packages[i].name, package_name) == 0) {
            pkg = &lock->packages[i];
            break;
        }
    }

    if (!pkg)
        return false;

    // Expand dependencies array (doubling strategy)
    if (pkg->dep_count >= pkg->dep_capacity) {
        int new_cap = (pkg->dep_capacity < 4) ? 4 : pkg->dep_capacity * 2;
        char **new_deps = (char **) xr_realloc(pkg->dependencies, new_cap * sizeof(char *));
        if (!new_deps)
            return false;
        pkg->dependencies = new_deps;
        pkg->dep_capacity = new_cap;
    }

    pkg->dependencies[pkg->dep_count++] = xr_strdup(dep_spec);

    return true;
}

const XrLockedPackage *xr_lockfile_find(const XrLockfile *lock, const char *name) {
    if (!lock || !name)
        return NULL;

    for (int i = 0; i < lock->package_count; i++) {
        if (strcmp(lock->packages[i].name, name) == 0) {
            return &lock->packages[i];
        }
    }

    return NULL;
}

bool xr_lockfile_has(const XrLockfile *lock, const char *name) {
    return xr_lockfile_find(lock, name) != NULL;
}

bool xr_lockfile_remove(XrLockfile *lock, const char *name) {
    if (!lock || !name)
        return false;

    for (int i = 0; i < lock->package_count; i++) {
        if (strcmp(lock->packages[i].name, name) == 0) {
            // Free package memory
            free_locked_package(&lock->packages[i]);

            // Move remaining elements
            for (int j = i; j < lock->package_count - 1; j++) {
                lock->packages[j] = lock->packages[j + 1];
            }

            lock->package_count--;
            return true;
        }
    }

    return false;
}

/* ========== Checksum API Implementation ========== */

/*
 * Calculate SHA256 checksum using xray built-in crypto library.
 */
bool xr_lockfile_checksum_file(const char *filepath, char *out_checksum) {
    if (!filepath || !out_checksum)
        return false;

    // Read file content
    FILE *f = fopen(filepath, "rb");
    if (!f)
        return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *) xr_malloc(size);
    if (!data) {
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(data, 1, size, f);
    fclose(f);

    // Calculate SHA256
#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
    uint8_t digest[32];
    xr_sha256(data, read_bytes, digest);
    xr_free(data);

    // Convert to hex string
    char hex[65];
    xr_bytes_to_hex(digest, 32, hex);

    // Format output
    snprintf(out_checksum, 72, "sha256:%s", hex);
    return true;
#else
    xr_free(data);
    snprintf(out_checksum, 72, "none:disabled");
    return false;
#endif
}

bool xr_lockfile_verify_checksum(const char *filepath, const char *expected) {
    if (!filepath || !expected)
        return false;

    char actual[72];
    if (!xr_lockfile_checksum_file(filepath, actual)) {
        return false;
    }

    return strcmp(actual, expected) == 0;
}
