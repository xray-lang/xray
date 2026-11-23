/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_file.c - One hosted file owner behind the class reference
 */
#include "xnative_host_file.h"
#include "xnative_backing_types.h"
#include "base/xchecks.h"
#include "shared/xnative_declaration.h"
#include <stdalign.h>

typedef struct XrtHostFile {
    XrtManagedHeader header;
    XrOsFileOwner owner;
} XrtHostFile;

static void host_file_drop(XrtManagedHeader *value, void *context) {
    (void)context;
    XrtHostFile *file = (XrtHostFile *)value;
    bool closed;
    (void)xr_os_io_file_close_sync(&file->owner, &closed);
    XR_CHECK(!file->owner.state, "final release must retire the private file handle");
}

XR_FUNC XrtManagedStatus xrt_host_file_bind(XrtBillingOwner *billing, XrtManagedLease identity,
    XrtManagedOps **empty) {
    XrtRuntimeTypePlan *physical = NULL;
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtHostFile),.alignment=alignof(XrtHostFile),
        .capacity_offset=SIZE_MAX,.drop_payload=host_file_drop,.code=identity};
    XrtManagedStatus status = xrt_runtime_type_plan_new(billing, &spec, &physical);
    if (status == XRT_MANAGED_OK) status = xrt_managed_ops_bind(billing, physical, empty);
    xrt_runtime_type_plan_drop(physical);
    return status;
}

static bool host_file_storage(XrtBoundClass binding) {
    XrtValuePlanSpec spec;
    return binding.ops && xrt_value_plan_view(binding.plan, &spec) == XRT_MANAGED_OK &&
        spec.kind == XRT_VALUE_CLASS && spec.class_storage == XR_NATIVE_DECLARATION_FILE_STORAGE;
}

static XrtHostFileOpen host_file_open(XrtManagedFrame *frame, size_t slot, XrtBoundClass binding,
    const XrOsIoPolicy *policy, const XrtStringBacking *text, XrOsFileAction action, bool append) {
    if (!text || !host_file_storage(binding)) return (XrtHostFileOpen){.managed=XRT_MANAGED_ARGUMENT};
    XrtManagedHeader *pending = NULL;
    XrtHostFileOpen result = {.managed=xrt_managed_slot_prepare(frame, slot, binding.ops, sizeof(XrtHostFile), &pending)};
    if (result.managed != XRT_MANAGED_OK) return result;
    if (!xrt_class_backing_matches(pending, binding.plan)) {
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "file open owns its unpublished backing");
        return (XrtHostFileOpen){.managed=XRT_MANAGED_ARGUMENT};
    }
    XrtHostFile *file = (XrtHostFile *)pending;
    XrOsPathSpan path = {text->bytes, text->length};
    result.host = action == XR_OS_FILE_OPEN_READ
        ? xr_os_io_file_open_read_sync(policy, path, &file->owner, &result.opened)
        : xr_os_io_file_open_write_sync(policy, path, append, &file->owner, &result.opened);
    if (result.host == XR_OS_IO_OK && result.opened)
        result.managed = xrt_managed_slot_commit(frame, slot);
    if (result.managed != XRT_MANAGED_OK || !result.opened) {
        XrtManagedStatus aborted = xrt_managed_slot_abort(frame, slot);
        if (result.managed == XRT_MANAGED_OK && result.host == XR_OS_IO_OK) result.managed = aborted;
    }
    return result;
}

XR_FUNC XrtHostFileOpen xrt_host_file_open_read(XrtManagedFrame *frame, size_t slot, XrtBoundClass binding,
    const XrOsIoPolicy *policy, const XrtStringBacking *text) {
    return host_file_open(frame, slot, binding, policy, text, XR_OS_FILE_OPEN_READ, false);
}

XR_FUNC XrtHostFileOpen xrt_host_file_open_write(XrtManagedFrame *frame, size_t slot, XrtBoundClass binding,
    const XrOsIoPolicy *policy, const XrtStringBacking *text, bool append) {
    return host_file_open(frame, slot, binding, policy, text, XR_OS_FILE_OPEN_WRITE, append);
}

XR_FUNC XrtHostFileRead xrt_host_file_read(XrtManagedFrame *frame, size_t slot, XrtBoundClass binding,
    const XrtManagedHeader *value, const XrtBackingTypes *types, int64_t max_bytes) {
    if (!types || !types->arrays[XRT_BACKING_ARRAY_U8] || !host_file_storage(binding) ||
        !xrt_class_backing_matches(value, binding.plan))
        return (XrtHostFileRead){.managed=XRT_MANAGED_ARGUMENT};
    XrtHostFile *file = (XrtHostFile *)value;
    /* Keep the historical byte Array extent and finish all allocation before
     * the provider can advance this file's position. */
    if (max_bytes < 0 || max_bytes > INT32_MAX || !file->owner.state ||
        (max_bytes && !xr_os_io_file_readable_sync(&file->owner))) return (XrtHostFileRead){0};
    if ((uint64_t)max_bytes > SIZE_MAX - sizeof(XrtArrayBacking))
        return (XrtHostFileRead){.managed=XRT_MANAGED_OVERFLOW};
    size_t capacity = (size_t)max_bytes;
    XrtManagedHeader *pending = NULL;
    XrtHostFileRead result = {.managed=xrt_managed_slot_prepare(frame, slot,
        types->arrays[XRT_BACKING_ARRAY_U8], sizeof(XrtArrayBacking) + capacity, &pending)};
    if (result.managed != XRT_MANAGED_OK) return result;
    XrtArrayBacking *array = (XrtArrayBacking *)pending;
    if (!xrt_backing_array_matches(pending, XRT_BACKING_ARRAY_U8) || array->capacity != capacity) {
        result.managed = XRT_MANAGED_ARGUMENT;
    } else {
        size_t received = 0;
        result.host = xr_os_io_file_read_sync(&file->owner, array->elements, capacity, &received);
        if (result.host == XR_OS_IO_OK) {
            XR_CHECK(received <= capacity, "read must fit the actual pending byte Array");
            array->length = received;
            result.managed = xrt_managed_slot_commit(frame, slot);
            result.present = result.managed == XRT_MANAGED_OK;
        } else if (result.host == XR_OS_IO_IO) {
            result.host = XR_OS_IO_OK;
        }
    }
    if (!result.present)
        XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK, "file read owns the pending byte Array");
    return result;
}

XR_FUNC XrtManagedStatus xrt_host_file_close(XrtBoundClass binding, const XrtManagedHeader *value, bool *closed) {
    if (!closed || !host_file_storage(binding) || !xrt_class_backing_matches(value, binding.plan))
        return XRT_MANAGED_ARGUMENT;
    XrtHostFile *file = (XrtHostFile *)value;
    bool result = false;
    XrOsIoStatus status = xr_os_io_file_close_sync(&file->owner, &result);
    if (status == XR_OS_IO_BAD_ARGUMENT) return XRT_MANAGED_ARGUMENT;
    *closed = status == XR_OS_IO_OK && result;
    return XRT_MANAGED_OK;
}

XR_FUNC XrOsIoStatus xrt_host_file_write(XrtBoundClass binding, XrNativeHostShape shape,
    const XrtManagedHeader *value, const XrtManagedHeader *data, int64_t offset, int64_t *result) {
    if (!result || !host_file_storage(binding) || !xrt_class_backing_matches(value, binding.plan))
        return XR_OS_IO_BAD_ARGUMENT;
    const uint8_t *bytes;
    size_t length;
    if (shape == XR_NATIVE_HOST_FILE_WRITE_BYTES && xrt_backing_array_matches(data, XRT_BACKING_ARRAY_U8)) {
        const XrtArrayBacking *array = (const XrtArrayBacking *)data;
        if (array->length > array->capacity) return XR_OS_IO_BAD_ARGUMENT;
        bytes = array->elements;
        length = array->length;
    } else if (shape == XR_NATIVE_HOST_FILE_WRITE_STRING && xrt_backing_string_matches(data)) {
        const XrtStringBacking *string = (const XrtStringBacking *)data;
        bytes = (const uint8_t *)string->bytes;
        length = string->length;
    } else {
        return XR_OS_IO_BAD_ARGUMENT;
    }
    XrtHostFile *file = (XrtHostFile *)value;
    if (!file->owner.state || offset < 0) { *result = -1; return XR_OS_IO_OK; }
    if ((uint64_t)offset >= length) { *result = 0; return XR_OS_IO_OK; }

    size_t start = (size_t)offset;
    size_t written = 0;
    XrOsIoStatus status = xr_os_io_file_write_sync(&file->owner, bytes + start, length - start, &written);
    if (status == XR_OS_IO_IO) { *result = -1; return XR_OS_IO_OK; }
    if (status != XR_OS_IO_OK) return status;
    if (written > length - start || written > INT64_MAX) return XR_OS_IO_OVERFLOW;
    *result = (int64_t)written;
    return XR_OS_IO_OK;
}
