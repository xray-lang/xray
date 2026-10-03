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
#include "xio_policy.h"

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
 * Root anchors and repeated trailing separators are preserved lexically.
 * Success transfers policy-owned storage; failure preserves output.
 */
XR_FUNC XrOsIoStatus xr_path_dirname_owned(const XrOsIoPolicy *policy, const char *path, char **output);

/*
 * Join directory and filename into a single path.
 * Handles platform separators and root anchors without granting authority.
 * E.g. ("/a/b/", "c.xr") -> "/a/b/c.xr". The result is policy-owned.
 */
XR_FUNC XrOsIoStatus xr_path_join_owned(const XrOsIoPolicy *policy, const char *dir, const char *name, char **output);

/*
 * Return the basename (filename) component of a path.
 * E.g. "/a/b/c.xr" -> "c.xr", "/" -> "/", "" -> "."
 * Returns xr_malloc'd string. Caller must xr_free().
 */
XR_FUNC char *xr_path_basename(const char *path);

/* Probe a non-directory file candidate. Source resolution allows link locators
 * for its later canonicalization; archive admission requires a regular file.
 * Only NOT_FOUND permits trying another candidate. Other kinds are BAD_ARGUMENT;
 * conversion/allocation and OS failures retain their exact category. */
XR_FUNC XrOsIoStatus xr_file_probe_owned(const XrOsIoPolicy *policy, const char *path, bool allow_links);
/* Policy-owned absolute path. POSIX uses realpath; Windows retains the
 * existing lexical GetFullPathName behavior, not handle-based canonicalization.
 * Failure preserves output. */
XR_FUNC XrOsIoStatus xr_realpath_owned(const XrOsIoPolicy *policy, const char *path, char **output);

#endif  // XFILEIO_H
