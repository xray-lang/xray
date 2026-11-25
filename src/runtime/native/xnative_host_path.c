/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_path.c - Shared synchronous host path calls
 */
#include "xnative_values.h"
#include "xnative_execution.h"
#include "xnative_host_io.h"
#include "xnative_backing_types.h"
#include "base/xchecks.h"
#include "base/xmax_align.h"
#include "os/os_file_mutation.h"
#include "shared/xnative_host_leaf.h"
#include "xray_native_values.h"

/* The policy releases scratch by address. Keep the exact reservation in the
 * allocation while preserving the allocator's alignment for OS conversion. */
typedef union HostPathAllocation {
    XrMaxAlignment alignment;
    size_t size;
} HostPathAllocation;

static XrOsIoStatus host_path_allocate(void *context, size_t size, void **output) {
    if (size > SIZE_MAX - sizeof(HostPathAllocation)) return XR_OS_IO_OVERFLOW;
    size_t total = sizeof(HostPathAllocation) + size;
    void *memory = NULL;
    switch (xrt_managed_metadata_allocate(context, total, &memory)) {
    case XRT_MANAGED_OK: {
        HostPathAllocation *block = memory;
        block->size = total;
        *output = block + 1;
        return XR_OS_IO_OK;
    }
    case XRT_MANAGED_LIMIT: return XR_OS_IO_MEMORY_LIMIT;
    case XRT_MANAGED_OOM: return XR_OS_IO_OUT_OF_MEMORY;
    default: return XR_OS_IO_BAD_ARGUMENT;
    }
}

static void host_path_free(void *context, void *memory) {
    HostPathAllocation *block = (HostPathAllocation *)memory - 1;
    xrt_managed_metadata_free(context, block, block->size);
}

XR_FUNC XrOsIoPolicy xrt_host_io_policy(XrtBillingOwner *billing) {
    return (XrOsIoPolicy){billing, host_path_allocate, host_path_free};
}

XR_FUNC XrXirValueStatus xrt_host_io_failure(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_BAD_ARGUMENT: return XR_XIR_VALUE_BAD_ARGUMENT;
    case XR_OS_IO_OVERFLOW: return XR_XIR_VALUE_OVERFLOW;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_XIR_VALUE_OOM;
    case XR_OS_IO_MEMORY_LIMIT: return XR_XIR_VALUE_MEMORY_LIMIT;
    case XR_OS_IO_CANCELLED: return XR_XIR_VALUE_CANCELLED;
    default: return XR_XIR_VALUE_IO;
    }
}

typedef struct HostPathRequest {
    XrOsIoPolicy billed;
    const XrtHostExecutionControl *control;
} HostPathRequest;

static XrOsIoStatus host_path_request_allocate(void *context, size_t size, void **output) {
    const HostPathRequest *request = context;
    return request->billed.alloc(request->billed.context, size, output);
}

static void host_path_request_free(void *context, void *memory) {
    const HostPathRequest *request = context;
    request->billed.free(request->billed.context, memory);
}

static bool host_path_request_current(void *context) {
    const HostPathRequest *request = context;
    return request->control->current(request->control->context);
}

static bool host_path_bool(uint32_t callable, const XrtStringBacking *text,
    const XrtStringBacking *destination, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    bool remove = callable == XR_NATIVE_CALLABLE_IO_REMOVE;
    bool deletion = remove || callable == XR_NATIVE_CALLABLE_IO_RMDIR;
    if (!text || xr_native_blocking_path_count(callable) != (destination ? 2u : 1u) ||
        (deletion && !xrt_backing_string_matches((const XrtManagedHeader *)text)))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    XrtBillingOwner *billing = NULL;
    XrOsFileMetadataOwner *owner = NULL;
    XrtManagedStatus prepared = remove ? xrt_native_values_stat_prepare(root, &billing, &owner) :
        xrt_native_values_prepare(root, &billing);
    if (prepared != XRT_MANAGED_OK) xrt_native_managed_failure(prepared, source_id);
    const XrtHostExecutionControl *control = deletion ? xrt_execution_host_control() : NULL;
    HostPathRequest request = {xrt_host_io_policy(billing), control};
    XrOsIoPolicy policy = control ?
        (XrOsIoPolicy){&request, host_path_request_allocate, host_path_request_free} : request.billed;
    bool (*current)(void *) = control ? host_path_request_current : NULL;
    XrOsPathSpan path = {text->bytes, text->length};
    XrOsIoStatus status;
    bool result = false;
    switch (callable) {
#define XR_NATIVE_BLOCKING_ENV_GET(id)
#define XR_NATIVE_BLOCKING_PATH_BOOL(id, implementation) \
    case id: status = implementation(&policy, path, current, &result); break;
#define XR_NATIVE_BLOCKING_OWNED_PATH_BOOL(id, implementation) \
    case id: status = implementation(&policy, path, current, owner, &result); break;
#define XR_NATIVE_BLOCKING_PATH_PAIR_BOOL(id, implementation) \
    case id: { \
        XrOsPathSpan target = {destination->bytes, destination->length}; \
        status = implementation(&policy, path, target, NULL, &result); break; \
    }
#define XR_NATIVE_BLOCKING_FILE_OPEN_READ(id)
#define XR_NATIVE_BLOCKING_FILE_STAT(id)
#define XR_NATIVE_BLOCKING_FILE_OPEN_WRITE(id)
#define XR_NATIVE_BLOCKING_FILE_CLOSE(id)
#define XR_NATIVE_BLOCKING_FILE_READ(id)
#define XR_NATIVE_BLOCKING_FILE_WRITE(id, kind)
#define XR_NATIVE_BLOCKING_TEXT_RESULT(id)
#include "shared/xnative_host_leaves.inc.h"
#undef XR_NATIVE_FILE_STAT_FIELDS
#undef XR_NATIVE_BLOCKING_TEXT_RESULT
#undef XR_NATIVE_BLOCKING_FILE_WRITE
#undef XR_NATIVE_BLOCKING_FILE_READ
#undef XR_NATIVE_BLOCKING_FILE_CLOSE
#undef XR_NATIVE_BLOCKING_FILE_OPEN_WRITE
#undef XR_NATIVE_BLOCKING_FILE_OPEN_READ
#undef XR_NATIVE_BLOCKING_FILE_STAT
#undef XR_NATIVE_BLOCKING_PATH_PAIR_BOOL
#undef XR_NATIVE_BLOCKING_OWNED_PATH_BOOL
#undef XR_NATIVE_BLOCKING_PATH_BOOL
#undef XR_NATIVE_BLOCKING_ENV_GET
    default: status = XR_OS_IO_BAD_ARGUMENT; break;
    }
    if (status != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(status), source_id);
    XR_CHECK(!owner || !owner->handle, "host deletion must close before reporting success");
    if (current && !current(policy.context)) xrt_value_failure(XR_XIR_VALUE_CANCELLED, source_id);
    return result;
}

XRAY_API bool xrt_native_host_path_bool(uint32_t callable, const XrtStringBacking *text, uint32_t source_id) {
    return host_path_bool(callable, text, NULL, source_id);
}

XRAY_API bool xrt_native_host_path_pair_bool(uint32_t callable, const XrtStringBacking *text,
    const XrtStringBacking *destination, uint32_t source_id) {
    if (!xrt_backing_string_matches((const XrtManagedHeader *)text) ||
        !xrt_backing_string_matches((const XrtManagedHeader *)destination))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    return host_path_bool(callable, text, destination, source_id);
}
