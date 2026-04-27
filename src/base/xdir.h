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
 *   Usage:
 *     XrDirIter *it = xr_dir_open(path);
 *     if (it) {
 *         XrDirEntry e;
 *         while (xr_dir_next(it, &e)) {
 *             // e.name is the basename, e.is_dir is set for
 *             // directories (without an extra stat() call).
 *         }
 *         xr_dir_close(it);
 *     }
 *
 *   The "." and ".." entries are filtered automatically; callers
 *   never see them.
 *
 *   The is_dir flag is determined natively where possible:
 *     - Windows: FILE_ATTRIBUTE_DIRECTORY from WIN32_FIND_DATA
 *     - Linux/BSD: dirent->d_type when != DT_UNKNOWN
 *     - Fallback: stat() the full path
 *
 * RELATED MODULES:
 *   - base/xdefs.h for visibility macros
 */

#ifndef XDIR_H
#define XDIR_H

#include "xdefs.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum length of a directory entry's basename, including the
// terminating NUL. Sized to fit Windows MAX_PATH (260) so the
// fixed-buffer copy on iteration is always safe.
#define XR_DIR_ENTRY_NAME_MAX 260

typedef struct XrDirIter XrDirIter;

typedef struct {
    char name[XR_DIR_ENTRY_NAME_MAX];
    bool is_dir;
} XrDirEntry;

// Open `path` for iteration. Returns NULL if the path is not a
// directory or cannot be opened.
XR_FUNC XrDirIter *xr_dir_open(const char *path);

// Read the next entry into `out`. Returns true if an entry was
// produced, false on end-of-stream or error. "." and ".." are
// skipped internally so callers do not need to filter them.
XR_FUNC bool xr_dir_next(XrDirIter *it, XrDirEntry *out);

// Release iterator resources.
XR_FUNC void xr_dir_close(XrDirIter *it);

#ifdef __cplusplus
}
#endif

#endif  // XDIR_H
