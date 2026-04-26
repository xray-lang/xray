/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll_select.c - select fallback (other platforms)
 *
 * Uses POSIX select as fallback. Lower performance, only for platforms
 * without epoll/kqueue/IOCP support.
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 */

// Only compile when included by xnetpoll.c
#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32) && defined(XR_NETPOLL_INCLUDED)

#include "../base/xchecks.h"
#include <sys/select.h>
#include <errno.h>

// Max fd count supported by select
#define XR_SELECT_MAX_FDS 1024

// Active descriptor list
static XrPollDesc *active_pds[XR_SELECT_MAX_FDS];
static int active_count = 0;
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;

// Initialize select
int xr_netpoll_init(XrNetpoll *np) {
    if (atomic_load(&np->inited)) {
        return 0;
    }

    poll_cache_init(&np->cache);

    if (create_wakeup_pipe(np->wakeup_pipe) < 0) {
        return -1;
    }

    active_count = 0;

    atomic_store(&np->waiters, 0);
    atomic_store(&np->break_pending, false);
    atomic_store(&np->inited, true);

    return 0;
}

// Cleanup select
void xr_netpoll_cleanup(XrNetpoll *np) {
    if (!atomic_load(&np->inited)) {
        return;
    }

    close_wakeup_pipe(np->wakeup_pipe);
    poll_cache_cleanup(&np->cache);

    pthread_mutex_lock(&active_lock);
    active_count = 0;
    pthread_mutex_unlock(&active_lock);

    atomic_store(&np->inited, false);
}

// Open fd
XrPollDesc *xr_netpoll_open(XrNetpoll *np, int fd) {
    if (!atomic_load(&np->inited)) {
        return NULL;
    }

    if (fd >= XR_SELECT_MAX_FDS) {
        return NULL;  // select doesn't support large fd
    }

    XrPollDesc *pd = xr_poll_cache_alloc(&np->cache);
    if (!pd) {
        return NULL;
    }

    pd->fd = fd;

    // Add to active list
    pthread_mutex_lock(&active_lock);
    if (active_count < XR_SELECT_MAX_FDS) {
        active_pds[active_count++] = pd;
    }
    pthread_mutex_unlock(&active_lock);

    return pd;
}

// Close fd
void xr_netpoll_close(XrNetpoll *np, XrPollDesc *pd) {
    if (!pd)
        return;

    atomic_store(&pd->closing, true);

    // Remove from active list
    pthread_mutex_lock(&active_lock);
    for (int i = 0; i < active_count; i++) {
        if (active_pds[i] == pd) {
            active_pds[i] = active_pds[--active_count];
            break;
        }
    }
    pthread_mutex_unlock(&active_lock);

    xr_netpoll_unblock(pd, XR_POLL_READ, false);
    xr_netpoll_unblock(pd, XR_POLL_WRITE, false);

    xr_poll_cache_free(&np->cache, pd);
}

// Poll ready events
XrReadyList xr_netpoll_poll(XrNetpoll *np, int64_t delta_ns) {
    XrReadyList list;
    ready_list_init(&list);

    if (!atomic_load(&np->inited)) {
        return list;
    }

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    int maxfd = np->wakeup_pipe[0];
    FD_SET(np->wakeup_pipe[0], &rfds);

    // Build fd_set
    pthread_mutex_lock(&active_lock);
    for (int i = 0; i < active_count; i++) {
        XrPollDesc *pd = active_pds[i];
        if (pd && pd->fd >= 0 && pd->fd < FD_SETSIZE) {
            FD_SET(pd->fd, &rfds);
            FD_SET(pd->fd, &wfds);
            if (pd->fd > maxfd)
                maxfd = pd->fd;
        }
    }
    pthread_mutex_unlock(&active_lock);

    // Set timeout
    struct timeval tv;
    struct timeval *timeout = NULL;

    if (delta_ns == 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        timeout = &tv;
    } else if (delta_ns > 0) {
        tv.tv_sec = delta_ns / 1000000000;
        tv.tv_usec = (delta_ns % 1000000000) / 1000;
        timeout = &tv;
    }

    int n = select(maxfd + 1, &rfds, &wfds, NULL, timeout);

    if (n < 0) {
        if (errno != EINTR) {
            // Error
        }
        return list;
    }

    // Check wakeup pipe
    if (FD_ISSET(np->wakeup_pipe[0], &rfds)) {
        char buf[16];
        while (read(np->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
        }
        atomic_store(&np->break_pending, false);
    }

    // Process ready fds
    pthread_mutex_lock(&active_lock);
    for (int i = 0; i < active_count; i++) {
        XrPollDesc *pd = active_pds[i];
        if (!pd || pd->fd < 0)
            continue;

        int mode = 0;
        if (FD_ISSET(pd->fd, &rfds))
            mode |= XR_POLL_READ;
        if (FD_ISSET(pd->fd, &wfds))
            mode |= XR_POLL_WRITE;

        if (mode) {
            xr_netpoll_ready(&list, pd, mode);
        }
    }
    pthread_mutex_unlock(&active_lock);

    return list;
}

// Wake select
void xr_netpoll_break(XrNetpoll *np) {
    if (!atomic_load(&np->inited)) {
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true)) {
        return;
    }

    char c = 0;
    ssize_t n;
    do {
        n = write(np->wakeup_pipe[1], &c, 1);
    } while (n < 0 && errno == EINTR);
}

#endif  // fallback select
