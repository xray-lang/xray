/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_temp.h - Managed publication of an exclusive temporary directory
 */
#ifndef XNATIVE_HOST_TEMP_H
#define XNATIVE_HOST_TEMP_H
#include "xnative_host_nullable.h"
#include "os/os_file_temp.h"

typedef struct XrtHostTempRetry XrtHostTempRetry;
typedef struct XrtHostTempResult {
    XrtManagedStatus managed;
    XrOsIoStatus host, rollback;
    bool present;
} XrtHostTempResult;

/* The active execution owns the frame. The current callback returns normally
 * and cannot reenter guest or modify the frame. Failed publication preserves
 * the destination. The caller supplies an empty owner that outlives the frame
 * and assumes any remaining rollback obligation even if the call fails. */
XR_FUNC XrtHostTempResult xrt_host_temp_directory_publish(const XrtHostNullableSlots *,
    const XrtManagedHeader *root, bool (*current)(void *), void *context, uint32_t source_id,
    XrtHostTempRetry **retained);

/* The sole caller owning this handle attempts rollback once, without an
 * active execution. Failure preserves the actual handles and allocation lease. */
XR_FUNC XrOsIoStatus xrt_host_temp_retry(XrtHostTempRetry **);
#endif // XNATIVE_HOST_TEMP_H
