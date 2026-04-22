/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjit_compile_queue.c - Background JIT compilation queue
 *
 * KEY CONCEPT:
 *   N background worker threads consume compilation tasks from an MPSC
 *   ring buffer via CAS on the tail index. Each task runs the full XIR
 *   pipeline (build → optimize → codegen) and publishes the result via
 *   proto->jit_entry_pending.  Main thread installs pending code at
 *   GC safepoints.
 *
 * THREAD SAFETY:
 *   - XrProto fields read by builder are immutable after creation
 *   - Each worker owns a private XirCodeAlloc (no sharing)
 *   - Only jit_entry_pending is written by bg threads (atomic)
 *   - Main thread reads jit_entry_pending and writes jit_entry (at safepoint)
 */

#include "xjit_compile_queue.h"
#include "xir_jit.h"
#include "xir_builder.h"
#include "xir_pass.h"
#include "xir_codegen.h"
#include "xir_code_alloc.h"
#include "xir_tfa.h"
#include "xir_eligibility.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/object/xstring.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"
#include "../base/xchecks.h"
#include <string.h>
#include <unistd.h>  // sysconf(_SC_NPROCESSORS_ONLN)

/* ========== Worker context (passed via thread arg) ========== */

typedef struct {
    XirCompileQueue *queue;
    uint32_t         worker_id;  // 0..n_workers-1
} BgWorkerArg;

/* ========== Background compilation (single task) ========== */

// Run the full compilation pipeline for one proto using a pre-snapshotted task.
// Called from a background worker. All allocations use xr_malloc.
// On success, publishes result via atomic store to proto->jit_entry_pending.
//
// IMPORTANT: this function must never write back to |proto|; every field it
// needs is already present in |task| or is a truly immutable proto field
// (bytecode array, constants pool, child protos, etc.).
static void bg_compile_one(XirCompileQueue *q, uint32_t worker_id,
                           const XirBgTask *task) {
    XirJitState *jit = q->jit;
    if (!task || !task->proto || !jit) return;
    XR_DCHECK(worker_id < XJIT_MAX_WORKERS, "worker_id out of range");

    XrProto *proto = task->proto;
    bool is_recompile = task->is_recompile;

    // Skip if already compiled at target level
    if (!is_recompile && proto->jit_entry) return;
    if (is_recompile && proto->jit_opt_level >= XIR_OPT_FULL) return;

    // Eligibility re-check: the main thread already ran this, but a
    // concurrent deopt may have bumped deopt_count in between; re-checking
    // here keeps eligibility a read-only query and costs very little.
    if (!is_jit_eligible(proto, false)) return;

    /* Build XIR from the immutable proto bytecode.
     *
     * - shared_protos come from the task snapshot: builder can emit
     *   CALL_KNOWN for module-level fn calls without touching shared state.
     * - shape_hint also comes from the snapshot, captured on the main
     *   thread from dominant_shape analysis.
     * - NULL isolate: class-registry dynarrays are not safe for bg
     *   concurrent access; builder works in a bg-safe subset. */
    XrProto **shared_protos = task->nshared > 0
        ? (XrProto **)task->shared_protos : NULL;
    XirFunc *func = xir_build_from_proto_jit(proto, shared_protos,
                                              task->nshared,
                                              task->shape_hint, NULL);
    if (!func) {
        xr_log_warning("jit-bg", "builder failed for %s",
                proto->name ? XR_STRING_CHARS(proto->name) : "?");
        return;
    }

    // Guard: reject oversized functions
    if (func->nvreg > 4096) {
        xir_func_destroy(func);
        return;
    }

    // Optimization passes
    XirOptLevel opt = is_recompile ? XIR_OPT_FULL : XIR_OPT_BASIC;
    xir_run_pipeline_ex(func, opt, proto);

    // Codegen — each worker uses its own dedicated code_alloc.
    // This eliminates data races between workers and the main thread.
#if defined(__aarch64__)
    XirCodegenResult res = xir_codegen_arm64(func, &q->worker_code_alloc[worker_id]);
#elif defined(__x86_64__)
    XirCodegenResult res = xir_codegen_x64(func, &q->worker_code_alloc[worker_id]);
#else
    XirCodegenResult res = { .success = false, .error = "unsupported architecture" };
#endif
    if (!res.success) {
        xr_log_warning("jit-bg", "codegen failed for %s",
                proto->name ? XR_STRING_CHARS(proto->name) : "?");
        xir_func_destroy(func);
        return;
    }

    // Bundle ALL compilation outputs into a heap-allocated result struct.
    // The bg thread MUST NOT write any proto fields directly (data race).
    // The main thread installs everything atomically at OP_CALL time.
    XirBgResult *bgr = (XirBgResult *)xr_calloc(1, sizeof(XirBgResult));
    if (!bgr) {
        xir_func_destroy(func);
        return;
    }

    bgr->code = res.code;
    bgr->fast_entry = (char *)res.code + res.fast_entry_offset * 4;
    bgr->resume_entry = res.resume_entry_offset
        ? (char *)res.code + res.resume_entry_offset * 4 : NULL;
    bgr->opt_level = (uint8_t)opt;
    bgr->stack_map = res.stack_map;

    // Copy deopt table
    if (res.ndeopt > 0) {
        size_t deopt_size = res.ndeopt * sizeof(XirRtDeoptEntry);
        XirRtDeoptEntry *entries = (XirRtDeoptEntry *)xr_malloc(deopt_size);
        if (entries) {
            memcpy(entries, res.deopt_entries, deopt_size);
            bgr->deopt_table = entries;
            bgr->ndeopt = res.ndeopt;
        }
    }

    // Copy OSR entries
    if (res.nosr > 0) {
        size_t osr_size = res.nosr * sizeof(XirOsrEntry);
        XirOsrEntry *entries = (XirOsrEntry *)xr_malloc(osr_size);
        if (entries) {
            memcpy(entries, res.osr_entries, osr_size);
            bgr->osr_entries = entries;
            bgr->nosr = res.nosr;
        }
    }

    // Publish result to main thread via jit_entry_pending.
    // Main thread reads with acquire in OP_CALL, installing all metadata.
    atomic_store_explicit(&proto->jit_entry_pending, (void *)bgr, memory_order_release);

    if (jit->verbose) {
        xr_log_verbose("jit-bg", "%s %s (O%d, %u bytes, %u vregs)",
                is_recompile ? "recompile" : "compile",
                proto->name ? XR_STRING_CHARS(proto->name) : "?",
                (int)opt, res.code_size, func->nvreg);
    }

    xir_func_destroy(func);
}

/* ========== Background worker main loop ========== */

// Each worker loops: wait on condvar, CAS-dequeue a task, compile it.
// CAS on tail ensures exactly-once delivery across multiple workers.
static void *bg_worker_main(void *arg) {
    BgWorkerArg *wa = (BgWorkerArg *)arg;
    XirCompileQueue *q = wa->queue;
    uint32_t wid = wa->worker_id;
    xr_free(wa);  // heap-allocated by init, owned by this thread

#if defined(__APPLE__)
    // Set thread name for debugging (macOS: can only set own name)
    char tname[32];
    snprintf(tname, sizeof(tname), "jit-bg-%u", wid);
    pthread_setname_np(tname);
#endif

    while (!atomic_load_explicit(&q->shutdown, memory_order_acquire)) {
        // Wait for tasks
        pthread_mutex_lock(&q->mutex);
        while (atomic_load_explicit(&q->head, memory_order_acquire) ==
               atomic_load_explicit(&q->tail, memory_order_acquire) &&
               !atomic_load_explicit(&q->shutdown, memory_order_acquire)) {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
        pthread_mutex_unlock(&q->mutex);

        if (atomic_load_explicit(&q->shutdown, memory_order_acquire))
            break;

        // Drain tasks via CAS (multiple workers may compete)
        while (true) {
            uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
            uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
            if (tail == head) break;

            // CAS: try to claim slot [tail]
            if (!atomic_compare_exchange_weak_explicit(
                    &q->tail, &tail, tail + 1,
                    memory_order_acq_rel, memory_order_acquire)) {
                continue;  // another worker got it, retry
            }

            // Copy the snapshot out of the ring buffer before compiling
            XirBgTask task = q->tasks[tail & (XJIT_QUEUE_CAPACITY - 1)];
            bg_compile_one(q, wid, &task);
        }
    }

    return NULL;
}

/* ========== Public API ========== */

void xjit_queue_init(XirCompileQueue *q, XirJitState *jit) {
    XR_DCHECK(q != NULL, "xjit_queue_init: q is NULL");
    XR_DCHECK(jit != NULL, "xjit_queue_init: jit is NULL");

    memset(q, 0, sizeof(*q));
    q->jit = jit;
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    atomic_store(&q->shutdown, false);

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);

    // Determine worker count: min(XJIT_MAX_WORKERS, nCPU - 1), at least 1
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    uint32_t nw = (uint32_t)(ncpu - 1);
    if (nw < 1) nw = 1;
    if (nw > XJIT_MAX_WORKERS) nw = XJIT_MAX_WORKERS;

    // Initialize per-worker code allocators
    for (uint32_t i = 0; i < nw; i++) {
        xir_code_alloc_init(&q->worker_code_alloc[i]);
    }

    // Start worker threads
    q->n_workers = 0;
    for (uint32_t i = 0; i < nw; i++) {
        BgWorkerArg *wa = (BgWorkerArg *)xr_malloc(sizeof(BgWorkerArg));
        if (!wa) break;
        wa->queue = q;
        wa->worker_id = i;
        int err = pthread_create(&q->workers[i], NULL, bg_worker_main, wa);
        if (err == 0) {
            q->n_workers++;
        } else {
            xr_free(wa);
            xr_log_warning("jit-bg", "failed to create worker %u: %d", i, err);
            break;
        }
    }
    q->started = (q->n_workers > 0);

    if (jit->verbose && q->started) {
        xr_log_verbose("jit-bg", "started %u background compile workers",
                       q->n_workers);
    }
}

void xjit_queue_destroy(XirCompileQueue *q) {
    if (!q) return;

    // Signal shutdown
    atomic_store_explicit(&q->shutdown, true, memory_order_release);

    // Wake up all blocked workers
    pthread_mutex_lock(&q->mutex);
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    // Join all worker threads
    for (uint32_t i = 0; i < q->n_workers; i++) {
        pthread_join(q->workers[i], NULL);
    }
    q->n_workers = 0;
    q->started = false;

    // Destroy per-worker code allocators
    for (uint32_t i = 0; i < XJIT_MAX_WORKERS; i++) {
        xir_code_alloc_destroy(&q->worker_code_alloc[i]);
    }

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

bool xjit_queue_push(XirCompileQueue *q, const XirBgTask *task) {
    if (!q || !q->started || !task || !task->proto) return false;

    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    // Full check
    if (head - tail >= XJIT_QUEUE_CAPACITY) return false;

    // Copy the task snapshot into the ring buffer slot (bg thread sees a
    // self-contained view and never has to re-read mutable proto fields).
    q->tasks[head & (XJIT_QUEUE_CAPACITY - 1)] = *task;
    atomic_store_explicit(&q->head, head + 1, memory_order_release);

    // Wake one worker (or broadcast if queue was empty — multiple items queued rapidly)
    pthread_mutex_lock(&q->mutex);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    return true;
}

void xjit_install_pending(XirCompileQueue *q, XrProto *proto) {
    // Installation is done inline in xvm.c OP_CALL handler.
    // This function exists for future batch-install at GC safepoints.
    (void)q;
    (void)proto;
}
