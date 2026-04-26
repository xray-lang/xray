/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfileio.h - File I/O and path utilities
 *
 * KEY CONCEPT:
 *   Provides unified file reading and common path operations,
 *   eliminating duplicate fopen/fseek/fread boilerplate across modules.
 */

#ifndef XFILEIO_H
#define XFILEIO_H

#include <stddef.h>
#include "xdefs.h"

/*
 * Read entire file into xr_malloc'd buffer (NUL-terminated).
 * Returns NULL on failure. Caller must xr_free() the result.
 * If out_size is non-NULL, stores the number of bytes read.
 * mode: "r" for text, "rb" for binary.
 */
XR_FUNC char *xr_file_read_all(const char *path, const char *mode, size_t *out_size);

/*
 * Return the directory component of a file path.
 * E.g. "/a/b/c.xr" -> "/a/b", "file.xr" -> "."
 * Returns xr_malloc'd string. Caller must xr_free().
 */
XR_FUNC char *xr_path_dirname(const char *path);

/*
 * Join directory and filename into a single path.
 * Handles trailing '/' on dir. Returns xr_malloc'd string.
 * E.g. xr_path_join("/a/b/", "c.xr") -> "/a/b/c.xr"
 */
XR_FUNC char *xr_path_join(const char *dir, const char *name);

/*
 * Return the basename (filename) component of a path.
 * E.g. "/a/b/c.xr" -> "c.xr", "/" -> "/", "" -> "."
 * Returns xr_malloc'd string. Caller must xr_free().
 */
XR_FUNC char *xr_path_basename(const char *path);

/*
 * Return realpath() result as an xr_malloc'd string.
 * System realpath() uses libc malloc; this function converts to xr_malloc.
 * Returns NULL if realpath() fails. Caller must xr_free().
 */
XR_FUNC char *xr_realpath(const char *path);

#endif  // XFILEIO_H
