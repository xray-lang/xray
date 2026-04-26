/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpoll_epoll.h - epoll backend for xpoll (Linux)
 */

#ifndef XPOLL_EPOLL_H
#define XPOLL_EPOLL_H

// ============================================================================
// Platform: epoll (Linux)
// Extracted from xnetpoll_epoll.c with all optimizations
// ============================================================================

static inline int xr_poll_init(XrPoll *p) {
    memset(p, 0, sizeof(XrPoll));

    // Create epoll instance (from xnetpoll_epoll.c)
    // EPOLL_CLOEXEC: close on exec (prevent fd leak to child processes)
    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epfd < 0)
        return -1;

    // Create wakeup pipe (from xnetpoll_epoll.c)
    if (xr_poll_create_wakeup_pipe(p->wakeup_pipe) < 0) {
        close(p->epfd);
        p->epfd = -1;
        return -1;
    }

    // Register wakeup pipe to epoll (from xnetpoll_epoll.c)
    // EPOLLIN: watch for read
    // EPOLLET: edge-triggered (only notify on state change, reduces syscalls)
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = NULL;  // NULL indicates wakeup pipe (xnetpoll convention)
    if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, p->wakeup_pipe[0], &ev) < 0) {
        xr_poll_close_wakeup_pipe(p->wakeup_pipe);
        close(p->epfd);
        p->epfd = -1;
        return -1;
    }

    p->wakeup_pending = false;
    p->initialized = true;
    return 0;
}

static inline void xr_poll_destroy(XrPoll *p) {
    if (!p->initialized)
        return;

    xr_poll_close_wakeup_pipe(p->wakeup_pipe);

    if (p->epfd >= 0) {
        close(p->epfd);
        p->epfd = -1;
    }

    p->initialized = false;
}

static inline int xr_poll_add(XrPoll *p, int fd, int events, void *user_data) {
    if (!p->initialized)
        return -1;
    if (p->entry_count >= XR_POLL_MAX_FDS)
        return -1;

    // Build epoll event (from xnetpoll_epoll.c: xr_netpoll_open)
    struct epoll_event ev;
    ev.data.ptr = user_data;
    ev.events = EPOLLET;  // Edge-triggered (key optimization from xnetpoll)

    if (events & XR_POLL_IN) {
        ev.events |= EPOLLIN | EPOLLRDHUP;  // EPOLLRDHUP: detect peer close
    }
    if (events & XR_POLL_OUT) {
        ev.events |= EPOLLOUT;
    }

    if (epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return -1;
    }

    // Track entry
    XrPollEntry *entry = &p->entries[p->entry_count++];
    entry->fd = fd;
    entry->events = events;
    entry->user_data = user_data;
    entry->active = true;

    return 0;
}

static inline int xr_poll_del(XrPoll *p, int fd) {
    if (!p->initialized)
        return -1;

    // Remove from epoll (ignore errors, fd may already be closed)
    epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL);

    // Remove from entries
    for (int i = 0; i < p->entry_count; i++) {
        if (p->entries[i].fd == fd && p->entries[i].active) {
            p->entries[i].active = false;
            break;
        }
    }

    return 0;
}

static inline int xr_poll_wait(XrPoll *p, XrPollEvent *out, int max_events, int timeout_ms) {
    if (!p->initialized)
        return -1;
    if (max_events > XR_POLL_MAX_EVENTS)
        max_events = XR_POLL_MAX_EVENTS;

    // Poll events (from xnetpoll_epoll.c: xr_netpoll_poll)
    struct epoll_event events[XR_POLL_MAX_EVENTS];
    int n = epoll_wait(p->epfd, events, max_events, timeout_ms);

    if (n < 0) {
        if (errno == EINTR)
            return 0;  // Interrupted, not error
        return -1;
    }

    // Process events (from xnetpoll_epoll.c)
    int count = 0;
    for (int i = 0; i < n; i++) {
        // Check if wakeup pipe (NULL user_data is xnetpoll convention)
        if (events[i].data.ptr == NULL) {
            // Drain pipe (from xnetpoll_epoll.c)
            xr_poll_drain_wakeup(p->wakeup_pipe[0]);
            p->wakeup_pending = false;
            continue;
        }

        // Find fd from user_data (need to search entries)
        int fd = -1;
        for (int j = 0; j < p->entry_count; j++) {
            if (p->entries[j].user_data == events[i].data.ptr && p->entries[j].active) {
                fd = p->entries[j].fd;
                break;
            }
        }

        out[count].fd = fd;
        out[count].user_data = events[i].data.ptr;
        out[count].events = 0;

        // Convert epoll events to xpoll events (from xnetpoll_epoll.c)
        if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            out[count].events |= XR_POLL_IN;
        }
        if (events[i].events & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            out[count].events |= XR_POLL_OUT;
        }
        if (events[i].events & EPOLLERR) {
            out[count].events |= XR_POLL_ERR;
        }
        if (events[i].events & EPOLLHUP) {
            out[count].events |= XR_POLL_HUP;
        }

        count++;
    }

    return count;
}

static inline void xr_poll_wakeup(XrPoll *p) {
    if (!p->initialized)
        return;

    // Avoid duplicate wakeup (key optimization from xnetpoll_epoll.c: xr_netpoll_break)
    // This prevents thundering herd when multiple threads try to wake
    if (p->wakeup_pending)
        return;
    p->wakeup_pending = true;

    xr_poll_signal_wakeup(p->wakeup_pipe[1]);
}

#endif  // XPOLL_EPOLL_H
