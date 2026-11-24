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
#include "base/xchecks.h"

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

static bool host_stat_optional_i64(const XrtValuePlanSpec *spec) {
    XrtValuePlanSpec element;
    return spec->kind == XRT_VALUE_NULLABLE && spec->size == sizeof(XrtHostStatOptionalI64) &&
        spec->alignment == _Alignof(XrtHostStatOptionalI64) &&
        spec->nullable.tag_offset == offsetof(XrtHostStatOptionalI64, present) &&
        spec->nullable.payload_offset == offsetof(XrtHostStatOptionalI64, value) &&
        xrt_value_plan_view(spec->nullable.element, &element) == XRT_MANAGED_OK && element.kind == XRT_VALUE_I64;
}

XR_FUNC bool xrt_host_stat_row_matches(const XrtValuePlanSpec *spec) {
    static const size_t offsets[] = {offsetof(XrtHostStatRow, size), offsetof(XrtHostStatRow, mode),
        offsetof(XrtHostStatRow, mtime), offsetof(XrtHostStatRow, atime), offsetof(XrtHostStatRow, ctime),
        offsetof(XrtHostStatRow, uid), offsetof(XrtHostStatRow, gid), offsetof(XrtHostStatRow, is_file),
        offsetof(XrtHostStatRow, is_dir), offsetof(XrtHostStatRow, is_symlink)};
    if (!spec || spec->kind != XRT_VALUE_RECORD || spec->owned_count || spec->size != sizeof(XrtHostStatRow) ||
        spec->alignment != _Alignof(XrtHostStatRow) || !spec->fields ||
        spec->field_count != sizeof(offsets) / sizeof(*offsets)) return false;
    for (size_t i = 0; i < spec->field_count; ++i) {
        const XrtValueSlotSpec *slot = &spec->fields[i];
        XrtValueKind expected = i < 5 ? XRT_VALUE_I64 : i < 7 ? XRT_VALUE_NULLABLE : XRT_VALUE_BOOL;
        XrtValuePlanSpec field;
        if (slot->flags || slot->offset != offsets[i] ||
            xrt_value_plan_view(slot->type, &field) != XRT_MANAGED_OK || field.owned_count ||
            field.kind != expected || (expected == XRT_VALUE_NULLABLE && !host_stat_optional_i64(&field)))
            return false;
    }
    return true;
}

XR_FUNC XrtHostStatRow xrt_host_stat_row(const XrOsFileMetadata *metadata) {
    XR_DCHECK(metadata && metadata->present, "only present metadata has a class payload");
    return (XrtHostStatRow){.size=metadata->size, .mode=metadata->mode,
        .mtime=metadata->mtime, .atime=metadata->atime, .ctime=metadata->ctime,
        .uid={metadata->uid.present, metadata->uid.present ? metadata->uid.value : 0},
        .gid={metadata->gid.present, metadata->gid.present ? metadata->gid.value : 0},
        .is_file=metadata->is_file, .is_dir=metadata->is_dir, .is_symlink=metadata->is_symlink};
}
