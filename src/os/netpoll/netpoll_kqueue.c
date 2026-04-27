/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll_kqueue.c - kqueue backend (macOS/BSD)
 *
 * KEY CONCEPT:
 *   Ops-based kqueue backend. Provides XrNetpollOps function pointer
 *   table consumed by xnetpoll.c for runtime backend dispatch.
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 */

#if defined(XR_OS_MACOS) && defined(XR_NETPOLL_INCLUDED)

#include "../../base/xchecks.h"
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <errno.h>

// ========== kqueue backend ops ==========

static int kqueue_init(XrNetpoll *np) {
    np->poll_fd = kqueue();
    if (np->poll_fd < 0)
        return -1;
    np->backend_state = NULL;

    // Register wakeup pipe
    struct kevent kev;
    EV_SET(&kev, np->wakeup_pipe[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(np->poll_fd, &kev, 1, NULL, 0, NULL) < 0) {
        close(np->poll_fd);
        np->poll_fd = -1;
        return -1;
    }
    return 0;
}

static void kqueue_cleanup(XrNetpoll *np) {
    if (np->poll_fd >= 0) {
        close(np->poll_fd);
        np->poll_fd = -1;
    }
}

static int kqueue_add_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, pd);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, pd);
    return kevent(np->poll_fd, kev, 2, NULL, 0, NULL);
}

static void kqueue_del_fd(XrNetpoll *np, int fd) {
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(np->poll_fd, kev, 2, NULL, 0, NULL);
}

static int kqueue_poll_events(XrNetpoll *np, int64_t delta_ns, XrReadyList *list) {
    struct timespec ts;
    struct timespec *timeout = NULL;

    if (delta_ns == 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        timeout = &ts;
    } else if (delta_ns > 0) {
        ts.tv_sec = delta_ns / 1000000000;
        ts.tv_nsec = delta_ns % 1000000000;
        timeout = &ts;
    }

    struct kevent events[128];
    int n = kevent(np->poll_fd, NULL, 0, events, 128, timeout);
    if (n < 0)
        return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++) {
        struct kevent *ev = &events[i];

        if ((int) ev->ident == np->wakeup_pipe[0]) {
            char buf[16];
            while (read(np->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
            }
            atomic_store(&np->break_pending, false);
            continue;
        }

        XrPollDesc *pd = (XrPollDesc *) ev->udata;
        if (!pd)
            continue;

        int mode = 0;
        if (ev->filter == EVFILT_READ)
            mode = XR_POLL_READ;
        else if (ev->filter == EVFILT_WRITE)
            mode = XR_POLL_WRITE;
        if (ev->flags & EV_ERROR)
            mode = XR_POLL_BOTH;

        if (mode)
            xr_netpoll_ready(list, pd, mode);
    }
    return n;
}

static void kqueue_wakeup(XrNetpoll *np) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true))
        return;
    char c = 0;
    ssize_t n;
    do {
        n = write(np->wakeup_pipe[1], &c, 1);
    } while (n < 0 && errno == EINTR);
}

static const XrNetpollOps kqueue_ops = {
    .name = "kqueue",
    .init = kqueue_init,
    .cleanup = kqueue_cleanup,
    .add_fd = kqueue_add_fd,
    .del_fd = kqueue_del_fd,
    .poll_events = kqueue_poll_events,
    .wakeup = kqueue_wakeup,
};

#endif  // XR_OS_MACOS
