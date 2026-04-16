/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xpoll_kqueue.h - kqueue backend for xpoll (macOS/BSD)
 */

#ifndef XPOLL_KQUEUE_H
#define XPOLL_KQUEUE_H

// ============================================================================
// Platform: kqueue (macOS/BSD)
// Extracted from xnetpoll_kqueue.c with all optimizations
// ============================================================================


static inline int xr_poll_init(XrPoll *p) {
    memset(p, 0, sizeof(XrPoll));
    
    // Create kqueue (from xnetpoll_kqueue.c)
    p->kq = kqueue();
    if (p->kq < 0) return -1;
    
    // Create wakeup pipe (from xnetpoll_kqueue.c)
    if (xr_poll_create_wakeup_pipe(p->wakeup_pipe) < 0) {
        close(p->kq);
        p->kq = -1;
        return -1;
    }
    
    // Register wakeup pipe to kqueue (from xnetpoll_kqueue.c)
    // EV_ADD: add event
    // EV_CLEAR: edge-triggered (clear state after delivery)
    struct kevent kev;
    EV_SET(&kev, p->wakeup_pipe[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(p->kq, &kev, 1, NULL, 0, NULL) < 0) {
        xr_poll_close_wakeup_pipe(p->wakeup_pipe);
        close(p->kq);
        p->kq = -1;
        return -1;
    }
    
    p->wakeup_pending = false;
    p->initialized = true;
    return 0;
}

static inline void xr_poll_destroy(XrPoll *p) {
    if (!p->initialized) return;
    
    xr_poll_close_wakeup_pipe(p->wakeup_pipe);
    
    if (p->kq >= 0) {
        close(p->kq);
        p->kq = -1;
    }
    
    p->initialized = false;
}

static inline int xr_poll_add(XrPoll *p, int fd, int events, void *user_data) {
    if (!p->initialized) return -1;
    if (p->entry_count >= XR_POLL_MAX_FDS) return -1;
    
    // Build kevent array (from xnetpoll_kqueue.c: xr_netpoll_open)
    struct kevent kev[2];
    int nev = 0;
    
    // EV_CLEAR: edge-triggered (key optimization from xnetpoll)
    if (events & XR_POLL_IN) {
        EV_SET(&kev[nev++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, user_data);
    }
    if (events & XR_POLL_OUT) {
        EV_SET(&kev[nev++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, user_data);
    }
    
    if (nev > 0 && kevent(p->kq, kev, nev, NULL, 0, NULL) < 0) {
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
    if (!p->initialized) return -1;
    
    // Remove from kqueue (from xnetpoll_kqueue.c: xr_netpoll_close)
    // Delete both read and write filters (ignore errors)
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(p->kq, kev, 2, NULL, 0, NULL);
    
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
    if (!p->initialized) return -1;
    if (max_events > XR_POLL_MAX_EVENTS) max_events = XR_POLL_MAX_EVENTS;
    
    // Set timeout (from xnetpoll_kqueue.c: xr_netpoll_poll)
    struct timespec ts;
    struct timespec *timeout = NULL;
    
    if (timeout_ms == 0) {
        // Non-blocking
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        timeout = &ts;
    } else if (timeout_ms > 0) {
        // Finite wait
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        timeout = &ts;
    }
    // timeout_ms < 0: infinite wait (timeout = NULL)
    
    // Poll events (from xnetpoll_kqueue.c)
    struct kevent events[XR_POLL_MAX_EVENTS];
    int n = kevent(p->kq, NULL, 0, events, max_events, timeout);
    
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    
    // Process events (from xnetpoll_kqueue.c)
    int count = 0;
    for (int i = 0; i < n; i++) {
        struct kevent *ev = &events[i];
        
        // Check if wakeup pipe (from xnetpoll_kqueue.c)
        if ((int)ev->ident == p->wakeup_pipe[0]) {
            xr_poll_drain_wakeup(p->wakeup_pipe[0]);
            p->wakeup_pending = false;
            continue;
        }
        
        out[count].fd = (int)ev->ident;
        out[count].user_data = ev->udata;
        out[count].events = 0;
        
        // Convert kqueue filter to xpoll events (from xnetpoll_kqueue.c)
        if (ev->filter == EVFILT_READ) {
            out[count].events |= XR_POLL_IN;
        } else if (ev->filter == EVFILT_WRITE) {
            out[count].events |= XR_POLL_OUT;
        }
        
        // Check error (from xnetpoll_kqueue.c)
        if (ev->flags & EV_ERROR) {
            out[count].events |= XR_POLL_ERR;
        }
        if (ev->flags & EV_EOF) {
            out[count].events |= XR_POLL_HUP;
        }
        
        count++;
    }
    
    return count;
}

static inline void xr_poll_wakeup(XrPoll *p) {
    if (!p->initialized) return;
    
    // Avoid duplicate wakeup (from xnetpoll_kqueue.c: xr_netpoll_break)
    if (p->wakeup_pending) return;
    p->wakeup_pending = true;
    
    xr_poll_signal_wakeup(p->wakeup_pipe[1]);
}



#endif // XPOLL_KQUEUE_H
