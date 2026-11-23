/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpipe_guard.inc.h - Thread-local containment for writes that can raise SIGPIPE
 */
#ifndef XR_OS_XPIPE_GUARD_H
#define XR_OS_XPIPE_GUARD_H
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

typedef struct XrOsPipeGuard {
    sigset_t previous, pipe;
    bool pending;
} XrOsPipeGuard;

static inline int xr_os_pipe_guard_begin(XrOsPipeGuard *guard) {
    sigset_t pending;
    sigemptyset(&guard->pipe);
    sigaddset(&guard->pipe, SIGPIPE);
    int code = pthread_sigmask(SIG_BLOCK, &guard->pipe, &guard->previous);
    if (code) return code;
    if (sigpending(&pending)) {
        code = errno;
        (void)pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
        return code;
    }
    guard->pending = sigismember(&pending, SIGPIPE) == 1;
    return 0;
}

/* Consume a SIGPIPE newly pending after EPIPE, then restore the caller's mask. */
static inline int xr_os_pipe_guard_end(XrOsPipeGuard *guard, bool broken_pipe) {
    sigset_t pending;
    int waited = 0;
    if (broken_pipe && !guard->pending && sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1) {
        int signal_number;
        waited = sigwait(&guard->pipe, &signal_number);
    }
    int restored = pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
    return waited ? waited : restored;
}
#endif // XR_OS_XPIPE_GUARD_H
