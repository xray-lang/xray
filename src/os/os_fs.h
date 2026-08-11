/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_fs.h - Cross-platform filesystem metadata and path operations.
 *
 * Why a shim:
 *   POSIX <sys/stat.h>, <unistd.h>, <stdlib.h> realpath() and the
 *   Win32 GetFileAttributesEx / CreateDirectoryA / DeleteFileA /
 *   GetCurrentDirectoryA / GetFullPathNameA family don't share
 *   headers, types, or semantics. Callers across cli/, module/,
 *   stdlib/ used to reach for the POSIX side directly, which
 *   broke the lint and required local #ifdef ladders to port.
 *
 *   This header presents a small, opinionated FS surface that:
 *     - Treats "exists" / "kind" as a single stat call so callers
 *       don't pay for two round-trips.
 *     - Reports mkdir-already-exists as success (matches the
 *       common "ensure dir" pattern; callers can pre-check kind
 *       if they need stricter semantics).
 *     - Classifies symbolic links and Windows reparse points as
 *       XR_FS_OTHER without following them.
 *     - Returns 0 / -1 for simple mutating calls. errno is set on
 *       POSIX; on Windows the GetLastError mapping is reflected
 *       through the return code only (callers needing details
 *       should use the platform-specific layer directly, but
 *       no caller in tree currently does).
 *
 * Path size convention:
 *   The absolute-path helpers take an explicit `out_size` so
 *   callers don't have to assume PATH_MAX; the impl writes a
 *   nul-terminated string and returns NULL if the result would
 *   not fit.
 */

#ifndef XR_OS_OS_FS_H
#define XR_OS_OS_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"
#include "../shared/xr_path_limit.h"

// Cross-platform maximum path length. Shared with freestanding AOT helpers so
// VM and AOT stack path buffers follow one platform policy.
#ifndef XR_PATH_MAX
#define XR_PATH_MAX XR_PATH_LIMIT_MAX_PATH
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XrFsKind {
    XR_FS_NONE = 0,  // path does not exist or is not accessible
    XR_FS_FILE,      // regular file
    XR_FS_DIR,       // directory
    XR_FS_OTHER,     // symlink, device, pipe, socket, etc.
} XrFsKind;

typedef struct XrFsStat {
    XrFsKind kind;
    uint64_t size;     // file size in bytes; 0 for non-files
    int64_t mtime_ns;  // last-modified time in ns since unix epoch; 0 if unknown
} XrFsStat;

typedef enum XrFsPublishResult {
    XR_FS_PUBLISH_ERROR = -1,
    XR_FS_PUBLISH_OK = 0,
    XR_FS_PUBLISH_EXISTS = 1,
} XrFsPublishResult;

typedef enum XrFsSyncResult {
    XR_FS_SYNC_ERROR = -1,
    XR_FS_SYNC_OK = 0,
    XR_FS_SYNC_UNSUPPORTED = 1,
} XrFsSyncResult;

typedef struct XrFsExclusiveLock {
    intptr_t handle;
} XrFsExclusiveLock;

/* Acquires an OS-released exclusive lock for a regular lock file. Reparse
 * points and symlinks are rejected so cache roots never silently escape scope. */
XR_FUNC int xr_fs_lock_exclusive(const char *path, XrFsExclusiveLock *out);
XR_FUNC int xr_fs_unlock_exclusive(XrFsExclusiveLock *lock);

// Inspect `path` without following a final symbolic link or reparse point.
// Returns 0 on success, -1 on error (path missing, permission denied, etc.).
// On error `out->kind == XR_FS_NONE`.
XR_FUNC int xr_fs_stat(const char *path, XrFsStat *out);

// Convenience predicates. Each makes a single stat call and folds
// the answer; they never set errno on a "no" result.
XR_FUNC bool xr_fs_exists(const char *path);
XR_FUNC bool xr_fs_is_file(const char *path);
XR_FUNC bool xr_fs_is_dir(const char *path);

// Create a directory if it doesn't already exist. `mode` is
// honoured on POSIX (0755 is the typical value) and ignored on
// Windows. Returns 0 on success or if `path` already names a
// directory; -1 on real failure.
XR_FUNC int xr_fs_mkdir(const char *path, unsigned int mode);

// Remove a regular file. Returns 0 on success, -1 on error.
XR_FUNC int xr_fs_remove(const char *path);

// Rename `old_path` to `new_path` atomically when both live on
// the same filesystem. Returns 0 on success, -1 on error.
XR_FUNC int xr_fs_rename(const char *old_path, const char *new_path);

// Read exactly one regular file without following a final symbolic link or
// reparse point. Files larger than `max_size`, short reads, and concurrent
// growth are rejected. On success `*out_bytes` is owned by xr_malloc and must
// be released with xr_free. Empty files still return a non-NULL allocation.
XR_FUNC int xr_fs_read_regular_file(const char *path, size_t max_size, uint8_t **out_bytes,
                                    size_t *out_size);

// Create `path` exclusively, write all bytes, and durably flush the file.
// Existing paths are never opened or replaced. The partial file is removed
// when writing or flushing fails.
XR_FUNC int xr_fs_write_new_file_sync(const char *path, const uint8_t *data, size_t size);

// Atomically publish a same-directory temporary regular file without
// replacing an existing destination. A successful publish consumes the
// temporary name. XR_FS_PUBLISH_EXISTS leaves the temporary file untouched.
XR_FUNC XrFsPublishResult xr_fs_publish_noreplace(const char *temp_path, const char *final_path);

// Flush `path`, which must name a directory, after publication or removal.
// POSIX implementations provide a durable directory fsync. Windows reports
// XR_FS_SYNC_UNSUPPORTED because Win32 exposes no portable directory-flush
// contract; callers must retain a recoverable temp protocol on that platform.
XR_FUNC XrFsSyncResult xr_fs_sync_directory(const char *path);

// Update a regular file's modification time without following a final
// symbolic link or reparse point. Intended for bounded cache recency metadata.
XR_FUNC int xr_fs_touch(const char *path);

// Resolve `path` to an absolute, canonical, nul-terminated form
// in `out` (at most `out_size` bytes incl. terminator). Returns
// `out` on success, NULL on error. On POSIX uses realpath();
// on Windows uses GetFullPathNameA + canonical case.
XR_FUNC char *xr_fs_realpath(const char *path, char *out, size_t out_size);

// Read the current working directory into `out`. Returns `out` on
// success, NULL on error.
XR_FUNC char *xr_fs_getcwd(char *out, size_t out_size);

// Change the current working directory. Returns 0 on success,
// -1 on error.
XR_FUNC int xr_fs_chdir(const char *path);

#ifdef __cplusplus
}
#endif

#endif  // XR_OS_OS_FS_H
