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
    XrMachine *m = (XrMachine *)xr_calloc(1, sizeof(XrMachine));
    if (!m) return NULL;
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

    // Initialize futex-based park state
    atomic_store(&m->park_state, XR_PARK_IDLE);
}

void xr_machine_destroy(XrMachine *m) {
    if (!m) return;

    // Free string buffer
    if (m->vm_ctx.tmp_strbuf) {
        xr_strbuf_free(m->vm_ctx.tmp_strbuf);
        m->vm_ctx.tmp_strbuf = NULL;
    }

    // Futex-based park: no resources to destroy
    (void)m;
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

XrMachine *xr_get_idle_m(struct XrRuntime *runtime) {
    if (!runtime) return NULL;

    pthread_mutex_lock(&runtime->sched_lock);
    XrMachine *m = runtime->idle_m_head;
    if (m) {
        runtime->idle_m_head = m->idle_link;
        m->idle_link = NULL;
        atomic_fetch_sub(&runtime->idle_m_count, 1);
    }
    pthread_mutex_unlock(&runtime->sched_lock);
    return m;
}

void xr_put_idle_m(struct XrRuntime *runtime, XrMachine *m) {
    if (!runtime || !m) return;

    atomic_store(&m->state, M_PARKED);

    pthread_mutex_lock(&runtime->sched_lock);
    m->idle_link = runtime->idle_m_head;
    runtime->idle_m_head = m;
    atomic_fetch_add(&runtime->idle_m_count, 1);
    pthread_mutex_unlock(&runtime->sched_lock);
}

// Start or wake an M to run P.
// If an idle M exists, start a handoff thread for it.
// Otherwise, allocate a new M and start its handoff thread.
void xr_startm(struct XrProc *p, bool spinning) {
    if (!p) return;
    struct XrRuntime *runtime = p->runtime;
    if (!runtime) return;

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
        pthread_t t;
        pthread_create(&t, NULL, xr_handoff_thread_entry, m);
        pthread_detach(t);
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
    pthread_t t;
    pthread_create(&t, NULL, xr_handoff_thread_entry, m);
    pthread_detach(t);
}
