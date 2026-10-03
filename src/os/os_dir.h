/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdir.h - Cross-platform directory iteration.
 *
 * KEY CONCEPT:
 *   POSIX <dirent.h> (opendir / readdir / closedir) does not exist
 *   on Windows; the equivalent is FindFirstFile / FindNextFile /
 *   FindClose. This header presents a single iteration API that
 *   maps onto either platform.
 *
 *   A mandatory allocation/work policy is copied into the iterator. Typed
 *   results distinguish end-of-stream from resource and OS failures.
 */
#ifndef XDIR_H
#define XDIR_H
#include "../base/xio_policy.h"
#define XR_DIR_ENTRY_NAME_MAX 260

typedef struct XrDirIter XrDirIter;
typedef struct XrDirEntry { char name[XR_DIR_ENTRY_NAME_MAX]; bool is_dir; } XrDirEntry;

XR_FUNC XrOsIoStatus xr_os_io_dir_open(const XrOsIoPolicy *policy, const char *path, XrDirIter **output);
/* OK publishes an entry; END or any failure preserves output. Dot entries
 * are filtered. A name that cannot fit is a failure, never silent omission. */
XR_FUNC XrOsIoStatus xr_os_io_dir_next(XrDirIter *iterator, XrDirEntry *output);
XR_FUNC void xr_os_io_dir_close(XrDirIter *iterator);
#endif /* XDIR_H */
