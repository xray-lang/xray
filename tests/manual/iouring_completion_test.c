/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * iouring_completion_test.c - Linux io_uring completion-mode primitives test
 *
 * Exercises the per-worker ring primitives xr_uring_ring_submit_recv / _send
 * against a socketpair: a recv/send SQE is queued on a standalone ring, a reap
 * cycle drains the CQE, and the byte count is delivered to the caller's
 * XrUringOp. This validates the completion path on a real io_uring ring (Linux
 * 5.6+; needs seccomp to allow io_uring syscalls).
 *
 * Linked directly against libxray_core.a (not part of ctest) and driven by
 * scripts/run_iouring_completion_test.sh, since io_uring is Linux-only and the
 * container needs --security-opt seccomp=unconfined.
 */
#include "../../src/coro/xnetpoll.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if !(defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING))

int main(void) {
    printf("SKIP: io_uring completion test requires Linux + XR_HAS_IO_URING\n");
    return 0;
}

#else

static int failures = 0;

static void check(int cond, const char *msg) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
        failures++;
}

// Pump the reap loop until op->done or the budget runs out. The raw primitives
// set op->pd = NULL, so reaping just publishes op->done (no wakeup routing).
static void drive_until_done(void *ring, XrUringOp *op) {
    for (int i = 0; i < 200 && !atomic_load_explicit(&op->done, memory_order_acquire); i++) {
        XrReadyList rl = {0};
        xr_uring_ring_reap(ring, &rl);
        if (atomic_load_explicit(&op->done, memory_order_acquire))
            break;
        usleep(1000);  // 1ms between non-blocking reaps
    }
}

int main(void) {
    void *ring = xr_uring_ring_create();
    if (!ring) {
        printf("SKIP: io_uring ring unavailable (old kernel / seccomp?)\n");
        return 0;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("FAIL socketpair\n");
        xr_uring_ring_destroy(ring);
        return 1;
    }

    // --- recv completion: data written to sv[1] is delivered via a recv SQE on sv[0]
    {
        const char *msg = "hello-io_uring-completion";
        size_t mlen = strlen(msg);
        char buf[128];
        memset(buf, 0, sizeof(buf));
        XrUringOp op;
        atomic_store(&op.done, 0);

        int rc = xr_uring_ring_submit_recv(ring, &op, sv[0], buf, (unsigned) sizeof(buf));
        check(rc == 0, "submit_recv queued");
        ssize_t w = write(sv[1], msg, mlen);
        check(w == (ssize_t) mlen, "peer write");
        drive_until_done(ring, &op);
        check(atomic_load(&op.done) == 1, "recv op completed");
        check(op.res == (long) mlen, "recv byte count matches");
        check(memcmp(buf, msg, mlen) == 0, "recv payload matches");
    }

    // --- send completion: a send SQE on sv[0] is received by a plain read on sv[1]
    {
        const char *msg = "send-via-completion";
        size_t mlen = strlen(msg);
        XrUringOp op;
        atomic_store(&op.done, 0);

        int rc = xr_uring_ring_submit_send(ring, &op, sv[0], msg, (unsigned) mlen);
        check(rc == 0, "submit_send queued");
        drive_until_done(ring, &op);
        check(atomic_load(&op.done) == 1, "send op completed");
        check(op.res == (long) mlen, "send byte count matches");

        char rbuf[128];
        memset(rbuf, 0, sizeof(rbuf));
        ssize_t r = read(sv[1], rbuf, sizeof(rbuf));
        check(r == (ssize_t) mlen, "peer read byte count");
        check(memcmp(rbuf, msg, mlen) == 0, "peer received payload");
    }

    close(sv[0]);
    close(sv[1]);
    xr_uring_ring_destroy(ring);

    printf("RESULT: %s\n", failures == 0 ? "ALL PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}

#endif
