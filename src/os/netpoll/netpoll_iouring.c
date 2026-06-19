/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * netpoll_iouring.c - per-worker io_uring completion rings (Linux 5.6+)
 *
 * KEY CONCEPT:
 *   Socket/file I/O (recv/send/accept/connect/file read+write) runs in the
 *   io_uring completion model: a coroutine submits an operation SQE and parks
 *   until the matching CQE carries the byte count (or -errno) back, saving the
 *   "readiness -> separate read()/write()" second syscall that epoll needs.
 *
 * WHY PER-WORKER (no global ring, no lock):
 *   An io_uring ring is single-producer (SQ) and single-consumer (CQ). The old
 *   design drove one shared ring from every worker behind a mutex, serialising
 *   all 18 workers' submissions/reaps — io_uring ended up slower than epoll. Now
 *   each worker owns its own ring (XrLocalPoll::uring). A completion op submits
 *   to the CURRENT worker's ring (the fd is bound to it, so submission is always
 *   on the owning thread => single producer, no lock) and is reaped by that
 *   worker's poll cycle (single consumer, no lock). Readiness still flows
 *   through the per-worker local epoll; this ring carries completion ops only.
 *
 * CROSS-WORKER CLOSE:
 *   A close racing an in-flight op may run off the fd's owner worker, which
 *   cannot submit a cancel SQE to the owner's ring (single producer). Such a
 *   close pins the pd and queues it on the owner's uring_cancel stack; the owner
 *   cancels on its own ring during xr_netpoll_drain_uring_cancel. Cancellation
 *   is by user_data (the XrUringOp pointer), never by fd, so it is immune to the
 *   fd-reuse that follows close(2).
 *
 * Note: This file is #included by xnetpoll.c, not compiled separately.
 *       Only active when XR_HAS_IO_URING is defined by CMake.
 */

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING) && defined(XR_NETPOLL_INCLUDED)

#include "../../base/xchecks.h"
#include <liburing.h>
#include <sys/socket.h>  // SOCK_NONBLOCK, struct sockaddr (accept/connect ops)
#include <errno.h>

/* CQE user_data tagging. A completion op carries its XrUringOp pointer marked
 * with bit 62; user-space pointers stay below 2^48, so the tag never collides
 * with a real pointer. The IGNORE sentinel marks CQEs the reaper must discard
 * (linked-timeout halves and cancel completions). */
#define URING_UDATA_IGNORE 1
#define URING_UDATA_OP_FLAG (1ULL << 62)
#define URING_UDATA_OP(op) ((uint64_t) (uintptr_t) (op) | URING_UDATA_OP_FLAG)
#define URING_UDATA_IS_OP(udata) (((udata) & URING_UDATA_OP_FLAG) != 0)
#define URING_UDATA_GET_OP(udata) ((XrUringOp *) (uintptr_t) ((udata) & ~URING_UDATA_OP_FLAG))

// Ring size: must be power of 2. 256 covers typical server workloads.
#define URING_ENTRIES 256

/* Per-worker io_uring completion ring. No lock: the owning worker is the sole
 * producer and consumer.
 *
 * inflight counts submitted-but-unreaped completion ops. When it is > 0 the
 * owning worker, instead of parking on the scheduler futex (which an arriving
 * CQE cannot wake), blocks on this ring via xr_uring_ring_wait so a completion
 * resumes it immediately. Without this a parked worker would only reap its CQEs
 * on the futex timeout, making completion I/O an order of magnitude slower than
 * the old shared ring (which any busy worker drained). */
typedef struct XrUringRing {
    struct io_uring ring;
    _Atomic int inflight;
} XrUringRing;

/* ========== Ring lifecycle ========== */

void *xr_uring_ring_create(void) {
    XrUringRing *r = (XrUringRing *) xr_calloc(1, sizeof(XrUringRing));
    if (!r)
        return NULL;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    /* No SQPOLL: it needs root / CAP_SYS_NICE on older kernels. */
    if (io_uring_queue_init_params(URING_ENTRIES, &r->ring, &params) < 0) {
        xr_free(r);
        return NULL;
    }
    return r;
}

void xr_uring_ring_destroy(void *ring) {
    XrUringRing *r = (XrUringRing *) ring;
    if (!r)
        return;
    io_uring_queue_exit(&r->ring);
    xr_free(r);
}

/* ========== Completion-op submission ========== */

// Prep one completion SQE from `req`. Caller has reserved the slot.
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

// Queue a completion SQE (described by `req`) tagged with `op` on `r`. The CQE
// is reaped by xr_uring_ring_reap on a later poll cycle, which writes the result
// (byte count / new fd / 0, or -errno) into op->res and sets op->done. When
// req->timeout_ms > 0 a native io_uring linked timeout is chained after the op:
// if it does not complete in time the kernel cancels it (op CQE res ==
// -ECANCELED) and the timeout half completes (discarded via URING_UDATA_IGNORE).
// The op and its linked timeout must be consecutive SQEs, so both slots are
// reserved up front. Single producer (owner worker) — no lock.
static int uring_ring_submit_op(XrUringRing *r, XrUringOp *op, int fd, const XrUringReq *req) {
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

    // Reserve all slots up front: never submit a half-prepared SQE, and keep the
    // op adjacent to its linked timeout.
    if (io_uring_sq_space_left(&r->ring) < need)
        io_uring_submit(&r->ring);  // flush already-prepared SQEs to free slots
    if (io_uring_sq_space_left(&r->ring) < need)
        return -1;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
    iouring_prep_op(sqe, fd, req);
    io_uring_sqe_set_data64(sqe, URING_UDATA_OP(op));

    if (with_timeout) {
        sqe->flags |= IOSQE_IO_LINK;
        struct io_uring_sqe *tsqe = io_uring_get_sqe(&r->ring);
        io_uring_prep_link_timeout(tsqe, &ts, 0);
        io_uring_sqe_set_data64(tsqe, URING_UDATA_IGNORE);
    }

    // Do NOT submit here: SQEs are batched and flushed once per scheduler cycle
    // (xr_uring_ring_reap) or folded into the park wait (xr_uring_ring_wait via
    // io_uring_submit_and_wait_timeout). Batching many ops into one submit
    // syscall is io_uring's throughput advantage over per-op epoll read/write.
    atomic_fetch_add_explicit(&r->inflight, 1, memory_order_relaxed);
    return 0;
}

// Cancel the in-flight op identified by its user_data (the XrUringOp pointer).
// Cancel-by-user_data is immune to fd reuse after close(2). The op then
// completes with res == -ECANCELED, which wakes its parked waiter. Single
// producer — caller is the ring's owner worker.
static void uring_ring_cancel_op(XrUringRing *r, XrUringOp *op) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
    if (!sqe) {
        io_uring_submit(&r->ring);
        sqe = io_uring_get_sqe(&r->ring);
        if (!sqe)
            return;  // SQ exhausted; the op still completes on its own timeline
    }
    io_uring_prep_cancel64(sqe, URING_UDATA_OP(op), 0);
    io_uring_sqe_set_data64(sqe, URING_UDATA_IGNORE);  // discard the cancel CQE
    io_uring_submit(&r->ring);
}

/* ========== Reaping ========== */

// Handle one reaped CQE. Only completion ops and IGNORE sentinels reach a
// per-worker ring (no poll-add / wakeup: readiness lives on the local epoll).
static void uring_ring_dispatch_cqe(XrUringRing *r, uint64_t udata, int res, XrReadyList *list) {
    if (udata == URING_UDATA_IGNORE)
        return;  // linked-timeout half or cancel completion — nothing to do
    if (!URING_UDATA_IS_OP(udata))
        return;  // unknown tag — ignore defensively

    // A completion op CQE: one fewer outstanding op on this ring.
    atomic_fetch_sub_explicit(&r->inflight, 1, memory_order_relaxed);

    XrUringOp *op = URING_UDATA_GET_OP(udata);
    if (!op)
        return;
    op->res = res;
    atomic_store_explicit(&op->done, 1, memory_order_release);
    /* Release the direction *after* publishing the result so this op's own
     * wakeup is not dropped by the xr_netpoll_ready completion guard, and any
     * racing readiness wakeup that slips through now finds done == 1. Wake the
     * parked waiter through the normal readiness path; the op stays valid
     * (pinned by pd->uring_refs) until the waiter consumes the result. */
    atomic_store(&op->active, false);
    if (op->pd)
        xr_netpoll_ready(list, op->pd, op->mode);
}

int xr_uring_ring_reap(void *ring, XrReadyList *list) {
    XrUringRing *r = (XrUringRing *) ring;
    if (!r)
        return 0;

    // Flush SQEs batched by submit since the last cycle (one submit syscall for
    // all of this worker's pending ops), then drain completions.
    if (io_uring_sq_ready(&r->ring) > 0)
        io_uring_submit(&r->ring);

    struct io_uring_cqe *cqe;
    unsigned head;
    int count = 0;
    io_uring_for_each_cqe(&r->ring, head, cqe) {
        uring_ring_dispatch_cqe(r, io_uring_cqe_get_data64(cqe), cqe->res, list);
        count++;
    }
    if (count)
        io_uring_cq_advance(&r->ring, count);
    return count;
}

// Block until a completion is available on `ring` or `timeout_us` elapses. Used
// by an idle worker that has outstanding ops: a CQE then resumes it immediately
// instead of waiting out the scheduler's futex park timeout. Does not consume
// the CQE; the worker's poll cycle reaps it.
void xr_uring_ring_wait(void *ring, int64_t timeout_us) {
    XrUringRing *r = (XrUringRing *) ring;
    if (!r || timeout_us <= 0)
        return;
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_us / 1000000;
    ts.tv_nsec = (timeout_us % 1000000) * 1000;
    struct io_uring_cqe *cqe = NULL;
    // Flush any batched SQEs and wait for >= 1 completion (or timeout) in a
    // single syscall — submit + park folded together.
    io_uring_submit_and_wait_timeout(&r->ring, &cqe, 1, &ts, NULL);
}

// Number of submitted-but-unreaped completion ops on `ring`.
int xr_uring_ring_inflight(void *ring) {
    XrUringRing *r = (XrUringRing *) ring;
    return r ? atomic_load_explicit(&r->inflight, memory_order_relaxed) : 0;
}

/* ========== Cross-worker cancel queue (drained by the owner worker) ========== */

// Push pd onto the owner worker's cancel stack (MPSC Treiber). Caller has taken
// a uring_refs pin so pd survives until the owner drains it. Reuses pd->link
// (safe: a pinned pd is never freed, hence never on the free list, while queued).
static void uring_cancel_enqueue(XrProc *owner_p, XrPollDesc *pd) {
    void *old = atomic_load_explicit(&owner_p->uring_cancel_head, memory_order_relaxed);
    do {
        pd->link = (XrPollDesc *) old;
    } while (!atomic_compare_exchange_weak_explicit(&owner_p->uring_cancel_head, &old, pd,
                                                    memory_order_release, memory_order_relaxed));
}

void xr_netpoll_drain_uring_cancel(XrProc *p) {
    if (!p)
        return;
    void *head = atomic_exchange_explicit(&p->uring_cancel_head, NULL, memory_order_acquire);
    if (!head)
        return;
    XrUringRing *r = (XrUringRing *) p->local_poll.uring;
    XrPollDesc *pd = (XrPollDesc *) head;
    while (pd) {
        XrPollDesc *next = pd->link;
        // Cancel still-active ops on our own ring (single producer = us). An op
        // that already completed between the close and now is simply skipped.
        if (r) {
            if (atomic_load(&pd->uring_rop.active))
                uring_ring_cancel_op(r, &pd->uring_rop);
            if (atomic_load(&pd->uring_wop.active))
                uring_ring_cancel_op(r, &pd->uring_wop);
        }
        // Drop the queue pin; free if we were the last reference.
        XrNetpoll *np = pd->netpoll;
        if (atomic_fetch_sub(&pd->uring_refs, 1) == 1 && np) {
            if (netpoll_pd_reclaimable(pd))
                xr_poll_cache_free(&np->cache, pd);
            else
                xr_netpoll_deferred_free(np, pd);
        }
        pd = next;
    }
}

/* ========== Public completion API (declared in xnetpoll.h) ========== */

bool xr_netpoll_uring_active(XrNetpoll *np) {
    (void) np;
    // Completion mode is usable iff the submitting thread is a worker with a
    // ring. Outside a worker (bootstrap / CLI) callers use the readiness path.
    XrWorker *w = xr_current_worker();
    return w && w->p.local_poll.uring != NULL;
}

// Raw primitives (no pd wake routing, no timeout): caller polls op->done and
// reaps via xr_uring_ring_reap. Used by the standalone io_uring completion test.
int xr_uring_ring_submit_recv(void *ring, XrUringOp *op, int fd, void *buf, unsigned len) {
    if (!ring || !op)
        return -1;
    op->pd = NULL;
    op->mode = 0;
    XrUringReq req = {.kind = XR_URING_OP_RECV, .buf = buf, .len = len};
    return uring_ring_submit_op((XrUringRing *) ring, op, fd, &req);
}

int xr_uring_ring_submit_send(void *ring, XrUringOp *op, int fd, const void *buf, unsigned len) {
    if (!ring || !op)
        return -1;
    op->pd = NULL;
    op->mode = 0;
    XrUringReq req = {.kind = XR_URING_OP_SEND, .buf = (void *) buf, .len = len};
    return uring_ring_submit_op((XrUringRing *) ring, op, fd, &req);
}

// Yieldable completion submit: caller (xr_yield_for_uring_io) has parked the
// coro on pd->rg/wg for `mode` and claimed op->active. Store the op in the pd,
// take a pd ref (pin), and queue the op on the CURRENT worker's ring (the fd is
// bound to this worker, so this is single-producer). The CQE wakes the parked
// coro via xr_netpoll_ready; the continuation consumes the result with
// xr_netpoll_uring_xfer_result.
int xr_netpoll_uring_op_submit(XrPollDesc *pd, int mode, const XrUringReq *req) {
    if (!pd || !req)
        return XR_URING_XFER_FALLBACK;
    XrWorker *w = xr_current_worker();
    if (!w || !w->p.local_poll.uring)
        return XR_URING_XFER_FALLBACK;
    XrUringRing *r = (XrUringRing *) w->p.local_poll.uring;

    XrUringOp *op = (mode == XR_POLL_WRITE) ? &pd->uring_wop : &pd->uring_rop;
    op->pd = pd;
    op->mode = mode;
    atomic_fetch_add(&pd->uring_refs, 1);  // pin pd until the result is consumed

    if (uring_ring_submit_op(r, op, pd->fd, req) != 0) {
        atomic_fetch_sub(&pd->uring_refs, 1);
        return XR_URING_XFER_FALLBACK;
    }
    return 0;
}

#endif  // XR_OS_LINUX && XR_HAS_IO_URING
