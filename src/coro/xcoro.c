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
 *   stack/frame/resume payload behind backend_state and expose lifecycle hooks
 *   through XrCoroBackendVTable.
 */

#include "xcoroutine.h"
#include "../runtime/xisolate_api.h"       // xr_runtime_error
#include "../runtime/xisolate_internal.h"  // XrayIsolate definition
#include "../runtime/gc/xgc.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/xray_debug.h"
#include <string.h>
#include <stdio.h>
#include "xworker.h"
#include "xchannel.h"
#include "xtimer_wheel.h"
#include "../runtime/gc/xcoro_gc.h"
#include "xdeep_copy.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xexception.h"
#include "xcoro_registry.h"
#include "xtask.h"
#include "xcoro_pool.h"
#include "xyieldable.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xstring.h"

// Initial capacities (balanced: fast allocation + minimal grow_stack for common cases)
// 64 VM slots = 1024B VM stack + 288B frames = 1312B total per coroutine
#define INITIAL_STACK_CAPACITY XR_CORO_POOL_STACK_SLOTS
#define INITIAL_FRAME_CAPACITY XR_CORO_POOL_FRAME_SLOTS

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

    // Bump worker heartbeat so sysmon doesn't misdetect long-running
    // JIT code as stuck (JIT stays inside run_on_worker across many
    // C helper calls like go/await without returning to worker loop)
    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (jit_state && jit_state->scratch && jit_state->scratch->heartbeat_ptr) {
        atomic_fetch_add_explicit(jit_state->scratch->heartbeat_ptr, 1, memory_order_relaxed);
    }

    // Cancel check: watchdog sets this via xr_runtime_force_stop
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        return 1;
    }
    return 0;
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
}

// ========== Memory Sync Helper Functions ==========

// Reset vm_ctx execution state (stack/frames pointers already set during allocation)
void xr_coro_sync_vm_ctx(XrCoroutine *coro, XrayIsolate *X) {
    if (!coro)
        return;

    // Targeted field resets instead of memset (avoids zeroing preserved pointers)
    XrVMContext *ctx = xr_coro_vm_ctx(coro);
    ctx->stack_top = ctx->stack;
    ctx->frame_count = 0;
    ctx->module_base_frame = 0;
    ctx->handlers = ctx->handler_inline;
    ctx->handler_count = 0;
    ctx->handler_capacity = XR_HANDLER_INLINE_CAP;
    ctx->current_exception = xr_null();
    ctx->pending_error = xr_null();
    ctx->current_coro = coro;
    ctx->instruction_count = 0;
    ctx->preempt_pending = false;
    ctx->last_nret = 0;
    ctx->defer_count = 0;
    ctx->trace_execution = false;
    ctx->isolate = X;
}

// Upgrade coroutine heap (for main coroutine)
// Replace small heap with large heap for deep recursion
bool xr_coro_upgrade_heap(XrCoroutine *coro, size_t size) {
    if (!coro)
        return false;
    // Arena GC: arena grows automatically, no explicit upgrade needed
    (void) size;
    return true;
}

// ========== Coroutine Creation and Destruction ==========

// Forward declaration (defined after bootstrap)
static bool coro_init_common(XrCoroutine *coro, XrayIsolate *X, const char *name, bool need_stack);

static bool coro_ensure_vm_state(XrCoroutine *coro) {
    if (!coro)
        return false;
    if (xr_coro_maybe_vm_state(coro))
        return true;
    XrVmCoroState *vm_state = (XrVmCoroState *) xr_calloc(1, sizeof(XrVmCoroState));
    if (!vm_state)
        return false;
    vm_state->ctx.handlers = vm_state->ctx.handler_inline;
    vm_state->ctx.handler_capacity = XR_HANDLER_INLINE_CAP;
    coro->backend = xr_coro_vm_backend_vtable();
    coro->backend_state = vm_state;
    coro->gc_flags |= XR_CORO_GC_VM_STATE_OWNED;
    return true;
}

XrJitCoroState *xr_coro_ensure_jit_state(XrCoroutine *coro) {
    if (!coro)
        return NULL;
    if (!coro_ensure_vm_state(coro))
        return NULL;
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    if (!vm_state)
        return NULL;
    if (vm_state->jit_state)
        return vm_state->jit_state;
    vm_state->jit_state = (XrJitCoroState *) xr_calloc(1, sizeof(XrJitCoroState));
    return vm_state->jit_state;
}

XrJitCoroState *xr_coro_prepare_jit_state(XrCoroutine *coro) {
    XrJitCoroState *jit_state = xr_coro_ensure_jit_state(coro);
    if (!jit_state)
        return NULL;
    XrWorker *worker = xr_current_worker();
    if (worker) {
        jit_state->scratch = &worker->p.jit_scratch;
        worker->p.jit_scratch.heartbeat_ptr = &worker->m->heartbeat;
    }
    return jit_state->scratch ? jit_state : NULL;
}

void xr_coro_reset_jit_state(XrCoroutine *coro) {
    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (!jit_state)
        return;
    XrJitSuspendState *suspend = jit_state->suspend;
    memset(jit_state, 0, sizeof(*jit_state));
    jit_state->suspend = suspend;
}

void xr_coro_free_jit_state(XrCoroutine *coro) {
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    if (!vm_state || !vm_state->jit_state)
        return;
    XrJitCoroState *jit_state = vm_state->jit_state;
    if (jit_state->suspend) {
        xr_free(jit_state->suspend);
    }
    xr_free(jit_state);
    vm_state->jit_state = NULL;
}

static void coro_free_owned_vm_state(XrCoroutine *coro) {
    if (!coro || !(coro->gc_flags & XR_CORO_GC_VM_STATE_OWNED))
        return;
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    xr_free(vm_state);
    coro->backend_state = NULL;
    coro->backend = NULL;
    coro->gc_flags &= ~XR_CORO_GC_VM_STATE_OWNED;
}

static void coro_vm_entry_reset_no_free(XrVmCoroState *vm_state) {
    if (!vm_state)
        return;
    vm_state->entry_type = XR_CORO_ENTRY_CLOSURE;
    vm_state->entry.closure = NULL;
    vm_state->args = NULL;
    vm_state->arg_count = 0;
    for (int i = 0; i < 4; i++)
        vm_state->inline_args[i] = xr_null();
}

void xr_coro_clear_vm_entry_state(XrCoroutine *coro) {
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    if (!vm_state)
        return;
    if (vm_state->args && vm_state->args != vm_state->inline_args)
        xr_free(vm_state->args);
    coro_vm_entry_reset_no_free(vm_state);
}

static void coro_discard_uninitialized(XrCoroutine *coro) {
    if (!coro)
        return;
    xr_coro_free_jit_state(coro);
    xr_coro_clear_vm_entry_state(coro);
    coro_free_owned_vm_state(coro);
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
    .release = native_backend_release,
    .destroy = native_backend_release,
    .debug_name = native_backend_debug_name,
    .debug_snapshot = native_backend_debug_snapshot,
};

// Create bootstrap main coroutine (no closure, no scheduler)
// Called during isolate init before any script execution.
// Provides coro_gc so all init-phase allocations go through coro heap.
// Later upgraded by xr_coro_setup_main() when proto is ready.
XrCoroutine *xr_coro_create_bootstrap(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "coro_create_bootstrap: NULL isolate");
    XrCoroutine *coro = NULL;

    if (X->sys_heap) {
        coro = xr_sysheap_alloc_coro(X->sys_heap);
    } else {
        coro = (XrCoroutine *) xr_malloc(sizeof(XrCoroutine));
        if (coro) {
            memset(coro, 0, sizeof(XrCoroutine));
            coro->gc.type = XR_TCOROUTINE;
        }
    }
    if (!coro)
        return NULL;
    if (!coro_ensure_vm_state(coro)) {
        coro_discard_uninitialized(coro);
        return NULL;
    }

    // New object: ensure NULL pointers for coro_init_common
    xr_coro_vm_ctx(coro)->stack = NULL;
    xr_coro_vm_ctx(coro)->frames = NULL;
    coro->coro_gc = NULL;

    // Common initialization (flags, stack/frames, field resets, timer, GC fields, vm_ctx sync, ID)
    if (!coro_init_common(coro, X, "main", true)) {
        xr_coro_free(coro);
        coro_discard_uninitialized(coro);
        return NULL;
    }

    // Main coroutine needs coro_gc immediately (GC API, init-phase allocations)
    // Use main-specific config: larger threshold, slower GC (long-lived coroutine)
    if (!coro->coro_gc) {
        XrCoroGCConfig main_config = {
            .gc_threshold = XR_MAIN_CORO_GC_THRESHOLD,
            .gc_pause = XR_MAIN_CORO_GC_PAUSE,
            .gc_stepmul = XR_MAIN_CORO_GC_STEPMUL,
        };
        coro->coro_gc = xr_coro_gc_create(coro, &main_config);
        if (!coro->coro_gc)
            return NULL;
    }

    // Bootstrap-specific: mark as main coroutine
    coro->flags |= XR_CORO_FLG_MAIN;
    coro_vm_entry_reset_no_free(xr_coro_maybe_vm_state(coro));
    (void) xr_coro_set_pending_spawn(coro, NULL);

    return coro;
}

// Setup bootstrap main_coro for script execution (called from xr_execute)
// Upgrades the bootstrap coro with closure and proto, ready for VM run.
void xr_coro_setup_main(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure) {
    XR_DCHECK(coro != NULL, "coro_setup_main: NULL coro");
    XR_DCHECK(X != NULL, "coro_setup_main: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_setup_main: NULL closure");
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    XR_DCHECK(vm_state != NULL, "coro_setup_main: missing VM state");
    vm_state->entry_type = XR_CORO_ENTRY_CLOSURE;
    vm_state->entry.closure = closure;
    (void) xr_coro_set_source(coro, closure->proto ? closure->proto->source_file : NULL, 0);
    xr_coro_upgrade_heap(coro, 0);
    xr_coro_sync_vm_ctx(coro, X);
}

// Reset main_coro for sequential re-execution (test runner, REPL).
// Reuses existing stack/frames/GC heap, only resets execution state.
//
// WHY THIS DESIGN:
//   Between sequential calls (e.g. @test functions), the GC heap accumulates
//   dead objects from previous executions. A fullgc before reset ensures:
//   1. Dead objects are properly collected (stack references are about to vanish)
//   2. gc_disabled counter is reset (previous call may have left GC disabled)
//   3. GC state machine is in PAUSE (clean slate for next execution)
void xr_coro_reset_for_call(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure) {
    XR_DCHECK(coro != NULL, "coro_reset_for_call: NULL coro");
    XR_DCHECK(X != NULL, "coro_reset_for_call: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_reset_for_call: NULL closure");

    // Reset gc_disabled counter: previous call may have done gc.disable()
    // without gc.enable(). Leave gcstate alone — GC manages its own transitions.
    if (coro->coro_gc) {
        coro->coro_gc->gc_disabled = 0;
    }

    // Reset VM execution state (stack_top, frames, handlers, etc.)
    xr_coro_sync_vm_ctx(coro, X);

    // Set new entry closure
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    XR_DCHECK(vm_state != NULL, "coro_reset_for_call: missing VM state");
    xr_coro_clear_vm_entry_state(coro);
    vm_state->entry_type = XR_CORO_ENTRY_CLOSURE;
    vm_state->entry.closure = closure;
    (void) xr_coro_set_source(coro, closure->proto ? closure->proto->source_file : NULL, 0);

    // Clear result/error from previous execution
    coro->result = xr_null();
    coro->error = xr_null();
    coro->current_scope = NULL;
}

// Common coroutine initialization after object allocation.
// Handles: flags, coro_gc, stack/frames, field resets, timer, GC fields, vm_ctx sync, ID.
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

static bool coro_init_common(XrCoroutine *coro, XrayIsolate *X, const char *name, bool need_stack) {
    XrCoroState *sched = (XrCoroState *) X->vm.coro_state;
    if (need_stack && !coro_ensure_vm_state(coro))
        return false;
    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    XrVMContext *vm_ctx = vm_state ? &vm_state->ctx : NULL;

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
        XrVmCoroState *saved_vm_state = vm_state;
        XrValue *saved_stack = vm_ctx ? vm_ctx->stack : NULL;
        int saved_stack_cap = vm_ctx ? vm_ctx->stack_capacity : 0;
        XrBcCallFrame *saved_frames = vm_ctx ? vm_ctx->frames : NULL;
        int saved_frame_cap = vm_ctx ? vm_ctx->frame_capacity : 0;
        XrExceptionHandler *saved_handlers = vm_ctx ? vm_ctx->handlers : NULL;
        int saved_handler_cap = vm_ctx ? vm_ctx->handler_capacity : 0;
        struct XrCoroGC *saved_coro_gc = coro->coro_gc;
        uint16_t saved_pool_bits =
            coro->gc_flags & (XR_CORO_GC_SLAB_STACK | XR_CORO_GC_FROM_POOL |
                              XR_CORO_GC_VM_STATE_OWNED | XR_CORO_GC_LIGHTWEIGHT);
        XrCoroExt *saved_ext = coro->ext;

        memset((char *) coro + offsetof(XrCoroutine, flags), 0,
               sizeof(XrCoroutine) - offsetof(XrCoroutine, flags));

        if (saved_vm_state) {
            coro->backend = xr_coro_vm_backend_vtable();
            coro->backend_state = saved_vm_state;
        }
        vm_ctx = saved_vm_state ? &saved_vm_state->ctx : NULL;
        if (vm_ctx) {
            memset(vm_ctx, 0, sizeof(*vm_ctx));
            vm_ctx->stack = saved_stack;
            vm_ctx->stack_capacity = saved_stack_cap;
            vm_ctx->frames = saved_frames;
            vm_ctx->frame_capacity = saved_frame_cap;
            vm_ctx->handlers = saved_handlers;
            vm_ctx->handler_capacity = saved_handler_cap;
        }
        coro_vm_entry_reset_no_free(saved_vm_state);
        coro->coro_gc = saved_coro_gc;
        coro->gc_flags = saved_pool_bits;
        coro->ext = saved_ext;
        xr_coro_reset_jit_state(coro);
        // Reset ext fields that must not persist across lifetimes
        if (coro->ext) {
            coro->ext->locals = NULL;
            coro->ext->watched_by = NULL;
            coro_clear_scope_membership(coro);
            coro->ext->yield_info.wait_fd = 0;
            coro->ext->yield_info.wait_events = 0;
            coro->ext->yield_info.result_events = 0;
            coro->ext->yield_info.deadline = 0;
            coro->ext->yield_info.timed_out = false;
            xr_coro_clear_debug_identity(coro);
            atomic_store_explicit(&coro->ext->lock_count, 0, memory_order_relaxed);
            coro->ext->locked_worker = -1;
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            coro->ext->timer.slot = XR_TW_SLOT_INACTIVE;
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
    coro->backend = vm_ctx ? xr_coro_vm_backend_vtable() : NULL;
    coro->backend_state = vm_ctx ? vm_state : NULL;
    if (!is_clean) {
        // Fresh allocation: set sentinel values (-1 means "not set")
        if (coro->ext)
            coro->ext->wait_bucket_owner = -1;
        // timer.slot/lock_count/locked_worker initialized lazily in ext when alloc'd
    }
    // Clean path: recycle_local already set these to their sentinel values

    // Cache worker pointer (single TLS lookup for VM stack slab + ID allocation)
    XrWorker *w = xr_current_worker();

    // Allocate VM stack and bytecode frames if needed.
    // coro_gc is created lazily on first heap allocation.
    if (need_stack) {
        if (!vm_ctx)
            return false;
        if (!vm_ctx->stack) {
            size_t stack_bytes = sizeof(XrValue) * INITIAL_STACK_CAPACITY;
            size_t frames_bytes = sizeof(XrBcCallFrame) * INITIAL_FRAME_CAPACITY;
            char *block = NULL;
            // Try per-Worker VM stack slab free list first (lock-free, no malloc)
            if (w && w->p.stack_slab_free) {
                block = (char *) w->p.stack_slab_free;
                w->p.stack_slab_free = *(void **) block;
                w->p.stack_slab_count--;
            } else {
                block = (char *) xr_malloc(stack_bytes + frames_bytes);
            }
            if (!block) {
                return false;
            }
            vm_ctx->stack = (XrValue *) block;
            vm_ctx->stack_capacity = INITIAL_STACK_CAPACITY;
            vm_ctx->frames = (XrBcCallFrame *) (block + stack_bytes);
            vm_ctx->frame_capacity = INITIAL_FRAME_CAPACITY;
            memset(block, 0, stack_bytes + frames_bytes);
        }
        if (!is_clean) {
            // Slab-free-list or pool path: stack pointer already set but
            // contents may be stale. Conservative GC scanning in
            // mark_coro_roots walks every slot up to proto->maxstacksize
            // (not stack_top), so all unwritten slots must be zeroed.
            size_t total = sizeof(XrValue) * (size_t) vm_ctx->stack_capacity +
                           sizeof(XrBcCallFrame) * (size_t) vm_ctx->frame_capacity;
            memset(vm_ctx->stack, 0, total);
        }
    }

    // Inline vm_ctx sync
    if (vm_ctx && !is_clean) {
        // Fresh allocation: full sync needed
        vm_ctx->stack_top = vm_ctx->stack;
        vm_ctx->isolate = X;
    }
    // stack_top already reset by recycle_local for clean coros
    if (vm_ctx)
        vm_ctx->current_coro = coro;

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

// Create VM coroutine (GC managed)
// Optimization: try to get stack and frames from pool
XrCoroutine *xr_coro_create(XrayIsolate *X, XrClosure *closure, XrValue *args, int arg_count,
                            const char *name, const char *file, int line) {
    XR_DCHECK(X != NULL, "coro_create: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_create: NULL closure");
    XR_DCHECK(arg_count >= 0, "coro_create: negative arg_count");
    XR_DCHECK(arg_count == 0 || args != NULL, "coro_create: NULL args with count > 0");
    // Check coroutine limit
    XrCoroState *sched = (XrCoroState *) X->vm.coro_state;
    if (sched && sched->coro_count >= XR_MAX_COROUTINES) {
        xr_runtime_error(X, "coroutine limit exceeded (%d)", XR_MAX_COROUTINES);
        return NULL;
    }

    // Allocate: try pool first, then system heap
    XrCoroutine *coro = NULL;
    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
    if (runtime) {
        coro = xr_coro_pool_get(runtime);
    }
    if (!coro) {
        if (X->sys_heap) {
            coro = xr_sysheap_alloc_coro(X->sys_heap);
            // sysheap pool may have pre-set stack/frames from arena slab — keep them
            if (!coro)
                return NULL;
            coro->coro_gc = NULL;
        } else {
            coro = (XrCoroutine *) xr_malloc(sizeof(XrCoroutine));
            if (coro) {
                memset(coro, 0, sizeof(XrCoroutine));
                coro->gc.type = XR_TCOROUTINE;
            }
            if (!coro)
                return NULL;
            if (!coro_ensure_vm_state(coro)) {
                coro_discard_uninitialized(coro);
                return NULL;
            }
            xr_coro_vm_ctx(coro)->stack = NULL;
            xr_coro_vm_ctx(coro)->frames = NULL;
            coro->coro_gc = NULL;
        }
    }
    if (!coro_ensure_vm_state(coro)) {
        coro_discard_uninitialized(coro);
        return NULL;
    }

    // Common initialization (flags, coro_gc, stack/frames, field resets, timer, GC, ID)
    if (!coro_init_common(coro, X, name, true)) {
        xr_coro_free(coro);
        coro_discard_uninitialized(coro);
        return NULL;
    }

    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    XR_DCHECK(vm_state != NULL, "coro_create: missing VM state");

    // Closure-specific: entry type
    vm_state->entry_type = XR_CORO_ENTRY_CLOSURE;

    // Share parent's closure directly — no copy needed.
    // Compiler-enforced is_coro_safe guarantees all upvalues are shared const
    // (closed immediately, value in upvalue->closed, independent of any stack).
    // Parent coroutine outlives children via scope mechanism, so closure and
    // its upvalue objects remain valid for the child's entire lifetime.
    vm_state->entry.closure = closure;
    if (xr_coro_name(coro) && !xr_coro_set_source(coro, file, line)) {
        xr_coro_free(coro);
        coro_discard_uninitialized(coro);
        return NULL;
    }

    // Deep copy args to coroutine private heap (cross-coroutine safe)
    vm_state->arg_count = arg_count;
    if (arg_count <= 4) {
        vm_state->args = vm_state->inline_args;
        for (int i = 0; i < arg_count; i++) {
            // Fast path: non-pointer values (int/float/bool/null) need no copy
            vm_state->inline_args[i] =
                XR_IS_PTR(args[i]) ? xr_deep_copy_to_coro(X, args[i], coro) : args[i];
        }
    } else if (arg_count > 0 && args != NULL) {
        vm_state->args = (XrValue *) xr_malloc(sizeof(XrValue) * arg_count);
        if (!vm_state->args) {
            xr_coro_free(coro);
            coro_discard_uninitialized(coro);
            return NULL;
        }
        for (int i = 0; i < arg_count; i++) {
            vm_state->args[i] =
                XR_IS_PTR(args[i]) ? xr_deep_copy_to_coro(X, args[i], coro) : args[i];
        }
    } else {
        vm_state->args = NULL;
    }

    // Async stack trace: parent pointer + caller-provided file/line.
    // vm_go pre-computes these from the current frame, so no redundant frame walk here.
    if (xr_coro_name(coro) &&
        !xr_coro_set_spawn_origin(coro, (XrCoroutine *) X->vm.current_coro, file, line)) {
        xr_coro_free(coro);
        coro_discard_uninitialized(coro);
        return NULL;
    }

    return coro;
}

XrCoroutine *xr_coro_create_empty(XrayIsolate *X, const char *name, bool need_stack) {
    XR_DCHECK(X != NULL, "coro_create_empty: NULL isolate");

    XrCoroState *sched = (XrCoroState *) X->vm.coro_state;
    if (sched && sched->coro_count >= XR_MAX_COROUTINES) {
        return NULL;
    }

    XrCoroutine *coro = NULL;
    if (!need_stack) {
        coro = coro_alloc_lightweight_shell();
    } else {
        XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
        if (runtime) {
            coro = xr_coro_pool_get(runtime);
        }
        if (!coro) {
            if (X->sys_heap) {
                coro = xr_sysheap_alloc_coro(X->sys_heap);
                if (!coro)
                    return NULL;
                coro->coro_gc = NULL;
            } else {
                coro = (XrCoroutine *) xr_malloc(sizeof(XrCoroutine));
                if (coro) {
                    memset(coro, 0, sizeof(XrCoroutine));
                    coro->gc.type = XR_TCOROUTINE;
                }
                if (!coro)
                    return NULL;
                if (!coro_ensure_vm_state(coro)) {
                    coro_discard_uninitialized(coro);
                    return NULL;
                }
                xr_coro_vm_ctx(coro)->stack = NULL;
                xr_coro_vm_ctx(coro)->frames = NULL;
                coro->coro_gc = NULL;
            }
        }
        if (!coro_ensure_vm_state(coro)) {
            coro_discard_uninitialized(coro);
            return NULL;
        }
    }
    if (!coro)
        return NULL;

    if (!coro_init_common(coro, X, name, need_stack)) {
        xr_coro_free(coro);
        coro_discard_uninitialized(coro);
        return NULL;
    }

    return coro;
}

// Create C function coroutine (supports Yieldable I/O)
// Unlike native coroutine, has stack and frames, supports internal Yieldable C calls
// Used for HTTP connection handling and other I/O wait scenarios
XrCoroutine *xr_coro_create_cfunc(XrayIsolate *X,
                                  XrCFuncResult (*cfunc)(XrayIsolate *, XrValue *, int, XrValue *),
                                  XrValue *args, int argc, const char *name) {
    XrCoroState *sched = (XrCoroState *) X->vm.coro_state;
    if (sched && sched->coro_count >= XR_MAX_COROUTINES) {
        return NULL;
    }

    // Allocate: try pool first, then system heap
    XrCoroutine *coro = NULL;
    XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
    if (runtime) {
        coro = xr_coro_pool_get(runtime);
    }
    if (!coro) {
        if (X->sys_heap) {
            coro = xr_sysheap_alloc_coro(X->sys_heap);
            if (!coro)
                return NULL;
            coro->coro_gc = NULL;
        } else {
            coro = (XrCoroutine *) xr_malloc(sizeof(XrCoroutine));
            if (coro) {
                memset(coro, 0, sizeof(XrCoroutine));
                coro->gc.type = XR_TCOROUTINE;
            }
            if (!coro)
                return NULL;
            if (!coro_ensure_vm_state(coro)) {
                coro_discard_uninitialized(coro);
                return NULL;
            }
            xr_coro_vm_ctx(coro)->stack = NULL;
            xr_coro_vm_ctx(coro)->frames = NULL;
            coro->coro_gc = NULL;
        }
    }
    if (!coro_ensure_vm_state(coro)) {
        coro_discard_uninitialized(coro);
        return NULL;
    }

    // Common initialization (flags, coro_gc, stack/frames, field resets, timer, GC, ID)
    if (!coro_init_common(coro, X, name, true)) {
        xr_coro_free(coro);
        coro_discard_uninitialized(coro);
        return NULL;
    }

    XrVmCoroState *vm_state = xr_coro_maybe_vm_state(coro);
    XR_DCHECK(vm_state != NULL, "coro_create_cfunc: missing VM state");

    // CFunc-specific: entry type
    vm_state->entry_type = XR_CORO_ENTRY_CFUNC;
    vm_state->entry.cfunc = cfunc;

    // Copy args (no deep copy needed for C function args)
    vm_state->arg_count = argc;
    if (argc > 0 && args) {
        if (argc <= 4) {
            for (int i = 0; i < argc; i++) {
                vm_state->inline_args[i] = args[i];
            }
            vm_state->args = vm_state->inline_args;
        } else {
            vm_state->args = (XrValue *) xr_malloc(argc * sizeof(XrValue));
            if (!vm_state->args) {
                xr_coro_free(coro);
                coro_discard_uninitialized(coro);
                return NULL;
            }
            memcpy(vm_state->args, args, argc * sizeof(XrValue));
        }
    } else {
        vm_state->args = NULL;
    }

    return coro;
}

// Create Native coroutine (C function callback, no Yieldable support)
// For simple callbacks without I/O wait
XrCoroutine *xr_coro_create_native(XrayIsolate *X, void (*func)(void *), void *arg,
                                   const char *name) {
    if (!X || !func)
        return NULL;

    XrCoroutine *coro = xr_coro_create_empty(X, name, false);
    if (!coro)
        return NULL;

    XrNativeCoroState *state = (XrNativeCoroState *) xr_calloc(1, sizeof(XrNativeCoroState));
    if (!state) {
        xr_coro_destroy(coro);
        return NULL;
    }
    state->func = func;
    state->arg = arg;

    coro->backend = &native_backend_vtable;
    coro->backend_state = state;

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

// Release only the Immix heap (coro_gc) while preserving result/error.
// Called by await paths to free the largest memory consumer without
// invalidating task.result / task.error for later access.
// The coroutine struct itself is reclaimed by GC when no references remain.
void xr_coro_release_heap(XrCoroutine *coro) {
    if (!coro)
        return;
    XrCoroGC *gc = atomic_exchange_explicit((_Atomic(XrCoroGC *) *) &coro->coro_gc, NULL,
                                            memory_order_acq_rel);
    if (gc)
        xr_coro_gc_destroy(gc);
}

// Release completed coroutine's heap resources (arena, stack, frames)
// Called after OP_AWAIT consumes the result, to reclaim memory.
// Optimization: try to recycle to Worker local pool (keep stack/frames allocated).
// Falls back to full release if no Worker context or pool is full.
void xr_coro_release_resources(XrCoroutine *coro) {
    if (!coro)
        return;

    coro_release_backend_state(coro, false);

    // Destroy coro_gc (Immix heap) — use atomic exchange to prevent
    // double-free race with early release in xr_coro_run_on_worker
    {
        XrCoroGC *gc = atomic_exchange_explicit((_Atomic(XrCoroGC *) *) &coro->coro_gc, NULL,
                                                memory_order_acq_rel);
        if (gc)
            xr_coro_gc_destroy(gc);
    }

    if (!xr_coro_maybe_vm_state(coro)) {
        coro->result = xr_null();
        coro->error = xr_null();
        return;
    }

    // Try to recycle: keep stack/frames, put into Worker local pool
    XrWorker *worker = xr_current_worker();
    if (worker && xr_coro_vm_ctx(coro)->stack && xr_coro_vm_ctx(coro)->frames &&
        worker->p.local_free_count < XR_CORO_LOCAL_FREE_MAX) {
        // Reset minimal state for reuse
        xr_coro_vm_ctx(coro)->stack[0] = xr_null();
        xr_coro_vm_ctx(coro)->stack_top = xr_coro_vm_ctx(coro)->stack;
        xr_coro_vm_ctx(coro)->frame_count = 0;
        coro->result = xr_null();
        coro->error = xr_null();
        xr_coro_clear_vm_entry_state(coro);
        // Free heap-allocated handlers (inline storage is part of struct)
        if (xr_coro_vm_ctx(coro)->handlers &&
            xr_coro_vm_ctx(coro)->handlers != xr_coro_vm_ctx(coro)->handler_inline) {
            xr_free(xr_coro_vm_ctx(coro)->handlers);
        }
        xr_coro_vm_ctx(coro)->handlers = xr_coro_vm_ctx(coro)->handler_inline;
        xr_coro_vm_ctx(coro)->handler_count = 0;
        xr_coro_vm_ctx(coro)->handler_capacity = XR_HANDLER_INLINE_CAP;
        // Per-frame struct storage: free individual areas, keep arrays for reuse
        if (xr_coro_vm_ctx(coro)->struct_areas) {
            for (int i = 0; i < xr_coro_vm_ctx(coro)->struct_areas_cap; i++) {
                if (xr_coro_vm_ctx(coro)->struct_areas[i]) {
                    xr_free(xr_coro_vm_ctx(coro)->struct_areas[i]);
                    xr_coro_vm_ctx(coro)->struct_areas[i] = NULL;
                    xr_coro_vm_ctx(coro)->struct_area_caps[i] = 0;
                }
            }
        }
        // Struct return arena: reset usage (keep buffer for reuse)
        xr_coro_vm_ctx(coro)->struct_ret_arena_used = 0;
        // Inline-cache tables are tied to a specific (coro, proto_id)
        // pairing — recycling the coro to a fresh closure means the IC
        // entries are no longer valid feedback for the next user, so
        // drop them here rather than handing back stale state.
        xr_vm_ctx_free_ic_tables(xr_coro_vm_ctx(coro));
        // ext->io_buf: keep alive for reuse across coro lifetimes (free only on full destroy)
        coro_channel_wait_reset(coro->ext);
        coro_recv_slot_reset(coro->ext);
        coro_wait_state_reset(coro->ext);
        coro_select_storage_reset(coro->ext);
        coro_clear_scope_membership(coro);
        // Add to pool
        coro->next = worker->p.local_free_list;
        worker->p.local_free_list = coro;
        worker->p.local_free_count++;
        return;
    }

    // Release VM stack and bytecode frames
    if (xr_coro_vm_ctx(coro)->stack) {
        if (coro->gc_flags & XR_CORO_GC_SLAB_STACK) {
            // Slab-embedded VM stack: don't free, arena owns the memory.
            // But if the VM stack was grown, it is a separate allocation now.
            if (xr_coro_vm_ctx(coro)->stack_capacity != INITIAL_STACK_CAPACITY) {
                // VM stack was grown and is now independently allocated.
                xr_free(xr_coro_vm_ctx(coro)->stack);
            }
            // If capacity matches initial, the VM stack is still in slab — skip free.
        } else {
            // Independently allocated VM stack — recycle to per-Worker slab free list or free.
            char *stack_end = (char *) xr_coro_vm_ctx(coro)->stack +
                              sizeof(XrValue) * xr_coro_vm_ctx(coro)->stack_capacity;
            bool combined = (xr_coro_vm_ctx(coro)->frames &&
                             (char *) xr_coro_vm_ctx(coro)->frames == stack_end);
            XrWorker *w = xr_current_worker();
            if (combined && w && xr_coro_vm_ctx(coro)->stack_capacity == INITIAL_STACK_CAPACITY &&
                w->p.stack_slab_count < 4096) {
                *(void **) xr_coro_vm_ctx(coro)->stack = w->p.stack_slab_free;
                w->p.stack_slab_free = xr_coro_vm_ctx(coro)->stack;
                w->p.stack_slab_count++;
            } else {
                xr_free(xr_coro_vm_ctx(coro)->stack);
                if (!combined && xr_coro_vm_ctx(coro)->frames) {
                    xr_free(xr_coro_vm_ctx(coro)->frames);
                }
            }
        }
        xr_coro_vm_ctx(coro)->stack = NULL;
        xr_coro_vm_ctx(coro)->frames = NULL;
    } else if (xr_coro_vm_ctx(coro)->frames) {
        xr_free(xr_coro_vm_ctx(coro)->frames);
        xr_coro_vm_ctx(coro)->frames = NULL;
    }
    if (xr_coro_vm_ctx(coro)->handlers &&
        xr_coro_vm_ctx(coro)->handlers != xr_coro_vm_ctx(coro)->handler_inline) {
        xr_free(xr_coro_vm_ctx(coro)->handlers);
        xr_coro_vm_ctx(coro)->handlers = NULL;
    }
    // Per-frame struct storage
    if (xr_coro_vm_ctx(coro)->struct_areas) {
        for (int i = 0; i < xr_coro_vm_ctx(coro)->struct_areas_cap; i++) {
            if (xr_coro_vm_ctx(coro)->struct_areas[i])
                xr_free(xr_coro_vm_ctx(coro)->struct_areas[i]);
        }
        xr_free(xr_coro_vm_ctx(coro)->struct_areas);
        xr_free(xr_coro_vm_ctx(coro)->struct_area_caps);
        xr_coro_vm_ctx(coro)->struct_areas = NULL;
        xr_coro_vm_ctx(coro)->struct_area_caps = NULL;
        xr_coro_vm_ctx(coro)->struct_areas_cap = 0;
    }
    // Struct return arena
    if (xr_coro_vm_ctx(coro)->struct_ret_arena) {
        xr_free(xr_coro_vm_ctx(coro)->struct_ret_arena);
        xr_coro_vm_ctx(coro)->struct_ret_arena = NULL;
        xr_coro_vm_ctx(coro)->struct_ret_arena_used = 0;
        xr_coro_vm_ctx(coro)->struct_ret_arena_cap = 0;
    }
    // Per-coroutine inline caches
    xr_vm_ctx_free_ic_tables(xr_coro_vm_ctx(coro));
    if (coro->ext && coro->ext->io_buf) {
        xr_free(coro->ext->io_buf);
        coro->ext->io_buf = NULL;
        coro->ext->io_buf_cap = 0;
    }
    coro->result = xr_null();
    xr_coro_clear_vm_entry_state(coro);
}

// Free coroutine internal resources.
// The owner frees or recycles the coroutine object shell after this returns.
void xr_coro_free(XrCoroutine *coro) {
    if (!coro)
        return;

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

    if (!coro->backend || !coro->backend->prepare_recycle) {
        xr_coro_destroy(coro);
        return;
    }

    // Reset GC context: finalize objects, bulk free Immix blocks, reset state.
    // Uses xr_coro_gc_reset which handles large objects and finalizers
    // correctly (the previous partial reset skipped those).
    if (coro->coro_gc) {
        xr_coro_gc_reset(coro->coro_gc, coro);
    }
    if (!coro->backend->prepare_recycle(coro, worker)) {
        xr_coro_destroy(coro);
        return;
    }

    // Thorough reset: zero all fields that coro_init_common would memset.
    // This allows coro_init_common to skip the full shell memset for recycled coros.
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
    // ext fields (yield_info, lock_count, locked_worker, locals, watched_by)
    // are reset in coro_init_common dirty path; ext pointer preserved for io_buf reuse
    xr_coro_clear_debug_identity(coro);
    atomic_store_explicit(&coro->flags, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_NONE, memory_order_relaxed);
    atomic_store_explicit(&coro->resume_status, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->affinity_p, 0, memory_order_relaxed);
    // timer fields live in ext; reset happens in coro_init_common dirty path
    // Mark as "clean" — coro_init_common can skip memset
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
void xr_sched_init(XrCoroState *sched) {
    if (!sched)
        return;

    // Initialize multi-level priority queues
    for (int i = 0; i < XR_CORO_PRIORITY_COUNT; i++) {
        sched->ready_head[i] = NULL;
        sched->ready_tail[i] = NULL;
    }
    sched->coro_count = 0;
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
void xr_sched_destroy(XrCoroState *sched) {
    if (!sched)
        return;

    // Coroutines managed by GC, just clear list refs
    for (int i = 0; i < XR_CORO_PRIORITY_COUNT; i++) {
        sched->ready_head[i] = NULL;
        sched->ready_tail[i] = NULL;
    }

    sched->coro_count = 0;

    // Destroy named coroutine registry
    if (sched->coro_registry) {
        xr_coro_registry_destroy(sched->coro_registry);
        xr_free(sched->coro_registry);
        sched->coro_registry = NULL;
    }
}

// Add coroutine to ready queue tail (select queue by priority)
void xr_sched_enqueue(XrCoroState *sched, XrCoroutine *coro) {
    if (!sched || !coro)
        return;

    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING);
    xr_coro_flags_set(coro, XR_CORO_FLG_READY);
    coro->next = NULL;

    // Select queue by priority (from flags)
    int prio = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (prio < 0)
        prio = 0;
    if (prio >= XR_CORO_PRIORITY_COUNT)
        prio = XR_CORO_PRIORITY_COUNT - 1;

    if (sched->ready_tail[prio]) {
        sched->ready_tail[prio]->next = coro;
        sched->ready_tail[prio] = coro;
    } else {
        sched->ready_head[prio] = coro;
        sched->ready_tail[prio] = coro;
    }
    sched->coro_count++;
}

// Remove specific coroutine from ready queue (for await direct execution)
// Note: don't decrement coro_count, coroutine still active (just not in ready queue)
void xr_sched_remove(XrCoroState *sched, XrCoroutine *target) {
    if (!sched || !target)
        return;

    // Search all priority queues
    for (int prio = 0; prio < XR_CORO_PRIORITY_COUNT; prio++) {
        XrCoroutine *prev = NULL;
        XrCoroutine *coro = sched->ready_head[prio];

        while (coro) {
            if (coro == target) {
                // Found, remove from queue
                if (prev) {
                    prev->next = coro->next;
                } else {
                    sched->ready_head[prio] = coro->next;
                }
                if (coro == sched->ready_tail[prio]) {
                    sched->ready_tail[prio] = prev;
                }
                coro->next = NULL;
                // Don't decrement coro_count, coroutine still active
                return;
            }
            prev = coro;
            coro = coro->next;
        }
    }
}

// Dequeue coroutine from ready queue (high to low priority)
XrCoroutine *xr_sched_dequeue(XrCoroState *sched) {
    if (!sched)
        return NULL;

    // Search from high to low priority
    for (int prio = XR_CORO_PRIORITY_COUNT - 1; prio >= 0; prio--) {
        if (sched->ready_head[prio]) {
            XrCoroutine *coro = sched->ready_head[prio];
            sched->ready_head[prio] = coro->next;

            if (!sched->ready_head[prio]) {
                sched->ready_tail[prio] = NULL;
            }

            coro->next = NULL;
            sched->coro_count--;
            return coro;
        }
    }

    return NULL;
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
        xr_worker_wake_all(current, channel);
        while (xr_worker_wake_select_with_status(current, channel, XR_RESUME_CHANNEL_CLOSED)) {
            // Keep waking until no more select waiters
        }
    }

    // Remote workers: dispatch close commands via mask
    XrChannel *ch = (XrChannel *) channel;
    uint64_t mask = xr_channel_any_waiter_mask(ch);
    mask &= ~xr_channel_worker_bit(current_id);
    while (mask) {
        int wid = __builtin_ctzll(mask);
        mask &= mask - 1;
        if (wid >= runtime->worker_count)
            continue;

        xr_worker_dispatch_chan_wake(runtime, wid, channel, false, true);
    }

    // Clear the mask — channel is closed, no future waiters expected.
    xr_channel_clear_all_waiter_masks(ch);
}

// ========== Deadlock Diagnosis ==========

// Format coroutine identifier into caller-provided buffer
static XR_UNUSED const char *format_coro_id(XrCoroutine *coro, char *buf, size_t bufsz) {
    const char *name = xr_coro_name(coro);
    if (name) {
        snprintf(buf, bufsz, "#%d \"%s\"", coro->id, name);
    } else {
        snprintf(buf, bufsz, "#%d", coro->id);
    }
    return buf;
}

// Print deadlock diagnosis info (simplified: blocked queue managed by Runtime)
static XR_UNUSED void xr_sched_print_deadlock(XrCoroState *sched) {
    if (!sched)
        return;

    int ready_count = 0;
    for (int prio = 0; prio < XR_CORO_PRIORITY_COUNT; prio++) {
        XrCoroutine *r = sched->ready_head[prio];
        while (r) {
            ready_count++;
            r = r->next;
        }
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Coroutine Deadlock Detection Report\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Ready coroutines: %d, Total created: %d\n", ready_count, sched->total_created);
    fprintf(stderr, "Note: Blocked queue managed by Runtime\n");
    fprintf(stderr, "========================================\n\n");
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
    coro_channel_wait_reset(coro->ext);
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

/* ========== VM Stack Growth ========== */

bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots) {
    if (!coro || !xr_coro_vm_ctx(coro)->stack)
        return false;
    XR_DCHECK(extra_slots > 0, "grow_stack: non-positive extra_slots");
    XR_DCHECK(xr_coro_vm_ctx(coro)->stack_capacity > 0, "grow_stack: zero stack_capacity");

    int new_capacity = xr_coro_vm_ctx(coro)->stack_capacity + extra_slots;
    if (new_capacity > 1024 * 1024)
        return false;

    // Check if stack and frames are in a combined allocation block.
    // If frames pointer is right after the stack, they share one malloc.
    char *stack_end = (char *) xr_coro_vm_ctx(coro)->stack +
                      sizeof(XrValue) * xr_coro_vm_ctx(coro)->stack_capacity;
    bool combined = ((char *) xr_coro_vm_ctx(coro)->frames == stack_end);

    // Check if stack is from arena slab (gc_flags bit 0)
    bool slab_stack = (coro->gc_flags & XR_CORO_GC_SLAB_STACK) != 0;

    if (combined) {
        // Split: allocate new separate stack, copy data
        XrValue *new_stack = (XrValue *) xr_malloc(sizeof(XrValue) * new_capacity);
        if (!new_stack)
            return false;
        memcpy(new_stack, xr_coro_vm_ctx(coro)->stack,
               sizeof(XrValue) * xr_coro_vm_ctx(coro)->stack_capacity);
        memset(new_stack + xr_coro_vm_ctx(coro)->stack_capacity, 0, sizeof(XrValue) * extra_slots);

        // Allocate separate frames, copy from old combined block
        XrBcCallFrame *new_frames = (XrBcCallFrame *) xr_malloc(
            sizeof(XrBcCallFrame) * xr_coro_vm_ctx(coro)->frame_capacity);
        if (!new_frames) {
            xr_free(new_stack);
            return false;
        }
        memcpy(new_frames, xr_coro_vm_ctx(coro)->frames,
               sizeof(XrBcCallFrame) * xr_coro_vm_ctx(coro)->frame_count);

        // Free old block only if it is independently allocated.
        if (!slab_stack) {
            xr_free(xr_coro_vm_ctx(coro)->stack);
        }
        xr_coro_vm_ctx(coro)->stack = new_stack;
        xr_coro_vm_ctx(coro)->stack_capacity = new_capacity;
        xr_coro_vm_ctx(coro)->frames = new_frames;
        // Clear slab flag: VM stack is now independently allocated.
        coro->gc_flags &= ~XR_CORO_GC_SLAB_STACK;
    } else {
        // Already separate
        if (slab_stack) {
            // Slab VM stack (not combined): allocate new storage, copy, don't free old.
            XrValue *new_stack = (XrValue *) xr_malloc(sizeof(XrValue) * new_capacity);
            if (!new_stack)
                return false;
            memcpy(new_stack, xr_coro_vm_ctx(coro)->stack,
                   sizeof(XrValue) * xr_coro_vm_ctx(coro)->stack_capacity);
            memset(new_stack + xr_coro_vm_ctx(coro)->stack_capacity, 0,
                   sizeof(XrValue) * extra_slots);
            xr_coro_vm_ctx(coro)->stack = new_stack;
            xr_coro_vm_ctx(coro)->stack_capacity = new_capacity;
            coro->gc_flags &= ~XR_CORO_GC_SLAB_STACK;
        } else {
            XrValue *new_stack =
                (XrValue *) xr_realloc(xr_coro_vm_ctx(coro)->stack, sizeof(XrValue) * new_capacity);
            if (!new_stack)
                return false;
            memset(new_stack + xr_coro_vm_ctx(coro)->stack_capacity, 0,
                   sizeof(XrValue) * extra_slots);
            xr_coro_vm_ctx(coro)->stack = new_stack;
            xr_coro_vm_ctx(coro)->stack_capacity = new_capacity;
        }
    }

    if (xr_coro_vm_ctx(coro)->frame_count + 8 >= xr_coro_vm_ctx(coro)->frame_capacity) {
        int new_frame_cap = xr_coro_vm_ctx(coro)->frame_capacity * 2;
        // If frames were from slab and split out in the combined path above,
        // they may already be independently allocated. If only the VM stack
        // grew in the non-combined slab path, frames still point to the slab.
        bool frames_in_slab = slab_stack && !combined;
        if (frames_in_slab) {
            XrBcCallFrame *new_frames =
                (XrBcCallFrame *) xr_malloc(sizeof(XrBcCallFrame) * new_frame_cap);
            if (!new_frames)
                return false;
            memcpy(new_frames, xr_coro_vm_ctx(coro)->frames,
                   sizeof(XrBcCallFrame) * xr_coro_vm_ctx(coro)->frame_count);
            xr_coro_vm_ctx(coro)->frames = new_frames;
        } else {
            XrBcCallFrame *new_frames = (XrBcCallFrame *) xr_realloc(
                xr_coro_vm_ctx(coro)->frames, sizeof(XrBcCallFrame) * new_frame_cap);
            if (!new_frames)
                return false;
            xr_coro_vm_ctx(coro)->frames = new_frames;
        }
        xr_coro_vm_ctx(coro)->frame_capacity = new_frame_cap;
    }

    return true;
}
