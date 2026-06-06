/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro.c - Coroutine shell lifecycle and scheduler-facing helpers
 *
 * KEY CONCEPT:
 *   XrCoroutine owns scheduler-visible state. Execution backends keep their
 *   stack/frame/resume payload behind backend_state. Scheduler dispatch uses
 *   the narrow coroutine ABI; VM-specific lifecycle services live in backend
 *   ops outside that ABI.
 */

#include "xcoroutine.h"
#include "../runtime/xisolate_internal.h"  // XrayIsolate definition
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/xray_debug.h"
#include <string.h>
#include <stdio.h>
#include "xworker.h"
#include "xchannel.h"
#include "xtimer_wheel.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/object/xexception.h"
#include "xcoro_registry.h"
#include "xtask.h"
#include "xcoro_pool.h"
#include "xyieldable.h"
#include "xwork_queue.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xstring.h"

// Note: blocked queue moved to XrRuntime, see xworker.c

// ========== JIT Integration ==========

// GC safepoint for JIT code: GC step + cancel check.
// Returns 0 to continue, non-zero to request deopt exit.
// Each backend's safepoint stub checks return value and jumps to deopt_stub
// if non-zero — only 1-2 extra instructions per platform.
int xr_coro_gc_safepoint(XrCoroutine *coro) {
    if (!coro)
        return 0;

    // Reset reductions for next safepoint interval
    coro->reductions = XR_CORO_REDUCTIONS;

    xr_coro_backend_on_safepoint(coro);

    // Cancel check: watchdog sets this via xr_runtime_force_stop
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        return 1;
    }
    return 0;
}

void xr_coro_backend_on_safepoint(XrCoroutine *coro) {
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->on_safepoint)
        return;
    backend->on_safepoint(coro);
}

void xr_coro_detach_worker_state(XrCoroutine *coro) {
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->detach_worker_state)
        return;
    backend->detach_worker_state(coro);
}

bool xr_coro_backend_in_try_mode(const XrCoroutine *coro) {
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->is_try_mode)
        return false;
    return backend->is_try_mode(coro);
}

bool xr_coro_reset_execution_state(XrCoroutine *coro, XrayIsolate *X) {
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->reset_execution_state)
        return false;
    backend->reset_execution_state(coro, X);
    return true;
}

// Forward write barrier for JIT: retired (RC owns reclamation, no tri-color
// invariant). Kept as a no-op so the JIT runtime-stub table symbol resolves.
void xr_jit_barrier_fwd(XrCoroutine *coro, void *parent, void *child) {
    (void) coro;
    (void) parent;
    (void) child;
}

// Back write barrier for JIT: retired. Kept as a no-op (see xr_jit_barrier_fwd).
void xr_jit_barrier_back(XrCoroutine *coro, void *container) {
    (void) coro;
    (void) container;
}

XrScopeContext *xr_coro_parent_scope(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->parent_scope : NULL;
}

bool xr_coro_set_parent_scope(XrCoroutine *coro, XrScopeContext *scope) {
    if (!coro)
        return false;
    if (!scope) {
        if (coro->ext)
            coro->ext->parent_scope = NULL;
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->parent_scope = scope;
    return true;
}

XrCoroutine *xr_coro_scope_sibling(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->scope_sibling : NULL;
}

bool xr_coro_set_scope_sibling(XrCoroutine *coro, XrCoroutine *sibling) {
    if (!coro)
        return false;
    if (!sibling) {
        if (coro->ext)
            coro->ext->scope_sibling = NULL;
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->scope_sibling = sibling;
    return true;
}

XrSelectWait *xr_coro_select_wait(XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return NULL;
    XrSelectWait *sw = &coro->ext->select_storage.wait;
    return atomic_load_explicit(&sw->active, memory_order_acquire) ? sw : NULL;
}

void xr_coro_clear_select_wait(XrCoroutine *coro) {
    XrSelectWait *sw = xr_coro_select_wait(coro);
    if (!sw)
        return;
    atomic_store_explicit(&sw->active, false, memory_order_release);
    sw->case_count = 0;
    sw->cases = NULL;
    sw->timer_channel = NULL;
    xr_timer_wait_token_finish(&coro->ext->wait.timer_token);
}

// ========== Coroutine Creation and Destruction ==========

void xr_coro_discard_uninitialized(XrCoroutine *coro) {
    if (!coro)
        return;
    if (coro->backend && coro->backend->destroy)
        coro->backend->destroy(coro);
    if (!(coro->gc_flags & XR_CORO_GC_FROM_POOL)) {
        xr_free(coro);
    }
}

static XrCoroutine *coro_alloc_lightweight_shell(void) {
    XrCoroutine *coro = (XrCoroutine *) xr_calloc(1, sizeof(XrCoroutine));
    if (!coro)
        return NULL;
    coro->gc.type = XR_TCOROUTINE;
    coro->gc_flags = XR_CORO_GC_LIGHTWEIGHT;
    return coro;
}

typedef struct XrNativeCoroState {
    void (*func)(void *);
    void *arg;
} XrNativeCoroState;

static XrNativeCoroState *native_state_from_coro(XrCoroutine *coro) {
    if (!coro || !coro->backend_state)
        return NULL;
    return (XrNativeCoroState *) coro->backend_state;
}

static void native_backend_release(XrCoroutine *coro) {
    if (!coro)
        return;
    xr_free(coro->backend_state);
    coro->backend_state = NULL;
    coro->backend = NULL;
}

static void coro_release_backend_state(XrCoroutine *coro, bool destroy) {
    if (!coro || !coro->backend)
        return;
    if (destroy && coro->backend->destroy) {
        coro->backend->destroy(coro);
    } else if (!destroy && coro->backend->release) {
        coro->backend->release(coro);
    }
}

static XrCoroRunResult native_backend_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                             const XrCoroRunContext *run_ctx) {
    (void) event;
    (void) run_ctx;
    XrNativeCoroState *state = native_state_from_coro(coro);
    if (!coro || !state || !state->func)
        return xr_coro_run_error(XR_NULL_VAL, false);
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED))
        return xr_coro_run_result(XR_CORO_RUN_CANCELLED);
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE))
        return xr_coro_run_done(coro->result);

    uint32_t flags = xr_coro_flags_load(coro);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&coro->flags,
                          (flags & ~(uint32_t) (XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                              XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                          memory_order_release);

    state->func(state->arg);
    coro->result = xr_null();
    return xr_coro_run_done(coro->result);
}

static const char *native_backend_debug_name(const XrCoroutine *coro) {
    (void) coro;
    return "native";
}

static void native_backend_debug_snapshot(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot) {
    (void) coro;
    if (!snapshot)
        return;
    snapshot->backend_name = "native";
    snapshot->function_name = "native";
    snapshot->frame_count = 0;
    snapshot->in_c_frame = 0;
}

static const XrCoroBackendVTable native_backend_vtable = {
    .kind = XR_CORO_BACKEND_NATIVE,
    .resume = native_backend_resume,
    .trace_roots = NULL,
    .prepare_recycle = NULL,
    .reset_reusable = NULL,
    .on_safepoint = NULL,
    .detach_worker_state = NULL,
    .is_try_mode = NULL,
    .setup_yield_continuation = NULL,
    .has_continuation = NULL,
    .call_closure = NULL,
    .ensure_state = NULL,
    .prepare_execution_state = NULL,
    .reset_execution_state = NULL,
    .clear_entry_state = NULL,
    .reset_entry_state_no_free = NULL,
    .bind_closure_entry = NULL,
    .bind_cfunc_entry = NULL,
    .release = native_backend_release,
    .destroy = native_backend_release,
    .debug_name = native_backend_debug_name,
    .debug_snapshot = native_backend_debug_snapshot,
};

// Common coroutine initialization after object allocation.
// Handles: flags, coro_gc, backend execution storage, timer, GC fields, ID.
// need_stack: true for closure/cfunc coroutines, false for native callbacks.
// Returns false on allocation failure (coro_gc/stack/frames cleaned up).
//
// Optimization: bulk memset instead of 47 individual field resets.
// XR_TAG_NULL == 0, so memset(0) automatically produces xr_null() for XrValue fields.
static void coro_select_storage_reset(XrCoroExt *ext) {
    if (!ext)
        return;
    XrSelectCase *heap_cases = ext->select_storage.heap_cases;
    int heap_capacity = ext->select_storage.heap_capacity;
    memset(&ext->select_storage, 0, sizeof(ext->select_storage));
    ext->select_storage.heap_cases = heap_cases;
    ext->select_storage.heap_capacity = heap_capacity;
}

static void coro_select_storage_free(XrCoroExt *ext) {
    if (!ext)
        return;
    if (ext->select_storage.heap_cases) {
        xr_free(ext->select_storage.heap_cases);
    }
    memset(&ext->select_storage, 0, sizeof(ext->select_storage));
}

static void coro_wait_state_reset(XrCoroExt *ext) {
    if (!ext)
        return;
    atomic_store_explicit(&ext->wait.await_task, NULL, memory_order_relaxed);
    atomic_store_explicit(&ext->wait.wait_count, 0, memory_order_relaxed);
    atomic_store_explicit(&ext->wait.any_done, false, memory_order_relaxed);
    xr_await_wait_token_reset(&ext->wait.await_token);
    xr_multi_await_wait_token_reset(&ext->wait.multi_await_token);
    xr_scope_wait_token_reset(&ext->wait.scope_token);
    xr_timer_wait_token_reset(&ext->wait.timer_token);
    xr_io_wait_token_reset(&ext->wait.io_token);
    xr_work_queue_wait_token_reset(&ext->wait.work_queue_token);
}

static void coro_recv_slot_reset(XrCoroExt *ext) {
    if (!ext)
        return;
    ext->recv_slot = NULL;
    ext->recv_slot_ref = xr_slot_none();
}

static void coro_channel_wait_reset(XrCoroExt *ext) {
    if (!ext)
        return;
    xr_channel_wait_token_reset(&ext->chan_wait_token);
    atomic_store_explicit(&ext->wait_channel, NULL, memory_order_relaxed);
    ext->chan_wait_next = NULL;
    ext->chan_wait_prev = NULL;
    ext->chan_wait_queue = NULL;
    ext->wait_link = NULL;
    ext->wait_prev = NULL;
    ext->wait_bucket = NULL;
    ext->wait_bucket_owner = -1;
    ext->wait_send = false;
    ext->send_value = xr_null();
    ext->pending_spawn = NULL;
}

bool xr_coro_set_pending_spawn(XrCoroutine *coro, XrCoroutine *child) {
    if (!coro)
        return false;
    if (!child) {
        if (coro->ext)
            coro->ext->pending_spawn = NULL;
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->pending_spawn = child;
    return true;
}

XrCoroutine *xr_coro_take_pending_spawn(XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return NULL;
    XrCoroutine *child = coro->ext->pending_spawn;
    coro->ext->pending_spawn = NULL;
    return child;
}

static void coro_clear_scope_membership(XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return;
    coro->ext->parent_scope = NULL;
    coro->ext->scope_sibling = NULL;
}

bool xr_coro_init_shell(XrCoroutine *coro, XrayIsolate *X, const char *name, bool need_storage) {
    XrCoroState *sched = (XrCoroState *) X->vm.coro_state;
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (need_storage && (!backend || !backend->prepare_execution_state))
        return false;

    // Check if coro was recycled with thorough cleanup (XR_CORO_GC_RECYCLED_CLEAN).
    // Recycled coros already have all fields zeroed by xr_coro_recycle_local,
    // so we skip the expensive shell memset and only set non-zero fields.
    // NOTE: must NOT use XR_CORO_GC_FROM_POOL here — that bit is set for ALL
    // pool-allocated coros including fresh uninitialized ones.
    bool is_clean = (coro->gc_flags & XR_CORO_GC_RECYCLED_CLEAN) != 0;

    if (!is_clean) {
        // Fresh allocation from pool/slab: bulk memset is faster than individual
        // field stores on ARM64 (vectorized stp instructions).
        // Save fields set by pool_get, memset the rest, then restore.
        const XrCoroBackendVTable *saved_backend = coro->backend;
        void *saved_backend_state = coro->backend_state;
        struct XrCoroGC *saved_coro_gc = coro->coro_gc;
        uint16_t saved_pool_bits =
            coro->gc_flags &
            (XR_CORO_GC_FROM_POOL | XR_CORO_GC_BACKEND_STATE_OWNED | XR_CORO_GC_LIGHTWEIGHT);
        XrCoroExt *saved_ext = coro->ext;

        memset((char *) coro + offsetof(XrCoroutine, flags), 0,
               sizeof(XrCoroutine) - offsetof(XrCoroutine, flags));

        coro->backend = saved_backend;
        coro->backend_state = saved_backend_state;
        coro->coro_gc = saved_coro_gc;
        coro->gc_flags = saved_pool_bits;
        coro->ext = saved_ext;
        // Reset ext fields that must not persist across lifetimes
        if (coro->ext) {
            coro->ext->locals = NULL;
            coro->ext->watched_by = NULL;
            coro_clear_scope_membership(coro);
            xr_coro_clear_debug_identity(coro);
            atomic_store_explicit(&coro->ext->lock_count, 0, memory_order_relaxed);
            coro->ext->locked_worker = -1;
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            coro->ext->timer.prev = NULL;
            coro->ext->timer.next = NULL;
            atomic_store_explicit(&coro->ext->timer.cancel_next, NULL, memory_order_relaxed);
            coro->ext->timer.slot = XR_TW_SLOT_INACTIVE;
            atomic_store_explicit(&coro->ext->timer.state, XR_TIMER_STATE_ACTIVE,
                                  memory_order_relaxed);
            coro->ext->timer_wheel_owner = -1;
            atomic_store_explicit(&coro->ext->timer_seq, 0, memory_order_relaxed);
            coro_channel_wait_reset(coro->ext);
            coro_recv_slot_reset(coro->ext);
            coro_wait_state_reset(coro->ext);
            coro_select_storage_reset(coro->ext);
        }
    } else {
        // Clear the clean bit (consumed)
        coro->gc_flags &= ~XR_CORO_GC_RECYCLED_CLEAN;
    }

    // Atomic fields
    atomic_store_explicit(&coro->flags, XR_CORO_FLG_READY | XR_CORO_PRIO_NORMAL,
                          memory_order_relaxed);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_READY, memory_order_relaxed);
    if (!is_clean) {
        // Fresh allocation: all atomic fields need explicit init
        atomic_store_explicit(&coro->resume_status, 0, memory_order_relaxed);
        atomic_store_explicit(&coro->affinity_p, 0, memory_order_relaxed);
    }
    // Clean path: recycle_local already zeroed all atomic fields via atomic_store

    // Set non-zero fields (always needed)
    coro->reductions = XR_CORO_REDUCTIONS;

    // Runtime-managed: a coroutine's lifetime is owned by the scheduler/pool,
    // not the compiler's per-coroutine RC. Mark here (covers every alloc path:
    // pool slab init resets the gc header, so set the flag centrally). dup/drop
    // become no-ops for coroutine handles. See docs/design/706.
    XR_OBJ_SET_FLAG(&coro->gc, XR_OBJ_MANAGED);
    coro->schedule_count = 1;
    coro->isolate = X;
    if (!xr_coro_set_name(coro, name))
        return false;
    if (!is_clean) {
        // Fresh allocation: set sentinel values (-1 means "not set")
        if (coro->ext)
            coro->ext->wait_bucket_owner = -1;
        // timer.slot/lock_count/locked_worker initialized lazily in ext when alloc'd
    }
    // Clean path: recycle_local already set these to their sentinel values

    // Cache worker pointer (single TLS lookup for VM stack slab + ID allocation)
    XrWorker *w = xr_current_worker();

    backend = coro->backend;
    if (backend && backend->prepare_execution_state) {
        if (!backend->prepare_execution_state(coro, X, w, need_storage, is_clean))
            return false;
    } else if (need_storage) {
        return false;
    }

    // Allocate ID (per-Worker batch cache to avoid atomic_fetch_add per spawn)
    {
        if (w && w->p.id_cache < w->p.id_cache_end) {
            coro->id = w->p.id_cache++;
        } else if (w && sched) {
            // Batch allocate 64 IDs
            int base = atomic_fetch_add(&sched->total_created, 64);
            w->p.id_cache = base + 1;
            w->p.id_cache_end = base + 64;
            coro->id = base;
        } else if (sched) {
            coro->id = atomic_fetch_add(&sched->total_created, 1);
        } else {
            static _Atomic int global_coro_id = 0;
            coro->id = atomic_fetch_add(&global_coro_id, 1);
        }
    }

    // Auto-register named coroutines
    if (name && sched && sched->coro_registry) {
        xr_coro_registry_register(sched->coro_registry, name, coro);
    }

    return true;
}

XrCoroutine *xr_coro_create_empty(XrayIsolate *X, const char *name) {
    XR_DCHECK(X != NULL, "coro_create_empty: NULL isolate");

    XrCoroutine *coro = coro_alloc_lightweight_shell();
    if (!coro)
        return NULL;

    if (!xr_coro_init_shell(coro, X, name, false)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }

    return coro;
}

// Create Native coroutine (C function callback, no Yieldable support)
// For simple callbacks without I/O wait
XrCoroutine *xr_coro_create_native(XrayIsolate *X, void (*func)(void *), void *arg,
                                   const char *name) {
    if (!X || !func)
        return NULL;

    XrCoroutine *coro = xr_coro_create_empty(X, name);
    if (!coro)
        return NULL;

    XrNativeCoroState *state = (XrNativeCoroState *) xr_calloc(1, sizeof(XrNativeCoroState));
    if (!state) {
        xr_coro_destroy(coro);
        return NULL;
    }
    state->func = func;
    state->arg = arg;

    xr_coro_attach_backend(coro, &native_backend_vtable, state);

    return coro;
}

// Native-stackful coroutine creation was removed; coroutines resume from VM state.

// Add coroutine to scheduler queue
void xr_coro_spawn(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro)
        return;

    // Use multi-core Runtime
    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
    if (runtime) {
        xr_runtime_spawn(runtime, coro);
    }
}

// Free coroutine internal resources.
// The owner frees or recycles the coroutine object shell after this returns.
void xr_coro_free(XrCoroutine *coro) {
    if (!coro)
        return;

    xr_task_cancel_await_waiters(coro);
    if (coro->ext)
        xr_io_wait_token_cancel(&coro->ext->wait.io_token);
    coro_release_backend_state(coro, true);

    // Free GC context — atomic exchange to prevent double-free race
    {
        XrCoroGC *gc = atomic_exchange_explicit((_Atomic(XrCoroGC *) *) &coro->coro_gc, NULL,
                                                memory_order_acq_rel);
        if (gc)
            xr_coro_gc_destroy(gc);
    }

    // Cold extension (io_buf, locals, watched_by)
    if (coro->ext) {
        if (coro->ext->io_buf)
            xr_free(coro->ext->io_buf);
        coro_select_storage_free(coro->ext);
        xr_free(coro->ext);
        coro->ext = NULL;
    }

    // Coroutine object shell is freed or pooled by the caller.
}

void xr_coro_destroy(XrCoroutine *coro) {
    if (!coro)
        return;
    bool free_shell = (coro->gc_flags & XR_CORO_GC_LIGHTWEIGHT) != 0;
    xr_coro_free(coro);
    if (free_shell)
        xr_free(coro);
}

// Recycle coroutine to Worker local pool (thread-safe, lock-free)
// Key optimization: keep memory, only reset state, avoid repeated malloc/free
void xr_coro_recycle_local(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro)
        return;
    XR_DCHECK(xr_coro_flags_has(coro, XR_CORO_FLG_DONE), "recycle_local: coro not done");
    XR_DCHECK(!coro->coro_gc || !coro->coro_gc->in_gc, "recycle_local: GC active during recycle");

    // Cancel timer using cross-worker cancellation
    // This handles both local (direct) and cross-worker (async queue) cancellation
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        xr_worker_cancel_timer(worker, coro);
        // Note: ext->timer_active is set to false inside xr_worker_cancel_timer
    }
    xr_task_cancel_await_waiters(coro);
    if (coro->ext)
        xr_io_wait_token_cancel(&coro->ext->wait.io_token);

    // Reset GC context: finalize objects, bulk free Immix blocks, reset state.
    // Uses xr_coro_gc_reset which handles large objects and finalizers
    // correctly (the previous partial reset skipped those).
    if (coro->coro_gc) {
        xr_coro_gc_reset(coro->coro_gc, coro);
    }
    if (!xr_coro_backend_prepare_recycle(coro, worker)) {
        xr_coro_destroy(coro);
        return;
    }

    // Thorough reset: zero all fields that xr_coro_init_shell would memset.
    // This allows xr_coro_init_shell to skip the full shell memset for recycled coros.
    coro->result = xr_null();
    coro->error = xr_null();
    coro->task = NULL;
    coro->current_scope = NULL;
    coro->sched_link = NULL;
    coro->next = NULL;
    coro->prev = NULL;
    coro_channel_wait_reset(coro->ext);
    coro_recv_slot_reset(coro->ext);
    coro_wait_state_reset(coro->ext);
    coro_select_storage_reset(coro->ext);
    coro_clear_scope_membership(coro);
    (void) xr_coro_set_pending_spawn(coro, NULL);
    // ext fields (lock_count, locked_worker, locals, watched_by)
    // are reset in xr_coro_init_shell dirty path; ext pointer preserved for io_buf reuse
    xr_coro_clear_debug_identity(coro);
    atomic_store_explicit(&coro->flags, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_NONE, memory_order_relaxed);
    atomic_store_explicit(&coro->resume_status, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->affinity_p, 0, memory_order_relaxed);
    // timer fields live in ext; reset happens in xr_coro_init_shell dirty path
    // Mark as "clean" — xr_coro_init_shell can skip memset
    coro->gc_flags |= XR_CORO_GC_RECYCLED_CLEAN;

    // Add to Worker local free list (lock-free, only this Worker accesses)
    if (worker->p.local_free_count < XR_CORO_LOCAL_FREE_MAX) {
        coro->next = worker->p.local_free_list;
        worker->p.local_free_list = coro;
        worker->p.local_free_count++;
    } else {
        // Local full: return to global pool via pool_put
        // (handles batch drain of local list + global free list addition)
        XrRuntime *runtime = worker->p.runtime;
        if (runtime) {
            xr_coro_pool_put(runtime, coro);
        }
    }
}

// ========== Scheduler Operations ==========

// Initialize scheduler
void xr_coro_state_init(XrCoroState *sched) {
    if (!sched)
        return;

    sched->total_created = 0;

    // Initialize scope
    sched->current_scope = NULL;

    // Initialize named coroutine registry
    sched->coro_registry = (XrCoroRegistry *) xr_malloc(sizeof(XrCoroRegistry));
    if (sched->coro_registry) {
        xr_coro_registry_init(sched->coro_registry);
    }
}

// Destroy scheduler
void xr_coro_state_destroy(XrCoroState *sched) {
    if (!sched)
        return;

    // Destroy named coroutine registry
    if (sched->coro_registry) {
        xr_coro_registry_destroy(sched->coro_registry);
        xr_free(sched->coro_registry);
        sched->coro_registry = NULL;
    }
}

// Note: xr_coro_save_context, xr_coro_restore_context, xr_coro_run removed.
// All coroutine execution goes through xr_coro_run_on_worker (zero-copy path).

// ========== Channel Wake (ownership-safe routing) ==========
//
// Design:
//   - Local worker: direct wake via xr_worker_wake_one / xr_worker_wake_select
//   - Remote workers: dispatch command via MPSC chan_wake_queue
//   - Never directly access remote worker's blocked buckets or run queues
//   - Uses channel waiter masks to skip workers with no relevant waiters

static bool runtime_dispatch_channel_wake_to_first(XrRuntime *runtime, uint64_t mask, void *channel,
                                                   bool wake_sender, bool is_close) {
    if (!runtime || !channel)
        return false;

    while (mask) {
        int wid = __builtin_ctzll(mask);
        mask &= mask - 1;
        if (wid >= runtime->worker_count)
            continue;

        xr_worker_dispatch_chan_wake(runtime, wid, channel, wake_sender, is_close);
        return true;
    }

    return false;
}

XrCoroutine *xr_runtime_wake_channel(XrayIsolate *X, void *channel, bool wake_sender) {
    if (!X || !channel)
        return NULL;

    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
    if (!runtime)
        return NULL;

    XrWorker *current = xr_current_worker();
    int current_id = current ? current->p.id : -1;

    // Step 1: Local worker — direct wake (owner-safe)
    if (current) {
        XrCoroutine *coro = xr_worker_wake_one(current, channel, wake_sender);
        if (coro)
            return coro;
        coro = xr_worker_wake_select(current, channel);
        if (coro)
            return coro;
    }

    // Step 2: Remote workers — dispatch via command queue (mask-guided)
    XrChannel *ch = (XrChannel *) channel;
    uint64_t any_mask = xr_channel_any_waiter_mask(ch);
    uint64_t preferred_mask = xr_channel_preferred_wake_mask(ch, wake_sender);
    uint64_t mask = preferred_mask ? preferred_mask : any_mask;
    uint64_t local_bit = xr_channel_worker_bit(current_id);
    // Clear local worker bit (already handled)
    mask &= ~local_bit;

    if (!runtime_dispatch_channel_wake_to_first(runtime, mask, channel, wake_sender, false) &&
        preferred_mask != 0) {
        uint64_t fallback_mask = any_mask & ~preferred_mask & ~local_bit;
        (void) runtime_dispatch_channel_wake_to_first(runtime, fallback_mask, channel, wake_sender,
                                                      false);
    }

    // Synchronous return is only possible for local wake.  Remote wakes
    // are asynchronous via command queue — callers that relied on the
    // return value for unbuffered rendezvous now use chan_direct_recv
    // which handles this case inside the channel lock.
    return NULL;
}

// Wake all coroutines waiting on Channel (for Channel close)
//
// xr_channel_close() already dequeues all normal waiters from ch->sendq/recvq
// and wakes them via channel_wake_coro_ex().  This function handles:
//   1. Select waiters (not in ch->sendq/recvq, only in blocked buckets)
//   2. Cleanup of stale blocked bucket entries for timer-based waiters
void xr_runtime_wake_channel_all(XrayIsolate *X, void *channel) {
    if (!X || !channel)
        return;

    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
    if (!runtime)
        return;

    XrWorker *current = xr_current_worker();
    int current_id = current ? current->p.id : -1;

    // Local worker: direct wake (owner-safe)
    if (current) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_close_local_worker_count);
        xr_worker_wake_all(current, channel);
        (void) xr_worker_wake_select_all_with_status(current, channel, XR_RESUME_CHANNEL_CLOSED);
    }

    // Remote workers: dispatch close commands via mask
    XrChannel *ch = (XrChannel *) channel;
    uint64_t mask = xr_channel_any_waiter_mask(ch);
    mask &= ~xr_channel_worker_bit(current_id);
    uint64_t remote_workers = 0;
    while (mask) {
        int wid = __builtin_ctzll(mask);
        mask &= mask - 1;
        if (wid >= runtime->worker_count)
            continue;

        xr_worker_dispatch_chan_wake(runtime, wid, channel, false, true);
        remote_workers++;
    }
    if (remote_workers > 0) {
        xr_sched_metric_add(runtime, &runtime->sched_stats.chan_close_remote_worker_count,
                            remote_workers);
    }

    // Clear the mask — channel is closed, no future waiters expected.
    xr_channel_clear_all_waiter_masks(ch);
}

// ========== GC Integration ==========

// GC destructor: free coroutine internal resources
void xr_gc_destroy_coroutine(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    xr_coro_free((XrCoroutine *) obj);
}

// Cancel coroutine
// Cancel logic:
// 1. Cancel timer if sleeping (must happen before flags change)
// 2. Set CANCELLED and DONE flags
// 3. Clear blocked state
void xr_coro_cancel(XrCoroutine *coro) {
    if (!coro || xr_coro_flags_has(coro, XR_CORO_FLG_DONE))
        return;

    // Cancel timer if active (e.g. time.sleep)
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        XrWorker *worker = xr_current_worker();
        if (worker) {
            xr_worker_cancel_timer(worker, coro);
        } else {
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        }
    }

    // Set cancelled and done flags
    xr_coro_flags_set(coro, XR_CORO_FLG_CANCELLED | XR_CORO_FLG_DONE);
    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING | XR_CORO_FLG_READY);

    // Clear blocked info
    if (coro->ext) {
        xr_task_cancel_await_waiters(coro);
        xr_await_wait_token_cancel(&coro->ext->wait.await_token);
        xr_multi_await_wait_token_cancel(&coro->ext->wait.multi_await_token);
        xr_scope_wait_token_cancel(&coro->ext->wait.scope_token);
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        xr_io_wait_token_cancel(&coro->ext->wait.io_token);
        xr_work_queue_cancel_waiter(coro);
        coro_channel_wait_reset(coro->ext);
    }
    coro->result = xr_null();
}

// ========== Scope Structured Concurrency ==========

// Add coroutine to current scope
//
// Per-coroutine scope tracking: prefer parent->current_scope,
// fallback to runtime/sched globals for main thread.
void xr_scope_add_coro(XrCoroState *sched, XrCoroutine *coro, XrCoroutine *parent) {
    if (!coro)
        return;

    XrScopeContext *scope = NULL;

    // Per-coroutine scope (preferred)
    if (parent) {
        scope = parent->current_scope;
    }

    // Fallback: Runtime global (main thread)
    if (!scope) {
        XrWorker *worker = xr_current_worker();
        if (worker && worker->p.runtime) {
            scope = worker->p.runtime->current_scope;
        }
    }

    // Fallback: Scheduler global
    if (!scope && sched) {
        scope = sched->current_scope;
    }

    if (!scope)
        return;  // Not in scope

    // Record belonging scope (decrement count on complete)
    if (!xr_coro_set_parent_scope(coro, scope))
        return;
    atomic_fetch_add(&scope->count, 1);
}

// ========== Multi-core Runtime Initialization ==========

// Initialize multi-core runtime
// @param X          Isolate instance
// @param num_workers Worker count (0 means auto-detect CPU cores)
//
// Multi-core parallel execution:
// - Each Worker thread executes backend-neutral coroutines
// - Work stealing for load balancing across Workers
// - VM backend coroutines have independent VM stacks/frames, no global VM lock
void xr_multicore_init(XrayIsolate *X, int num_workers) {
    if (!X)
        return;

    XrRuntime *runtime = xr_runtime_create(X, num_workers);
    if (runtime) {
        X->vm.runtime = runtime;
        X->vm.multicore_enabled = true;

        // Start Worker threads
        xr_runtime_start(runtime);
    }
}

// Destroy multi-core runtime
void xr_multicore_destroy(XrayIsolate *X) {
    if (!X || !X->vm.runtime)
        return;

    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;

    // Stop Runtime (if started)
    xr_runtime_stop(runtime);

    // Free resources
    xr_runtime_destroy(runtime);

    X->vm.runtime = NULL;
    X->vm.multicore_enabled = false;
}

// xr_current_coro - Get current coroutine
XrCoroutine *xr_current_coro(XrayIsolate *X) {
    if (!X)
        return NULL;

    XrWorker *worker = xr_current_worker();
    if (worker && worker->m) {
        XrCoroutine *c = atomic_load_explicit(&worker->m->current_coro, memory_order_relaxed);
        if (c)
            return c;
    }
    // Fallback: before VM starts, use main coroutine
    return X->main_coro;
}

// xr_coro_ready - Wake coroutine
//
// Put coroutine into run queue
// next=true puts into runnext for priority execution
void xr_coro_ready(XrayIsolate *X, XrCoroutine *gp, bool next) {
    if (!X || !gp)
        return;

    // Atomically claim the BLOCKED -> READY transition; only the winner
    // enqueues. Prevents double-wake when multiple paths race on this coro.
    if (!xr_coro_claim_wake(gp)) {
        return;  // Already woken by another thread
    }

    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
    if (!runtime)
        return;

    XrWorker *worker = xr_current_worker();
    if (worker && next) {
        // Thread-locked coro must go to locked worker, not current.
        if (xr_coro_is_thread_locked(gp) && gp->ext->locked_worker != worker->p.id) {
            int target_id = gp->ext->locked_worker;
            if (target_id >= 0 && target_id < runtime->worker_count) {
                xr_worker_inbox_enqueue(runtime, target_id, gp);
                xr_runtime_wake_idle_worker(runtime);
                return;
            }
        }
        // LIFO slot: woken coroutine runs immediately on current worker (DFS style)
        xr_worker_push_lifo(worker, gp);
    } else {
        // No current worker or next=false: send to target worker's inbox.
        // Respects Coro.lockThread(): locked coros return to their locked worker.
        int target_id = xr_coro_wake_target_id(gp);
        if (target_id < 0 || target_id >= runtime->worker_count) {
            target_id = 0;
        }
        xr_worker_inbox_enqueue(runtime, target_id, gp);
        // Ensure a spinner exists to discover the inbox entry
        if (atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed) == 0) {
            xr_runtime_wake_idle_worker(runtime);
        }
        return;
    }

    // Wake one idle worker
    xr_runtime_wake_idle_worker(runtime);
}

/* ========== Scope Child List — Lock Helpers ==========
 *
 * scope->child_lock is a spin-lock guarding three pieces of state:
 *
 *   - scope->first_child / child's scope sibling    (the child list)
 *   - scope->cancel_requested                       (linked-mode latch)
 *   - child parent-scope clearing during sibling cancel
 *
 * Hold time is bounded (a single list mutation or a small sibling
 * walk); callers must not perform GC-triggering allocations or call
 * back into the scheduler while holding it. */
static inline void scope_lock_acquire(XrScopeContext *scope) {
    while (atomic_exchange_explicit(&scope->child_lock, true, memory_order_acquire)) {
    }
}

static inline void scope_lock_release(XrScopeContext *scope) {
    atomic_store_explicit(&scope->child_lock, false, memory_order_release);
}

/* ========== Scope Completion Wake Helpers ==========
 *
 * Scope completion is split from task await wake so each helper owns a
 * single concurrency surface:
 *
 * - policy state update and child-list unlink run under scope->child_lock;
 * - scope count decrement and owner wake run after releasing the lock;
 * - per-task await/listener wake stays delegated to xr_task_wake_waiter.
 */

/* Record this child's error and update scope->first_error / scope->errors
 * per policy mode. Caller must hold scope->child_lock so concurrent failing
 * children cannot race on first_error or errors[] updates.
 *
 * task->error may be either an Exception instance or a fallback string for
 * non-exception failures. Linked scope preserves the value as-is; supervisor
 * scope flattens to message strings to honor its Array<string> contract. */
static bool wake_waiter_record_child_error_locked(XrCoroutine *coro, XrScopeContext *scope) {
    /* coro->task is cleared by the scheduler when a sibling worker
     * recycles the coroutine slot, and our caller's scope->child_lock
     * does not pin that field. Loading the pointer once into a local
     * is therefore necessary for correctness, not defensive: a second
     * deref would re-read the field and UBSan flags the resulting NULL
     * access on the LINKED-cancel race.
     */
    XrTask *task = coro->task;
    if (scope->mode == XR_SCOPE_WAIT || !task)
        return false;
    XrValue err = task->error;
    if (XR_IS_NULL(err))
        return false;

    if (scope->mode == XR_SCOPE_LINKED) {
        // linked scope: deterministic "first failure wins" under the lock.
        if (XR_IS_NULL(scope->first_error)) {
            scope->first_error = err;
            scope->first_error_is_value = coro->error_is_value;
        }
    } else if (scope->mode == XR_SCOPE_SUPERVISOR) {
        // supervisor scope: append every error; errors[] is preallocated.
        // Spec contract is Array<string>, so extract the message when the
        // error is an Exception instance.
        if (scope->errors) {
            XrValue msg = err;
            XrayIsolate *iso = coro->isolate;
            if (iso && xr_value_is_exception(iso, err)) {
                const char *m = xr_exception_get_message(iso, err);
                if (!m)
                    m = "";
                XrString *s = xr_string_intern(iso, m, strlen(m), 0);
                msg = xr_string_value(s);
            }
            xr_array_push(scope->errors, msg);
        }
    }
    return true;
}

/* Unlink coro from scope->first_child. Caller must hold scope->child_lock.
 * This prevents recycled coroutine slots from remaining in the child list. */
static void wake_waiter_unlink_from_scope_locked(XrCoroutine *coro, XrScopeContext *scope) {
    XrCoroutine *prev = NULL;
    XrCoroutine *cur = scope->first_child;
    while (cur) {
        XrCoroutine *next = xr_coro_scope_sibling(cur);
        if (cur == coro) {
            if (prev) {
                (void) xr_coro_set_scope_sibling(prev, next);
            } else {
                scope->first_child = next;
            }
            xr_coro_set_scope_sibling(coro, NULL);
            return;
        }
        prev = cur;
        cur = next;
    }
}

/* Linked-mode failure propagation. Caller must hold scope->child_lock and
 * must have already unlinked the failing child, so the walk only visits
 * still-live siblings. Each sibling receives a cooperative cancel request. */
static void wake_waiter_cancel_linked_siblings_locked(XrScopeContext *scope) {
    for (XrCoroutine *sib = scope->first_child; sib; sib = xr_coro_scope_sibling(sib)) {
        if (xr_coro_flags_has(sib, XR_CORO_FLG_DONE))
            continue;
        xr_coro_flags_set(sib, XR_CORO_FLG_CANCEL_REQUESTED);
        xr_coro_request_yield(sib);
    }
}

static bool wake_waiter_scope_owner_ready(const XrScopeContext *scope, const XrCoroutine *owner) {
    if (!scope || !owner || owner->current_scope != scope)
        return false;
    if (xr_coro_get_wait_reason(xr_coro_flags_load(owner)) !=
        (XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT)) {
        return false;
    }
    return xr_coro_flags_has(owner, XR_CORO_FLG_BLOCKED);
}

static void wake_waiter_finish_scope_completion(XrayIsolate *X, XrCoroutine *coro,
                                                XrScopeContext *scope) {
    XrCoroutine *owner = scope->owner;
    bool owner_waiting_scope = wake_waiter_scope_owner_ready(scope, owner);
    int remaining = atomic_fetch_sub(&scope->count, 1) - 1;
    xr_coro_set_parent_scope(coro, NULL);
    if (remaining == 0 && owner_waiting_scope) {
        XrCoroWaitState *wait = xr_coro_wait_state(owner);
        if (wait)
            xr_scope_wait_token_resolve(&wait->scope_token);
        xr_coro_ready(X, owner, true);
    }
}

static void wake_waiter_handle_scope_completion(XrayIsolate *X, XrCoroutine *coro,
                                                XrScopeContext *scope) {
    scope_lock_acquire(scope);
    bool child_failed = wake_waiter_record_child_error_locked(coro, scope);
    wake_waiter_unlink_from_scope_locked(coro, scope);
    if (child_failed && scope->mode == XR_SCOPE_LINKED &&
        !atomic_exchange(&scope->cancel_requested, true)) {
        wake_waiter_cancel_linked_siblings_locked(scope);
    }
    scope_lock_release(scope);

    wake_waiter_finish_scope_completion(X, coro, scope);
}

static void wake_waiter_notify_task(XrayIsolate *X, XrCoroutine *coro) {
    if (coro->task) {
        xr_task_wake_waiter(X, coro->task);
    }
}

// xr_coro_wake_waiter - Wake waiter when coroutine completes.
//
// All await coordination lives on XrTask. This function handles the
// scope-side bookkeeping (which is orthogonal to the task tree, see
// the doc comments on XrScopeContext / XrTask) and then delegates to
// xr_task_wake_waiter for the per-task await/listener path.
void xr_coro_wake_waiter(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro)
        return;

    XrScopeContext *scope = xr_coro_parent_scope(coro);
    if (scope) {
        wake_waiter_handle_scope_completion(X, coro, scope);
    }

    wake_waiter_notify_task(X, coro);
}
