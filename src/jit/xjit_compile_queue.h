/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjit_compile_queue.h - Background JIT compilation queue
 *
 * KEY CONCEPT:
 *   N background worker threads compile hot functions asynchronously.
 *   Main thread enqueues compilation tasks at hot-function detection,
 *   continues interpreting without stalling. Compiled code is installed
 *   at GC safepoints via atomic jit_entry_pending → jit_entry swap.
 *
 * WHY THIS DESIGN:
 *   - XrProto is immutable after creation → safe for concurrent read
 *   - XIR build + codegen uses only xr_malloc (no GC heap interaction)
 *   - Each worker owns a private XirCodeAlloc (no lock needed)
 *   - MPSC queue: one producer (main thread), N consumers (bg workers)
 */

#ifndef XJIT_COMPILE_QUEUE_H
#define XJIT_COMPILE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include "../base/xdefs.h"
#include "xir_code_alloc.h"
#include "../runtime/value/xtype_feedback.h"

typedef struct XrProto XrProto;
typedef struct XirJitState XirJitState;
struct XrShape;

/* Maximum number of enclosing-proto SETSHARED entries captured into a
 * background compile task.  Functions referenced past this are simply
 * not elevated to CALL_KNOWN — JIT falls back to CALL_C, which is a
 * slower but fully correct path. */
#define XJIT_BG_SHARED_CAP 32

/* ========== Background Compile Result ========== */

// Bundled compilation output written by bg thread, installed by main thread.
// All fields are written BEFORE jit_entry_pending is published.
typedef struct XirBgResult {
    void *code;         // compiled machine code entry point
    void *fast_entry;   // fast entry (skip param setup)
    void *deopt_table;  // XirRtDeoptEntry array
    uint32_t ndeopt;
    void *osr_entries;  // XirOsrEntry array
    uint32_t nosr;
    void *stack_map;     // XrStackMapTable*
    void *resume_entry;  // resume entry for suspend/resume (NULL = none)
    uint8_t opt_level;   // XIR_OPT_BASIC or XIR_OPT_FULL
} XirBgResult;

/* ========== Compile Task ========== */

/*
 * Snapshot of every mutable input the background compiler needs.
 *
 * The main thread populates this struct when it enqueues a task, while
 * the bg thread only reads from it.  Crucially this means the bg thread
 * never races against concurrent main-thread mutations of
 * proto->type_feedback, which in turn lets us drop the old pattern of
 * temporarily NULL-ing out proto->type_feedback during compilation.
 *
 * Only |proto| itself is dereferenced from the bg thread, and only for
 * fields that are immutable after creation (bytecode, constants, etc.).
 */
typedef struct XirBgTask {
    XrProto *proto;     // target proto (immutable after creation)
    bool is_recompile;  // Tier 1 → Tier 2 recompilation
    bool has_feedback;  // feedback_snapshot is valid
    XirTypeFeedback feedback_snapshot;
    int nshared;  // number of valid shared_protos entries
    XrProto *shared_protos[XJIT_BG_SHARED_CAP];
    struct XrShape *shape_hint;  // dominant-shape hint for param PTR shaping

    /*
     * Inline-cache snapshots captured on the foreground thread at task
     * push time. Owned by the task: the bg worker hands them to the
     * builder (read-only) and frees them after compilation completes.
     * Either may be NULL when the live ctx had no IC recorded yet.
     */
    struct XrICFieldTable *ic_fields_snapshot;
    struct XrICMethodTable *ic_methods_snapshot;
    struct XrICBuiltinTable *ic_builtin_snapshot;
} XirBgTask;

/* ========== MPSC Ring Buffer ========== */

#define XJIT_QUEUE_CAPACITY 256  // power of 2, increased for multi-worker
#define XJIT_MAX_WORKERS 4       // max background compile threads

typedef struct XirCompileQueue {
    XirBgTask tasks[XJIT_QUEUE_CAPACITY];
    _Atomic uint32_t head;  // written by producer (main thread)
    _Atomic uint32_t tail;  // CAS-advanced by consumers (bg workers)

    // Background worker threads
    pthread_t workers[XJIT_MAX_WORKERS];
    uint32_t n_workers;  // actual number of bg threads started
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    _Atomic bool shutdown;  // signal background threads to exit
    bool started;           // at least one worker thread is running

    // Owning JIT state (for compilation pipeline access)
    XirJitState *jit;

    // Per-worker dedicated code allocator.
    // Each worker owns one to eliminate contention with main thread
    // and with other workers.
    XirCodeAlloc worker_code_alloc[XJIT_MAX_WORKERS];
} XirCompileQueue;

/* ========== API ========== */

// Initialize queue and start background compilation thread.
XR_FUNC void xjit_queue_init(XirCompileQueue *q, XirJitState *jit);

// Shutdown: signal background thread to exit and join.
XR_FUNC void xjit_queue_destroy(XirCompileQueue *q);

// Enqueue a pre-populated snapshot for background compilation (non-blocking).
// Returns true if enqueued, false if queue is full (caller should compile sync).
// |task| is copied into the ring buffer; the caller retains ownership.
XR_FUNC bool xjit_queue_push(XirCompileQueue *q, const XirBgTask *task);

// Check if queue has pending tasks (for safepoint polling).
static inline bool xjit_queue_has_pending(XirCompileQueue *q) {
    return atomic_load_explicit(&q->head, memory_order_acquire) !=
           atomic_load_explicit(&q->tail, memory_order_acquire);
}

#endif  // XJIT_COMPILE_QUEUE_H
