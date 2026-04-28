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
 *     - Returns 0 / -1 for every mutating call. errno is set on
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

// Cross-platform maximum path length. On POSIX PATH_MAX is 4096
// (Linux) or 1024 (macOS); on Windows MAX_PATH is 260 but extended
// paths can reach 32767. We pick 4096 as a pragmatic compromise.
#ifndef XR_PATH_MAX
#ifdef _WIN32
#define XR_PATH_MAX 4096
#else
#include <limits.h>
#ifdef PATH_MAX
#define XR_PATH_MAX PATH_MAX
#else
#define XR_PATH_MAX 4096
#endif
#endif
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

// Inspect `path`. Returns 0 on success, -1 on error (path missing,
// permission denied, etc.). On error `out->kind == XR_FS_NONE`.
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
