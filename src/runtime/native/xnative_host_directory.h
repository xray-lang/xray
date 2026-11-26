/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_directory.h - Private synchronous directory ownership
 */
#ifndef XNATIVE_HOST_DIRECTORY_H
#define XNATIVE_HOST_DIRECTORY_H
#include "xnative_value.h"
#include "os/os_directory_owned.h"
#include "xray_native_managed.h"

typedef struct XrtHostDirectory XrtHostDirectory;
typedef struct XrtHostDirectoryName {
    XrOsDirectoryText text;
    XrtBillingOwner *billing;
} XrtHostDirectoryName;

/* An active explicit value root owns every cursor. Only OK writes a result.
 * Open publishes zero on an ordinary refusal. An authenticated close
 * publishes false for an OS close error. Only a retained handle can retry. */
XR_FUNC XrOsIoStatus xrt_host_directory_open(XrOsPathSpan, int64_t *token);
/* Next requires an empty output. OK transfers a name that survives later
 * reads, close, and root retirement. END and typed failures preserve output. */
XR_FUNC XrOsIoStatus xrt_host_directory_next(int64_t token, XrtHostDirectoryName *output);
XR_FUNC void xrt_host_directory_name_drop(XrtHostDirectoryName *);
XR_FUNC XrOsIoStatus xrt_host_directory_close(int64_t token, bool *closed);
/* Static cleanup calls need the same token and owner validation. Cancellation
 * is observed by the caller after the complete cleanup frontier has exited. */
XR_FUNC XrOsIoStatus xrt_host_directory_cleanup_close(int64_t token, bool *closed);

/* The private list belongs to the current generation of its value root.
 * This lookup never creates a billing owner or adopts a cursor. */
XR_FUNC XrtHostDirectory **xrt_native_values_directories(XrtValueRoot *, XrtBillingOwner **);
/* Drain consults no request. A consumed owner is retired even on error.
 * Remaining handles stay attached for a close-only retry. */
XR_FUNC XrOsIoStatus xrt_host_directory_drain(XrtHostDirectory **, XrtBillingOwner *);
#endif // XNATIVE_HOST_DIRECTORY_H
