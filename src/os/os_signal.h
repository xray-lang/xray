/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_signal.h - Owned persistent signal mailboxes without guest callbacks
 */
#ifndef XR_OS_SIGNAL_H
#define XR_OS_SIGNAL_H
#include "../base/xio_policy.h"

/* A stable owner address and all three fields identify one registration.
 * Copying these bytes to another owner never transfers ownership. */
typedef struct XrOsSignalOwner {
    uint64_t token;
    uint32_t generation;
    int32_t signal;
} XrOsSignalOwner;

/* All calls run in normal execution context. Output is mandatory and changes
 * only on OK; failures preserve it. A zero owner requests first registration.
 * The same actual owner can replace its registration, changing epoch and
 * clearing old pending state. A different owner of the same signal returns
 * OK+false. Epochs never wrap; exhaustion is typed OVERFLOW before mutation.
 * Windows supports persistent console INT only when attached to a console.
 * TERM has no persistent native delivery there and returns OK+false without
 * installing a one-shot CRT handler. POSIX supports real INT and TERM.
 * KILL and other values return OK+false. No call grants guest execution. */
XR_FUNC XrOsIoStatus xr_os_io_signal_open(int64_t signal, XrOsSignalOwner *owner, bool *installed);

/* Coalescing observation for the exact open owner, not a count of OS events.
 * OS callbacks touch only static lock-free epoch+pending words, never owners,
 * paths, values, allocators, locks or guest execution state. */
XR_FUNC XrOsIoStatus xr_os_io_signal_take(XrOsSignalOwner *owner, bool *pending);

/* Disables pending publication before unregistering/restoring the real native
 * action. Failure retains the closing owner and saved native state for retry.
 * A closing owner cannot be reopened or consumed. Success clears it; empty
 * close is idempotent. Cleanup does not replay delivery or invoke a handler. */
XR_FUNC XrOsIoStatus xr_os_io_signal_close(XrOsSignalOwner *owner);
#endif // XR_OS_SIGNAL_H
