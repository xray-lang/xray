/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xio_policy.h - Explicit allocation and submitted-work policy for OS I/O
 */
#ifndef XIO_POLICY_H
#define XIO_POLICY_H

#include "xdefs.h"
#include <stddef.h>
#include <stdint.h>

typedef enum XrOsIoStatus {
    XR_OS_IO_OK, XR_OS_IO_END, XR_OS_IO_NOT_FOUND, XR_OS_IO_EXISTS,
    XR_OS_IO_UNSUPPORTED, XR_OS_IO_BAD_ARGUMENT, XR_OS_IO_BUDGET,
    XR_OS_IO_OUT_OF_MEMORY, XR_OS_IO_IO
} XrOsIoStatus;

/* Context and every callback are mandatory. Owners copy the policy by value;
 * its context must remain live through the owner's last free. Allocation
 * outputs change only on OK. No operation chooses a policy implicitly.
 * Work covers submitted OS calls and explicit buffer/span operations, not
 * private kernel/libc allocations or CPU instruction counts. Cleanup and
 * physical release do not require new work admission. */
typedef struct XrOsIoPolicy {
    void *context;
    XrOsIoStatus (*alloc)(void *context, size_t bytes, void **output);
    void (*free)(void *context, void *memory);
    XrOsIoStatus (*work)(void *context, uint64_t units);
} XrOsIoPolicy;

typedef struct XrCompileResources XrCompileResources;
/* System policy is an explicit unmetered choice for non-compiler owners. */
XR_FUNC XrOsIoPolicy xr_os_io_system_policy(void);
/* No new ledger is created. Ledger-backed blocks retain this context. */
XR_FUNC XrOsIoPolicy xr_compile_io_policy(XrCompileResources *resources);

#endif /* XIO_POLICY_H */
