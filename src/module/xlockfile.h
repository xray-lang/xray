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
#include "../base/xio_policy.h"

// A locked package with resolved version
typedef struct XrLockedPackage {
    char *name;           // owner/name format
    char *version;        // e.g. "1.2.3"
    char *resolved;       // Download URL
    char *checksum;       // sha256:...
    char **dependencies;  // name@constraint list
    int dep_count;
    int dep_capacity;
} XrLockedPackage;

// Complete xray.lock file content
typedef struct XrLockfile {
    XrOsIoPolicy policy;  // The mandatory allocator owns the root and all records.
    int version;  // Lock file format version
    XrLockedPackage *packages;
    int package_count;
    int package_capacity;
} XrLockfile;

/* Construction and reading publish only a complete owner. The caller's policy
 * context stays live through the last owned allocation; no default is chosen. */
XR_FUNC XrOsIoStatus xr_lockfile_new_owned(const XrOsIoPolicy *policy, XrLockfile **output);
XR_FUNC XrOsIoStatus xr_lockfile_load_owned(const XrOsIoPolicy *policy, const char *path, XrLockfile **output);
/* Serialization completes before creating a private sibling temporary file.
 * A successful rename atomically replaces the target. Earlier failure leaves
 * the target intact; exhausted cleanup may leave the owned temporary on disk. */
XR_FUNC XrOsIoStatus xr_lockfile_save_owned(const XrOsIoPolicy *policy, const XrLockfile *lock, const char *path);
XR_FUNC void xr_lockfile_free_owned(XrLockfile *lock);
/* Constant-time identity check against the policy actually held by the owner. */
XR_FUNC bool xr_lockfile_uses_policy(const XrLockfile *lock, const XrOsIoPolicy *policy);
XR_FUNC XrOsIoStatus xr_lockfile_add_package_owned(XrLockfile *lock, const char *name, const char *version,
    const char *resolved, const char *checksum);
XR_FUNC XrOsIoStatus xr_lockfile_add_dependency_owned(XrLockfile *lock, const char *package_name, const char *dep_spec);
/* A found package is borrowed until the owner changes or is destroyed.
 * NOT_FOUND and other failures preserve output. */
XR_FUNC XrOsIoStatus xr_lockfile_find_owned(const XrOsIoPolicy *policy, const XrLockfile *lock,
    const char *name, const XrLockedPackage **output);
XR_FUNC XrOsIoStatus xr_lockfile_has_owned(const XrOsIoPolicy *policy, const XrLockfile *lock, const char *name, bool *output);
XR_FUNC XrOsIoStatus xr_lockfile_remove_owned(XrLockfile *lock, const char *name);
#define XR_LOCKFILE_CHECKSUM_CAPACITY 72
/* SHA-256 text includes its seven-byte prefix and terminator. On failure the
 * entire caller buffer is preserved. Mismatch is OK with false, never an I/O
 * or resource failure. All temporary storage uses the supplied policy. */
XR_FUNC XrOsIoStatus xr_lockfile_checksum_file_owned(const XrOsIoPolicy *policy, const char *filepath,
    char *output, size_t capacity);
XR_FUNC XrOsIoStatus xr_lockfile_verify_checksum_owned(const XrOsIoPolicy *policy, const char *filepath,
    const char *expected, bool *output);

#endif  // XLOCKFILE_H
