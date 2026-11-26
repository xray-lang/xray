/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * directory_owned_unix.c - Owned directory descriptors and name publication
 */
#include "../os_directory_owned.h"

#if defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
#include "../file_metadata_path.inc.h"
#include <fcntl.h>
#include <unistd.h>
#if defined(XR_OS_LINUX)
#include <dirent.h>
#else
#include <sys/attr.h>
#endif

typedef struct DirectoryOwned {
    XrOsIoPolicy policy;
    unsigned char *batch;
    size_t capacity;
    size_t offset;
    size_t limit;
#if defined(XR_OS_MACOS)
    size_t remaining;
#endif
    int descriptor;
    XrOsIoStatus failure;
    bool ended;
    bool busy;
} DirectoryOwned;

typedef struct DirectoryQuery {
    const XrOsIoPolicy *policy;
    void *context;
    bool (*current)(void *context);
} DirectoryQuery;

static bool directory_current(void *context) {
    const DirectoryQuery *query = context;
    return !query->current || query->current(query->context);
}
static XrOsIoStatus directory_allocate(void *context, size_t bytes, void **output) {
    const DirectoryQuery *query = context;
    return query->policy->alloc(query->policy->context, bytes, output);
}
static void directory_free(void *context, void *memory) {
    const DirectoryQuery *query = context;
    query->policy->free(query->policy->context, memory);
}

static XrOsIoStatus directory_open_error(int error) {
    switch (error) {
    case EACCES: case EPERM: case ENOENT: case ENOTDIR: case ELOOP: case ENAMETOOLONG:
        return XR_OS_IO_OK;
    case EMFILE: case ENFILE: return XR_OS_IO_MEMORY_LIMIT;
    case EOVERFLOW: return XR_OS_IO_OVERFLOW;
    default: return io_errno_status(error);
    }
}

XR_FUNC XrOsIoStatus xr_os_io_directory_close(XrOsDirectoryOwner *owner) {
    if (!owner) return XR_OS_IO_BAD_ARGUMENT;
    DirectoryOwned *state = owner->state;
    if (!state) return XR_OS_IO_OK;
    if (state->busy) return XR_OS_IO_BAD_ARGUMENT;

    /* On Linux and Darwin the close error can follow descriptor retirement. */
    XrOsIoStatus status = close(state->descriptor) == 0 ? XR_OS_IO_OK : io_errno_status(errno);
    XrOsIoPolicy policy = state->policy;
    owner->state = NULL;
    if (state->batch) policy.free(policy.context, state->batch);
    policy.free(policy.context, state);
    return status;
}

XR_FUNC XrOsIoStatus xr_os_io_directory_open(const XrOsDirectoryRequest *request,
    XrOsDirectoryOwner *owner, bool *opened) {
    if (!request || !owner || owner->state || !opened || !io_policy_valid(request->policy))
        return XR_OS_IO_BAD_ARGUMENT;

    DirectoryQuery query = {request->policy, request->context, request->current};
    XrOsIoPolicy path_policy = {&query, directory_allocate, directory_free};
    char *path = NULL;
    XrOsIoStatus status = metadata_path_owned(&path_policy, request->path.text, request->path.length,
        directory_current, &path);
    if (status != XR_OS_IO_OK) return status;

    XrIoContext io = {request->policy, XR_OS_IO_OK};
    DirectoryOwned *state = io_alloc(&io, sizeof(*state));
    int descriptor = -1;
    if (!state || (uintptr_t)state % _Alignof(DirectoryOwned)) {
        io_status(&io, XR_OS_IO_BAD_ARGUMENT);
    } else {
        for (;;) {
            if (!directory_current(&query)) { io_status(&io, XR_OS_IO_CANCELLED); break; }
            descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (descriptor >= 0) break;
            int error = errno;
            if (error == EINTR) continue;
            io_status(&io, directory_open_error(error));
            break;
        }
    }
    io_free(&io, path);
    if (descriptor >= 0) {
        *state = (DirectoryOwned){.policy=*request->policy, .descriptor=descriptor, .busy=true};
        owner->state = state;
    } else {
        io_free(&io, state);
    }
    if (io.status == XR_OS_IO_OK && !directory_current(&query)) io_status(&io, XR_OS_IO_CANCELLED);
    if (descriptor >= 0) state->busy = false;
    if (io.status == XR_OS_IO_OK) *opened = descriptor >= 0;
    else (void)xr_os_io_directory_close(owner);
    return io.status;
}

static XrOsIoStatus directory_resize(DirectoryOwned *state) {
    const size_t max_capacity = (size_t)INT_MAX & ~(size_t)7;
    if (state->capacity == max_capacity) return XR_OS_IO_OVERFLOW;
    size_t capacity = state->capacity ? (state->capacity > max_capacity / 2 ?
        max_capacity : state->capacity * 2) : 4096;
    XrIoContext io = {&state->policy, XR_OS_IO_OK};
    unsigned char *batch = io_alloc(&io, capacity);
    if (!batch) {
        io_status(&io, XR_OS_IO_BAD_ARGUMENT);
        return io.status;
    }
    io_free(&io, state->batch);
    state->batch = batch;
    state->capacity = capacity;
    return XR_OS_IO_OK;
}

typedef struct DirectoryRecord {
    const char *name;
    size_t length;
    size_t bytes;
} DirectoryRecord;

#if defined(XR_OS_LINUX)
static bool directory_has_record(const DirectoryOwned *state) {
    return state->offset != state->limit;
}
static void directory_advance(DirectoryOwned *state, size_t bytes) {
    state->offset += bytes;
}
static ssize_t directory_read_batch(DirectoryOwned *state) {
    return getdents64(state->descriptor, state->batch, state->capacity);
}
static bool directory_accept_batch(DirectoryOwned *state, size_t bytes) {
    state->limit = bytes;
    return bytes <= state->capacity;
}
static XrOsIoStatus directory_record(const DirectoryOwned *state, DirectoryRecord *record) {
    const unsigned char *entry = state->batch + state->offset;
    const size_t header = offsetof(struct dirent64, d_name);
    const size_t available = state->limit - state->offset;
    if (available <= header) return XR_OS_IO_IO;

    uint16_t bytes;
    uint64_t inode;
    memcpy(&bytes, entry + offsetof(struct dirent64, d_reclen), sizeof(bytes));
    if (bytes <= header || bytes > available || bytes % sizeof(uint64_t)) return XR_OS_IO_IO;
    memcpy(&inode, entry + offsetof(struct dirent64, d_ino), sizeof(inode));
    record->bytes = bytes;
    if (!inode) return XR_OS_IO_OK;
    const char *name = (const char *)entry + header;
    const char *end = memchr(name, 0, bytes - header);
    if (!end || end == name) return XR_OS_IO_IO;
    record->name = name;
    record->length = (size_t)(end - name);
    return XR_OS_IO_OK;
}
#define DIRECTORY_SMALL_BATCH EINVAL

#else
static bool directory_has_record(const DirectoryOwned *state) {
    return state->remaining != 0;
}
static void directory_advance(DirectoryOwned *state, size_t bytes) {
    state->offset += bytes;
    --state->remaining;
}
static ssize_t directory_read_batch(DirectoryOwned *state) {
    struct attrlist attributes = {0};
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_ERROR | ATTR_CMN_NAME;
    /* Darwin publishes a record count, while leaving the rest of the buffer untouched. */
    memset(state->batch, 0, state->capacity);
    return getattrlistbulk(state->descriptor, &attributes, state->batch, state->capacity, 0);
}
static bool directory_accept_batch(DirectoryOwned *state, size_t count) {
    state->remaining = count;
    state->limit = state->capacity;
    return count <= state->capacity / (sizeof(uint32_t) + sizeof(attribute_set_t));
}
static XrOsIoStatus directory_record(const DirectoryOwned *state, DirectoryRecord *record) {
    const unsigned char *entry = state->batch + state->offset;
    const size_t available = state->limit - state->offset;
    uint32_t bytes;
    attribute_set_t returned;
    size_t field = sizeof(bytes) + sizeof(returned);
    if (available < field) return XR_OS_IO_IO;
    memcpy(&bytes, entry, sizeof(bytes));
    /* The final group may lack padding when it consumes the remaining buffer. */
    if (bytes < field || bytes > available || (bytes % 8 && state->remaining != 1)) return XR_OS_IO_IO;
    memcpy(&returned, entry + sizeof(bytes), sizeof(returned));
    if ((returned.commonattr & ~(ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_ERROR | ATTR_CMN_NAME)) ||
        returned.volattr || returned.dirattr || returned.fileattr || returned.forkattr) return XR_OS_IO_IO;

    if (returned.commonattr & ATTR_CMN_ERROR) {
        uint32_t error;
        if (bytes - field < sizeof(error)) return XR_OS_IO_IO;
        memcpy(&error, entry + field, sizeof(error));
        if (error) return error <= INT_MAX ? io_errno_status((int)error) : XR_OS_IO_IO;
        field += sizeof(error);
    }
    attrreference_t reference;
    if (!(returned.commonattr & ATTR_CMN_NAME) || bytes - field < sizeof(reference)) return XR_OS_IO_IO;
    memcpy(&reference, entry + field, sizeof(reference));
    if (reference.attr_dataoffset < 0) return XR_OS_IO_IO;
    size_t offset = (size_t)reference.attr_dataoffset;
    size_t length = reference.attr_length;
    if (offset < sizeof(reference) || offset % sizeof(uint32_t) || offset > bytes - field ||
        length < 2 || length > bytes - field - offset) return XR_OS_IO_IO;
    const char *name = (const char *)entry + field + offset;
    if (memchr(name, 0, length) != name + length - 1) return XR_OS_IO_IO;
    *record = (DirectoryRecord){name, length - 1, bytes};
    return XR_OS_IO_OK;
}
#define DIRECTORY_SMALL_BATCH ERANGE
#endif

static XrOsIoStatus directory_fetch(DirectoryOwned *state, DirectoryQuery *query) {
    if (!state->batch) {
        XrOsIoStatus status = directory_resize(state);
        if (status != XR_OS_IO_OK) return status;
    }
    while (directory_current(query)) {
        ssize_t count = directory_read_batch(state);
        int error = count < 0 ? errno : 0;
        if (!directory_current(query)) return XR_OS_IO_CANCELLED;
        if (!count) { state->ended = true; return XR_OS_IO_END; }
        if (count > 0) {
            state->offset = 0;
            return directory_accept_batch(state, (size_t)count) ? XR_OS_IO_OK : XR_OS_IO_IO;
        }
        if (error == DIRECTORY_SMALL_BATCH) {
            XrOsIoStatus status = directory_resize(state);
            if (status != XR_OS_IO_OK) return status;
        } else if (error != EINTR) {
            return io_errno_status(error);
        }
    }
    return XR_OS_IO_CANCELLED;
}

static bool directory_dot_name(const DirectoryRecord *record) {
    return record->name[0] == '.' &&
        (record->length == 1 || (record->length == 2 && record->name[1] == '.'));
}

static XrOsIoStatus directory_publish(const DirectoryOwned *state, DirectoryQuery *query,
    const DirectoryRecord *record, XrOsDirectoryText *output) {
    if (xr_utf8_core_scan_strict((const uint8_t *)record->name, record->length).error != XR_UTF8_OK)
        return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {&state->policy, XR_OS_IO_OK};
    char *name = io_alloc(&io, record->length + 1);
    if (!name) {
        io_status(&io, XR_OS_IO_BAD_ARGUMENT);
        return io.status;
    }
    memcpy(name, record->name, record->length + 1);
    if (!directory_current(query)) { io_free(&io, name); return XR_OS_IO_CANCELLED; }
    *output = (XrOsDirectoryText){name, record->length};
    return XR_OS_IO_OK;
}

static XrOsIoStatus directory_next(DirectoryOwned *state, DirectoryQuery *query, XrOsDirectoryText *output) {
    while (directory_current(query)) {
        if (state->ended) return XR_OS_IO_END;
        if (!directory_has_record(state)) {
            XrOsIoStatus status = directory_fetch(state, query);
            if (status != XR_OS_IO_OK) return status;
        }
        DirectoryRecord record = {0};
        XrOsIoStatus status = directory_record(state, &record);
        if (status != XR_OS_IO_OK) return status;
        if (record.name && !directory_dot_name(&record)) {
            status = directory_publish(state, query, &record, output);
            if (status == XR_OS_IO_OK) directory_advance(state, record.bytes);
            return status;
        }
        directory_advance(state, record.bytes);
    }
    return XR_OS_IO_CANCELLED;
}

XR_FUNC XrOsIoStatus xr_os_io_directory_next(XrOsDirectoryOwner *owner,
    void *context, bool (*current)(void *), XrOsDirectoryText *output) {
    if (!owner || !owner->state || !output || output->bytes || output->length)
        return XR_OS_IO_BAD_ARGUMENT;
    DirectoryOwned *state = owner->state;
    if (state->busy) return XR_OS_IO_BAD_ARGUMENT;
    if (state->failure != XR_OS_IO_OK) return state->failure;
    state->busy = true;
    DirectoryQuery query = {&state->policy, context, current};
    XrOsIoStatus status = directory_next(state, &query, output);
    if (status != XR_OS_IO_OK && status != XR_OS_IO_END) state->failure = status;
    state->busy = false;
    return status;
}

#else
#include "../../base/xio_policy.inc.h"
XR_FUNC XrOsIoStatus xr_os_io_directory_open(const XrOsDirectoryRequest *request,
    XrOsDirectoryOwner *owner, bool *opened) {
    if (!request || !owner || owner->state || !opened || !request->path.text || !io_policy_valid(request->policy))
        return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_next(XrOsDirectoryOwner *owner,
    void *context, bool (*current)(void *), XrOsDirectoryText *output) {
    (void)context;
    (void)current;
    if (!owner || owner->state || !output || output->bytes || output->length)
        return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_close(XrOsDirectoryOwner *owner) {
    return !owner || owner->state ? XR_OS_IO_BAD_ARGUMENT : XR_OS_IO_OK;
}
#endif
