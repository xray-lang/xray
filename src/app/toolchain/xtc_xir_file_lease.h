/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_file_lease.h - Private same-handle native input leases
 */
#ifndef XTC_XIR_FILE_LEASE_H
#define XTC_XIR_FILE_LEASE_H
#include "xtc_xir_target.h"
#include <wchar.h>

typedef struct XtcXirFileLease XtcXirFileLease;
typedef struct XtcXirFileFacts {
    const char *path;
    uint64_t length;
    uint8_t digest[32];
} XtcXirFileFacts;
typedef struct XtcXirDirectoryFacts {
    const char *path;
    const wchar_t *native_path;
    uint64_t volume;
    uint8_t file_id[16];
} XtcXirDirectoryFacts;

/* Own all ancestor leases and an independent file handle on the original
 * ledger. Existing source text and external ledger references may be freed.
 * These facts have no Target role and grant no execution permission. */
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output);
/* A directory lease blocks ancestor rename, not child creation or mutation. */
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_directory_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output);
XR_FUNC XrCompileResources *xtc_xir_file_lease_resources(const XtcXirFileLease *lease);
XR_FUNC const XtcXirFileFacts *xtc_xir_file_lease_facts(const XtcXirFileLease *lease);
XR_FUNC const XtcXirDirectoryFacts *xtc_xir_file_lease_directory_facts(const XtcXirFileLease *lease);
/* Read a regular file from the same handle, resetting its position each time.
 * Output bytes independently retain the ledger; zero-length files still have
 * an owned allocation. Both outputs must be empty and remain unchanged on
 * failure. A failed resource or I/O operation stops further reads on this
 * lease; facts and cleanup remain available. */
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_read(XtcXirFileLease *lease, size_t limit,
    void **owned_bytes, size_t *length);
XR_FUNC void xtc_xir_file_lease_free(XtcXirFileLease *lease);
#endif // XTC_XIR_FILE_LEASE_H
