/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_file_mutation.h - Controlled boolean mutations of exact UTF-8 paths
 */
#ifndef XR_OS_FILE_MUTATION_H
#define XR_OS_FILE_MUTATION_H
#include "os_file_metadata.h"

/* A synchronous borrowed span, never retained by an operation or its owner. */
typedef struct XrOsPathSpan { const char *text; size_t length; } XrOsPathSpan;

/* All outputs are mandatory and change only on OK. Ordinary OS/path refusals
 * return OK+false; malformed UTF-8/NUL, resource, cancellation and protocol
 * failures remain typed. Current borrows policy.context and is checked at
 * entry and immediately before each actual mutation. Caller checks again
 * after return; completed side effects are not rolled back or replayed.
 * Existing-directory mkdir succeeds. Rmdir never recursively walks a target.
 * Rename never silently copies across volumes: Windows refuses an existing
 * destination while POSIX retains native replacement semantics. Both spans
 * are validated and converted before attempting the rename. */
XR_FUNC XrOsIoStatus xr_os_io_path_mkdir(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *context), bool *output);
XR_FUNC XrOsIoStatus xr_os_io_path_rmdir(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *context), bool *output);
XR_FUNC XrOsIoStatus xr_os_io_path_rename(const XrOsIoPolicy *policy,
    XrOsPathSpan old_path, XrOsPathSpan new_path, bool (*current)(void *context), bool *output);

/* Owner is mandatory and must start empty. Windows uses the existing single
 * temporary file-handle close protocol; retained handles are released through
 * xr_os_io_file_metadata_close after execution drains, without replaying the
 * mutation. Cleanup retains no path, policy, callback or stack query.
 * Remove acts on the named leaf, including directory links/junctions, never
 * the external target. Ordinary Windows directories are not removed here.
 * Utime updates access/modification times only, follows the target, accepts
 * directories and never creates a missing path. Creation remains with touch's
 * source-owned fallback. Both operations publish only after close succeeds. */
XR_FUNC XrOsIoStatus xr_os_io_path_remove(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *context), XrOsFileMetadataOwner *owner, bool *output);
XR_FUNC XrOsIoStatus xr_os_io_path_utime_now(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *context), XrOsFileMetadataOwner *owner, bool *output);
#endif // XR_OS_FILE_MUTATION_H
