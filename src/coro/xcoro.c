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
#include "../runtime/xisolate_api.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/xray_debug.h"
#include <string.h>
#include "xworker.h"
#include "xchannel.h"
#include "xchannel_ops.h"
#include "xtimer_wheel.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/object/xexception.h"
#include "xcoro_registry.h"
#include "xtask.h"
#include "xcoro_pool.h"
#include "xyieldable.h"
#include "xresult_group.h"
#include "xwork_queue.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xstring.h"

// Note: blocked queue moved to XrRuntime, see xworker.c

// ========== GC Safepoint ==========

// GC safepoint: runs a GC step and checks for cancellation.
// Returns 0 to continue, non-zero to request the coroutine to stop.
int xr_coro_heap_safepoint(XrCoroutine *coro) {
    if (!coro)
        return 0;

    // Reset reductions for next safepoint interval
    xr_coro_set_reds(coro, XR_CORO_REDUCTIONS);

    // Cancel check: watchdog sets this via xr_runtime_force_stop
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        return 1;
    }
    return 0;
}

bool xr_coro_reset_execution_state(XrCoroutine *coro, XrayIsolate *X) {
    const XrCoroBackendVTable *backend = coro ? coro->backend : NULL;
    if (!backend || !backend->reset_execution_state)
        return false;
    backend->reset_execution_state(coro, X);
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
    coro->hdr.type = XR_TCOROUTINE;
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

    xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED,
                       XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);

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
// Handles: flags, heap, backend execution storage, timer, GC fields, ID.
// need_stack: true for closure/cfunc coroutines, false for native callbacks.
// Returns false on allocation failure (heap/stack/frames cleaned up).
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
    xr_result_group_wait_token_reset(&ext->wait.result_group_token);
}

static void coro_wait_state_free(XrCoroExt *ext) {
    if (!ext)
        return;
    coro_wait_state_reset(ext);
    if (ext->wait.multi_await_token.heap_nodes) {
        xr_free(ext->wait.multi_await_token.heap_nodes);
    }
    ext->wait.multi_await_token.heap_nodes = NULL;
    ext->wait.multi_await_token.heap_capacity = 0;
}

static void coro_recv_slot_reset(XrCoroExt *ext) {
    if (!ext)
        return;
    ext->recv_slot = NULL;
    ext->recv_slot_ref = xr_slot_none();
    ext->chan_ok_slot_ref = xr_slot_none();
    ext->chan_resume_delivered = false;
}

static void coro_channel_wait_links_reset(XrRuntimeCore *core, XrCoroExt *ext) {
    if (!ext)
        return;
    atomic_store_explicit(&ext->wait_channel, NULL, memory_order_relaxed);
    ext->chan_wait_next = NULL;
    ext->chan_wait_prev = NULL;
    ext->chan_wait_queue = NULL;
    ext->wait_link = NULL;
    ext->wait_prev = NULL;
    ext->work_queue_hint = -1;
    ext->wait_bucket = NULL;
    ext->wait_bucket_owner = -1;
    ext->wait_send = false;
    /* Safety net for cancel/kill while blocked on send: a still-parked
     * value never reached a receiver, so release the channel-side
     * reference instead of orphaning the transit graph. */
    xr_chan_abandon_send_core(core, ext->send_value);
    ext->send_value = xr_null();
    ext->pending_spawn = NULL;
}

static void coro_channel_wait_reset(XrRuntimeCore *core, XrCoroExt *ext) {
    if (!ext)
        return;
    xr_channel_wait_token_reset(&ext->chan_wait_token);
    coro_channel_wait_links_reset(core, ext);
}

static void coro_timer_state_reset(XrCoroExt *ext) {
    if (!ext)
        return;
    atomic_store_explicit(&ext->timer_active, false, memory_order_relaxed);
    ext->timer.prev = NULL;
    ext->timer.next = NULL;
    atomic_store_explicit(&ext->timer.cancel_next, NULL, memory_order_relaxed);
    ext->timer.slot = XR_TW_SLOT_INACTIVE;
    ext->timer.timeout = NULL;
    ext->timer.arg = NULL;
    ext->timer.owner_worker_id = -1;
    atomic_store_explicit(&ext->timer.state, XR_TIMER_STATE_ACTIVE, memory_order_relaxed);
    ext->timer_wheel_owner = -1;
    atomic_store_explicit(&ext->timer_seq, 0, memory_order_relaxed);
}

static bool coro_timer_safe_for_recycle(XrWorker *worker, XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return true;

    XrCoroExt *ext = coro->ext;
    XrTWheelTimer *timer = &ext->timer;
    if (atomic_load_explicit(&ext->timer_active, memory_order_relaxed)) {
        xr_worker_cancel_timer(worker, coro);
    }

    XrTimerWheel *owner_tw = NULL;
    if (worker && worker->p.runtime && timer->owner_worker_id >= 0 &&
        timer->owner_worker_id < worker->p.runtime->worker_count) {
        owner_tw = worker->p.runtime->workers[timer->owner_worker_id].p.timer_wheel;
    }

    if (owner_tw && worker && timer->owner_worker_id == worker->p.id) {
        if (xr_timer_cancel_pending(owner_tw)) {
            (void) xr_timer_process_canceled_queue(owner_tw);
        }
        if (timer->slot != XR_TW_SLOT_INACTIVE) {
            xr_twheel_cancel_timer(owner_tw, timer);
        }
    }

    if (timer->slot != XR_TW_SLOT_INACTIVE)
        return false;
    if (atomic_load_explicit(&timer->state, memory_order_acquire) == XR_TIMER_STATE_ZOMBIE)
        return false;
    if (atomic_load_explicit(&timer->cancel_next, memory_order_acquire) != NULL)
        return false;
    return true;
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

static bool xr_coro_init_shell_owner(XrCoroutine *coro, XrayIsolate *X, XrRuntimeCore *core,
                                     XrRuntime *runtime, XrCoroState *sched, const char *name,
                                     bool need_storage) {
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
        struct XrCoroHeap *saved_heap = coro->heap;
        uint16_t saved_pool_bits =
            coro->gc_flags &
            (XR_CORO_GC_FROM_POOL | XR_CORO_GC_BACKEND_STATE_OWNED | XR_CORO_GC_LIGHTWEIGHT);
        XrCoroExt *saved_ext = coro->ext;

        memset((char *) coro + offsetof(XrCoroutine, flags), 0,
               sizeof(XrCoroutine) - offsetof(XrCoroutine, flags));

        coro->backend = saved_backend;
        coro->backend_state = saved_backend_state;
        coro->heap = saved_heap;
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
            coro_timer_state_reset(coro->ext);
            coro_channel_wait_reset(coro->core, coro->ext);
            coro_recv_slot_reset(coro->ext);
            coro_wait_state_reset(coro->ext);
            coro_select_storage_reset(coro->ext);
        }
    } else {
        // Consume the clean bit and drop per-lifetime bits, mirroring the
        // fresh path which restores only allocation-provenance bits. A stale
        // XR_CORO_GC_RECYCLABLE leaking from a previous lifetime (invoke /
        // fire-and-forget coro) would let the deferred-recycle path reset
        // this coroutine's heap while its Task result is still unread.
        coro->gc_flags &=
            (XR_CORO_GC_FROM_POOL | XR_CORO_GC_BACKEND_STATE_OWNED | XR_CORO_GC_LIGHTWEIGHT);
    }

    // Atomic fields
    atomic_store_explicit(&coro->flags, XR_CORO_FLG_READY, memory_order_relaxed);
    if (!is_clean) {
        // Fresh allocation: all atomic fields need explicit init
        atomic_store_explicit(&coro->resume_status, 0, memory_order_relaxed);
        atomic_store_explicit(&coro->affinity_p, 0, memory_order_relaxed);
    }
    // Clean path: recycle_local already zeroed all atomic fields via atomic_store

    // Set non-zero fields (always needed)
    xr_coro_set_reds(coro, XR_CORO_REDUCTIONS);

    // Runtime-managed: a coroutine's lifetime is owned by the scheduler/pool,
    // not the compiler's per-coroutine RC. Mark here (covers every alloc path:
    // pool slab init resets the gc header, so set the flag centrally). dup/drop
    // become no-ops for coroutine handles. See docs/design/706.
    XR_OBJ_SET_FLAG(&coro->hdr, XR_OBJ_MANAGED);
    coro->schedule_count = 1;
    coro->spawn_burst_count = 0;
    coro->core = core;
    coro->scheduler = runtime;
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
        if (!X)
            return false;
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
        } else if (runtime) {
            coro->id = xr_runtime_next_coro_id(runtime);
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

bool xr_coro_init_shell(XrCoroutine *coro, XrayIsolate *X, const char *name, bool need_storage) {
    if (!X)
        return false;
    return xr_coro_init_shell_owner(coro, X, xr_isolate_get_runtime_core(X),
                                    X->vm.scheduler ? (XrRuntime *) X->vm.scheduler : NULL,
                                    (XrCoroState *) X->vm.coro_state, name, need_storage);
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

XrCoroutine *xr_coro_create_runtime_empty(XrRuntimeCore *core, XrRuntime *runtime,
                                          const char *name) {
    if (!core)
        return NULL;

    XrCoroutine *coro = coro_alloc_lightweight_shell();
    if (!coro)
        return NULL;

    if (!xr_coro_init_shell_owner(coro, NULL, core, runtime, NULL, name, false)) {
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
    XrRuntime *runtime = (XrRuntime *) X->vm.scheduler;
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

    // Free coroutine heap — atomic exchange to prevent double-free race
    {
        XrCoroHeap *heap = atomic_exchange_explicit((_Atomic(XrCoroHeap *) *) &coro->heap, NULL,
                                                    memory_order_acq_rel);
        if (heap)
            xr_coro_heap_destroy(heap);
    }

    // Cold extension (io_buf, locals, watched_by)
    if (coro->ext) {
        if (coro->ext->io_buf)
            xr_free(coro->ext->io_buf);
        coro_wait_state_free(coro->ext);
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
    XR_DCHECK(!coro->heap || !coro->heap->is_collecting,
              "recycle_local: collector active during recycle");

    // Timer nodes are intrusive wheel entries. Reuse is safe only after the
    // owner wheel has physically unlinked any active/zombie node.
    if (!coro_timer_safe_for_recycle(worker, coro)) {
        coro->gc_flags &= ~XR_CORO_GC_RECYCLABLE;
        return;
    }
    xr_task_cancel_await_waiters(coro);
    if (coro->ext)
        xr_io_wait_token_cancel(&coro->ext->wait.io_token);

    // Reset coroutine heap: finalize objects, bulk free Region blocks, reset state.
    // Uses xr_coro_heap_reset which handles large objects and finalizers
    // correctly (the previous partial reset skipped those).
    if (coro->heap) {
        xr_coro_heap_reset(coro->heap, coro);
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
    atomic_store_explicit(&coro->current_scope, NULL, memory_order_relaxed);
    coro->sched_link = NULL;
    coro->next = NULL;
    coro->prev = NULL;
    coro->spawn_burst_count = 0;
    coro_channel_wait_reset(coro->core, coro->ext);
    coro_recv_slot_reset(coro->ext);
    coro_wait_state_reset(coro->ext);
    coro_select_storage_reset(coro->ext);
    coro_timer_state_reset(coro->ext);
    coro_clear_scope_membership(coro);
    (void) xr_coro_set_pending_spawn(coro, NULL);
    // Recycled shells take the clean path in xr_coro_init_shell, which skips
    // the dirty-path ext resets — so the per-lifetime ext fields must be
    // dropped here. locals lives in this coroutine's heap, which
    // xr_coro_heap_reset above just released: a stale pointer would be a UAF
    // for the next lifetime's Coro.setLocal.
    if (coro->ext) {
        coro->ext->locals = NULL;
        coro->ext->watched_by = NULL;
        atomic_store_explicit(&coro->ext->lock_count, 0, memory_order_relaxed);
        coro->ext->locked_worker = -1;
    }
    xr_coro_clear_debug_identity(coro);
    atomic_store_explicit(&coro->flags, 0, memory_order_relaxed);
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
        worker->p.stats.pool_local_put_count++;
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

XrCoroutine *xr_runtime_wake_channel(XrRuntime *runtime, void *channel, bool wake_sender) {
    if (!runtime || !channel)
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
void xr_runtime_wake_channel_all(XrRuntime *runtime, void *channel) {
    if (!runtime || !channel)
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
void xr_obj_destroy_coroutine(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    xr_coro_free((XrCoroutine *) obj);
}

static XrRuntime *coro_cancel_runtime(XrCoroutine *coro, XrWorker *current_worker) {
    if (current_worker && current_worker->p.runtime)
        return current_worker->p.runtime;
    return (XrRuntime *) xr_coro_scheduler(coro);
}

static bool coro_cancel_detach_channel_waiter(XrCoroutine *coro, XrWorker *current_worker) {
    if (!coro || !coro->ext)
        return true;

    XrCoroExt *ext = coro->ext;
    XrChannel *ch = (XrChannel *) atomic_load_explicit(&ext->wait_channel, memory_order_acquire);
    if (ch) {
        // Untimed waits were already unlinked by the arbitration step in
        // xr_coro_cancel; timed waits are unlinked here (their wakers go
        // through claim-wake, which bails once DONE is visible).
        (void) xr_channel_remove_waiter(ch, coro);
    }

    bool has_owner_bucket = ext->wait_bucket != NULL;
    if (!has_owner_bucket)
        return true;

    int owner_id = ext->wait_bucket_owner;
    if (current_worker && current_worker->p.id == owner_id) {
        xr_worker_unblock(current_worker, coro);
        return true;
    }

    XrRuntime *runtime = coro_cancel_runtime(coro, current_worker);
    if (runtime && owner_id >= 0 && owner_id < runtime->worker_count) {
        xr_worker_inbox_enqueue(runtime, owner_id, coro);
    }
    return false;
}

static bool coro_cancel_detach_select_waiter(XrCoroutine *coro, XrWorker *current_worker) {
    XrSelectWait *sw = xr_coro_select_wait(coro);
    if (!sw)
        return true;

    int owner_id = atomic_load_explicit(&coro->affinity_p, memory_order_acquire);
    if (current_worker && current_worker->p.id == owner_id) {
        xr_worker_unblock_select(current_worker, coro);
        xr_select_wait_cancel(sw);
        // Cancelled while blocked in select: the bytecode dispose at the select
        // merge never runs, so release the `after` timer channel here. See design/885.
        if (sw->timer_channel)
            xr_channel_timer_dispose((XrChannel *) sw->timer_channel);
        xr_coro_clear_select_wait(coro);
        return true;
    }

    XrRuntime *runtime = coro_cancel_runtime(coro, current_worker);
    if (runtime && owner_id >= 0 && owner_id < runtime->worker_count) {
        xr_worker_inbox_enqueue(runtime, owner_id, coro);
    }
    return false;
}

// Cancel coroutine
// Cancel logic:
// 1. Untimed channel waits: arbitrate ownership by unlinking under ch->lock
//    BEFORE the kill mark. The untimed wake fast path asserts exclusive
//    ownership after dequeue, so a cancel that loses the unlink race must
//    not force-kill — it degrades to cooperative CANCEL_REQUESTED and the
//    coroutine dies at its next resume safepoint.
// 2. Timed waits keep the original mark-first order: the timer fires without
//    holding ch->lock and its claim-wake bails once DONE is visible.
// 3. Cancel timer if sleeping (must happen before flags change)
// 4. Set CANCELLED and DONE flags, then clear blocked bookkeeping
void xr_coro_cancel(XrCoroutine *coro) {
    if (!coro || xr_coro_flags_has(coro, XR_CORO_FLG_DONE))
        return;

    xr_coro_flags_set(coro, XR_CORO_FLG_CANCEL_REQUESTED);

    bool timed = coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed);

    if (coro->ext && !timed) {
        XrChannel *wait_ch =
            (XrChannel *) atomic_load_explicit(&coro->ext->wait_channel, memory_order_acquire);
        if (wait_ch && !xr_channel_remove_waiter(wait_ch, coro)) {
            // A concurrent send/recv/close dequeued the waiter first and owns
            // its wake. Cooperative cancellation only.
            return;
        }
    }

    // Cancel timer if active (e.g. time.sleep)
    if (timed) {
        XrWorker *worker = xr_current_worker();
        if (worker) {
            xr_worker_cancel_timer(worker, coro);
        } else {
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
        }
    }

    // Set cancelled and done flags
    xr_coro_flags_set(coro, XR_CORO_FLG_CANCELLED | XR_CORO_FLG_DONE);
    xr_coro_flags_clear(coro, XR_CORO_FLG_CANCEL_REQUESTED | XR_CORO_FLG_BLOCKED |
                                  XR_CORO_FLG_RUNNING | XR_CORO_FLG_READY);

    // Clear blocked info
    XrCoroExt *cancel_ext = coro->ext;
    if (cancel_ext) {
        XrWorker *current_worker = xr_current_worker();
        bool channel_owned = coro_cancel_detach_channel_waiter(coro, current_worker);
        bool select_owned = coro_cancel_detach_select_waiter(coro, current_worker);
        /* Token cancels are CAS state machines designed for cross-thread
         * arbitration — safe even when the coro was handed to its owner
         * worker via the inbox above. */
        xr_task_cancel_await_waiters(coro);
        xr_await_wait_token_cancel(&cancel_ext->wait.await_token);
        xr_multi_await_wait_token_cancel(&cancel_ext->wait.multi_await_token);
        xr_scope_wait_token_cancel(&cancel_ext->wait.scope_token);
        xr_timer_wait_token_cancel(&cancel_ext->wait.timer_token);
        xr_io_wait_token_cancel(&cancel_ext->wait.io_token);
        xr_work_queue_cancel_waiter(coro);
        xr_result_group_cancel_waiter(coro);
        xr_channel_wait_token_cancel(&cancel_ext->chan_wait_token);
        /* Plain-field surgery requires full ownership: if either detach
         * handed the coro to its owner worker (inbox), that worker finishes
         * the blocked-side cleanup and touching the links here would race
         * its inbox drain. */
        if (channel_owned && select_owned)
            coro_channel_wait_links_reset(coro->core, cancel_ext);
    }
    /* No result write here: a cooperatively cancelled coro may still be
     * running on its owner worker, and nothing consumes a cancelled
     * coroutine's result slot. */
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

// xr_scheduler_ready - Wake coroutine on an explicit scheduler.
//
// Put coroutine into run queue. next=true uses the run-next slot for locality.
void xr_scheduler_ready(XrRuntime *runtime, XrCoroutine *gp, bool next) {
    if (!runtime || !gp)
        return;

    if (!runtime->workers || runtime->worker_count <= 0)
        return;

    XrWorker *worker = xr_current_worker();
    if (worker && worker->p.runtime != runtime)
        worker = NULL;

    // Atomically claim the BLOCKED -> READY transition; only the winner
    // enqueues. Prevents double-wake when multiple paths race on this coro.
    if (!xr_coro_claim_wake(gp)) {
        return;  // Already woken by another thread
    }

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

// xr_coro_ready - VM-facing wake wrapper.
void xr_coro_ready(XrayIsolate *X, XrCoroutine *gp, bool next) {
    if (!X)
        return;
    xr_scheduler_ready((XrRuntime *) X->vm.scheduler, gp, next);
}
