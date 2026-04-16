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
 *   Single background thread consumes compilation tasks from a SPSC
 *   ring buffer. Each task runs the full XIR pipeline (build → optimize
 *   → codegen) and publishes the result via proto->jit_entry_pending.
 *   The main thread installs pending code at GC safepoints.
 *
 * THREAD SAFETY:
 *   - XrProto fields read by builder are immutable after creation
 *   - Background thread owns its own XirCodeAlloc (no sharing)
 *   - Only jit_entry_pending is written by bg thread (atomic)
 *   - Main thread reads jit_entry_pending and writes jit_entry (at safepoint)
 */

#include "xjit_compile_queue.h"
#include "xir_jit.h"
#include "xir_builder.h"
#include "xir_pass.h"
#include "xir_codegen.h"
#include "xir_code_alloc.h"
#include "xir_tfa.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/object/xstring.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"
#include "../base/xchecks.h"
#include <string.h>

/* ========== Forward declarations ========== */

// Defined in xir_jit.c — reused by background thread
extern XR_FUNC bool is_jit_eligible(XrProto *proto, bool verbose);

/* ========== Background compilation (single task) ========== */

// Run the full compilation pipeline for one proto.
// Called from the background thread. All allocations use xr_malloc.
// On success, publishes result via atomic store to proto->jit_entry_pending.
static void bg_compile_one(XirCompileQueue *q, XrProto *proto, bool is_recompile) {
    XirJitState *jit = q->jit;
    if (!proto || !jit) return;

    // Skip if already compiled at target level
    if (!is_recompile && proto->jit_entry) return;
    if (is_recompile && proto->jit_opt_level >= XIR_OPT_FULL) return;

    // Eligibility check (pure read of proto fields, thread-safe)
    if (!is_jit_eligible(proto, false)) return;

    // Build shared_protos mapping (reads enclosing proto chain, immutable)
    // NOTE: we call the internal helper from xir_jit.c
    // For simplicity, pass NULL shared_protos in background mode.
    // CALL_KNOWN for shared functions is a nice-to-have but not required
    // for correctness; the builder falls back to CALL_C.
    int nshared = 0;
    XrProto **shared_protos = NULL;

    // Build XIR from bytecode.
    // Pass NULL isolate — class registry internals use dynarrays that are
    // not safe for concurrent access from the bg thread.
    // Suppress type_feedback for the ENTIRE compilation pipeline (build +
    // optimize + codegen). Pipeline passes may read proto->type_feedback,
    // and the main thread concurrently writes to it via xfb_record_arg.
    struct XirTypeFeedback *saved_fb = proto->type_feedback;
    proto->type_feedback = NULL;  // benign race: main thread skips recording

    XirFunc *func = xir_build_from_proto_jit(proto, shared_protos, nshared,
                                              NULL, NULL);
    if (!func) {
        proto->type_feedback = saved_fb;  // restore on early exit
        xr_log_warning("jit-bg", "builder failed for %s",
                proto->name ? XR_STRING_CHARS(proto->name) : "?");
        return;
    }

    // Guard: reject oversized functions
    if (func->nvreg > 512) {
        xir_func_destroy(func);
        proto->type_feedback = saved_fb;
        return;
    }

    // Optimization passes
    XirOptLevel opt = is_recompile ? XIR_OPT_FULL : XIR_OPT_BASIC;
    xir_run_pipeline_ex(func, opt, proto);

    // Codegen — background thread uses its own dedicated code_alloc.
    // This eliminates data races with the main thread's sync/recompile
    // path which uses jit->code_alloc concurrently.
    XirCodegenResult res = xir_codegen_arm64(func, &q->bg_code_alloc);
    // Restore type_feedback now that all proto reads are done
    proto->type_feedback = saved_fb;
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

/* ========== Background thread main loop ========== */

static void *bg_thread_main(void *arg) {
    XirCompileQueue *q = (XirCompileQueue *)arg;
    XirJitState *jit = q->jit;

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

        // Drain all available tasks
        while (true) {
            uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
            uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
            if (tail == head) break;

            XirCompileTask task = q->tasks[tail & (XJIT_QUEUE_CAPACITY - 1)];
            atomic_store_explicit(&q->tail, tail + 1, memory_order_release);

            bg_compile_one(q, task.proto, task.is_recompile);
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
    xir_code_alloc_init(&q->bg_code_alloc);

    // Start background thread
    int err = pthread_create(&q->bg_thread, NULL, bg_thread_main, q);
    if (err == 0) {
        q->started = true;
        // Set thread name for debugging
#if defined(__APPLE__)
        // macOS: pthread_setname_np takes only name (sets current thread)
        // Can't set name from parent thread on macOS, bg thread would need to do it
#endif
    } else {
        xr_log_warning("jit-bg", "failed to create background compile thread: %d", err);
    }
}

void xjit_queue_destroy(XirCompileQueue *q) {
    if (!q) return;

    // Signal shutdown
    atomic_store_explicit(&q->shutdown, true, memory_order_release);

    // Wake up blocked thread
    pthread_mutex_lock(&q->mutex);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    // Join thread
    if (q->started) {
        pthread_join(q->bg_thread, NULL);
        q->started = false;
    }

    xir_code_alloc_destroy(&q->bg_code_alloc);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

bool xjit_queue_push(XirCompileQueue *q, XrProto *proto, bool is_recompile) {
    if (!q || !q->started || !proto) return false;

    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    // Full check
    if (head - tail >= XJIT_QUEUE_CAPACITY) return false;

    // Write task
    q->tasks[head & (XJIT_QUEUE_CAPACITY - 1)] = (XirCompileTask){
        .proto = proto,
        .is_recompile = is_recompile,
    };
    atomic_store_explicit(&q->head, head + 1, memory_order_release);

    // Wake background thread
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
