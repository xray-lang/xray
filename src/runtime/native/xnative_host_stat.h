/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_stat.h - Billed filesystem query and immutable class payload
 */
#ifndef XNATIVE_HOST_STAT_H
#define XNATIVE_HOST_STAT_H
#include "xnative_value_plan.h"
#include "os/os_file_metadata.h"

typedef struct XrtHostStatQuery {
    XrtBillingOwner *billing;
    XrOsFileMetadataOwner *owner;
    bool (*current)(void *context);
    void *context;
} XrtHostStatQuery;

/* The caller owns the query's actual temporary-handle storage through drain.
 * Its optional cancellation check and context are synchronous borrows. */
XR_FUNC XrOsIoStatus xrt_host_stat_query(const XrtHostStatQuery *, const XrtManagedHeader *path,
    XrOsFileMetadata *output);

typedef struct XrtHostStatOptionalI64 {
    uint8_t present;
    int64_t value;
} XrtHostStatOptionalI64;

typedef struct XrtHostStatRow {
    int64_t size, mode, mtime, atime, ctime;
    XrtHostStatOptionalI64 uid, gid;
    uint8_t is_file, is_dir, is_symlink;
} XrtHostStatRow;

/* The runtime image must already authenticate the private class. Only its
 * exact owner-free Layout can consume this C11 staging row. */
XR_FUNC bool xrt_host_stat_row_matches(const XrtValuePlanSpec *);
/* Initialize the full row's representation, including padding copied during
 * class construction. The metadata and output are distinct live objects. */
XR_FUNC void xrt_host_stat_row(const XrOsFileMetadata *, XrtHostStatRow *output);
#endif // XNATIVE_HOST_STAT_H
