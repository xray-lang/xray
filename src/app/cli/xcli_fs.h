/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_fs.h - Unified file system operations for CLI commands
 *
 * KEY CONCEPT:
 *   Single module for all CLI file operations: read/write/exists/traverse.
 *   Provides unified directory traversal with hardcoded ignore rules so
 *   that check/fmt/test use identical filtering.  Also houses safe
 *   parsing helpers (int, port) that were previously in xcli_utils.
 *
 *   The ignore rule set is compile-time constant.  The API reserves a
 *   parameter slot for future custom patterns (e.g. from .xrayignore).
 */

#ifndef XCLI_FS_H
#define XCLI_FS_H

#include <stdbool.h>
#include <stddef.h>
#include "../../base/xdefs.h"

/* ========== Constants ========== */

#define XR_CLI_PATH_MAX 1024
#define XR_CLI_MAX_FILES 8192

/* ========== File List ========== */

/* Growable list of file paths collected by traversal. */
typedef struct XrCliFileList {
    char **paths; /* Array of heap-allocated paths */
    int count;    /* Number of paths */
    int capacity; /* Allocated slots */
} XrCliFileList;

/* Initialize an empty file list. */
XR_FUNC void xr_cli_filelist_init(XrCliFileList *fl);

/* Free all paths and the array itself. */
XR_FUNC void xr_cli_filelist_free(XrCliFileList *fl);

/* Append a path (takes ownership of a copy). */
XR_FUNC void xr_cli_filelist_add(XrCliFileList *fl, const char *path);

/* Sort paths lexicographically (for deterministic output). */
XR_FUNC void xr_cli_filelist_sort(XrCliFileList *fl);

/* ========== Directory Traversal ========== */

/* Traversal options */
typedef struct XrCliWalkOpts {
    bool xr_only;                    /* Only collect .xr files (default: true) */
    bool skip_hidden;                /* Skip hidden dirs/files (default: true) */
    bool skip_underscore;            /* Skip _prefixed dirs (default: false) */
    const char *const *extra_ignore; /* Extra ignore patterns (NULL-terminated, may be NULL) */
} XrCliWalkOpts;

/* Return default walk options (xr_only=true, skip_hidden=true). */
XR_FUNC XrCliWalkOpts xr_cli_walk_defaults(void);

/* Recursively collect files from path into fl.
 * If path is a file, adds it directly.
 * If path is a directory, walks recursively with ignore rules.
 * Returns 0 on success, -1 on error. */
XR_FUNC int xr_cli_collect_files(const char *path, const XrCliWalkOpts *opts, XrCliFileList *fl);

/* ========== File I/O ========== */

/* Read entire file (caller must xr_free). Returns NULL on failure. */
XR_FUNC char *xr_cli_read_file(const char *path);

/* Read all stdin (caller must xr_free). Returns NULL on failure. */
XR_FUNC char *xr_cli_read_stdin(void);

/* Write content to file. Returns 0 on success, -1 on failure. */
XR_FUNC int xr_cli_write_file(const char *path, const char *content);

/* Atomic write: write to tmp then rename. Returns 0 on success, -1 on failure. */
XR_FUNC int xr_cli_write_file_atomic(const char *path, const char *content);

/* ========== Path Queries ========== */

XR_FUNC bool xr_cli_file_exists(const char *path);
XR_FUNC bool xr_cli_is_directory(const char *path);
XR_FUNC bool xr_cli_is_xr_file(const char *filename);

/* ========== Safe Parsing Helpers ========== */

/* Parse integer string. Returns false on error. */
XR_FUNC bool xr_cli_parse_int(const char *str, int *out);

/* Parse port number (0-65535). Returns false on error. */
XR_FUNC bool xr_cli_parse_port(const char *str, int *out);

#endif  // XCLI_FS_H
