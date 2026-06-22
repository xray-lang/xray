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
struct XrVMRuntime;
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

// ========== io_uring Completion Op (Linux only) ==========
//
// Defined ahead of XrPollDesc so a read/write op can be embedded per-fd (see
// the uring_rop/uring_wop slots below). On Linux with io_uring active, socket
// read/write submit one of these as a recv/send SQE; the CQE delivers the byte
// count (or -errno) to res and sets done, then routes a wakeup through the
// owning pd. The op lives inside the pd, so its lifetime is pinned by
// pd->uring_inflight until the parked waiter consumes the result (see
// xr_netpoll_uring_xfer_result), which keeps the kernel's target memory valid
// even if the fd is closed while the op is in flight.
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
struct XrPollDesc;

// Completion op kind. recv/send transfer over a socket buffer; accept returns a
// new fd; connect resolves to 0 / -errno; file read/write transfer at an offset
// (io_uring's unique advantage — epoll cannot async regular files). One submit +
// park + result path serves them all (xr_netpoll_uring_op_submit /
// xr_yield_for_uring_io).
typedef enum {
    XR_URING_OP_RECV = 0,
    XR_URING_OP_SEND = 1,
    XR_URING_OP_ACCEPT = 2,
    XR_URING_OP_CONNECT = 3,
    XR_URING_OP_FILE_READ = 4,
    XR_URING_OP_FILE_WRITE = 5
} XrUringOpKind;

// Completion submit request. Only the fields a given kind needs are read:
//   recv/send/file: buf, len (file also offset)
//   connect:        addr, addrlen
//   accept:         none (peer address is not captured)
// timeout_ms > 0 attaches a native io_uring linked timeout.
typedef struct XrUringReq {
    XrUringOpKind kind;
    void *buf;
    unsigned len;
    const void *addr;
    unsigned addrlen;
    uint64_t offset;
    int64_t timeout_ms;
} XrUringReq;

typedef struct XrUringOp {
    _Atomic int done;       // 0 = pending, 1 = completed (release-paired with res)
    long res;               // recv/send: byte count; accept: new fd; connect: 0; or -errno
    struct XrPollDesc *pd;  // owning pd: the CQE routes a wakeup via xr_netpoll_ready
    int mode;               // XR_POLL_READ / XR_POLL_WRITE — which waiter slot to wake
    _Atomic bool active;    // a waiter is parked on this op (submit..result window);
                            // lets close skip the readiness unblock for this direction
} XrUringOp;
#endif

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

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    // io_uring completion-mode per-direction ops (recv/send). Embedded so the
    // kernel's target XrUringOp stays valid while in flight. uring_refs is a
    // tiny reference count = 1 (the fdmap/owner ref dropped by xr_netpoll_close)
    // + 1 per in-flight completion op (dropped by xr_netpoll_uring_xfer_result).
    // Whoever drops it to zero frees the pd, so a close racing an in-flight op
    // can neither free the pd out from under the kernel/continuation nor leak it.
    XrUringOp uring_rop;
    XrUringOp uring_wop;
    _Atomic int uring_refs;
#endif

#ifdef XR_OS_WINDOWS
    // IOCP backend per-socket state. The actual layout is defined in
    // netpoll_iocp.c and guarded by a _Static_assert so growth is
    // caught at build time; keeping the slot opaque here means
    // xnetpoll.h does not have to drag <winsock2.h> / <windows.h> /
    // <winternl.h> through every consumer of this header.
    //
    // 96 bytes covers IO_STATUS_BLOCK (16) + AFD_POLL_INFO (32) +
    // SOCKET base_socket (8) + user/pending event masks (8) + state
    // flags + an intrusive update-queue link (8) with comfortable
    // slack for future per-platform extensions.
    _Alignas(void *) unsigned char iocp_state[96];
    // Set by the IOCP backend's del_fd when an outstanding AFD poll
    // request still has a completion in flight whose lpOverlapped
    // points at the embedded iosb. xr_netpoll_close honours the flag
    // and skips the immediate cache free; iocp_handle_completion
    // takes over the free when the completion drains. Prevents the
    // close-before-completion use-after-free that would otherwise
    // recycle pd memory while the kernel still references it.
    _Atomic bool iocp_holds_ref;
#endif
} XrPollDesc;

// Hot path: every watched fd allocates one of these from the lock-free
// cache. Growing the struct past 1KB silently doubles the per-fd cost
// at scale; bumping the cap is fine but should be a deliberate review.
_Static_assert(sizeof(XrPollDesc) <= 1024, "XrPollDesc must stay <= 1024 bytes (per-fd hot path)");

// Size of the per-pd IOCP state buffer reserved above. The IOCP
// backend asserts sizeof(XrIocpPdState) <= XR_IOCP_PD_STATE_SIZE at
// compile time; bumping this value here requires the same review as
// growing XrPollDesc itself.
#define XR_IOCP_PD_STATE_SIZE 96

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
    const char *name;                                       // "kqueue", "epoll", "io_uring", "iocp"
    int (*init)(XrNetpoll *np);                             // Create backend handle
    void (*cleanup)(XrNetpoll *np);                         // Destroy backend handle
    int (*add_fd)(XrNetpoll *np, int fd, XrPollDesc *pd);   // Register fd
    void (*del_fd)(XrNetpoll *np, int fd, XrPollDesc *pd);  // Unregister fd
    int (*poll_events)(XrNetpoll *np, int64_t delta_ns, XrReadyList *list);  // Wait & collect
    void (*wakeup)(XrNetpoll *np);                                           // Interrupt wait
} XrNetpollOps;

// ========== Global Netpoll State ==========

// Two-level segmented fd_map: fd_map[fd >> 8][fd & 0xFF]
// First level: XR_FDMAP_PAGES page pointers (fixed, 8 bytes each)
// Second level: 256-entry pages, allocated on demand (~2KB each)
// Capacity = XR_FDMAP_PAGES * 256 fds. Default 1024 pages -> 262144 fds, which
// covers the common server ulimit -n (often 65536..262144); the first level is
// ~8KB of pointers (one global netpoll) and second-level pages stay on demand.
// Overridable at compile time (-DXR_FDMAP_PAGES=N) for hosts with a higher fd
// ceiling. An fd at/above the cap is rejected by the bounds checks below rather
// than corrupting memory.
#define XR_FDMAP_PAGE_BITS 8
#define XR_FDMAP_PAGE_SIZE (1 << XR_FDMAP_PAGE_BITS)  // 256
#define XR_FDMAP_PAGE_MASK (XR_FDMAP_PAGE_SIZE - 1)   // 0xFF
#ifndef XR_FDMAP_PAGES
#define XR_FDMAP_PAGES 1024  // 1024 pages
#endif
#define XR_NETPOLL_FD_MAX (XR_FDMAP_PAGES * XR_FDMAP_PAGE_SIZE)  // 262144 (default)

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

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    // True when per-worker io_uring completion rings are usable on this host
    // (kernel supports io_uring and XRAY_IO_BACKEND did not force epoll). The
    // global backend (ops) is always epoll on Linux now — io_uring lives only
    // in the per-worker rings (XrLocalPoll::uring). Set once during init before
    // any worker starts, read-only afterwards.
    bool uring_avail;
#endif
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
    int poll_fd;         // per-worker kqueue/epoll fd (readiness); -1 = uninitialized
    int wakeup_pipe[2];  // per-worker wakeup pipe
    _Atomic bool break_pending;
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    // Per-worker io_uring ring for completion-mode socket/file I/O (recv/send/
    // accept/connect/file read+write). NULL when io_uring is unavailable (the
    // worker then uses only the local epoll above). The ring is touched solely
    // by its owner worker thread: submission is single-producer and reaping is
    // single-consumer, so no lock is needed (this is what replaces the old
    // single global ring + mutex that serialised every worker). Readiness still
    // flows through the local epoll above; the ring carries completion ops only.
    void *uring;  // XrUringRing*
#endif
} XrLocalPoll;

// Initialize per-worker poll state (creates the kqueue/epoll fd + wakeup pipe,
// and a per-worker io_uring completion ring when use_uring is true on Linux).
XR_FUNC int xr_local_poll_init(XrLocalPoll *lp, bool use_uring);
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
XR_FUNC int xr_netpoll_wait(XrNetpoll *np, XrPollDesc *pd, int mode, struct XrVMRuntime *X);

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
XR_FUNC bool xr_netpoll_block_sync(XrPollDesc *pd, int mode, struct XrVMRuntime *X);
XR_FUNC void xr_netpoll_arm_mode(XrPollDesc *pd, int mode);

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

// ========== io_uring Completion Mode (Linux only) ==========
//
// On Linux with io_uring, network/file I/O uses the completion model: submit a
// recv/send SQE and let the CQE carry the byte count back, saving the
// "readiness -> separate read()/write()" second syscall. This unifies with the
// IOCP completion model on Windows; epoll / kqueue keep the readiness model.
//
// Each worker owns its own io_uring ring (XrLocalPoll::uring): completion ops
// submit to the current worker's ring (single-producer, no lock) and are reaped
// by that worker's poll cycle (single-consumer, no lock). The byte count (or
// -errno) is delivered to op->res with op->done set during a later poll cycle.
// The caller owns the XrUringOp and the buffer until done. Readiness still flows
// through the per-worker local epoll; the ring carries completion ops only.
// (XrUringOp is defined above so it can be embedded in XrPollDesc.)
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)

// ---- Per-worker io_uring completion ring (opaque XrUringRing handle) ----
//
// Created per worker at startup when XrNetpoll::uring_avail; touched only by the
// owning worker thread. Also used standalone by the io_uring completion test.
XR_FUNC void *xr_uring_ring_create(void);
XR_FUNC void xr_uring_ring_destroy(void *ring);
// Reap all currently-available CQEs on `ring` (non-blocking), routing completion
// wakeups into `list`. Returns the number of CQEs processed.
XR_FUNC int xr_uring_ring_reap(void *ring, XrReadyList *list);
// Number of submitted-but-unreaped completion ops on `ring`.
XR_FUNC int xr_uring_ring_inflight(void *ring);
// Block up to timeout_us for a completion on `ring` (does not consume the CQE).
// An idle worker with outstanding ops uses this instead of the futex park so an
// arriving completion resumes it immediately.
XR_FUNC void xr_uring_ring_wait(void *ring, int64_t timeout_us);
// Raw completion primitives (no pd wake routing, no timeout): submit a recv/send
// SQE on `ring`; the caller polls op->done and reaps via xr_uring_ring_reap.
// Returns 0 if queued, -1 if the submission queue is exhausted.
XR_FUNC int xr_uring_ring_submit_recv(void *ring, XrUringOp *op, int fd, void *buf, unsigned len);
XR_FUNC int xr_uring_ring_submit_send(void *ring, XrUringOp *op, int fd, const void *buf,
                                      unsigned len);

// True when the current worker has an io_uring completion ring (i.e. completion
// mode is usable for an op submitted from this thread). False outside a worker
// or when io_uring is unavailable (callers then use the readiness path).
XR_FUNC bool xr_netpoll_uring_active(XrNetpoll *np);

// ---- Yieldable completion socket I/O (the coroutine net path) ----
//
// Sentinel returned by xr_netpoll_uring_op_submit when completion mode is not
// usable for this call (backend is not io_uring, SQ exhausted, or a waiter is
// already parked on this direction). The caller must fall back to the readiness
// path (try syscall + xr_yield_for_io). Distinct from any byte count / -errno.
#define XR_URING_XFER_FALLBACK 1

// Submit a completion op (`req`) for the coroutine currently parking on `pd` in
// `mode`. The op is stored in pd and its CQE wakes the parked waiter via
// xr_netpoll_ready(pd, mode). req->timeout_ms > 0 attaches a native io_uring
// linked timeout so the op self-cancels on deadline (CQE res == -ECANCELED);
// <= 0 waits until the op naturally completes. On success returns 0 and pins pd;
// on failure returns XR_URING_XFER_FALLBACK and pins nothing. The caller must
// have already CAS-parked the coro on pd->rg/wg (see xr_yield_for_uring_io).
XR_FUNC int xr_netpoll_uring_op_submit(XrPollDesc *pd, int mode, const XrUringReq *req);

// Completion outcome categories returned by xr_netpoll_uring_xfer_result via
// *out_kind, so the caller maps to its own error model without inspecting errno.
typedef enum {
    XR_URING_XFER_DATA = 0,     // res >= 0 bytes transferred (0 == EOF for recv)
    XR_URING_XFER_TIMEOUT = 1,  // linked timeout fired
    XR_URING_XFER_CLOSED = 2,   // fd closed while in flight
    XR_URING_XFER_ERROR = 3     // other error; res holds -errno
} XrUringXferKind;

// Consume the completed op for `mode` after the waiter resumed. Returns the byte
// count (>= 0) when *out_kind == XR_URING_XFER_DATA, otherwise a negative errno.
// Releases the pd pin taken at submit; if a close raced the op, performs the
// deferred pd free here (the waiter's continuation is the last reader).
XR_FUNC long xr_netpoll_uring_xfer_result(XrPollDesc *pd, int mode, XrUringXferKind *out_kind);

// Drain the current worker's cross-worker io_uring cancel queue. When a close
// runs off the fd's owner worker it cannot submit a cancel SQE to the owner's
// ring (single-producer), so it queues the pd here; the owner drains this in its
// poll cycle and cancels the in-flight ops on its own ring. Called once per
// scheduling cycle by the owner worker.
XR_FUNC void xr_netpoll_drain_uring_cancel(struct XrProc *p);
#endif

#endif  // XNETPOLL_H
