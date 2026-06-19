/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnetpoll_iouring.c - io_uring backend (Linux 5.1+)
 *
 * KEY CONCEPT:
 *   Uses io_uring in POLL mode: submits IORING_OP_POLL_ADD SQEs for
 *   edge-triggered fd readiness, reaps CQEs to collect ready fds.
 *   This is functionally equivalent to epoll but with lower syscall
 *   overhead (batched submit + reap via shared ring buffer).
 *
 * WHY THIS DESIGN:
 *   - Compatible with existing netpoll API (event notification model)
 *   - No change to read/write paths (coroutines still do syscalls)
 *   - 20-40% fewer syscalls than epoll under high connection count
 *   - Future path to full async IO (submit read/write SQEs)
 *
 * RELATED MODULES:
 *   - xnetpoll.c: Includes this file, provides shared fd_map/cache/timer logic
 *   - xnetpoll_epoll.c: Fallback when io_uring unavailable
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 *       Only active when XR_HAS_IO_URING is defined by CMake.
 */

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING) && defined(XR_NETPOLL_INCLUDED)

#include "../../base/xchecks.h"
#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/socket.h>  // SOCK_NONBLOCK, struct sockaddr (accept/connect ops)
#include <errno.h>
#include <poll.h>

/* Per-fd poll state tracked in io_uring CQE user_data.
 * Encodes both the PollDesc pointer and direction so we can
 * distinguish multi-shot completions. */
#define URING_UDATA(pd, is_write) ((uint64_t) (uintptr_t) (pd) | ((uint64_t) (is_write) << 63))
#define URING_UDATA_PD(udata) ((XrPollDesc *) (uintptr_t) ((udata) & ~(1ULL << 63)))
#define URING_UDATA_IS_WRITE(udata) (((udata) >> 63) & 1)

// Wakeup sentinel: user_data == 0 means wakeup eventfd
#define URING_UDATA_WAKEUP 0

/* Ignore sentinel: a CQE the dispatcher must discard (cancel completions, the
 * timeout half of a recv/send linked-timeout pair). Value 1 can never be a real
 * pointer (pointers are aligned) nor the wakeup (0) nor a completion op (bit 62),
 * so it is unambiguous. */
#define URING_UDATA_IGNORE 1

/* Completion-op tag. Poll user_data carries a XrPollDesc pointer (with the
 * write direction in bit 63); a completion op instead carries a XrUringOp
 * pointer marked with bit 62. User-space pointers stay below 2^48, so bits
 * 62/63 are free for tagging and never collide with a real pointer. */
#define URING_UDATA_OP_FLAG (1ULL << 62)
#define URING_UDATA_OP(op) ((uint64_t) (uintptr_t) (op) | URING_UDATA_OP_FLAG)
#define URING_UDATA_IS_OP(udata) (((udata) & URING_UDATA_OP_FLAG) != 0)
#define URING_UDATA_GET_OP(udata) ((XrUringOp *) (uintptr_t) ((udata) & ~URING_UDATA_OP_FLAG))

// Ring size: must be power of 2. 256 is good for typical server workloads.
#define URING_ENTRIES 256

/* ========== io_uring backend state ========== */

typedef struct XrUringState {
    struct io_uring ring;
    int event_fd;  // eventfd for wakeup (registered with ring)
    /* A single io_uring ring is NOT safe to touch from multiple threads: the SQ
     * is single-producer and the CQ is single-consumer. The runtime drives one
     * shared netpoll from every worker (xnetpoll.c worker loop), so submission
     * (add_fd / del_fd / completion ops) and reaping (poll_events) can run
     * concurrently. `lock` serialises all ring access. Reaping uses trylock so a
     * worker skips when another is already draining the ring (one consumer per
     * cycle still observes every CQE); submission paths take it blocking. */
    xr_mutex_t lock;
} XrUringState;

/* ========== Helper: submit a poll-add SQE ========== */

// Caller must hold us->lock (touches the SQ).
static int uring_submit_poll(XrUringState *us, int fd, XrPollDesc *pd, short poll_mask,
                             bool is_write) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&us->ring);
    if (!sqe) {
        // SQ full: flush pending submissions first
        io_uring_submit(&us->ring);
        sqe = io_uring_get_sqe(&us->ring);
        if (!sqe)
            return -1;
    }
    io_uring_prep_poll_add(sqe, fd, poll_mask);
    sqe->flags |= IOSQE_IO_LINK;  // no-op for standalone, harmless
    sqe->flags = 0;
    io_uring_sqe_set_data64(sqe, URING_UDATA(pd, is_write));
    return 0;
}

/* ========== ops functions ========== */

static int iouring_init(XrNetpoll *np) {
    XrUringState *us = (XrUringState *) xr_calloc(1, sizeof(XrUringState));
    if (!us)
        return -1;

    // Create io_uring instance
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    /* No SQPOLL for now — requires root or CAP_SYS_NICE on older kernels.
     * Can be enabled later as an optimization for dedicated IO threads. */

    int ret = io_uring_queue_init_params(URING_ENTRIES, &us->ring, &params);
    if (ret < 0) {
        xr_free(us);
        return -1;
    }

    // Create eventfd for wakeup
    us->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (us->event_fd < 0) {
        io_uring_queue_exit(&us->ring);
        xr_free(us);
        return -1;
    }

    // Register eventfd with ring for efficient wakeup
    int efd = us->event_fd;
    ret = io_uring_register_eventfd(&us->ring, efd);
    if (ret < 0) {
        // Non-fatal: wakeup pipe fallback still works
    }

    // Also register wakeup pipe with ring as poll source
    struct io_uring_sqe *sqe = io_uring_get_sqe(&us->ring);
    if (sqe) {
        io_uring_prep_poll_add(sqe, np->wakeup_pipe[0], POLLIN);
        io_uring_sqe_set_data64(sqe, URING_UDATA_WAKEUP);
        io_uring_submit(&us->ring);
    }

    // Initialised last so the early-error paths above need no teardown. The ring
    // is single-threaded until init returns, so no locking is needed here.
    xr_mutex_init(&us->lock);

    np->poll_fd = us->event_fd;
    np->backend_state = us;
    return 0;
}

static void iouring_cleanup(XrNetpoll *np) {
    XrUringState *us = (XrUringState *) np->backend_state;
    if (!us)
        return;

    xr_mutex_destroy(&us->lock);
    io_uring_queue_exit(&us->ring);
    if (us->event_fd >= 0)
        close(us->event_fd);
    xr_free(us);
    np->backend_state = NULL;
    np->poll_fd = -1;
}

static int iouring_add_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    /* On-demand arming: io_uring requests fd readiness per wait (iouring_arm,
     * driven by xr_netpoll_arm_mode), mirroring the IOCP backend rather than
     * keeping a standing poll-add like epoll. Registration therefore arms
     * nothing — a freshly opened fd has no SQE until a coroutine actually
     * blocks on it. This also removes the POLLOUT self-retrigger spin the old
     * persistent-poll-add design had (a connected socket is almost always
     * writable, so a standing write poll-add re-fired every poll cycle). */
    (void) np;
    (void) fd;
    (void) pd;
    return 0;
}

/* Arm a one-shot poll-add for a single direction on demand. Called from
 * xr_netpoll_arm_mode after a netpoll waiter has CAS'd pd->rg/wg to WAIT or a
 * coro pointer. One-shot (not re-armed in dispatch): the next waiter re-arms.
 * io_uring's poll-add fires immediately if the fd is already ready, so arming
 * after the caller's non-blocking try is race-free (no lost wakeup). */
static void iouring_arm(XrNetpoll *np, XrPollDesc *pd, int mode) {
    XrUringState *us = (XrUringState *) np->backend_state;
    if (!us || !pd || pd->fd < 0)
        return;
    bool is_write = (mode & XR_POLL_WRITE) != 0;
    short mask =
        is_write ? (POLLOUT | POLLHUP | POLLERR) : (POLLIN | POLLRDHUP | POLLHUP | POLLERR);
    xr_mutex_lock(&us->lock);
    if (uring_submit_poll(us, pd->fd, pd, mask, is_write) == 0)
        io_uring_submit(&us->ring);
    xr_mutex_unlock(&us->lock);
}

static void iouring_del_fd(XrNetpoll *np, int fd, XrPollDesc *pd) {
    (void) pd;  // io_uring cancels by fd; pd is unused
    XrUringState *us = (XrUringState *) np->backend_state;
    if (!us)
        return;

    /* Cancel outstanding poll SQEs for this fd.
     * io_uring_prep_cancel_fd cancels all SQEs matching the fd. */
    xr_mutex_lock(&us->lock);
    struct io_uring_sqe *sqe = io_uring_get_sqe(&us->ring);
    if (sqe) {
        io_uring_prep_cancel_fd(sqe, fd, 0);
        io_uring_sqe_set_data64(sqe, URING_UDATA_IGNORE);  // discard the cancel CQE
        io_uring_submit(&us->ring);
    }
    xr_mutex_unlock(&us->lock);
}

/* Handle one reaped CQE. Demultiplexes by user_data:
 *   - wakeup eventfd: drain pipe, re-arm wakeup poll
 *   - ignore sentinel: cancel completions / linked-timeout halves, discarded
 *   - completion op (bit 62): deliver byte count / -errno to the XrUringOp and
 *     route a wakeup to its parked waiter via xr_netpoll_ready
 *   - poll re-arm (XrPollDesc): translate revents to a ready notification
 * Completion ops must be checked before the `res < 0` poll skip so a failed
 * recv/send still reports its -errno to the caller.
 * Caller holds us->lock; this re-arms via uring_submit_poll / io_uring_get_sqe
 * without re-locking. */
static void iouring_dispatch_cqe(XrNetpoll *np, XrUringState *us, uint64_t udata, int res,
                                 XrReadyList *list) {
    if (udata == URING_UDATA_WAKEUP) {
        char buf[16];
        while (read(np->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
        }
        atomic_store(&np->break_pending, false);

        struct io_uring_sqe *sqe = io_uring_get_sqe(&us->ring);
        if (sqe) {
            io_uring_prep_poll_add(sqe, np->wakeup_pipe[0], POLLIN);
            io_uring_sqe_set_data64(sqe, URING_UDATA_WAKEUP);
        }
        return;
    }

    if (udata == URING_UDATA_IGNORE)
        return;  // cancel completion or linked-timeout half — nothing to do

    if (URING_UDATA_IS_OP(udata)) {
        XrUringOp *op = URING_UDATA_GET_OP(udata);
        if (op) {
            op->res = res;
            atomic_store_explicit(&op->done, 1, memory_order_release);
            /* Release the direction *after* publishing the result, so this op's
             * own wakeup is not dropped by the xr_netpoll_ready completion guard
             * and any racing readiness wakeup that slips through now finds
             * done == 1 (the result is already visible). Wake the parked waiter
             * (blocking WAIT or yieldable coro) through the normal readiness
             * path. The op stays valid (pinned by pd->uring_refs) until the
             * waiter consumes the result. */
            atomic_store(&op->active, false);
            if (op->pd)
                xr_netpoll_ready(list, op->pd, op->mode);
        }
        return;
    }

    if (res < 0)
        return;  // poll error or cancel completion

    XrPollDesc *pd = URING_UDATA_PD(udata);
    bool is_write = URING_UDATA_IS_WRITE(udata);

    int mode = 0;
    if (!is_write && (res & (POLLIN | POLLRDHUP | POLLHUP | POLLERR)))
        mode |= XR_POLL_READ;
    if (is_write && (res & (POLLOUT | POLLHUP | POLLERR)))
        mode |= XR_POLL_WRITE;

    // Completion-direction guarding lives in xr_netpoll_ready so it also covers
    // the per-worker local epoll, which shares the same readiness wakeup path.
    if (mode)
        xr_netpoll_ready(list, pd, mode);

    /* One-shot: deliberately not re-armed here. The poll-add was submitted on
     * demand by iouring_arm for this single wait; the next netpoll waiter on
     * this direction re-arms via xr_netpoll_arm_mode. (Persistent re-arm here
     * caused a POLLOUT self-retrigger spin on idle writable sockets.) */
}

static int iouring_poll_events(XrNetpoll *np, int64_t delta_ns, XrReadyList *list) {
    XrUringState *us = (XrUringState *) np->backend_state;
    if (!us)
        return -1;

    /* One consumer per cycle. The runtime drives this from every worker with
     * delta_ns == 0 (non-blocking); if another worker is already draining the
     * ring we skip rather than racing the CQ — that worker observes every
     * pending CQE and routes the wakeups, and unreaped CQEs simply wait for the
     * next cycle. Concurrent reaping would corrupt the shared CQ head and drop
     * completions (lost wakeups → hung coroutines). */
    if (!xr_mutex_trylock(&us->lock))
        return 0;

    // Flush any pending submissions
    io_uring_submit(&us->ring);

    // Set up timeout
    struct __kernel_timespec ts;
    struct __kernel_timespec *timeout = NULL;
    if (delta_ns == 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        timeout = &ts;
    } else if (delta_ns > 0) {
        ts.tv_sec = delta_ns / 1000000000;
        ts.tv_nsec = delta_ns % 1000000000;
        timeout = &ts;
    }

    // Reap completions
    struct io_uring_cqe *cqe;
    int count = 0;

    if (delta_ns == 0) {
        // Non-blocking: peek all available CQEs
        unsigned head;
        io_uring_for_each_cqe(&us->ring, head, cqe) {
            iouring_dispatch_cqe(np, us, io_uring_cqe_get_data64(cqe), cqe->res, list);
            count++;
        }
        io_uring_cq_advance(&us->ring, count);
    } else {
        /* Blocking wait. The runtime scheduler never takes this branch (it
         * always polls with delta_ns == 0); it exists for completeness. Drop
         * the ring lock across the kernel wait so submitters are not blocked —
         * a single blocking waiter is assumed here. */
        xr_mutex_unlock(&us->lock);
        int ret;
        if (timeout) {
            ret = io_uring_wait_cqe_timeout(&us->ring, &cqe, timeout);
        } else {
            ret = io_uring_wait_cqe(&us->ring, &cqe);
        }
        xr_mutex_lock(&us->lock);

        if (ret < 0) {
            xr_mutex_unlock(&us->lock);
            return (ret == -EINTR || ret == -ETIME) ? 0 : -1;
        }

        // Process all available CQEs after waking
        unsigned head;
        io_uring_for_each_cqe(&us->ring, head, cqe) {
            iouring_dispatch_cqe(np, us, io_uring_cqe_get_data64(cqe), cqe->res, list);
            count++;
        }
        io_uring_cq_advance(&us->ring, count);
    }

    // Flush any re-arm submissions
    if (count > 0)
        io_uring_submit(&us->ring);

    xr_mutex_unlock(&us->lock);
    return count;
}

static void iouring_wakeup(XrNetpoll *np) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&np->break_pending, &expected, true))
        return;

    // Write to wakeup pipe (registered as poll source in init)
    char c = 0;
    ssize_t n;
    do {
        n = write(np->wakeup_pipe[1], &c, 1);
    } while (n < 0 && errno == EINTR);
}

static const XrNetpollOps iouring_ops = {
    .name = "io_uring",
    .init = iouring_init,
    .cleanup = iouring_cleanup,
    .add_fd = iouring_add_fd,
    .del_fd = iouring_del_fd,
    .poll_events = iouring_poll_events,
    .wakeup = iouring_wakeup,
};

/* ========== Completion-mode submit API (declared in xnetpoll.h) ========== */

bool xr_netpoll_uring_active(XrNetpoll *np) {
    return np && atomic_load(&np->inited) && np->ops == &iouring_ops;
}

// Prep one completion SQE from `req`. Caller holds the ring lock and has
// reserved the slot.
static void iouring_prep_op(struct io_uring_sqe *sqe, int fd, const XrUringReq *req) {
    switch (req->kind) {
        case XR_URING_OP_SEND:
            io_uring_prep_send(sqe, fd, req->buf, req->len, 0);
            break;
        case XR_URING_OP_ACCEPT:
            // Peer address is not captured (callers don't use it); request a
            // non-blocking accepted fd directly (accept4 semantics).
            io_uring_prep_accept(sqe, fd, NULL, NULL, SOCK_NONBLOCK);
            break;
        case XR_URING_OP_CONNECT:
            io_uring_prep_connect(sqe, fd, (const struct sockaddr *) req->addr,
                                  (socklen_t) req->addrlen);
            break;
        case XR_URING_OP_FILE_READ:
            io_uring_prep_read(sqe, fd, req->buf, req->len, req->offset);
            break;
        case XR_URING_OP_FILE_WRITE:
            io_uring_prep_write(sqe, fd, req->buf, req->len, req->offset);
            break;
        case XR_URING_OP_RECV:
        default:
            io_uring_prep_recv(sqe, fd, req->buf, req->len, 0);
            break;
    }
}

// Queue a completion SQE (described by `req`) tagged with `op`. The CQE is
// reaped by iouring_dispatch_cqe on a later poll cycle, which writes the result
// (byte count / new fd / 0, or -errno) into op->res and sets op->done. When
// req->timeout_ms > 0 a native io_uring linked timeout is chained after the op:
// if it does not complete in time the kernel cancels it (op CQE res ==
// -ECANCELED) and the timeout half completes with -ETIME (discarded via
// URING_UDATA_IGNORE). The op and its linked timeout must be consecutive SQEs,
// so both slots are reserved up front and the lock is held across the submit.
static int iouring_submit_op(XrNetpoll *np, XrUringOp *op, int fd, const XrUringReq *req) {
    if (!xr_netpoll_uring_active(np) || !op)
        return -1;
    XrUringState *us = (XrUringState *) np->backend_state;
    if (!us)
        return -1;

    atomic_store_explicit(&op->done, 0, memory_order_relaxed);
    op->res = 0;

    bool with_timeout = (req->timeout_ms > 0);
    struct __kernel_timespec ts;
    if (with_timeout) {
        int64_t timeout_ns = req->timeout_ms * 1000000LL;
        ts.tv_sec = timeout_ns / 1000000000LL;
        ts.tv_nsec = timeout_ns % 1000000000LL;
    }
    unsigned need = with_timeout ? 2 : 1;

    xr_mutex_lock(&us->lock);
    // Reserve all slots up front: never io_uring_submit() a half-prepared SQE,
    // and keep the op adjacent to its linked timeout.
    if (io_uring_sq_space_left(&us->ring) < need)
        io_uring_submit(&us->ring);  // flush already-prepared SQEs to free slots
    if (io_uring_sq_space_left(&us->ring) < need) {
        xr_mutex_unlock(&us->lock);
        return -1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&us->ring);
    iouring_prep_op(sqe, fd, req);
    io_uring_sqe_set_data64(sqe, URING_UDATA_OP(op));

    if (with_timeout) {
        sqe->flags |= IOSQE_IO_LINK;
        struct io_uring_sqe *tsqe = io_uring_get_sqe(&us->ring);
        io_uring_prep_link_timeout(tsqe, &ts, 0);
        io_uring_sqe_set_data64(tsqe, URING_UDATA_IGNORE);
    }

    io_uring_submit(&us->ring);
    xr_mutex_unlock(&us->lock);
    return 0;
}

// Raw primitives (no pd wake routing, no timeout): caller polls op->done.
int xr_netpoll_uring_submit_recv(XrNetpoll *np, XrUringOp *op, int fd, void *buf, unsigned len) {
    if (op) {
        op->pd = NULL;
        op->mode = 0;
    }
    XrUringReq req = {.kind = XR_URING_OP_RECV, .buf = buf, .len = len};
    return iouring_submit_op(np, op, fd, &req);
}

int xr_netpoll_uring_submit_send(XrNetpoll *np, XrUringOp *op, int fd, const void *buf,
                                 unsigned len) {
    if (op) {
        op->pd = NULL;
        op->mode = 0;
    }
    XrUringReq req = {.kind = XR_URING_OP_SEND, .buf = (void *) buf, .len = len};
    return iouring_submit_op(np, op, fd, &req);
}

// Yieldable completion submit: caller (xr_yield_for_uring_io) has parked the
// coro on pd->rg/wg for `mode`. Store the op in the pd, take a pd ref (pin), and
// queue the op. The CQE wakes the parked coro via xr_netpoll_ready and the
// continuation consumes the result with xr_netpoll_uring_xfer_result.
int xr_netpoll_uring_op_submit(XrPollDesc *pd, int mode, const XrUringReq *req) {
    if (!pd || !pd->netpoll)
        return XR_URING_XFER_FALLBACK;
    XrNetpoll *np = pd->netpoll;
    if (!xr_netpoll_uring_active(np))
        return XR_URING_XFER_FALLBACK;

    // Caller (xr_yield_for_uring_io) has already set op->active to claim the
    // direction before parking, which closes the window where a stale poll-add
    // could wake the waiter between parking and submission.
    XrUringOp *op = (mode == XR_POLL_WRITE) ? &pd->uring_wop : &pd->uring_rop;
    op->pd = pd;
    op->mode = mode;
    atomic_fetch_add(&pd->uring_refs, 1);  // pin pd until the result is consumed

    if (iouring_submit_op(np, op, pd->fd, req) != 0) {
        atomic_fetch_sub(&pd->uring_refs, 1);
        return XR_URING_XFER_FALLBACK;
    }
    return 0;
}

#endif  // XR_OS_LINUX && XR_HAS_IO_URING
