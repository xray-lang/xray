/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_owned_unix.c - Synchronous file owners and unavailable worker provider
 */
#include "../os_file_owned.h"
#include "../file_metadata_path.inc.h"
#include <fcntl.h>
#include <unistd.h>

typedef struct OwnedFileState {
    XrOsFileOwner *anchor;
    XrOsIoPolicy memory;
    int descriptor;
} OwnedFileState;

static XrOsIoStatus owned_file_open_error(int error) {
    switch (error) {
    case EACCES: case EPERM: case ENOENT: case ENOTDIR: case EISDIR:
    case ELOOP: case EROFS: case ENAMETOOLONG: case ENXIO: case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
        return XR_OS_IO_OK;
    case EMFILE: case ENFILE: return XR_OS_IO_MEMORY_LIMIT;
    case EOVERFLOW: return XR_OS_IO_OVERFLOW;
    default: return io_errno_status(error);
    }
}

XR_FUNC XrOsIoStatus xr_os_io_file_open_write_sync(const XrOsIoPolicy *memory,
    XrOsPathSpan path, bool append, XrOsFileOwner *owner, bool *opened) {
    if (!io_policy_valid(memory) || !path.text || !owner || owner->state || owner->busy || !opened)
        return XR_OS_IO_BAD_ARGUMENT;
    owner->busy = true;
    XrIoContext io = {memory, XR_OS_IO_OK};
    char *name = NULL;
    io_status(&io, metadata_path_owned(memory, path.text, path.length, NULL, &name));
    OwnedFileState *file = io_alloc(&io, sizeof(*file));
    if (file) {
        int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY | (append ? O_APPEND : O_TRUNC);
        int descriptor;
        do { descriptor = open(name, flags, 0644); } while (descriptor < 0 && errno == EINTR);
        if (descriptor >= 0) {
            *file = (OwnedFileState){owner, *memory, descriptor};
            owner->state = file;
        } else {
            io_status(&io, owned_file_open_error(errno));
            io_free(&io, file);
        }
    }
    io_free(&io, name);
    owner->busy = false;
    if (io.status == XR_OS_IO_OK) *opened = owner->state != NULL;
    return io.status;
}

XR_FUNC XrOsIoStatus xr_os_io_file_close_sync(XrOsFileOwner *owner, bool *closed) {
    if (!owner || owner->busy || !closed) return XR_OS_IO_BAD_ARGUMENT;
    if (!owner->state) { *closed = true; return XR_OS_IO_OK; }
    OwnedFileState *file = owner->state;
    if (file->anchor != owner) return XR_OS_IO_BAD_ARGUMENT;
    owner->busy = true;
    /* Linux and Darwin retire this descriptor before reporting close errors. */
    XrOsIoStatus status = close(file->descriptor) == 0 ? XR_OS_IO_OK : io_errno_status(errno);
    XrOsIoPolicy memory = file->memory;
    owner->state = NULL;
    memory.free(memory.context, file);
    owner->busy = false;
    if (status == XR_OS_IO_OK) *closed = true;
    return status;
}

XR_FUNC XrOsIoStatus xr_os_io_file_open_begin(const XrOsIoPolicy *memory,
                                              const XrOsFileQuery *query,
                                              const XrOsFileRequest *request, XrOsFileOwner *owner,
                                              XrOsFileOperation *operation) {
    (void) memory;
    (void) query;
    (void) request;
    (void) owner;
    (void) operation;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_begin(const XrOsIoPolicy *memory, const XrOsFileQuery *query,
                                         const XrOsFileRequest *request, XrOsFileOwner *owner,
                                         XrOsFileOperation *operation) {
    (void) memory;
    (void) query;
    (void) request;
    (void) owner;
    (void) operation;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_poll(XrOsFileOperation *operation, XrOsFileProgress *output) {
    (void) operation;
    (void) output;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_cancel(XrOsFileOperation *operation) {
    (void) operation;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_operation_close(XrOsFileOperation *operation, bool *closed) {
    (void) operation;
    (void) closed;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_file_close(XrOsFileOwner *owner, bool *closed) {
    (void) owner;
    (void) closed;
    return XR_OS_IO_UNSUPPORTED;
}
