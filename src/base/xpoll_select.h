/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpoll_select.h - select backend for xpoll (fallback)
 */

#ifndef XPOLL_SELECT_H
#define XPOLL_SELECT_H

// ============================================================================
// Platform: select (Fallback)
// Extracted from xnetpoll_select.c
// ============================================================================


static inline int xr_poll_init(XrPoll *p) {
    memset(p, 0, sizeof(XrPoll));
    
    // Create wakeup pipe (from xnetpoll_select.c)
    if (xr_poll_create_wakeup_pipe(p->wakeup_pipe) < 0) {
        return -1;
    }
    
    p->wakeup_pending = false;
    p->initialized = true;
    return 0;
}

static inline void xr_poll_destroy(XrPoll *p) {
    if (!p->initialized) return;
    
    xr_poll_close_wakeup_pipe(p->wakeup_pipe);
    p->initialized = false;
}

static inline int xr_poll_add(XrPoll *p, int fd, int events, void *user_data) {
    if (!p->initialized) return -1;
    if (p->entry_count >= XR_POLL_MAX_FDS) return -1;
    
    // select has FD_SETSIZE limit (from xnetpoll_select.c)
    if (fd >= FD_SETSIZE) return -1;
    
    // Track entry
    XrPollEntry *entry = &p->entries[p->entry_count++];
    entry->fd = fd;
    entry->events = events;
    entry->user_data = user_data;
    entry->active = true;
    
    return 0;
}

static inline int xr_poll_del(XrPoll *p, int fd) {
    if (!p->initialized) return -1;
    
    // Remove from entries (from xnetpoll_select.c: xr_netpoll_close)
    for (int i = 0; i < p->entry_count; i++) {
        if (p->entries[i].fd == fd && p->entries[i].active) {
            p->entries[i].active = false;
            break;
        }
    }
    
    return 0;
}

static inline int xr_poll_wait(XrPoll *p, XrPollEvent *out, int max_events, int timeout_ms) {
    if (!p->initialized) return -1;
    (void)max_events;
    
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    
    // Add wakeup pipe (from xnetpoll_select.c)
    int maxfd = p->wakeup_pipe[0];
    FD_SET(p->wakeup_pipe[0], &rfds);
    
    // Build fd_set (from xnetpoll_select.c: xr_netpoll_poll)
    for (int i = 0; i < p->entry_count; i++) {
        XrPollEntry *entry = &p->entries[i];
        if (!entry->active) continue;
        if (entry->fd < 0 || entry->fd >= FD_SETSIZE) continue;
        
        if (entry->events & XR_POLL_IN) {
            FD_SET(entry->fd, &rfds);
        }
        if (entry->events & XR_POLL_OUT) {
            FD_SET(entry->fd, &wfds);
        }
        if (entry->fd > maxfd) maxfd = entry->fd;
    }
    
    // Set timeout (from xnetpoll_select.c)
    struct timeval tv;
    struct timeval *timeout = NULL;
    
    if (timeout_ms == 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        timeout = &tv;
    } else if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        timeout = &tv;
    }
    
    int n = select(maxfd + 1, &rfds, &wfds, NULL, timeout);
    
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    
    // Check wakeup pipe (from xnetpoll_select.c)
    if (FD_ISSET(p->wakeup_pipe[0], &rfds)) {
        xr_poll_drain_wakeup(p->wakeup_pipe[0]);
        p->wakeup_pending = false;
    }
    
    // Process ready fds (from xnetpoll_select.c)
    int count = 0;
    for (int i = 0; i < p->entry_count && count < max_events; i++) {
        XrPollEntry *entry = &p->entries[i];
        if (!entry->active) continue;
        if (entry->fd < 0) continue;
        
        int events = 0;
        if (FD_ISSET(entry->fd, &rfds)) events |= XR_POLL_IN;
        if (FD_ISSET(entry->fd, &wfds)) events |= XR_POLL_OUT;
        
        if (events) {
            out[count].fd = entry->fd;
            out[count].events = events;
            out[count].user_data = entry->user_data;
            count++;
        }
    }
    
    return count;
}

static inline void xr_poll_wakeup(XrPoll *p) {
    if (!p->initialized) return;
    
    // Avoid duplicate wakeup (from xnetpoll_select.c: xr_netpoll_break)
    if (p->wakeup_pending) return;
    p->wakeup_pending = true;
    
    xr_poll_signal_wakeup(p->wakeup_pipe[1]);
}



#endif // XPOLL_SELECT_H
