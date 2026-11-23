/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_path.c - Shared synchronous host path calls
 */
#include "xnative_values.h"
#include "xnative_host_io.h"
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
    default: return XR_XIR_VALUE_IO;
    }
}

XRAY_API bool xrt_native_host_path_bool(uint32_t callable, const XrtStringBacking *text, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    if (!text) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    XrtBillingOwner *billing = NULL;
    XrtManagedStatus prepared = xrt_native_values_prepare(root, &billing);
    if (prepared != XRT_MANAGED_OK) xrt_native_managed_failure(prepared, source_id);
    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    XrOsPathSpan path = {text->bytes, text->length};
    XrOsIoStatus status;
    bool result = false;
    switch (callable) {
#define XR_NATIVE_BLOCKING_PATH_BOOL(id, implementation) \
    case id: status = implementation(&policy, path, NULL, &result); break;
#define XR_NATIVE_BLOCKING_FILE_OPEN_WRITE(id)
#define XR_NATIVE_BLOCKING_FILE_CLOSE(id)
#include "shared/xnative_host_leaves.inc.h"
#undef XR_NATIVE_BLOCKING_FILE_CLOSE
#undef XR_NATIVE_BLOCKING_FILE_OPEN_WRITE
#undef XR_NATIVE_BLOCKING_PATH_BOOL
    default: status = XR_OS_IO_BAD_ARGUMENT; break;
    }
    if (status != XR_OS_IO_OK) xrt_value_failure(xrt_host_io_failure(status), source_id);
    return result;
}
