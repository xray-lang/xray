/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_thread_token.h - Process-owned identities for actual OS-thread lifetimes
 */
#ifndef XR_OS_THREAD_TOKEN_H
#define XR_OS_THREAD_TOKEN_H
#include "../base/xio_policy.h"

typedef struct XrOsThreadTokenQuery {
    void *context;
    bool (*current)(void *context);
} XrOsThreadTokenQuery;

/* NULL query selects ordinary uncontrolled use. A supplied current callback
 * is mandatory and borrowed synchronously; neither it nor context is retained.
 * Outputs are mandatory and change only on OK. Tokens are positive i64 values,
 * stable for one OS-thread lifetime and never reassigned to another thread.
 * Unknown, nonpositive and retired tokens are not alive.
 *
 * Records use process allocation, independently of Root/Call/Domain ownership
 * and sandbox accounting. Only live threads and unresolved native close duties
 * remain recorded. Every query first retires exited threads; a close failure
 * preserves its owner and prevents creation of another record on that call.
 * A cancelled request may have completed this lazy process registration while
 * still preserving its output. It never leaves a borrowed caller pointer.
 * Reentry from current is rejected, rather than taking the owner lock twice. */
XR_FUNC XrOsIoStatus xr_os_io_thread_token_current(const XrOsThreadTokenQuery *query,
    int64_t *output);
XR_FUNC XrOsIoStatus xr_os_io_thread_token_alive(const XrOsThreadTokenQuery *query,
    int64_t token, bool *output);

/* POSIX token retirement starts at the real pthread-key exit destructor.
 * A terminating thread cannot register again from a later TLS destructor;
 * such queries return BAD_ARGUMENT without replacing its old identity. */

/* Private terminal service shutdown, never Root retirement. The host promises
 * no future queries and joins all other registered threads before calling.
 * A still-live other thread is BAD_ARGUMENT. Failed native cleanup retains its
 * process owner for another shutdown attempt; successful shutdown is idempotent.
 * Queries after shutdown begins are BAD_ARGUMENT. The current thread's token
 * is explicitly retired by shutdown. No automatic atexit hook guesses that
 * unrelated threads or later host cleanup callbacks have stopped using it. */
XR_FUNC XrOsIoStatus xr_os_io_thread_token_shutdown(void);
#endif // XR_OS_THREAD_TOKEN_H
