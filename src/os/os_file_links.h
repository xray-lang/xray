/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_file_links.h - Owned link text and following path resolution
 *
 * KEY CONCEPT:
 *   Link contents are not resolved paths. Temporary handles retain their
 *   existing close owner until close succeeds, independently of text storage.
 */
#ifndef XR_OS_FILE_LINKS_H
#define XR_OS_FILE_LINKS_H
#include "os_file_mutation.h"

/* NULL text denotes an ordinary absent/refused result. Present text is strict
 * NUL-terminated UTF-8, owned by the exact policy and freed through policy.free.
 * Outputs start empty and remain empty on typed failure. */
typedef struct XrOsFileLinkText { char *text; size_t length; } XrOsFileLinkText;

/* Both spans are validated before creating the named link. Relative source
 * text is preserved and interpreted relative to the link directory. Windows
 * detects existing directories there; absent targets retain its file default.
 * OS refusal returns OK+false. Typed faults preserve the boolean output.
 * Current borrows policy.context synchronously; a post-write refusal does not
 * undo the link. No fallback changes the requested link into a junction. */
XR_FUNC XrOsIoStatus xr_os_io_path_symlink(const XrOsIoPolicy *policy,
    XrOsPathSpan source, XrOsPathSpan link, bool (*current)(void *context), bool *output);
/* Readlink reads the named reparse/symlink leaf without following its target.
 * Relative contents keep their spelling. Windows accepts SYMLINK and
 * MOUNT_POINT substitute targets, mapping the NT DOS prefix to Win32 extended
 * syntax; unknown tags are UNSUPPORTED. Realpath follows an existing target.
 * Owner is mandatory and starts empty. Closing gets one attempt; failure
 * retains only that handle for xr_os_io_file_metadata_close, never the policy,
 * path, callback or a pending string. No output precedes successful close.
 * POSIX realpath uses the native PATH_MAX buffer contract on Linux/Darwin;
 * targets without that proven native bound return UNSUPPORTED, not a guessed
 * allocation limit or a lexical path. */
XR_FUNC XrOsIoStatus xr_os_io_path_readlink(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *context), XrOsFileMetadataOwner *owner,
    XrOsFileLinkText *output);
XR_FUNC XrOsIoStatus xr_os_io_path_realpath(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *context), XrOsFileMetadataOwner *owner,
    XrOsFileLinkText *output);
#endif // XR_OS_FILE_LINKS_H
