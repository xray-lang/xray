/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_stat.c - Query a filesystem snapshot before publishing it
 */
#include "xnative_host_stat.h"
#include "xnative_host_io.h"
#include "xnative_string_backing.h"
#include "xnative_values.h"
#include "base/xchecks.h"
#include "shared/xnative_host_leaf.h"
#include "xray_native_values.h"
#include <string.h>

typedef struct HostStatPolicy {
    XrOsIoPolicy billed;
    const XrtHostStatQuery *query;
} HostStatPolicy;

static XrOsIoStatus host_stat_allocate(void *context, size_t size, void **output) {
    const HostStatPolicy *policy = context;
    return policy->billed.alloc(policy->billed.context, size, output);
}

static void host_stat_free(void *context, void *memory) {
    const HostStatPolicy *policy = context;
    policy->billed.free(policy->billed.context, memory);
}

static bool host_stat_current(void *context) {
    const XrtHostStatQuery *query = ((const HostStatPolicy *)context)->query;
    return query->current(query->context);
}

XR_FUNC XrOsIoStatus xrt_host_stat_query(const XrtHostStatQuery *query, const XrtManagedHeader *path,
    XrOsFileMetadata *output) {
    if (!query || !query->billing || !query->owner || query->owner->handle || !output ||
        !xrt_backing_string_matches(path)) return XR_OS_IO_BAD_ARGUMENT;
    const XrtStringBacking *text = (const XrtStringBacking *)path;
    HostStatPolicy context = {xrt_host_io_policy(query->billing), query};
    const XrOsIoPolicy policy = {&context, host_stat_allocate, host_stat_free};
    return xr_os_io_file_metadata(&policy, text->bytes, text->length,
        query->current ? host_stat_current : NULL, query->owner, output);
}

static void host_stat_store_optional(unsigned char *bytes, const XrOsOptionalI64 *value) {
    bytes[offsetof(XrtHostStatOptionalI64, present)] = value->present;
    if (value->present) memcpy(bytes + offsetof(XrtHostStatOptionalI64, value), &value->value, sizeof(value->value));
}

XR_FUNC void xrt_host_stat_row(const XrOsFileMetadata *metadata, XrtHostStatRow *output) {
    XR_DCHECK(metadata && metadata->present && output, "only present metadata has a class payload");
    unsigned char *bytes = (unsigned char *)output;
    memset(bytes, 0, sizeof(*output));
    memcpy(bytes + offsetof(XrtHostStatRow, size), &metadata->size, sizeof(metadata->size));
    memcpy(bytes + offsetof(XrtHostStatRow, mode), &metadata->mode, sizeof(metadata->mode));
    memcpy(bytes + offsetof(XrtHostStatRow, mtime), &metadata->mtime, sizeof(metadata->mtime));
    memcpy(bytes + offsetof(XrtHostStatRow, atime), &metadata->atime, sizeof(metadata->atime));
    memcpy(bytes + offsetof(XrtHostStatRow, ctime), &metadata->ctime, sizeof(metadata->ctime));
    host_stat_store_optional(bytes + offsetof(XrtHostStatRow, uid), &metadata->uid);
    host_stat_store_optional(bytes + offsetof(XrtHostStatRow, gid), &metadata->gid);
    bytes[offsetof(XrtHostStatRow, is_file)] = metadata->is_file;
    bytes[offsetof(XrtHostStatRow, is_dir)] = metadata->is_dir;
    bytes[offsetof(XrtHostStatRow, is_symlink)] = metadata->is_symlink;
}

XR_FUNC XrtHostStatResult xrt_host_stat_publish(const XrtHostNullableSlots *output, XrtBoundClass binding,
    const XrtManagedHeader *path, uint32_t source_id) {
    XrtValuePlanSpec type;
    if (!binding.ops || xrt_value_plan_view(binding.plan, &type) != XRT_MANAGED_OK ||
        type.kind != XRT_VALUE_CLASS || type.class_storage != XR_NATIVE_DECLARATION_FILE_STAT ||
        !xrt_backing_string_matches(path)) return (XrtHostStatResult){.managed=XRT_MANAGED_ARGUMENT};
    XrtHostNullablePlaces places;
    XrtHostStatResult result = {.managed=xrt_host_nullable_prepare(output, xrt_native_managed_root(source_id), &places)};
    if (result.managed != XRT_MANAGED_OK) return result;
    if (places.current && !xrt_class_backing_matches(places.current, binding.plan))
        return (XrtHostStatResult){.managed=XRT_MANAGED_ARGUMENT};

    XrtHostStatQuery query = {0};
    result.managed = xrt_native_values_stat_prepare(xrt_value_root_current(source_id), &query.billing, &query.owner);
    if (result.managed != XRT_MANAGED_OK) return result;
    XrOsFileMetadata metadata;
    result.host = xrt_host_stat_query(&query, path, &metadata);
    if (result.host != XR_OS_IO_OK) return result;
    XR_CHECK(!query.owner->handle, "metadata must close before publishing a result");
    if (metadata.present) {
        XrtHostStatRow row;
        xrt_host_stat_row(&metadata, &row);
        result.managed = xrt_host_stat_class_create(places.temporary.frame, places.temporary.slot, binding, &row);
        if (result.managed != XRT_MANAGED_OK) return result;
    }
    XR_CHECK(xrt_host_nullable_publish(&places, metadata.present) == XRT_MANAGED_OK,
        "metadata publication keeps the preflighted nullable home");
    return result;
}
