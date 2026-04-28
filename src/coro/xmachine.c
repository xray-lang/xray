/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmachine.c - Machine (M/OS thread) implementation for P/M split scheduler
 *
 * KEY CONCEPT:
 *   M lifecycle: alloc -> init -> park (wait for P) -> acquire P -> run -> release P -> park
 *   When M blocks in C code, it releases P via handoff, stays blocked, then parks after return.
 */

#include "xmachine.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "xproc.h"
#include "xworker.h"
#include "../runtime/xstrbuf.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ========== M Lifecycle ==========

XrMachine *xr_machine_alloc(struct XrRuntime *runtime, int id) {
    XR_DCHECK(runtime != NULL, "machine_alloc: NULL runtime");
    XrMachine *m = (XrMachine *) xr_calloc(1, sizeof(XrMachine));
    if (!m)
        return NULL;
    xr_machine_init(m, id, runtime);
    return m;
}

void xr_machine_init(XrMachine *m, int id, struct XrRuntime *runtime) {
    XR_DCHECK(m != NULL, "machine_init: NULL machine");
    XR_DCHECK(runtime != NULL, "machine_init: NULL runtime");
    memset(m, 0, sizeof(XrMachine));
    m->id = id;
    m->runtime = runtime;
    atomic_store(&m->current_p, NULL);
    atomic_store(&m->state, M_PARKED);
    m->spinning = false;
    m->next_p = NULL;
    m->current_coro = NULL;
    m->all_link = NULL;
    m->idle_link = NULL;
    atomic_store(&m->in_idle_worker_list, false);
    atomic_store(&m->heartbeat, 0);

    // Initialize VM storage
    memset(&m->vm_storage, 0, sizeof(XrMachineVMStorage));

    // Initialize VM context bound to private storage
    XrVMContext *ctx = &m->vm_ctx;
    ctx->stack = m->vm_storage.stack;
    ctx->stack_top = m->vm_storage.stack;
    ctx->stack_capacity = XR_MACHINE_STACK_SIZE;
    ctx->frames = m->vm_storage.frames;
    ctx->frame_count = 0;
    ctx->frame_capacity = XR_MACHINE_FRAME_SIZE;
    ctx->handlers = m->vm_storage.handlers;
    ctx->handler_count = 0;
    ctx->handler_capacity = XMACHINE_HANDLER_SIZE;
    ctx->module_base_frame = 0;
    ctx->current_coro = NULL;
    ctx->trace_execution = false;
    if (runtime) {
        ctx->isolate = runtime->isolate;
    }
    ctx->tmp_strbuf = NULL;
    ctx->defer_stack = NULL;
    ctx->defer_count = 0;
    ctx->defer_capacity = 0;
    ctx->defer_frame_marks = NULL;

    // Initialize futex-based park state
    atomic_store(&m->park_state, XR_PARK_IDLE);
}

void xr_machine_destroy(XrMachine *m) {
    if (!m)
        return;

    // Free string buffer
    if (m->vm_ctx.tmp_strbuf) {
        xr_strbuf_free(m->vm_ctx.tmp_strbuf);
        m->vm_ctx.tmp_strbuf = NULL;
    }
    // Free per-context defer stack
    if (m->vm_ctx.defer_stack) {
        xr_free(m->vm_ctx.defer_stack);
        m->vm_ctx.defer_stack = NULL;
    }
    if (m->vm_ctx.defer_frame_marks) {
        xr_free(m->vm_ctx.defer_frame_marks);
        m->vm_ctx.defer_frame_marks = NULL;
    }
}

// ========== M Park/Unpark ==========

void xr_park_m(XrMachine *m) {
    XR_CHECK(m != NULL, "park_m: NULL machine");
    atomic_store(&m->state, M_PARKED);

    // Futex-based park: wait until next_p is set
    while (!m->next_p && atomic_load(&m->state) == M_PARKED) {
        atomic_store_explicit(&m->park_state, XR_PARK_IDLE, memory_order_release);
        // 10ms timeout to avoid missing wakeups
        xr_park_futex_wait(&m->park_state, XR_PARK_IDLE, 10000);
    }

    atomic_store(&m->state, M_RUNNING);
}

void xr_unpark_m(XrMachine *m) {
    XR_CHECK(m != NULL, "unpark_m: NULL machine");
    atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
    xr_park_futex_wake(&m->park_state);
}

// ========== Idle M Management ==========

// idle_m_head is a lock-free Treiber stack.
//
// ABA safety: XrMachine instances are never freed during runtime lifetime
// (allocated in a grow-only array keyed by handoff count). idle_link is
// shared with idle_worker_list but the two are mutually exclusive — an M
// is on idle_m_head only after handoff has unbound it from its Worker
// (see xr_handoff_thread_entry).
XrMachine *xr_get_idle_m(struct XrRuntime *runtime) {
    if (!runtime)
        return NULL;

    for (int retry = 0; retry < 8; retry++) {
        XrMachine *head = atomic_load_explicit(&runtime->idle_m_head, memory_order_acquire);
        if (!head)
            return NULL;
        XrMachine *next = head->idle_link;
        if (atomic_compare_exchange_weak_explicit(&runtime->idle_m_head, &head, next,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            head->idle_link = NULL;
            atomic_fetch_sub_explicit(&runtime->idle_m_count, 1, memory_order_relaxed);
            return head;
        }
    }
    return NULL;
}

void xr_put_idle_m(struct XrRuntime *runtime, XrMachine *m) {
    if (!runtime || !m)
        return;

    atomic_store(&m->state, M_PARKED);

    XrMachine *head;
    do {
        head = atomic_load_explicit(&runtime->idle_m_head, memory_order_relaxed);
        m->idle_link = head;
    } while (!atomic_compare_exchange_weak_explicit(&runtime->idle_m_head, &head, m,
                                                    memory_order_release, memory_order_relaxed));
    atomic_fetch_add_explicit(&runtime->idle_m_count, 1, memory_order_relaxed);
}

// Start or wake an M to run P.
// If an idle M exists, start a handoff thread for it.
// Otherwise, allocate a new M and start its handoff thread.
void xr_startm(struct XrProc *p, bool spinning) {
    if (!p)
        return;
    struct XrRuntime *runtime = p->runtime;
    if (!runtime)
        return;

    // Try to get an idle M
    XrMachine *m = xr_get_idle_m(runtime);
    if (m) {
        m->spinning = spinning;
        atomic_store(&m->state, M_RUNNING);

        // Set next_p before signaling (thread checks this after waking)
        m->next_p = p;
        atomic_store_explicit(&m->park_state, XR_PARK_WOKEN, memory_order_release);
        xr_park_futex_wake(&m->park_state);

        if (atomic_load(&m->has_thread)) {
            // Thread reuse: parked thread will wake and process next_p
            return;
        }
        // No live thread: fall through to create one
        xr_thread_t t;
        xr_thread_create(&t, xr_handoff_thread_entry, m);
        xr_thread_detach(t);
        return;
    }

    // No idle M: allocate a new one
    int new_id = atomic_fetch_add(&runtime->m_count, 1);
    m = xr_machine_alloc(runtime, new_id);
    if (!m) {
        return;
    }
    m->next_p = p;
    m->spinning = spinning;
    atomic_store(&m->state, M_RUNNING);

    // Start new handoff thread
    xr_thread_t t;
    xr_thread_create(&t, xr_handoff_thread_entry, m);
    xr_thread_detach(t);
}
