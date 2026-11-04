/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_file_temp.h - Prepared results before exclusive temporary creation
 *
 * KEY CONCEPT:
 *   Preparation cannot expose a name. Creation retains its actual object until
 *   explicit publication or rollback; cleanup never recreates the operation.
 */
#ifndef XR_OS_FILE_TEMP_H
#define XR_OS_FILE_TEMP_H
#include "os_file_mutation.h"

typedef struct XrOsFileTempOwner { void *state; } XrOsFileTempOwner;
typedef struct XrOsFileTempRequest {
    const XrOsIoPolicy *policy;
    XrOsPathSpan root;
    void *context;
    bool (*current)(void *context);
    XrOsIoStatus (*prepare)(void *context,const char *candidate,size_t length);
} XrOsFileTempRequest;

/* Policy context is the stable allocation owner, not the synchronous request
 * context. It must remain live until finish discharges every owned byte.
 * Prepare synchronously copies each candidate into an unpublished result; it
 * must retire any previous candidate. No request callback/context is retained.
 * Each of at most 32 random candidates is prepared before exclusive creation.
 * Collisions never grant ownership of, truncate or delete an existing node.
 * Output changes only on OK: false means ordinary refusal/no created object;
 * true retains the real new object in owner. Typed failure preserves output.
 * A failed cleanup can also retain owner on failure, so the caller must keep
 * the owner even though no result was published. No failed guest is replayed.
 * Windows opens the real root once and creates relative to its handle. Root
 * path resolution is an observation, not a global namespace transaction.
 * Unix mutation is currently UNSUPPORTED before resource acquisition; this
 * does not change the older independent temporary-directory API. */
XR_FUNC XrOsIoStatus xr_os_io_temp_directory(const XrOsFileTempRequest *request,
    XrOsFileTempOwner *owner,bool *created);
XR_FUNC XrOsIoStatus xr_os_io_temp_file(const XrOsFileTempRequest *request,
    XrOsFileTempOwner *owner,bool *created);

/* The new policy must identify the original allocation context and callbacks.
 * Keep is the explicit publication commitment: the complete result must
 * already be owned, with no remaining fallible work before publication.
 * Parent closes before the new child; any failed close leaves the child
 * available for rollback by handle. False marks only that actual new child
 * for deletion. A failed delete/close retains state for a later single retry.
 * Cleanup needs no current, allocation or preparation callback. On any failed
 * finish, the caller must not publish or abandon the owner. */
XR_FUNC XrOsIoStatus xr_os_io_temp_finish(const XrOsIoPolicy *policy,
    XrOsFileTempOwner *owner,bool keep);
#endif // XR_OS_FILE_TEMP_H
