/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll.c - Cross-platform network poller (platform-independent part)
 *
 * KEY CONCEPT:
 *   Platform-independent interface with backend implementations.
 *   Deeply integrated with coroutine scheduler.
 */

#include "xnetpoll.h"
#include "../base/xchecks.h"

// The full POSIX-only implementation of this file (kqueue / epoll
// / pipe / unistd I/O) lives below the platform guard. On Windows
// we provide a small stub at the bottom that fails every public
// entry point with a clear error code; a future IOCP backend will
// replace it.
#ifndef XR_OS_WINDOWS

#include "xcoroutine.h"  // XrCoroutine
#include "xworker.h"     // XrRuntime, XrWorker
#include "xyieldable.h"  // XR_RESUME_TIMEOUT
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../os/os_thread.h"
#include <fcntl.h>
#include <errno.h>
#include <sched.h>

// ========== Descriptor Cache Pool ==========

// Initialize cache pool (lock-free, no mutex needed)
static void poll_cache_init(XrPollCache *cache) {
    XR_DCHECK(cache != NULL, "poll_cache_init: NULL cache");
    atomic_store_explicit(&cache->head, NULL, memory_order_relaxed);
}

// Cleanup cache pool (drain all cached descriptors)
//
// WHY THIS DESIGN (destroy mutex/cond before free):
//   Each cached pd has a pthread_mutex/cond initialized exactly once by
//   xr_poll_cache_alloc's "new allocation" branch. Before freeing the
//   underlying memory we must destroy them to avoid leaking kernel
//   objects (on Linux these wrap futex state, on macOS they wrap ulock).
static void poll_cache_cleanup(XrPollCache *cache) {
    XrPollDesc *pd = atomic_exchange_explicit(&cache->head, NULL, memory_order_acquire);
    while (pd) {
        XrPollDesc *next = pd->link;
        xr_cond_destroy(&pd->block_cond);
        xr_mutex_destroy(&pd->block_mu);
        xr_free(pd);
        pd = next;
    }
}

// Allocate pollDesc (lock-free Treiber stack pop)
//
// INVARIANT: block_mu and block_cond are initialized EXACTLY ONCE — on
// first allocation from xr_calloc. Recycled pds (popped from the
// Treiber stack) keep their already-initialized mutex/cond. Calling
// pthread_mutex_init twice on the same mutex is explicit UB per POSIX,
// and caused a latent kernel-object bug prior to this fix.
XrPollDesc *xr_poll_cache_alloc(XrPollCache *cache) {
    XrPollDesc *pd;
    bool is_new = false;
    do {
        pd = atomic_load_explicit(&cache->head, memory_order_acquire);
        if (!pd) {
            // Cache empty, allocate new and initialize mutex/cond ONCE
            pd = (XrPollDesc *) xr_calloc(1, sizeof(XrPollDesc));
            if (pd) {
                xr_mutex_init(&pd->block_mu);
                xr_cond_init(&pd->block_cond);
                is_new = true;
            }
            break;
        }
    } while (!atomic_compare_exchange_weak_explicit(&cache->head, &pd, pd->link,
                                                    memory_order_acq_rel, memory_order_acquire));

    if (pd) {
        // Reset all scalar fields except the (already-initialized) mutex/cond.
        pd->link = NULL;
        pd->fd = -1;
        atomic_store(&pd->fdseq, 0);
        pd->owner_worker_id = -1;
        atomic_store(&pd->rg, XR_PD_NIL);
        atomic_store(&pd->wg, XR_PD_NIL);
        atomic_store(&pd->closing, false);
        pd->rd = 0;
        pd->wd = 0;
        pd->rrun = false;
        pd->wrun = false;
        pd->rt_storage.slot = XR_TW_SLOT_INACTIVE;
        pd->wt_storage.slot = XR_TW_SLOT_INACTIVE;
        pd->user_data = NULL;
        pd->netpoll = NULL;
        (void) is_new;  // currently informational; reserved for future asserts
    }

    return pd;
}

// Free pollDesc (lock-free Treiber stack push)
void xr_poll_cache_free(XrPollCache *cache, XrPollDesc *pd) {
    // Increment sequence to invalidate stale notifications
    atomic_fetch_add(&pd->fdseq, 1);

    XrPollDesc *old_head;
    do {
        old_head = atomic_load_explicit(&cache->head, memory_order_relaxed);
        pd->link = old_head;
    } while (!atomic_compare_exchange_weak_explicit(&cache->head, &old_head, pd,
                                                    memory_order_release, memory_order_relaxed));
}

// ========== Ready List Operations ==========

// Initialize ready list
static void ready_list_init(XrReadyList *list) {
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
}

// Add coroutine to ready list (using sched_link)
static void ready_list_push(XrReadyList *list, struct XrCoroutine *coro) {
    if (!coro)
        return;

    coro->sched_link = NULL;  // New node's next is NULL

    if (list->tail) {
        // Link to tail
        list->tail->sched_link = coro;
        list->tail = coro;
    } else {
        // Empty list, set head and tail
        list->head = coro;
        list->tail = coro;
    }
    list->count++;
}

// ========== Block/Wake Mechanism ==========

// Block current thread waiting for I/O ready on pd
//
// Used by cfunc coroutines and io.c connect that cannot use yieldable mechanism.
// Uses xr_cond_wait instead of busy-wait (sched_yield loop).
// The condition variable is signaled by xr_netpoll_unblock when I/O is ready.
//
// Returns true if I/O ready, false if closed
bool xr_netpoll_block_sync(XrPollDesc *pd, int mode, XrayIsolate *X) {
    (void) X;
    _Atomic uintptr_t *gpp = (mode == XR_POLL_READ) ? &pd->rg : &pd->wg;

    // CAS state machine: consume ready or set wait
    for (;;) {
        uintptr_t old = atomic_load(gpp);

        if (old == XR_PD_READY) {
            if (atomic_compare_exchange_strong(gpp, &old, XR_PD_NIL)) {
                return true;
            }
            continue;
        }

        if (old == XR_PD_NIL) {
            if (atomic_compare_exchange_strong(gpp, &old, XR_PD_WAIT)) {
                break;
            }
            continue;
        }

        return false;
    }

    if (atomic_load(&pd->closing)) {
        uintptr_t old = atomic_exchange(gpp, XR_PD_NIL);
        return old == XR_PD_READY;
    }

    // Wait on condition variable instead of busy-wait
    xr_mutex_lock(&pd->block_mu);
    while (atomic_load(gpp) == XR_PD_WAIT && !atomic_load(&pd->closing)) {
        xr_cond_wait(&pd->block_cond, &pd->block_mu);
    }
    xr_mutex_unlock(&pd->block_mu);

    uintptr_t old = atomic_exchange(gpp, XR_PD_NIL);
    return old == XR_PD_READY;
}

// Unblock, wake waiting coroutine
// Returns the woken coroutine (if any)
// Also signals condition variable for threads blocked in xr_netpoll_block_sync
struct XrCoroutine *xr_netpoll_unblock(XrPollDesc *pd, int mode, bool io_ready) {
    _Atomic uintptr_t *gpp = (mode == XR_POLL_READ) ? &pd->rg : &pd->wg;

    for (;;) {
        uintptr_t old = atomic_load(gpp);

        if (old == XR_PD_READY) {
            return NULL;
        }

        if (old == XR_PD_NIL && !io_ready) {
            return NULL;
        }

        uintptr_t new_val = io_ready ? XR_PD_READY : XR_PD_NIL;

        if (atomic_compare_exchange_strong(gpp, &old, new_val)) {
            if (old == XR_PD_WAIT) {
                // Thread may be blocked in xr_cond_wait, signal it
                xr_mutex_lock(&pd->block_mu);
                xr_cond_signal(&pd->block_cond);
                xr_mutex_unlock(&pd->block_mu);
                return NULL;
            } else if (old > XR_PD_WAIT) {
                // Decrement yieldable I/O waiter count
                if (pd->netpoll) {
                    atomic_fetch_sub(&pd->netpoll->waiters, 1);
                }
                return (struct XrCoroutine *) old;
            }
            return NULL;
        }
    }
}

// Mark I/O ready, add waiting coroutine to ready list
void xr_netpoll_ready(XrReadyList *list, XrPollDesc *pd, int mode) {
    XR_DCHECK(list != NULL, "netpoll_ready: NULL list");
    XR_DCHECK(pd != NULL, "netpoll_ready: NULL pd");
    struct XrCoroutine *rg = NULL;
    struct XrCoroutine *wg = NULL;

    if (mode & XR_POLL_READ) {
        rg = xr_netpoll_unblock(pd, XR_POLL_READ, true);
    }

    if (mode & XR_POLL_WRITE) {
        wg = xr_netpoll_unblock(pd, XR_POLL_WRITE, true);
    }

    if (rg)
        ready_list_push(list, rg);
    if (wg)
        ready_list_push(list, wg);
}

// ========== Common Init/Cleanup ==========

// Create wakeup pipe
static int create_wakeup_pipe(int pipe_fds[2]) {
#ifdef XR_OS_WINDOWS
// Windows: a full IOCP-based wakeup integration is tracked separately
// (see xnetpoll_iocp.c which is not yet wired into netpoll_default_ops).
// Fail-fast here instead of silently returning -1 — callers that try to
// initialize netpoll on Windows will see a clear error rather than an
// ambiguous runtime hang.
#error                                                                                             \
    "Windows netpoll wakeup pipe not implemented; wire xnetpoll_iocp.c or build without networking"
#else
    if (pipe(pipe_fds) < 0) {
        return -1;
    }

    // Set non-blocking
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

    return 0;
#endif
}

// Close wakeup pipe
static void close_wakeup_pipe(int pipe_fds[2]) {
#ifndef XR_OS_WINDOWS
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);
    pipe_fds[0] = pipe_fds[1] = -1;
#endif
}

// ========== Platform-specific Backend (ops tables) ==========

#define XR_NETPOLL_INCLUDED

#ifdef XR_OS_MACOS
#include "../os/netpoll/netpoll_kqueue.c"
#elif defined(XR_OS_LINUX)
#include "../os/netpoll/netpoll_epoll.c"
#if defined(XR_HAS_IO_URING)
#include "../os/netpoll/netpoll_iouring.c"
#endif
#endif

// Select best available ops for current platform.
// On Linux with XR_HAS_IO_URING: try io_uring first, fall back to epoll.
static const XrNetpollOps *netpoll_default_ops(void) {
#ifdef XR_OS_MACOS
    return &kqueue_ops;
#elif defined(XR_OS_LINUX)
#if defined(XR_HAS_IO_URING)
    return &iouring_ops;
#else
    return &epoll_ops;
#endif
#else
    return NULL;
#endif
}

// ========== Per-Worker Local Poll Implementation ==========
//
// Each worker gets its own kqueue/epoll fd. The global XrNetpoll still
// owns the fd_map and PollDesc cache. Per-worker poll eliminates
// cross-worker contention on the shared poller.

int xr_local_poll_init(XrLocalPoll *lp) {
    if (!lp)
        return -1;
    lp->poll_fd = -1;
    lp->wakeup_pipe[0] = lp->wakeup_pipe[1] = -1;
    atomic_store(&lp->break_pending, false);

#ifdef XR_OS_MACOS
    lp->poll_fd = kqueue();
#elif defined(XR_OS_LINUX)
    lp->poll_fd = epoll_create1(EPOLL_CLOEXEC);
#else
    return -1;
#endif
    if (lp->poll_fd < 0)
        return -1;

    if (create_wakeup_pipe(lp->wakeup_pipe) < 0) {
        close(lp->poll_fd);
        lp->poll_fd = -1;
        return -1;
    }

    // Register wakeup pipe with per-worker poller
#ifdef XR_OS_MACOS
    struct kevent kev;
    EV_SET(&kev, lp->wakeup_pipe[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(lp->poll_fd, &kev, 1, NULL, 0, NULL);
#elif defined(XR_OS_LINUX)
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.fd = lp->wakeup_pipe[0]};
    epoll_ctl(lp->poll_fd, EPOLL_CTL_ADD, lp->wakeup_pipe[0], &ev);
#endif
    return 0;
}

void xr_local_poll_cleanup(XrLocalPoll *lp) {
    if (!lp)
        return;
    close_wakeup_pipe(lp->wakeup_pipe);
    if (lp->poll_fd >= 0) {
        close(lp->poll_fd);
        lp->poll_fd = -1;
    }
}

int xr_local_poll_add_fd(XrLocalPoll *lp, int fd, XrPollDesc *pd) {
    if (!lp || lp->poll_fd < 0 || fd < 0)
        return -1;
#ifdef XR_OS_MACOS
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, pd);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, pd);
    return kevent(lp->poll_fd, kev, 2, NULL, 0, NULL);
#elif defined(XR_OS_LINUX)
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = pd};
    return epoll_ctl(lp->poll_fd, EPOLL_CTL_ADD, fd, &ev);
#else
    return -1;
#endif
}

void xr_local_poll_del_fd(XrLocalPoll *lp, int fd) {
    if (!lp || lp->poll_fd < 0 || fd < 0)
        return;
#ifdef XR_OS_MACOS
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(lp->poll_fd, kev, 2, NULL, 0, NULL);
#elif defined(XR_OS_LINUX)
    epoll_ctl(lp->poll_fd, EPOLL_CTL_DEL, fd, NULL);
#endif
}

int xr_local_poll_events(XrLocalPoll *lp, int64_t delta_ns, XrReadyList *list) {
    if (!lp || lp->poll_fd < 0)
        return 0;

#ifdef XR_OS_MACOS
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
    int n = kevent(lp->poll_fd, NULL, 0, events, 128, timeout);
    if (n < 0)
        return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++) {
        struct kevent *ev = &events[i];
        if ((int) ev->ident == lp->wakeup_pipe[0]) {
            char buf[16];
            while (read(lp->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
            }
            atomic_store(&lp->break_pending, false);
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

#elif defined(XR_OS_LINUX)
    int timeout_ms = 0;
    if (delta_ns == 0)
        timeout_ms = 0;
    else if (delta_ns > 0)
        timeout_ms = (int) (delta_ns / 1000000);
    else
        timeout_ms = -1;

    struct epoll_event events[128];
    int n = epoll_wait(lp->poll_fd, events, 128, timeout_ms);
    if (n < 0)
        return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == lp->wakeup_pipe[0]) {
            char buf[16];
            while (read(lp->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
            }
            atomic_store(&lp->break_pending, false);
            continue;
        }
        XrPollDesc *pd = (XrPollDesc *) events[i].data.ptr;
        if (!pd)
            continue;
        int mode = 0;
        if (events[i].events & (EPOLLIN | EPOLLHUP))
            mode |= XR_POLL_READ;
        if (events[i].events & EPOLLOUT)
            mode |= XR_POLL_WRITE;
        if (events[i].events & EPOLLERR)
            mode = XR_POLL_BOTH;
        if (mode)
            xr_netpoll_ready(list, pd, mode);
    }
    return n;
#else
    return 0;
#endif
}

void xr_local_poll_wakeup(XrLocalPoll *lp) {
    if (!lp || lp->poll_fd < 0)
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&lp->break_pending, &expected, true))
        return;
    char c = 0;
    ssize_t n;
    do {
        n = write(lp->wakeup_pipe[1], &c, 1);
    } while (n < 0 && errno == EINTR);
}

// ========== Unified Public API (ops dispatch) ==========

int xr_netpoll_init(XrNetpoll *np) {
    if (atomic_load(&np->inited))
        return 0;

    np->ops = netpoll_default_ops();
    if (!np->ops)
        return -1;

    poll_cache_init(&np->cache);
    memset(np->fd_pages, 0, sizeof(np->fd_pages));
    np->poll_fd = -1;
    np->backend_state = NULL;

    if (create_wakeup_pipe(np->wakeup_pipe) < 0)
        return -1;

    if (np->ops->init(np) < 0) {
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
        // io_uring failed (old kernel?), fall back to epoll
        np->ops = &epoll_ops;
        if (np->ops->init(np) < 0) {
            close_wakeup_pipe(np->wakeup_pipe);
            return -1;
        }
#else
        close_wakeup_pipe(np->wakeup_pipe);
        return -1;
#endif
    }

    atomic_store(&np->waiters, 0);
    atomic_store(&np->break_pending, false);
    atomic_store(&np->inited, true);
    return 0;
}

void xr_netpoll_cleanup(XrNetpoll *np) {
    if (!atomic_load(&np->inited))
        return;

    close_wakeup_pipe(np->wakeup_pipe);
    if (np->ops)
        np->ops->cleanup(np);
    poll_cache_cleanup(&np->cache);
    xr_fdmap_destroy(np);

    atomic_store(&np->inited, false);
}

// Open fd: shared fd_map + timer logic, delegates backend_add to ops.
XrPollDesc *xr_netpoll_open(XrNetpoll *np, int fd) {
    if (!atomic_load(&np->inited) || fd < 0 || fd >= XR_NETPOLL_FD_MAX)
        return NULL;

    // Fast path: already registered
    XrPollDesc *pd = xr_fdmap_get(np, fd);
    if (pd && !atomic_load(&pd->closing)) {
        pd->netpoll = np;
        return pd;
    }

    // fd reuse (closing pd still in map): cancel old timers, re-register
    if (pd && atomic_load(&pd->closing)) {
        int old_owner = pd->owner_worker_id;
        if (old_owner >= 0) {
            XrWorker *current = xr_current_worker();
            XrTimerWheel *old_tw = NULL;
            if (current && current->p.runtime) {
                XrRuntime *rt = current->p.runtime;
                if (old_owner < rt->worker_count)
                    old_tw = rt->workers[old_owner].p.timer_wheel;
            }
            if (old_tw && current && current->p.id == old_owner) {
                if (pd->rt_storage.slot != XR_TW_SLOT_INACTIVE)
                    xr_twheel_cancel_timer(old_tw, &pd->rt_storage);
                if (pd->wt_storage.slot != XR_TW_SLOT_INACTIVE)
                    xr_twheel_cancel_timer(old_tw, &pd->wt_storage);
            } else if (old_tw) {
                if (pd->rt_storage.slot != XR_TW_SLOT_INACTIVE &&
                    atomic_load_explicit(&pd->rt_storage.state, memory_order_acquire) !=
                        XR_TIMER_STATE_ZOMBIE)
                    xr_timer_queue_cancel(old_tw, &pd->rt_storage, NULL);
                if (pd->wt_storage.slot != XR_TW_SLOT_INACTIVE &&
                    atomic_load_explicit(&pd->wt_storage.state, memory_order_acquire) !=
                        XR_TIMER_STATE_ZOMBIE)
                    xr_timer_queue_cancel(old_tw, &pd->wt_storage, NULL);
            }
        }
        atomic_fetch_add(&pd->rseq, 1);
        atomic_fetch_add(&pd->wseq, 1);

        // Re-register with shared kqueue
        np->ops->add_fd(np, fd, pd);

        atomic_store(&pd->closing, false);
        atomic_store(&pd->rg, XR_PD_NIL);
        atomic_store(&pd->wg, XR_PD_NIL);
        pd->rrun = false;
        pd->wrun = false;
        pd->rd = 0;
        pd->wd = 0;
        pd->netpoll = np;
        return pd;
    }

    // New fd: allocate pd, register with backend
    pd = xr_poll_cache_alloc(&np->cache);
    if (!pd)
        return NULL;

    pd->fd = fd;
    pd->netpoll = np;

    // Register with shared kqueue
    if (np->ops->add_fd(np, fd, pd) < 0) {
        xr_poll_cache_free(&np->cache, pd);
        return NULL;
    }

    XrPollDesc *expected = NULL;
    if (!xr_fdmap_cas(np, fd, &expected, pd)) {
        np->ops->del_fd(np, fd);
        xr_poll_cache_free(&np->cache, pd);
        return expected;
    }
    return pd;
}

// Close fd: shared timer/cache/map logic, delegates backend_del to ops.
void xr_netpoll_close(XrNetpoll *np, XrPollDesc *pd) {
    if (!pd)
        return;

    atomic_store(&pd->closing, true);

    // Cancel active timers (cross-worker safe)
    XrTimerWheel *tw = xr_netpoll_get_timer_wheel(pd);
    bool can_free_pd = true;
    if (tw) {
        XrWorker *current = xr_current_worker();
        bool is_owner = current && (current->p.id == pd->owner_worker_id);
        if (is_owner) {
            if (pd->rrun) {
                xr_twheel_cancel_timer(tw, &pd->rt_storage);
                pd->rrun = false;
            }
            if (pd->wrun) {
                xr_twheel_cancel_timer(tw, &pd->wt_storage);
                pd->wrun = false;
            }
        } else {
            if (pd->rrun) {
                xr_timer_queue_cancel(tw, &pd->rt_storage, NULL);
                pd->rrun = false;
            }
            if (pd->wrun) {
                xr_timer_queue_cancel(tw, &pd->wt_storage, NULL);
                pd->wrun = false;
            }
            can_free_pd = false;
        }
    }

    int fd = pd->fd;
    if (fd >= 0 && fd < XR_NETPOLL_FD_MAX) {
        XrPollDesc *exp = pd;
        xr_fdmap_cas(np, fd, &exp, NULL);
    }

    // Unregister from owner worker's local poll (if bound)
    if (fd >= 0 && pd->owner_worker_id >= 0) {
        XrWorker *current = xr_current_worker();
        if (current && current->p.runtime) {
            XrRuntime *rt = current->p.runtime;
            if (pd->owner_worker_id < rt->worker_count) {
                XrLocalPoll *lp = &rt->workers[pd->owner_worker_id].p.local_poll;
                xr_local_poll_del_fd(lp, fd);
            }
        }
    }

    // Unregister from shared kqueue
    np->ops->del_fd(np, fd);

    xr_netpoll_unblock(pd, XR_POLL_READ, false);
    xr_netpoll_unblock(pd, XR_POLL_WRITE, false);

    if (can_free_pd) {
        xr_poll_cache_free(&np->cache, pd);
    } else {
        xr_netpoll_deferred_free(np, pd);
    }
}

XrReadyList xr_netpoll_poll(XrNetpoll *np, int64_t delta_ns) {
    XrReadyList list;
    ready_list_init(&list);
    if (!atomic_load(&np->inited))
        return list;

    np->ops->poll_events(np, delta_ns, &list);
    return list;
}

void xr_netpoll_break(XrNetpoll *np) {
    if (!atomic_load(&np->inited) || !np->ops)
        return;
    np->ops->wakeup(np);
}

bool xr_netpoll_any_waiters(XrNetpoll *np) {
    return atomic_load(&np->waiters) > 0;
}

// Wait for I/O ready
// X: VM instance (for coroutine scheduling, NULL downgrades to busy-wait)
int xr_netpoll_wait(XrNetpoll *np, XrPollDesc *pd, int mode, XrayIsolate *X) {
    XR_DCHECK(np != NULL, "netpoll_wait: NULL netpoll");
    XR_DCHECK(pd != NULL, "netpoll_wait: NULL poll desc");
    // Check if already closed
    if (atomic_load(&pd->closing)) {
        return XR_POLL_ERR_CLOSING;
    }

    // Increment waiter count
    atomic_fetch_add(&np->waiters, 1);

    // Block wait (supports coroutine suspend)
    bool ready = xr_netpoll_block_sync(pd, mode, X);

    // Decrement waiter count
    atomic_fetch_sub(&np->waiters, 1);

    if (!ready) {
        if (atomic_load(&pd->closing)) {
            return XR_POLL_ERR_CLOSING;
        }
        return XR_POLL_ERR_TIMEOUT;
    }

    return XR_POLL_OK;
}

// Read timeout callback (called by Timer Wheel)
static void read_deadline_callback(void *arg) {
    XrPollDesc *pd = (XrPollDesc *) arg;
    // Use the saved sequence number from when the timer was set,
    // not the current sequence (which may have been incremented by new timer set)
    uintptr_t seq = pd->rseq_saved;
    xr_netpoll_deadline_impl(pd, seq, true);
}

// Write timeout callback (called by Timer Wheel)
static void write_deadline_callback(void *arg) {
    XrPollDesc *pd = (XrPollDesc *) arg;
    // Use the saved sequence number from when the timer was set,
    // not the current sequence (which may have been incremented by new timer set)
    uintptr_t seq = pd->wseq_saved;
    xr_netpoll_deadline_impl(pd, seq, false);
}

// Timeout callback implementation (lock-free design)
// Check sequence, skip if mismatch (timer cancelled)
void xr_netpoll_deadline_impl(XrPollDesc *pd, uintptr_t seq, bool read) {
    XR_DCHECK(pd != NULL, "netpoll_deadline_impl: NULL pd");
    _Atomic uintptr_t *gpp = read ? &pd->rg : &pd->wg;
    _Atomic uintptr_t *seqp = read ? &pd->rseq : &pd->wseq;

    if (atomic_load(seqp) != seq) {
        return;
    }

    if (read) {
        pd->rd = -1;
        pd->rrun = false;
    } else {
        pd->wd = -1;
        pd->wrun = false;
    }

    // Try to wake waiting coroutine
    for (;;) {
        uintptr_t old = atomic_load(gpp);

        if (old == XR_PD_NIL || old == XR_PD_READY) {
            return;
        }

        if (old == XR_PD_WAIT) {
            if (atomic_compare_exchange_weak(gpp, &old, XR_PD_NIL)) {
                // Signal condition variable for threads in xr_netpoll_block_sync
                xr_mutex_lock(&pd->block_mu);
                xr_cond_signal(&pd->block_cond);
                xr_mutex_unlock(&pd->block_mu);
                return;
            }
            continue;
        }

        // old is coroutine pointer, wake it
        if (atomic_compare_exchange_weak(gpp, &old, XR_PD_NIL)) {
            XrCoroutine *coro = (XrCoroutine *) old;
            xr_coro_resume_store(coro, XR_RESUME_TIMEOUT);

            xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);

            // Add coroutine to target Worker inbox for scheduling.
            // Respects Coro.lockThread(): locked coros return to their locked worker.
            XrWorker *current_worker = xr_current_worker();
            if (current_worker && current_worker->p.runtime) {
                XrRuntime *rt = current_worker->p.runtime;
                int target_id = xr_coro_wake_target_id(coro);
                if (target_id < 0 || target_id >= rt->worker_count) {
                    target_id = 0;
                }
                xr_worker_inbox_enqueue(rt, target_id, coro);
            }

            return;
        }
    }
}

// Rebind pd to current worker when coro has migrated.
// Cancels any running timers on the old owner's wheel (via cross-worker cancel
// queue), deregisters fd from old worker's local poll, and re-registers with
// the current worker.  After this call pd->owner_worker_id == current->p.id.
static void netpoll_rebind_worker(XrPollDesc *pd, XrWorker *current) {
    XR_DCHECK(pd != NULL, "netpoll_rebind_worker: NULL pd");
    XR_DCHECK(current != NULL, "netpoll_rebind_worker: NULL current");
    int old_id = pd->owner_worker_id;
    int new_id = current->p.id;
    XR_DCHECK(old_id != new_id, "netpoll_rebind_worker: already owner");

    XrRuntime *rt = current->p.runtime;
    XR_DCHECK(rt != NULL, "netpoll_rebind_worker: NULL runtime");

    // Cancel running timers on old owner's wheel via cross-worker cancel queue.
    // Sequence bumps (done by caller) will invalidate stale callbacks.
    if (old_id >= 0 && old_id < rt->worker_count) {
        XrWorker *old_w = &rt->workers[old_id];
        XrTimerWheel *old_tw = old_w->p.timer_wheel;
        if (old_tw) {
            if (pd->rrun) {
                xr_timer_queue_cancel(old_tw, &pd->rt_storage, NULL);
                pd->rrun = false;
            }
            if (pd->wrun) {
                xr_timer_queue_cancel(old_tw, &pd->wt_storage, NULL);
                pd->wrun = false;
            }
        }
        // Deregister fd from old worker's local poll
        if (pd->fd >= 0 && old_w->p.local_poll.poll_fd >= 0) {
            xr_local_poll_del_fd(&old_w->p.local_poll, pd->fd);
        }
    }

    // Bind to current worker
    pd->owner_worker_id = new_id;
    if (pd->fd >= 0 && current->p.local_poll.poll_fd >= 0) {
        xr_local_poll_add_fd(&current->p.local_poll, pd->fd, pd);
    }
}

// Set read/write timeout (fd bound to Worker)
//
// Key design:
// 1. On first call, bind fd to current Worker
// 2. Use bound Worker's Timer Wheel (no cross-Worker access)
// 3. Increment sequence to invalidate old timer callbacks
//
// If the coroutine has migrated to a different worker,
// rebind the pd to the current worker so that deadline timers are always set.
// The old "skip timer ops" silent degradation is eliminated.
void xr_netpoll_set_deadline(XrNetpoll *np, XrPollDesc *pd, int64_t deadline, int mode,
                             XrTimerWheel *tw) {
    (void) np;
    (void) tw;  // Ignore passed tw, use bound Worker's Timer Wheel

    // Bind fd to current Worker on first I/O
    xr_netpoll_bind_worker(pd);

    // If coro migrated, rebind pd to current worker instead of
    // silently skipping timer ops on the wrong wheel.
    XrWorker *current = xr_current_worker();
    if (current && pd->owner_worker_id >= 0 && current->p.id != pd->owner_worker_id) {
        netpoll_rebind_worker(pd, current);
    }

    XrTimerWheel *bound_tw = xr_netpoll_get_timer_wheel(pd);

    if (mode & XR_POLL_READ) {
        // Increment sequence to invalidate old read timer callbacks
        uintptr_t seq = atomic_fetch_add(&pd->rseq, 1) + 1;

        pd->rd = deadline;

        // Cancel existing read timer
        if (pd->rrun && bound_tw) {
            xr_twheel_cancel_timer(bound_tw, &pd->rt_storage);
            pd->rrun = false;
        }

        // Set new read timer
        if (deadline > 0 && bound_tw) {
            pd->rseq_saved = seq;
            pd->rt = &pd->rt_storage;
            int64_t timeout_ms = deadline / 1000000;
            xr_twheel_set_timer(bound_tw, pd->rt, read_deadline_callback, pd, timeout_ms);
            pd->rrun = true;
        }
    }

    if (mode & XR_POLL_WRITE) {
        // Increment sequence to invalidate old write timer callbacks
        uintptr_t seq = atomic_fetch_add(&pd->wseq, 1) + 1;

        pd->wd = deadline;

        // Cancel existing write timer
        if (pd->wrun && bound_tw) {
            xr_twheel_cancel_timer(bound_tw, &pd->wt_storage);
            pd->wrun = false;
        }

        // Set new write timer
        if (deadline > 0 && bound_tw) {
            pd->wseq_saved = seq;
            pd->wt = &pd->wt_storage;
            int64_t timeout_ms = deadline / 1000000;
            xr_twheel_set_timer(bound_tw, pd->wt, write_deadline_callback, pd, timeout_ms);
            pd->wrun = true;
        }
    }
}

// ========== fd bound to Worker ==========

// Bind fd to current Worker and register with worker's local poll.
// Dual-registration (shared netpoll + local poll) is safe: the CAS state
// machine in xr_netpoll_unblock ensures only one waker succeeds.
int xr_netpoll_bind_worker(XrPollDesc *pd) {
    if (!pd)
        return -1;

    // Already bound, return directly
    if (pd->owner_worker_id >= 0) {
        return pd->owner_worker_id;
    }

    // Bind to current Worker
    XrWorker *worker = xr_current_worker();
    if (worker) {
        pd->owner_worker_id = worker->p.id;
        // Register fd with worker's local poll for zero-contention IO delivery
        if (pd->fd >= 0 && worker->p.local_poll.poll_fd >= 0) {
            xr_local_poll_add_fd(&worker->p.local_poll, pd->fd, pd);
        }
        return worker->p.id;
    }

    return -1;
}

// Get Timer Wheel bound to fd
XrTimerWheel *xr_netpoll_get_timer_wheel(XrPollDesc *pd) {
    XrWorker *worker = NULL;

    // Prefer bound Worker
    if (pd && pd->owner_worker_id >= 0) {
        XrWorker *current = xr_current_worker();
        if (current && current->p.runtime) {
            XrRuntime *rt = current->p.runtime;
            if (pd->owner_worker_id < rt->worker_count) {
                worker = &rt->workers[pd->owner_worker_id];
            }
        }
    }

    // If not bound, use current Worker
    if (!worker) {
        worker = xr_current_worker();
    }

    return worker ? worker->p.timer_wheel : NULL;
}

// ========== Deferred Free (cross-worker PollDesc cleanup) ==========

// Queue PollDesc for deferred free on owner worker's Treiber stack.
// Thread-safe: multiple producers can push concurrently.
void xr_netpoll_deferred_free(XrNetpoll *np, XrPollDesc *pd) {
    if (!pd)
        return;
    (void) np;

    XrWorker *current = xr_current_worker();
    if (!current) {
        // No worker context (shutdown?), free directly
        xr_poll_cache_free(&np->cache, pd);
        return;
    }

    XrRuntime *rt = current->p.runtime;
    if (!rt || pd->owner_worker_id < 0 || pd->owner_worker_id >= rt->worker_count) {
        xr_poll_cache_free(&np->cache, pd);
        return;
    }

    // Push onto owner worker's deferred free stack (lock-free CAS loop)
    XrProc *owner_p = &rt->workers[pd->owner_worker_id].p;
    void *old_head = atomic_load_explicit(&owner_p->deferred_free_head, memory_order_relaxed);
    do {
        pd->link = (XrPollDesc *) old_head;
    } while (!atomic_compare_exchange_weak_explicit(&owner_p->deferred_free_head, &old_head, pd,
                                                    memory_order_release, memory_order_relaxed));
}

// Drain deferred free queue on current worker.
// Called by owner worker during poll cycle — single consumer, no lock needed.
void xr_netpoll_drain_deferred(XrNetpoll *np, XrProc *p) {
    if (!p || !np)
        return;

    // Atomic swap to get entire list (O(1))
    void *head = atomic_exchange_explicit(&p->deferred_free_head, NULL, memory_order_acquire);

    // Walk list and free each PollDesc
    XrPollDesc *pd = (XrPollDesc *) head;
    while (pd) {
        XrPollDesc *next = pd->link;
        xr_poll_cache_free(&np->cache, pd);
        pd = next;
    }
}

#else  // XR_OS_WINDOWS

/* ============================================================
 * Windows stub backend.
 *
 * The native poller pulls in kqueue (Apple/BSD), epoll (Linux)
 * and a pipe-based wakeup channel. None of those have a direct
 * Win32 equivalent; the long-term plan is an IOCP backend, but
 * shipping that is a separate, sizable effort.
 *
 * For now every public entry point returns a clear failure so
 * the runtime links and any caller that actually exercises
 * networking gets an obvious diagnostic instead of a silent
 * deadlock. Coroutines and pure CPU code paths are unaffected.
 * ============================================================ */

#include "xcoroutine.h"
#include "xworker.h"
#include <stdlib.h>
#include <string.h>

void xr_poll_cache_free(XrPollCache *cache, XrPollDesc *pd) {
    (void) cache;
    (void) pd;
}

XrPollDesc *xr_poll_cache_alloc(XrPollCache *cache) {
    (void) cache;
    return NULL;
}

bool xr_netpoll_block_sync(XrPollDesc *pd, int mode, struct XrayIsolate *X) {
    (void) pd;
    (void) mode;
    (void) X;
    return false;
}

void xr_netpoll_ready(XrReadyList *list, XrPollDesc *pd, int mode) {
    (void) list;
    (void) pd;
    (void) mode;
}

struct XrCoroutine *xr_netpoll_unblock(XrPollDesc *pd, int mode, bool io_ready) {
    (void) pd;
    (void) mode;
    (void) io_ready;
    return NULL;
}

int xr_local_poll_init(XrLocalPoll *lp) {
    (void) lp;
    return -1;
}

void xr_local_poll_cleanup(XrLocalPoll *lp) {
    (void) lp;
}

int xr_local_poll_add_fd(XrLocalPoll *lp, int fd, XrPollDesc *pd) {
    (void) lp;
    (void) fd;
    (void) pd;
    return -1;
}

void xr_local_poll_del_fd(XrLocalPoll *lp, int fd) {
    (void) lp;
    (void) fd;
}

int xr_local_poll_events(XrLocalPoll *lp, int64_t delta_ns, XrReadyList *list) {
    (void) lp;
    (void) delta_ns;
    (void) list;
    return 0;
}

void xr_local_poll_wakeup(XrLocalPoll *lp) {
    (void) lp;
}

int xr_netpoll_init(XrNetpoll *np) {
    if (np)
        memset(np, 0, sizeof(*np));
    return -1;
}

void xr_netpoll_cleanup(XrNetpoll *np) {
    (void) np;
}

XrPollDesc *xr_netpoll_open(XrNetpoll *np, int fd) {
    (void) np;
    (void) fd;
    return NULL;
}

void xr_netpoll_close(XrNetpoll *np, XrPollDesc *pd) {
    (void) np;
    (void) pd;
}

int xr_netpoll_wait(XrNetpoll *np, XrPollDesc *pd, int mode, struct XrayIsolate *X) {
    (void) np;
    (void) pd;
    (void) mode;
    (void) X;
    return -1;
}

XrReadyList xr_netpoll_poll(XrNetpoll *np, int64_t delta_ns) {
    (void) np;
    (void) delta_ns;
    XrReadyList r;
    memset(&r, 0, sizeof r);
    return r;
}

void xr_netpoll_break(XrNetpoll *np) {
    (void) np;
}

bool xr_netpoll_any_waiters(XrNetpoll *np) {
    (void) np;
    return false;
}

void xr_netpoll_set_deadline(XrNetpoll *np, XrPollDesc *pd, int64_t deadline, int mode,
                             XrTimerWheel *tw) {
    (void) np;
    (void) pd;
    (void) deadline;
    (void) mode;
    (void) tw;
}

void xr_netpoll_deadline_impl(XrPollDesc *pd, uintptr_t seq, bool read) {
    (void) pd;
    (void) seq;
    (void) read;
}

int xr_netpoll_bind_worker(XrPollDesc *pd) {
    (void) pd;
    return -1;
}

struct XrTimerWheel *xr_netpoll_get_timer_wheel(XrPollDesc *pd) {
    (void) pd;
    return NULL;
}

void xr_netpoll_deferred_free(XrNetpoll *np, XrPollDesc *pd) {
    (void) np;
    (void) pd;
}

void xr_netpoll_drain_deferred(XrNetpoll *np, struct XrProc *p) {
    (void) np;
    (void) p;
}

#endif  // XR_OS_WINDOWS
