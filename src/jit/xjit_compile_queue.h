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
 *   Single background thread compiles hot functions asynchronously.
 *   Main thread enqueues compilation tasks at hot-function detection,
 *   continues interpreting without stalling. Compiled code is installed
 *   at GC safepoints via atomic jit_entry_pending → jit_entry swap.
 *
 * WHY THIS DESIGN:
 *   - XrProto is immutable after creation → safe for concurrent read
 *   - XIR build + codegen uses only xr_malloc (no GC heap interaction)
 *   - Single thread avoids code_alloc contention (no lock needed)
 *   - SPSC queue: one producer (main thread), one consumer (bg thread)
 */

#ifndef XJIT_COMPILE_QUEUE_H
#define XJIT_COMPILE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include "../base/xdefs.h"
#include "xir_code_alloc.h"

typedef struct XrProto XrProto;
typedef struct XirJitState XirJitState;

/* ========== Background Compile Result ========== */

// Bundled compilation output written by bg thread, installed by main thread.
// All fields are written BEFORE jit_entry_pending is published.
typedef struct XirBgResult {
    void    *code;              // compiled machine code entry point
    void    *fast_entry;        // fast entry (skip param setup)
    void    *deopt_table;       // XirRtDeoptEntry array
    uint32_t ndeopt;
    void    *osr_entries;       // XirOsrEntry array
    uint32_t nosr;
    void    *stack_map;         // XrStackMapTable*
    void    *resume_entry;      // resume entry for suspend/resume (NULL = none)
    uint8_t  opt_level;         // XIR_OPT_BASIC or XIR_OPT_FULL
} XirBgResult;

/* ========== Compile Task ========== */

// Snapshot of compilation inputs, safe for background thread consumption.
typedef struct XirCompileTask {
    XrProto *proto;             // target proto (immutable after creation)
    bool     is_recompile;      // Tier 1 → Tier 2 recompilation
} XirCompileTask;

/* ========== SPSC Ring Buffer ========== */

#define XJIT_QUEUE_CAPACITY 64  // power of 2

typedef struct XirCompileQueue {
    XirCompileTask tasks[XJIT_QUEUE_CAPACITY];
    _Atomic uint32_t head;      // written by producer (main thread)
    _Atomic uint32_t tail;      // written by consumer (bg thread)

    // Background thread state
    pthread_t        bg_thread;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
    _Atomic bool     shutdown;  // signal background thread to exit
    bool             started;   // background thread has been created

    // Owning JIT state (for compilation pipeline access)
    XirJitState     *jit;

    // Dedicated code allocator for background thread.
    // Eliminates concurrent access with main thread's sync/recompile path
    // which uses jit->code_alloc.
    XirCodeAlloc     bg_code_alloc;
} XirCompileQueue;

/* ========== API ========== */

// Initialize queue and start background compilation thread.
XR_FUNC void xjit_queue_init(XirCompileQueue *q, XirJitState *jit);

// Shutdown: signal background thread to exit and join.
XR_FUNC void xjit_queue_destroy(XirCompileQueue *q);

// Enqueue a proto for background compilation (non-blocking).
// Returns true if enqueued, false if queue is full (caller should compile sync).
XR_FUNC bool xjit_queue_push(XirCompileQueue *q, XrProto *proto, bool is_recompile);

// Install all pending compiled code (called from main thread at OP_CALL).
// Atomically moves bg_result data into proto fields and sets jit_entry.
XR_FUNC void xjit_install_pending(XirCompileQueue *q, XrProto *proto);

// Check if queue has pending tasks (for safepoint polling).
static inline bool xjit_queue_has_pending(XirCompileQueue *q) {
    return atomic_load_explicit(&q->head, memory_order_acquire) !=
           atomic_load_explicit(&q->tail, memory_order_acquire);
}

#endif // XJIT_COMPILE_QUEUE_H
