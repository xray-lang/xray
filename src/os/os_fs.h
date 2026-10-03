/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_fs.h - Cross-platform filesystem metadata and path operations.
 *
 * All allocating and traversing calls require an explicit neutral I/O policy.
 * Typed errors distinguish resource admission, missing objects, and I/O faults.
 * Stat does not follow a final symbolic link or reparse point. Regular-file
 * operations reject those leaves; ordinary ancestor resolution still applies.
 * Existing directory checks follow their final component.
 * Fixed-buffer outputs change only on success, including path conversion.
 */

#ifndef XR_OS_OS_FS_H
#define XR_OS_OS_FS_H
#include "../base/xio_policy.h"
#include "../shared/xr_path_limit.h"
#ifndef XR_PATH_MAX
#define XR_PATH_MAX XR_PATH_LIMIT_MAX_PATH
#endif
#ifdef __cplusplus
extern "C" {
#endif

typedef enum XrFsKind { XR_FS_NONE = 0, XR_FS_FILE, XR_FS_DIR, XR_FS_OTHER } XrFsKind;
typedef struct XrFsStat {
    XrFsKind kind;
    uint64_t size;
    int64_t mtime_ns;
} XrFsStat;
typedef struct XrFsExclusiveLock { intptr_t handle; } XrFsExclusiveLock;

/* The mandatory policy covers owned memory, explicit scans and submitted OS
 * operations. Failure preserves outputs. Final symlink/reparse-point handling
 * is unchanged: stat classifies them OTHER, regular file operations reject
 * them, and mkdir follows its final component when checking an existing dir.
 * OS/libc internal storage remains outside the caller allocation domain. */
XR_FUNC XrOsIoStatus xr_os_io_lock_exclusive(const XrOsIoPolicy *policy, const char *path, XrFsExclusiveLock *out);
/* Unconditional cleanup does not request a fresh budget. */
XR_FUNC int xr_fs_unlock_exclusive(XrFsExclusiveLock *lock);
XR_FUNC XrOsIoStatus xr_os_io_stat(const XrOsIoPolicy *policy, const char *path, XrFsStat *out);
XR_FUNC XrOsIoStatus xr_os_io_exists(const XrOsIoPolicy *policy, const char *path, bool *out);
XR_FUNC XrOsIoStatus xr_os_io_is_file(const XrOsIoPolicy *policy, const char *path, bool *out);
XR_FUNC XrOsIoStatus xr_os_io_is_dir(const XrOsIoPolicy *policy, const char *path, bool *out);
XR_FUNC XrOsIoStatus xr_os_io_mkdir(const XrOsIoPolicy *policy, const char *path, unsigned int mode);
XR_FUNC XrOsIoStatus xr_os_io_remove(const XrOsIoPolicy *policy, const char *path);
XR_FUNC XrOsIoStatus xr_os_io_rename(const XrOsIoPolicy *policy, const char *old_path, const char *new_path);
/* Exact regular-file snapshot: reject oversized files, short reads or growth.
 * Empty success owns a one-byte allocation. Release through the same policy. */
XR_FUNC XrOsIoStatus xr_os_io_read_regular_file(const XrOsIoPolicy *policy, const char *path,
    size_t max_size, uint8_t **out_bytes, size_t *out_size);
XR_FUNC XrOsIoStatus xr_os_io_write_new_file_sync(const XrOsIoPolicy *policy, const char *path,
    const uint8_t *data, size_t size);
/* OK consumes temp, EXISTS leaves it untouched. */
XR_FUNC XrOsIoStatus xr_os_io_publish_noreplace(const XrOsIoPolicy *policy, const char *temp_path, const char *final_path);
/* Windows returns UNSUPPORTED for a portable directory-flush contract. */
XR_FUNC XrOsIoStatus xr_os_io_sync_directory(const XrOsIoPolicy *policy, const char *path);
XR_FUNC XrOsIoStatus xr_os_io_touch(const XrOsIoPolicy *policy, const char *path);
XR_FUNC XrOsIoStatus xr_os_io_realpath(const XrOsIoPolicy *policy, const char *path, char *out, size_t out_size);
XR_FUNC XrOsIoStatus xr_os_io_getcwd(const XrOsIoPolicy *policy, char *out, size_t out_size);
XR_FUNC XrOsIoStatus xr_os_io_chdir(const XrOsIoPolicy *policy, const char *path);
#ifdef __cplusplus
}
#endif
#endif /* XR_OS_OS_FS_H */
