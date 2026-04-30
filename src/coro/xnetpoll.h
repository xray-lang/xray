/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll.h - Cross-platform network poller
 *
 * KEY CONCEPT:
 *   Platform-independent interface layer with backend implementations:
 *   kqueue (macOS), epoll (Linux), IOCP (Windows).
 *   Deeply integrated with coroutine scheduler.
 *
 * CORE FLOW:
 *   1. Coroutine executes I/O operation
 *   2. If I/O would block, coroutine suspends and registers with netpoll
 *   3. netpoll thread waits for I/O ready events
 *   4. On I/O ready, wake corresponding coroutine
 */

#ifndef XNETPOLL_H
#define XNETPOLL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../os/os_thread.h"
#include "../base/xdefs.h"
#include "xtimer_wheel.h"  // XrTWheelTimer

// Forward declarations
struct XrCoroutine;
struct XrayIsolate;
struct XrCoroState;
struct XrProc;

// ========== Error Codes ==========

typedef enum {
    XR_POLL_OK = 0,           // Success
    XR_POLL_ERR_CLOSING = 1,  // fd closed
    XR_POLL_ERR_TIMEOUT = 2,  // I/O timeout
    XR_POLL_ERR_INVALID = 3   // Invalid fd
} XrPollError;

// ========== I/O Mode ==========

typedef enum {
    XR_POLL_READ = 0x01,   // Read ready
    XR_POLL_WRITE = 0x02,  // Write ready
    XR_POLL_BOTH = 0x03    // Both read and write
} XrPollMode;

// ========== Poll Descriptor State ==========

typedef enum {
    XR_PD_NIL = 0,    // No state
    XR_PD_READY = 1,  // I/O ready
    XR_PD_WAIT = 2    // Waiting
} XrPdState;

// ========== Poll Descriptor (one per fd, lock-free design) ==========

// Design notes:
// - Timer embedded in pollDesc, not coroutine
// - Lock-free cancellation via sequence number
// - rg/wg store coroutine pointer directly, no blocked queue

typedef struct XrPollDesc {
    struct XrPollDesc *link;  // Free list link

    int fd;                  // File descriptor
    _Atomic uint32_t fdseq;  // fd sequence (prevent stale notifications from fd reuse)

    // fd bound to fixed Worker (Port model)
    int owner_worker_id;  // Bound Worker ID (-1 = unbound)

    // Read/write waiting coroutine (atomic ops, state machine)
    // State: XR_PD_NIL -> XR_PD_WAIT -> coro ptr -> XR_PD_READY -> XR_PD_NIL
    _Atomic uintptr_t rg;  // Read wait: XR_PD_NIL/XR_PD_READY/XR_PD_WAIT/coro ptr
    _Atomic uintptr_t wg;  // Write wait: XR_PD_NIL/XR_PD_READY/XR_PD_WAIT/coro ptr

    // State flags
    _Atomic bool closing;  // Closing
    bool rrun;             // Read timer running
    bool wrun;             // Write timer running

    // Read timeout (deadline + timer + seq)
    _Atomic uintptr_t rseq;  // Read sequence (lock-free cancel: increment invalidates callback)
    uintptr_t rseq_saved;    // Read sequence saved at timer set time (for callback check)
    int64_t rd;              // Read deadline (ns, 0=no timeout, -1=expired)
    XrTWheelTimer *rt;       // Read timeout timer pointer

    // Write timeout (deadline + timer + seq)
    _Atomic uintptr_t wseq;  // Write sequence (lock-free cancel)
    uintptr_t wseq_saved;    // Write sequence saved at timer set time (for callback check)
    int64_t wd;              // Write deadline (ns, 0=no timeout, -1=expired)
    XrTWheelTimer *wt;       // Write timeout timer pointer

    // Embedded timer node storage (avoid dynamic alloc)
    XrTWheelTimer rt_storage;  // Read timer storage
    XrTWheelTimer wt_storage;  // Write timer storage

    // Condition variable for xr_netpoll_block_sync (thread-blocking wait)
    xr_mutex_t block_mu;
    xr_cond_t block_cond;

    // User data
    void *user_data;

    // Back-pointer to owning netpoll (for waiters decrement in unblock)
    struct XrNetpoll *netpoll;

    // self pointer (for timer callback)
    struct XrPollDesc *self;
} XrPollDesc;

// ========== Ready Coroutine List ==========

typedef struct XrReadyList {
    struct XrCoroutine *head;
    struct XrCoroutine *tail;
    int count;
} XrReadyList;

// ========== Poll Descriptor Cache ==========

typedef struct XrPollCache {
    _Atomic(XrPollDesc *) head;  // Treiber stack head (lock-free)
} XrPollCache;

// ========== Backend Operations (function pointer table) ==========
//
// Pluggable backend: kqueue (macOS), epoll (Linux), io_uring (Linux 5.1+).
// Selected at init time. io_uring auto-falls-back to epoll on old kernels.

typedef struct XrNetpoll XrNetpoll;

typedef struct XrNetpollOps {
    const char *name;                                      // "kqueue", "epoll", "io_uring"
    int (*init)(XrNetpoll *np);                            // Create backend handle
    void (*cleanup)(XrNetpoll *np);                        // Destroy backend handle
    int (*add_fd)(XrNetpoll *np, int fd, XrPollDesc *pd);  // Register fd
    void (*del_fd)(XrNetpoll *np, int fd);                 // Unregister fd
    int (*poll_events)(XrNetpoll *np, int64_t delta_ns, XrReadyList *list);  // Wait & collect
    void (*wakeup)(XrNetpoll *np);                                           // Interrupt wait
} XrNetpollOps;

// ========== Global Netpoll State ==========

// Two-level segmented fd_map: fd_map[fd >> 8][fd & 0xFF]
// First level: 256 page pointers (fixed, ~2KB)
// Second level: 256-entry pages, allocated on demand (~2KB each)
// Total capacity: 65536 fds, base overhead: ~2KB (vs ~512KB flat array)
#define XR_FDMAP_PAGE_BITS 8
#define XR_FDMAP_PAGE_SIZE (1 << XR_FDMAP_PAGE_BITS)             // 256
#define XR_FDMAP_PAGE_MASK (XR_FDMAP_PAGE_SIZE - 1)              // 0xFF
#define XR_FDMAP_PAGES 256                                       // 256 pages
#define XR_NETPOLL_FD_MAX (XR_FDMAP_PAGES * XR_FDMAP_PAGE_SIZE)  // 65536

typedef struct XrFdMapPage {
    _Atomic(XrPollDesc *) entries[XR_FDMAP_PAGE_SIZE];
} XrFdMapPage;

struct XrNetpoll {
    _Atomic bool inited;  // Whether initialized
    _Atomic int waiters;  // Number of coroutines waiting for I/O

    XrPollCache cache;  // Descriptor cache pool

    // Two-level fd to pd mapping (lock-free, on-demand page allocation)
    _Atomic(XrFdMapPage *) fd_pages[XR_FDMAP_PAGES];

    // Backend dispatch (set once during init, never changes)
    const XrNetpollOps *ops;

    // Backend handle (epoll fd / kqueue fd / io_uring event fd)
    int poll_fd;

    // Extra backend state (io_uring ring, etc. NULL for epoll/kqueue)
    void *backend_state;

    // Wakeup mechanism
    int wakeup_pipe[2];          // Wakeup pipe (read/write ends)
    _Atomic bool break_pending;  // Whether there's pending wakeup
};

// ========== fd_map Two-Level Access Helpers ==========

#include <stdlib.h>
#include <string.h>
#include "../base/xmalloc.h"

// Get PollDesc for fd (lock-free read, returns NULL if page not allocated)
static inline XrPollDesc *xr_fdmap_get(XrNetpoll *np, int fd) {
    if (fd < 0 || fd >= XR_NETPOLL_FD_MAX)
        return NULL;
    int page_idx = fd >> XR_FDMAP_PAGE_BITS;
    int entry_idx = fd & XR_FDMAP_PAGE_MASK;
    XrFdMapPage *page = atomic_load_explicit(&np->fd_pages[page_idx], memory_order_acquire);
    if (!page)
        return NULL;
    return atomic_load_explicit(&page->entries[entry_idx], memory_order_acquire);
}

// Ensure page exists for fd, allocate on demand (thread-safe via CAS)
static inline XrFdMapPage *xr_fdmap_ensure_page(XrNetpoll *np, int fd) {
    int page_idx = fd >> XR_FDMAP_PAGE_BITS;
    XrFdMapPage *page = atomic_load_explicit(&np->fd_pages[page_idx], memory_order_acquire);
    if (page)
        return page;

    // Allocate new page
    XrFdMapPage *new_page = (XrFdMapPage *) xr_calloc(1, sizeof(XrFdMapPage));
    if (!new_page)
        return NULL;

    // CAS to install (only one thread wins)
    XrFdMapPage *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&np->fd_pages[page_idx], &expected, new_page,
                                                memory_order_acq_rel, memory_order_acquire)) {
        return new_page;
    }
    // Another thread won, free ours and use theirs
    xr_free(new_page);
    return expected;
}

// CAS set PollDesc for fd (returns true if set, false if already occupied)
static inline bool xr_fdmap_cas(XrNetpoll *np, int fd, XrPollDesc **expected, XrPollDesc *desired) {
    if (fd < 0 || fd >= XR_NETPOLL_FD_MAX)
        return false;
    XrFdMapPage *page = xr_fdmap_ensure_page(np, fd);
    if (!page)
        return false;
    int entry_idx = fd & XR_FDMAP_PAGE_MASK;
    return atomic_compare_exchange_strong_explicit(&page->entries[entry_idx], expected, desired,
                                                   memory_order_acq_rel, memory_order_acquire);
}

// Set PollDesc for fd (unconditional store)
static inline void xr_fdmap_store(XrNetpoll *np, int fd, XrPollDesc *pd) {
    if (fd < 0 || fd >= XR_NETPOLL_FD_MAX)
        return;
    XrFdMapPage *page = xr_fdmap_ensure_page(np, fd);
    if (!page)
        return;
    int entry_idx = fd & XR_FDMAP_PAGE_MASK;
    atomic_store_explicit(&page->entries[entry_idx], pd, memory_order_release);
}

// Free all fd_map pages
static inline void xr_fdmap_destroy(XrNetpoll *np) {
    for (int i = 0; i < XR_FDMAP_PAGES; i++) {
        XrFdMapPage *page = atomic_load(&np->fd_pages[i]);
        if (page) {
            xr_free(page);
            atomic_store(&np->fd_pages[i], NULL);
        }
    }
}

// ========== Per-Worker Local Poll (kqueue/epoll fd per worker) ==========
//
// Each worker owns a separate kqueue/epoll fd for I/O event collection.
// Eliminates cross-worker contention on the shared poller and improves
// cache locality (IO events delivered to the worker that owns the fd).
// The global XrNetpoll still owns the fd_map and PollDesc cache.

typedef struct XrLocalPoll {
    int poll_fd;         // per-worker kqueue/epoll fd (-1 = uninitialized)
    int wakeup_pipe[2];  // per-worker wakeup pipe
    _Atomic bool break_pending;
} XrLocalPoll;

// Initialize per-worker poll state (creates kqueue/epoll fd + wakeup pipe)
XR_FUNC int xr_local_poll_init(XrLocalPoll *lp);
// Cleanup per-worker poll state
XR_FUNC void xr_local_poll_cleanup(XrLocalPoll *lp);
// Register fd with per-worker poller
XR_FUNC int xr_local_poll_add_fd(XrLocalPoll *lp, int fd, XrPollDesc *pd);
// Unregister fd from per-worker poller
XR_FUNC void xr_local_poll_del_fd(XrLocalPoll *lp, int fd);
// Poll events from per-worker poller (non-blocking if delta_ns == 0)
XR_FUNC int xr_local_poll_events(XrLocalPoll *lp, int64_t delta_ns, XrReadyList *list);
// Wake per-worker poller from sleep
XR_FUNC void xr_local_poll_wakeup(XrLocalPoll *lp);

// ========== Platform-independent API ==========

// Initialize network poller (call only once)
XR_FUNC int xr_netpoll_init(XrNetpoll *np);

// Cleanup network poller
XR_FUNC void xr_netpoll_cleanup(XrNetpoll *np);

// Open fd, register with poller
// Returns pollDesc, NULL on failure
XR_FUNC XrPollDesc *xr_netpoll_open(XrNetpoll *np, int fd);

// Close fd, remove from poller
XR_FUNC void xr_netpoll_close(XrNetpoll *np, XrPollDesc *pd);

// Wait for I/O ready
// Current coroutine will be suspended until I/O ready or timeout
// X: VM instance (for coroutine scheduling)
// Returns error code
XR_FUNC int xr_netpoll_wait(XrNetpoll *np, XrPollDesc *pd, int mode, struct XrayIsolate *X);

// Poll for ready events
// delta_ns: < 0 wait forever, = 0 return immediately, > 0 wait at most delta_ns nanoseconds
// Returns list of ready coroutines
XR_FUNC XrReadyList xr_netpoll_poll(XrNetpoll *np, int64_t delta_ns);

// Wake thread blocked in netpoll
XR_FUNC void xr_netpoll_break(XrNetpoll *np);

// Check if any coroutine is waiting for I/O
XR_FUNC bool xr_netpoll_any_waiters(XrNetpoll *np);

// Set read/write timeout (lock-free design)
// np: network poller
// pd: poll descriptor
// deadline: deadline (ns timestamp, 0 = no timeout, -1 = expire immediately)
// mode: XR_POLL_READ / XR_POLL_WRITE
// tw: Timer Wheel (for adding/canceling timers)
//
// Lock-free cancellation mechanism:
// 1. Increment rseq/wseq to invalidate old callbacks
// 2. New timer carries current sequence number
// 3. Callback checks sequence on trigger, skip if mismatch
XR_FUNC void xr_netpoll_set_deadline(XrNetpoll *np, XrPollDesc *pd, int64_t deadline, int mode,
                                     XrTimerWheel *tw);

// Timeout callback (internal use)
// Called by Timer Wheel, checks sequence then wakes coroutine
XR_FUNC void xr_netpoll_deadline_impl(XrPollDesc *pd, uintptr_t seq, bool read);

// ========== Internal API ==========

// Allocate pollDesc
XR_FUNC XrPollDesc *xr_poll_cache_alloc(XrPollCache *cache);

// Free pollDesc
XR_FUNC void xr_poll_cache_free(XrPollCache *cache, XrPollDesc *pd);

// Mark I/O ready, wake waiting coroutine
// Called by platform-specific netpoll backend
XR_FUNC void xr_netpoll_ready(XrReadyList *list, XrPollDesc *pd, int mode);

/*
 * Block the calling worker thread on a condition variable until I/O
 * is ready (or the pd is closed). The X parameter is currently unused
 * — pthread cond_wait suspends the OS thread, not the calling
 * coroutine. Any other coroutine waiting on the same worker stalls
 * until this one wakes.
 *
 * USE ONLY in paths that cannot be expressed as yieldable cfunc
 * state machines: bootstrap, CLI tooling, and short-lived helpers
 * deep inside synchronous C call chains (conn_pool TCP connect,
 * TLS handshake fallback). Hot paths owned by stdlib clients
 * (http / ws / cluster) must migrate to xr_yield_for_io to avoid
 * head-of-line blocking on the worker.
 *
 * Returns true if I/O ready, false on timeout or fd close.
 */
XR_FUNC bool xr_netpoll_block_sync(XrPollDesc *pd, int mode, struct XrayIsolate *X);

// Unblock, wake waiting coroutine
XR_FUNC struct XrCoroutine *xr_netpoll_unblock(XrPollDesc *pd, int mode, bool io_ready);

// ========== fd Bound to Worker ==========

// Bind fd to current Worker (called on first I/O)
// Returns bound Worker ID
XR_FUNC int xr_netpoll_bind_worker(XrPollDesc *pd);

// Get Timer Wheel bound to fd
// If not bound, returns current Worker's Timer Wheel
XR_FUNC struct XrTimerWheel *xr_netpoll_get_timer_wheel(XrPollDesc *pd);

// ========== Deferred Free (cross-worker PollDesc cleanup) ==========

// Queue PollDesc for deferred free on owner worker (MPSC Treiber stack push)
XR_FUNC void xr_netpoll_deferred_free(XrNetpoll *np, XrPollDesc *pd);

// Drain deferred free queue on current worker (called during poll cycle)
XR_FUNC void xr_netpoll_drain_deferred(XrNetpoll *np, struct XrProc *p);

#endif  // XNETPOLL_H
