/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlockfile.h - xray.lock file support for reproducible builds
 *
 * KEY CONCEPT:
 *   Read/write xray.lock to record resolved dependency tree.
 *   Ensures reproducible builds by locking exact versions.
 *
 * LOCK FILE FORMAT (TOML):
 *   [package.alice/utils]
 *   version = "1.2.3"
 *   resolved = "https://pkg.xray-lang.org/alice/utils/1.2.3.tar.gz"
 *   checksum = "sha256:abc123..."
 *   dependencies = ["bob/helper@^1.0.0"]
 */

#ifndef XLOCKFILE_H
#define XLOCKFILE_H

#include <stdbool.h>
#include "../base/xdefs.h"

// A locked package with resolved version
typedef struct XrLockedPackage {
    char *name;                 // owner/name format
    char *version;              // e.g. "1.2.3"
    char *resolved;             // Download URL
    char *checksum;             // sha256:...
    char **dependencies;        // name@constraint list
    int dep_count;
} XrLockedPackage;

// Complete xray.lock file content
typedef struct XrLockfile {
    int version;                // Lock file format version
    XrLockedPackage *packages;
    int package_count;
    int package_capacity;
} XrLockfile;

/* ========== Lockfile API ========== */

XR_FUNC XrLockfile* xr_lockfile_new(void);
XR_FUNC XrLockfile* xr_lockfile_load(const char *path);
XR_FUNC bool xr_lockfile_save(const XrLockfile *lock, const char *path);
XR_FUNC void xr_lockfile_free(XrLockfile *lock);

/* ========== Package Operations ========== */

XR_FUNC bool xr_lockfile_add_package(XrLockfile *lock,
                             const char *name,
                             const char *version,
                             const char *resolved,
                             const char *checksum);

// dep_spec format: "bob/helper@^1.0.0"
XR_FUNC bool xr_lockfile_add_dependency(XrLockfile *lock,
                                const char *package_name,
                                const char *dep_spec);

XR_FUNC const XrLockedPackage* xr_lockfile_find(const XrLockfile *lock, const char *name);
XR_FUNC bool xr_lockfile_has(const XrLockfile *lock, const char *name);
XR_FUNC bool xr_lockfile_remove(XrLockfile *lock, const char *name);

/* ========== Checksum API ========== */

// out_checksum buffer must be at least 65 bytes
XR_FUNC bool xr_lockfile_checksum_file(const char *filepath, char *out_checksum);
XR_FUNC bool xr_lockfile_verify_checksum(const char *filepath, const char *expected);

#endif // XLOCKFILE_H
