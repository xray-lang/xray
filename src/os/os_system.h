/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_system.h - Typed process identity and system information
 *
 * KEY CONCEPT:
 *   Platform absence differs from a failed query. Temporary process handles
 *   remain owned until close succeeds; callbacks are synchronous borrows.
 */
#ifndef XR_OS_SYSTEM_H
#define XR_OS_SYSTEM_H
#include "../base/xio_policy.h"

typedef struct XrOsOptionalI64 {
    bool present;
    int64_t value;
} XrOsOptionalI64;

/* A zero owner is empty. Failed close preserves the actual OS handle for a
 * caller-owned retry after execution drains, without replaying the operation. */
typedef struct XrOsSystemOwner { intptr_t handle; } XrOsSystemOwner;
typedef struct XrOsLoadAverages {
    bool available;
    double values[3];
} XrOsLoadAverages;

/* Successful identity queries report None on Windows and the real POSIX ID
 * elsewhere. Inactive optional payload bytes are not an identity value. */
XR_FUNC XrOsIoStatus xr_os_io_uid(XrOsOptionalI64 *output);
XR_FUNC XrOsIoStatus xr_os_io_gid(XrOsOptionalI64 *output);

/* Owner must be empty at entry. Both calls attempt unconditional close before
 * publishing output; a failure may retain owner but never retains context.
 * Kill checks current directly before the actual mutation. A completed
 * mutation is not rolled back by a later cancellation or close failure. */
XR_FUNC XrOsIoStatus xr_os_io_ppid(XrOsSystemOwner *owner, void *context,
    bool (*current)(void *context), int64_t *output);
XR_FUNC XrOsIoStatus xr_os_io_kill(XrOsSystemOwner *owner, int64_t pid,
    int64_t signal, void *context, bool (*current)(void *context), bool *output);
XR_FUNC XrOsIoStatus xr_os_io_system_close(XrOsSystemOwner *owner);

/* Memory uses bytes and uptime uses seconds. Free memory may acquire a Mach
 * host port and uses the same close-before-publication owner protocol.
 * Queries never retain callback state; their caller checks execution status.
 * All typed failures preserve output. Unavailable load averages never publish
 * fabricated sample values. An available result contains exactly 1/5/15 min. */
XR_FUNC XrOsIoStatus xr_os_io_total_memory(int64_t *output);
XR_FUNC XrOsIoStatus xr_os_io_free_memory(XrOsSystemOwner *owner, int64_t *output);
XR_FUNC XrOsIoStatus xr_os_io_uptime(double *output);
XR_FUNC XrOsIoStatus xr_os_io_loadavg(XrOsLoadAverages *output);
#endif // XR_OS_SYSTEM_H
