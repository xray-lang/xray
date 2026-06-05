/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_snapshot.c - Best-effort runtime coroutine snapshots
 */

#include "xcoro_snapshot.h"
#include "../base/xmalloc.h"
#include "xworker.h"

#define XR_CORO_SNAPSHOT_LOCAL_BUF 256

static bool snapshot_contains(const XrCoroSnapshotEntry *out, int count, XrCoroutine *coro) {
    if (!out || !coro)
        return false;
    for (int i = 0; i < count; i++) {
        if (out[i].coro == coro)
            return true;
    }
    return false;
}

static int snapshot_append(XrCoroSnapshotEntry *out, int count, int max_out, XrCoroutine *coro,
                           const char *state) {
    if (!out || count >= max_out || !coro || !state)
        return count;
    if (snapshot_contains(out, count, coro))
        return count;
    out[count].coro = coro;
    out[count].state = state;
    return count + 1;
}

static int snapshot_append_steal_queue(XrCoroSnapshotEntry *out, int count, int max_out,
                                       XrStealQueue *queue, const char *state,
                                       XrCoroutine **scratch, int scratch_cap) {
    int remaining = max_out - count;
    if (remaining <= 0 || !scratch || scratch_cap <= 0)
        return count;
    int limit = remaining < scratch_cap ? remaining : scratch_cap;
    int n = xr_steal_queue_snapshot(queue, scratch, limit);
    for (int i = 0; i < n && count < max_out; i++) {
        count = snapshot_append(out, count, max_out, scratch[i], state);
    }
    return count;
}

static int snapshot_append_worker_ready(XrCoroSnapshotEntry *out, int count, int max_out,
                                        XrWorker *worker, XrCoroutine **scratch, int scratch_cap) {
    for (int p = 0; p < XR_RUNQ_COUNT && count < max_out; p++) {
        XrRunQueue *rq = &worker->p.runq[p];
        count = snapshot_append_steal_queue(out, count, max_out, &rq->deque, "ready", scratch,
                                            scratch_cap);
        XrCoroutine *ov = rq->overflow_first;
        while (ov && count < max_out) {
            count = snapshot_append(out, count, max_out, ov, "ready");
            ov = ov->sched_link;
        }
    }

    XrCoroutine *lifo = atomic_load_explicit(&worker->p.lifo_slot, memory_order_relaxed);
    count = snapshot_append(out, count, max_out, lifo, "ready");
    count = snapshot_append_steal_queue(out, count, max_out, &worker->p.cont_deque, "ready",
                                        scratch, scratch_cap);
    return count;
}

static int snapshot_append_worker_blocked(XrCoroSnapshotEntry *out, int count, int max_out,
                                          XrWorker *worker) {
    XrCoroutine *blocked = worker->p.blocked_head;
    while (blocked && count < max_out) {
        count = snapshot_append(out, count, max_out, blocked, "blocked");
        blocked = blocked->next;
    }
    return count;
}

static int snapshot_append_worker_running(XrCoroSnapshotEntry *out, int count, int max_out,
                                          XrWorker *worker) {
    XrMachine *machine = atomic_load_explicit(&worker->p.current_m, memory_order_relaxed);
    if (!machine)
        machine = worker->m;
    XrCoroutine *running =
        machine ? atomic_load_explicit(&machine->current_coro, memory_order_relaxed) : NULL;
    return snapshot_append(out, count, max_out, running, "running");
}

static int snapshot_append_injectq(XrCoroSnapshotEntry *out, int count, int max_out,
                                   XrRuntime *runtime) {
    for (int p = 0; p < XR_CORO_PRIORITY_COUNT && count < max_out; p++) {
        XrInjectQueue *q = &runtime->injectq[p];
        xr_mutex_lock(&q->lock);
        XrCoroutine *cur = q->head;
        while (cur && count < max_out) {
            count = snapshot_append(out, count, max_out, cur, "ready");
            cur = cur->sched_link;
        }
        xr_mutex_unlock(&q->lock);
    }
    return count;
}

int xr_runtime_collect_coros(XrRuntime *runtime, XrCoroSnapshotEntry *out, int max_out) {
    if (!runtime || !out || max_out <= 0)
        return 0;

    XrCoroutine *local_buf[XR_CORO_SNAPSHOT_LOCAL_BUF];
    XrCoroutine **scratch = local_buf;
    int scratch_cap = max_out < XR_CORO_SNAPSHOT_LOCAL_BUF ? max_out : XR_CORO_SNAPSHOT_LOCAL_BUF;
    XrCoroutine **heap_buf = NULL;
    if (max_out > XR_CORO_SNAPSHOT_LOCAL_BUF) {
        heap_buf = (XrCoroutine **) xr_malloc(sizeof(XrCoroutine *) * (size_t) max_out);
        if (heap_buf) {
            scratch = heap_buf;
            scratch_cap = max_out;
        }
    }

    int count = 0;
    for (int wi = 0; wi < runtime->worker_count && count < max_out; wi++) {
        XrWorker *worker = &runtime->workers[wi];
        count = snapshot_append_worker_running(out, count, max_out, worker);
        count = snapshot_append_worker_ready(out, count, max_out, worker, scratch, scratch_cap);
        count = snapshot_append_worker_blocked(out, count, max_out, worker);
    }
    count = snapshot_append_injectq(out, count, max_out, runtime);
    if (heap_buf)
        xr_free(heap_buf);
    return count;
}
