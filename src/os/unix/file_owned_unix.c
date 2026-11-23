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
#if !defined(XR_OS_MACOS)
#include "xpipe_guard.inc.h"
#endif
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

typedef struct OwnedFileState {
    XrOsFileOwner *anchor;
    XrOsIoPolicy memory;
    int descriptor;
    XrOsFileAction action;
#if defined(XR_OS_MACOS)
    bool suppresses_sigpipe;
#endif
} OwnedFileState;

static OwnedFileState *owned_file_sync_state(const XrOsFileOwner *owner) {
    OwnedFileState *file = owner && !owner->busy ? owner->state : NULL;
    return file && file->anchor == owner && file->descriptor >= 0 &&
        (file->action == XR_OS_FILE_OPEN_READ || file->action == XR_OS_FILE_OPEN_WRITE) ? file : NULL;
}

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

static XrOsIoStatus owned_file_open_sync(const XrOsIoPolicy *memory, XrOsPathSpan path,
    XrOsFileAction action, bool append, XrOsFileOwner *owner, bool *opened) {
    if (!io_policy_valid(memory) || !path.text || !owner || owner->state || owner->busy || !opened)
        return XR_OS_IO_BAD_ARGUMENT;
    owner->busy = true;
    XrIoContext io = {memory, XR_OS_IO_OK};
    char *name = NULL;
    io_status(&io, metadata_path_owned(memory, path.text, path.length, NULL, &name));
    OwnedFileState *file = io_alloc(&io, sizeof(*file));
    if (file) {
        int flags = O_CLOEXEC | O_NOCTTY;
        flags |= action == XR_OS_FILE_OPEN_READ ? O_RDONLY : O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int descriptor;
        do { descriptor = open(name, flags, 0644); } while (descriptor < 0 && errno == EINTR);
        if (descriptor >= 0) {
            *file = (OwnedFileState){.anchor=owner,.memory=*memory,.descriptor=descriptor,.action=action};
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

XR_FUNC XrOsIoStatus xr_os_io_file_open_read_sync(const XrOsIoPolicy *memory,
    XrOsPathSpan path, XrOsFileOwner *owner, bool *opened) {
    return owned_file_open_sync(memory, path, XR_OS_FILE_OPEN_READ, false, owner, opened);
}

XR_FUNC XrOsIoStatus xr_os_io_file_open_write_sync(const XrOsIoPolicy *memory,
    XrOsPathSpan path, bool append, XrOsFileOwner *owner, bool *opened) {
    return owned_file_open_sync(memory, path, XR_OS_FILE_OPEN_WRITE, append, owner, opened);
}

XR_FUNC bool xr_os_io_file_readable_sync(const XrOsFileOwner *owner) {
    const OwnedFileState *file = owned_file_sync_state(owner);
    return file && file->action == XR_OS_FILE_OPEN_READ;
}

static XrOsIoStatus owned_file_transfer_error(int error) {
    switch (error) {
    case ENOMEM: return XR_OS_IO_OUT_OF_MEMORY;
    case EOVERFLOW: return XR_OS_IO_OVERFLOW;
    case ECANCELED: return XR_OS_IO_CANCELLED;
    default: return XR_OS_IO_IO;
    }
}

static size_t owned_file_transfer_limit(void) {
#if defined(XR_OS_MACOS) || defined(XR_OS_LINUX)
    /* Darwin rejects larger sizes. Linux applies a page-dependent lower cap. */
    return INT_MAX;
#else
    return SSIZE_MAX;
#endif
}

XR_FUNC XrOsIoStatus xr_os_io_file_read_sync(XrOsFileOwner *owner, uint8_t *bytes,
    size_t capacity, size_t *received) {
    const OwnedFileState *file = owned_file_sync_state(owner);
    if (!file || !received || (capacity && !bytes)) return XR_OS_IO_BAD_ARGUMENT;
    if (!capacity) { *received = 0; return XR_OS_IO_OK; }
    if (file->action != XR_OS_FILE_OPEN_READ) return XR_OS_IO_IO;

    size_t limit = owned_file_transfer_limit();
    owner->busy = true;
    ssize_t count;
    do { count = read(file->descriptor, bytes, capacity < limit ? capacity : limit); }
    while (count < 0 && errno == EINTR);
    int error = count < 0 ? errno : 0;
    owner->busy = false;
    if (count < 0) return owned_file_transfer_error(error);
    *received = (size_t)count;
    return XR_OS_IO_OK;
}

#if defined(XR_OS_MACOS)
static int owned_file_suppress_sigpipe(OwnedFileState *file) {
    if (file->suppresses_sigpipe) return 0;
    /* Darwin sends EPIPE's signal to the process. Protect this private descriptor. */
    int result;
    do { result = fcntl(file->descriptor, F_SETNOSIGPIPE, 1); } while (result < 0 && errno == EINTR);
    if (result < 0) return errno;
    file->suppresses_sigpipe = true;
    return 0;
}
#endif

XR_FUNC XrOsIoStatus xr_os_io_file_write_sync(XrOsFileOwner *owner, const uint8_t *bytes,
    size_t length, size_t *written) {
    OwnedFileState *file = owned_file_sync_state(owner);
    if (!file || !written || (length && !bytes)) return XR_OS_IO_BAD_ARGUMENT;
    if (!length) { *written = 0; return XR_OS_IO_OK; }
    if (file->action != XR_OS_FILE_OPEN_WRITE) return XR_OS_IO_IO;

    size_t limit = owned_file_transfer_limit();
    owner->busy = true;
#if defined(XR_OS_MACOS)
    int error = owned_file_suppress_sigpipe(file);
#else
    XrOsPipeGuard guard;
    int error = xr_os_pipe_guard_begin(&guard);
#endif
    if (error) {
        owner->busy = false;
        return owned_file_transfer_error(error);
    }
    ssize_t count;
    do { count = write(file->descriptor, bytes, length < limit ? length : limit); }
    while (count < 0 && errno == EINTR);
    error = count < 0 ? errno : 0;
#if !defined(XR_OS_MACOS)
    int guard_error = xr_os_pipe_guard_end(&guard, error == EPIPE);
    /* Preserve transferred bytes: the caller must never replay this prefix. */
    if (!count) error = guard_error;
#endif
    owner->busy = false;
    if (error) return owned_file_transfer_error(error);
    *written = (size_t)count;
    return XR_OS_IO_OK;
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
