/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll_epoll.c - epoll backend (Linux)
 *
 * KEY CONCEPT:
 *   Ops-based epoll backend. Provides XrNetpollOps function pointer
 *   table consumed by xnetpoll.c for runtime backend dispatch.
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 */

#if defined(__linux__) && defined(XR_NETPOLL_INCLUDED)

#include "../base/xchecks.h"
#include <sys/epoll.h>
#include <errno.h>

// ========== epoll backend ops ==========

static int epoll_np_init(XrNetpoll *np) {
    np->poll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (np->poll_fd < 0)
        return -1;
    np->backend_state = NULL;

    // Register wakeup pipe
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = NULL;
    if (epoll_ctl(np->poll_fd, EPOLL_CTL_ADD, np->wakeup_pipe[0], &ev) < 0) {
        close(np->poll_fd);
        np->poll_fd = -1;
        return -1;
    }
    return 0;
}

static void epoll_np_cleanup(XrNetpoll *np) {
    if (np->poll_fd >= 0) {
        close(np->poll_fd);
        np->poll_fd = -1;
    }
}

static int epoll_np_add_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.ptr = pd;
    return epoll_ctl(np->poll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static void epoll_np_del_fd(XrNetpoll *np, int fd) {
    epoll_ctl(np->poll_fd, EPOLL_CTL_DEL, fd, NULL);
}

static int epoll_np_poll_events(XrNetpoll *np, int64_t delta_ns, XrReadyList *list) {
    int timeout_ms;
    if (delta_ns < 0)
        timeout_ms = -1;
    else if (delta_ns == 0)
        timeout_ms = 0;
    else {
        timeout_ms = (int) (delta_ns / 1000000);
        if (timeout_ms == 0)
            timeout_ms = 1;
    }

    struct epoll_event events[128];
    int n = epoll_wait(np->poll_fd, events, 128, timeout_ms);
    if (n < 0)
        return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++) {
        struct epoll_event *ev = &events[i];

        if (ev->data.ptr == NULL) {
            char buf[16];
            while (read(np->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
            }
            atomic_store(&np->break_pending, false);
            continue;
        }

        XrPollDesc *pd = (XrPollDesc *) ev->data.ptr;
        int mode = 0;
        if (ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            mode |= XR_POLL_READ;
        if (ev->events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
            mode |= XR_POLL_WRITE;

        if (mode)
            xr_netpoll_ready(list, pd, mode);
    }
    return n;
}

static void epoll_np_wakeup(XrNetpoll *np) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true))
        return;
    char c = 0;
    ssize_t n;
    do {
        n = write(np->wakeup_pipe[1], &c, 1);
    } while (n < 0 && errno == EINTR);
}

static const XrNetpollOps epoll_ops = {
    .name = "epoll",
    .init = epoll_np_init,
    .cleanup = epoll_np_cleanup,
    .add_fd = epoll_np_add_fd,
    .del_fd = epoll_np_del_fd,
    .poll_events = epoll_np_poll_events,
    .wakeup = epoll_np_wakeup,
};

#endif  // __linux__
