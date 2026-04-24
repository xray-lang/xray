/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro.c - VM coroutine implementation
 *
 * KEY CONCEPT:
 *   Simplified coroutine integrated with VM main loop.
 *   Coroutines and their data are managed by GC.
 */

#include "../vm/xvm_internal.h"
#include "../runtime/gc/xgc.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/xray_debug.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h> // gettimeofday
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
#include "../runtime/object/xarray.h"

// Initial capacities (balanced: fast malloc + minimal grow_stack for common cases)
// 64 slots = 1024B stack + 288B frames = 1312B total per coroutine
#define INITIAL_STACK_CAPACITY 64
#define INITIAL_FRAME_CAPACITY 4

// Note: blocked queue moved to XrRuntime, see xworker.c

// ========== JIT Integration ==========

// GC safepoint for JIT code: GC step + cancel check.
// Returns 0 to continue, non-zero to request deopt exit.
// Each backend's safepoint stub checks return value and jumps to deopt_stub
// if non-zero — only 1-2 extra instructions per platform.
int xr_coro_gc_safepoint(XrCoroutine *coro) {
    if (!coro) return 0;

    // Reset reductions for next safepoint interval
    coro->reductions = XR_CORO_REDUCTIONS;

    // Bump worker heartbeat so sysmon doesn't misdetect long-running
    // JIT code as stuck (JIT stays inside run_on_worker across many
    // C helper calls like spawn_cont/await without returning to worker loop)
    if (coro->jit_ctx && coro->jit_ctx->heartbeat_ptr) {
        atomic_fetch_add_explicit(coro->jit_ctx->heartbeat_ptr, 1,
                                  memory_order_relaxed);
    }

    if (coro->coro_gc && coro->coro_gc->gc_requested) {
        xr_coro_gc_step(coro->coro_gc);
    }

    // Cancel check: watchdog sets this via xr_runtime_force_stop
    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        return 1;
    }
    return 0;
}

// Forward write barrier for JIT: black parent writes white child → mark child
void xr_jit_barrier_fwd(XrCoroutine *coro, void *parent, void *child) {
    if (!coro || !coro->coro_gc || !parent || !child) return;
    xr_coro_gc_barrier(coro->coro_gc, (XrGCHeader *)parent, (XrGCHeader *)child);
}

// Back write barrier for JIT: black container mutated → container becomes gray
void xr_jit_barrier_back(XrCoroutine *coro, void *container) {
    if (!coro || !coro->coro_gc || !container) return;
    xr_coro_gc_barrierback(coro->coro_gc, (XrGCHeader *)container);
}

// ========== Memory Sync Helper Functions ==========

// Reset vm_ctx execution state (stack/frames pointers already set during allocation)
void xr_coro_sync_vm_ctx(XrCoroutine *coro, XrayIsolate *X) {
    if (!coro) return;

    // Targeted field resets instead of memset (avoids zeroing preserved pointers)
    XrVMContext *ctx = &coro->vm_ctx;
    ctx->stack_top = ctx->stack;
    ctx->frame_count = 0;
    ctx->module_base_frame = 0;
    ctx->handler_count = 0;
    ctx->current_exception = xr_null();
    ctx->current_coro = coro;
    ctx->instruction_count = 0;
    ctx->preempt_pending = false;
    ctx->last_nret = 0;
    ctx->trace_execution = false;
    ctx->isolate = X;
}

// Upgrade coroutine heap (for main coroutine)
// Replace small heap with large heap for deep recursion
bool xr_coro_upgrade_heap(XrCoroutine *coro, size_t size) {
    if (!coro) return false;
    // Arena GC: arena grows automatically, no explicit upgrade needed
    (void)size;
    return true;
}

// ========== Coroutine Creation and Destruction ==========

// Forward declaration (defined after bootstrap)
static bool coro_init_common(XrCoroutine *coro, XrayIsolate *X,
                             const char *name, bool need_stack);

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
        coro = (XrCoroutine *)xr_malloc(sizeof(XrCoroutine));
        if (coro) {
            memset(coro, 0, sizeof(XrCoroutine));
            coro->gc.type = XR_TCOROUTINE;
        }
    }
    if (!coro) return NULL;

    // New object: ensure NULL pointers for coro_init_common
    coro->vm_ctx.stack = NULL;
    coro->vm_ctx.frames = NULL;
    coro->coro_gc = NULL;

    // Common initialization (flags, stack/frames, field resets, timer, GC fields, vm_ctx sync, ID)
    if (!coro_init_common(coro, X, "main", true)) {
        return NULL;
    }

    // Main coroutine needs coro_gc immediately (GC API, init-phase allocations)
    // Use main-specific config: larger threshold, slower GC (long-lived coroutine)
    if (!coro->coro_gc) {
        XrCoroGCConfig main_config = {
            .gc_threshold = XR_MAIN_CORO_GC_THRESHOLD,
            .gc_pause     = XR_MAIN_CORO_GC_PAUSE,
            .gc_stepmul   = XR_MAIN_CORO_GC_STEPMUL,
        };
        coro->coro_gc = xr_coro_gc_create(coro, &main_config);
        if (!coro->coro_gc) return NULL;
    }

    // Bootstrap-specific: mark as main coroutine
    coro->flags |= XR_CORO_FLG_MAIN;
    coro->entry_type = XR_CORO_ENTRY_CLOSURE;
    coro->entry.closure = NULL;
    coro->source_file = NULL;
    coro->source_line = 0;
    coro->args = NULL;
    coro->arg_count = 0;
    coro->parent_coro = NULL;
    coro->spawn_line = 0;
    coro->spawn_file = NULL;
    coro->pending_spawn = NULL;

    return coro;
}

// Setup bootstrap main_coro for script execution (called from xr_execute)
// Upgrades the bootstrap coro with closure and proto, ready for VM run.
void xr_coro_setup_main(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure) {
    XR_DCHECK(coro != NULL, "coro_setup_main: NULL coro");
    XR_DCHECK(X != NULL, "coro_setup_main: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_setup_main: NULL closure");
    coro->entry_type = XR_CORO_ENTRY_CLOSURE;
    coro->entry.closure = closure;
    coro->source_file = closure->proto ? closure->proto->source_file : NULL;
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
    coro->entry_type = XR_CORO_ENTRY_CLOSURE;
    coro->entry.closure = closure;
    coro->source_file = closure->proto ? closure->proto->source_file : NULL;

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
static bool coro_init_common(XrCoroutine *coro, XrayIsolate *X,
                             const char *name, bool need_stack) {
    XrScheduler *sched = (XrScheduler *)X->vm.scheduler;

    // Check if coro was recycled with thorough cleanup (XR_CORO_GC_RECYCLED_CLEAN).
    // Recycled coros already have all fields zeroed by xr_coro_recycle_local,
    // so we skip the expensive 640B memset and only set non-zero fields.
    // NOTE: must NOT use XR_CORO_GC_FROM_POOL here — that bit is set for ALL
    // pool-allocated coros including fresh uninitialized ones.
    bool is_clean = (coro->gc_flags & XR_CORO_GC_RECYCLED_CLEAN) != 0;

    if (!is_clean) {
        // Fresh allocation from pool/slab: bulk memset is faster than individual
        // field stores on ARM64 (vectorized stp instructions).
        // Save fields set by pool_get, memset the rest, then restore.
        XrValue *saved_stack = coro->vm_ctx.stack;
        int saved_stack_cap = coro->vm_ctx.stack_capacity;
        XrBcCallFrame *saved_frames = coro->vm_ctx.frames;
        int saved_frame_cap = coro->vm_ctx.frame_capacity;
        XrExceptionHandler *saved_handlers = coro->vm_ctx.handlers;
        int saved_handler_cap = coro->vm_ctx.handler_capacity;
        struct XrCoroGC *saved_coro_gc = coro->coro_gc;
        uint16_t saved_pool_bits = coro->gc_flags & (XR_CORO_GC_SLAB_STACK | XR_CORO_GC_FROM_POOL);
        XrCoroExt *saved_ext = coro->ext;
        // Preserve jit_suspend allocation for reuse (avoid free+malloc churn).
        // The memset below will zero the pointer; restore after.
        XrJitSuspendState *saved_jit_suspend = coro->jit_suspend;

        memset((char *)coro + offsetof(XrCoroutine, flags), 0,
               sizeof(XrCoroutine) - offsetof(XrCoroutine, flags));

        coro->vm_ctx.stack = saved_stack;
        coro->vm_ctx.stack_capacity = saved_stack_cap;
        coro->vm_ctx.frames = saved_frames;
        coro->vm_ctx.frame_capacity = saved_frame_cap;
        coro->vm_ctx.handlers = saved_handlers;
        coro->vm_ctx.handler_capacity = saved_handler_cap;
        coro->coro_gc = saved_coro_gc;
        coro->gc_flags = saved_pool_bits;
        coro->ext = saved_ext;
        coro->jit_suspend = saved_jit_suspend;
        // Reset ext fields that must not persist across lifetimes
        if (coro->ext) {
            coro->ext->locals = NULL;
            coro->ext->watched_by = NULL;
            coro->ext->yield_info.wait_fd = 0;
            coro->ext->yield_info.wait_events = 0;
            coro->ext->yield_info.result_events = 0;
            coro->ext->yield_info.deadline = 0;
            coro->ext->yield_info.timed_out = false;
            atomic_store_explicit(&coro->ext->lock_count, 0, memory_order_relaxed);
            coro->ext->locked_worker = -1;
            atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            coro->ext->timer.slot = XR_TW_SLOT_INACTIVE;
            coro->ext->timer_wheel_owner = -1;
            atomic_store_explicit(&coro->ext->timer_seq, 0, memory_order_relaxed);
        }
    } else {
        // Clear the clean bit (consumed)
        coro->gc_flags &= ~0x0002;
    }

    // Atomic fields
    atomic_store_explicit(&coro->flags,
                          XR_CORO_FLG_READY | XR_CORO_PRIO_NORMAL,
                          memory_order_relaxed);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_READY, memory_order_relaxed);
    if (!is_clean) {
        // Fresh allocation: all atomic fields need explicit init
        atomic_store_explicit(&coro->resume_status, 0, memory_order_relaxed);
        atomic_store_explicit(&coro->affinity_p, 0, memory_order_relaxed);
        atomic_store_explicit(&coro->wait_count, 0, memory_order_relaxed);
        atomic_store_explicit(&coro->any_done, false, memory_order_relaxed);
        atomic_store_explicit(&coro->await_task, NULL, memory_order_relaxed);
    }
    // Clean path: recycle_local already zeroed all atomic fields via atomic_store

    // Set non-zero fields (always needed)
    coro->reductions = XR_CORO_REDUCTIONS;
    coro->schedule_count = 1;
    coro->isolate = X;
    coro->name = name;
    if (!is_clean) {
        // Fresh allocation: set sentinel values (-1 means "not set")
        coro->recv_slot_offset = -1;
        coro->pending_result_slot = -1;
        // timer.slot/lock_count/locked_worker initialized lazily in ext when alloc'd
    }
    // Clean path: recycle_local already set these to their sentinel values

    // Cache worker pointer (single TLS lookup for stack slab + ID allocation)
    XrWorker *w = xr_current_worker();

    // Allocate stack + frames if needed (coro_gc is created lazily on first heap alloc)
    if (need_stack) {
        if (!coro->vm_ctx.stack) {
            size_t stack_bytes = sizeof(XrValue) * INITIAL_STACK_CAPACITY;
            size_t frames_bytes = sizeof(XrBcCallFrame) * INITIAL_FRAME_CAPACITY;
            char *block = NULL;
            // Try per-Worker stack slab free list first (lock-free, no malloc)
            if (w && w->p.stack_slab_free) {
                block = (char*)w->p.stack_slab_free;
                w->p.stack_slab_free = *(void**)block;
                w->p.stack_slab_count--;
            } else {
                block = (char*)xr_malloc(stack_bytes + frames_bytes);
            }
            if (!block) {
                return false;
            }
            coro->vm_ctx.stack = (XrValue*)block;
            coro->vm_ctx.stack_capacity = INITIAL_STACK_CAPACITY;
            coro->vm_ctx.frames = (XrBcCallFrame*)(block + stack_bytes);
            coro->vm_ctx.frame_capacity = INITIAL_FRAME_CAPACITY;
        }
        if (!is_clean) {
            // Fresh allocation: zero return slot. Recycled coros already done by recycle_local.
            coro->vm_ctx.stack[0] = xr_null();
        }
    }

    // Inline vm_ctx sync
    if (!is_clean) {
        // Fresh allocation: full sync needed
        coro->vm_ctx.stack_top = coro->vm_ctx.stack;
        coro->vm_ctx.isolate = X;
    }
    // stack_top already reset by recycle_local for clean coros
    coro->vm_ctx.current_coro = coro;

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
XrCoroutine *xr_coro_create(XrayIsolate *X, XrClosure *closure,
                            XrValue *args, int arg_count,
                            const char *name, const char *file, int line) {
    XR_DCHECK(X != NULL, "coro_create: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_create: NULL closure");
    XR_DCHECK(arg_count >= 0, "coro_create: negative arg_count");
    XR_DCHECK(arg_count == 0 || args != NULL, "coro_create: NULL args with count > 0");
    // Check coroutine limit
    XrScheduler *sched = (XrScheduler *)X->vm.scheduler;
    if (sched && sched->coro_count >= XR_MAX_COROUTINES) {
        xr_runtime_error(X, "coroutine limit exceeded (%d)", XR_MAX_COROUTINES);
        return NULL;
    }

    // Allocate: try pool first, then system heap
    XrCoroutine *coro = NULL;
    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (runtime) {
        coro = xr_coro_pool_get(runtime);
    }
    if (!coro) {
        if (X->sys_heap) {
            coro = xr_sysheap_alloc_coro(X->sys_heap);
            // sysheap pool may have pre-set stack/frames from arena slab — keep them
            if (!coro) return NULL;
            coro->coro_gc = NULL;
        } else {
            coro = (XrCoroutine *)xr_malloc(sizeof(XrCoroutine));
            if (coro) {
                memset(coro, 0, sizeof(XrCoroutine));
                coro->gc.type = XR_TCOROUTINE;
            }
            if (!coro) return NULL;
            coro->vm_ctx.stack = NULL;
            coro->vm_ctx.frames = NULL;
            coro->coro_gc = NULL;
        }
    }

    // Common initialization (flags, coro_gc, stack/frames, field resets, timer, GC, ID)
    if (!coro_init_common(coro, X, name, true)) {
        return NULL;
    }

    // Closure-specific: entry type
    coro->entry_type = XR_CORO_ENTRY_CLOSURE;

    // Share parent's closure directly — no copy needed.
    // Compiler-enforced is_coro_safe guarantees all upvalues are shared const
    // (closed immediately, value in upvalue->closed, independent of any stack).
    // Parent coroutine outlives children via scope mechanism, so closure and
    // its upvalue objects remain valid for the child's entire lifetime.
    coro->entry.closure = closure;
    coro->source_file = file;
    coro->source_line = line;

    // Deep copy args to coroutine private heap (cross-coroutine safe)
    coro->arg_count = arg_count;
    if (arg_count <= 4) {
        coro->args = coro->inline_args;
        for (int i = 0; i < arg_count; i++) {
            // Fast path: non-pointer values (int/float/bool/null) need no copy
            coro->inline_args[i] = XR_IS_PTR(args[i])
                ? xr_deep_copy_to_coro(X, args[i], coro) : args[i];
        }
    } else if (arg_count > 0 && args != NULL) {
        coro->args = (XrValue *)xr_malloc(sizeof(XrValue) * arg_count);
        if (!coro->args) return NULL;
        for (int i = 0; i < arg_count; i++) {
            coro->args[i] = XR_IS_PTR(args[i])
                ? xr_deep_copy_to_coro(X, args[i], coro) : args[i];
        }
    } else {
        coro->args = NULL;
    }

    // Async stack trace: parent pointer + caller-provided file/line.
    // vm_go pre-computes these from the current frame, so no redundant frame walk here.
    coro->parent_coro = (XrCoroutine *)X->vm.current_coro;
    coro->spawn_file = file;
    coro->spawn_line = line;

    return coro;
}

// Create C function coroutine (supports Yieldable I/O)
// Unlike native coroutine, has stack and frames, supports internal Yieldable C calls
// Used for HTTP connection handling and other I/O wait scenarios
XrCoroutine *xr_coro_create_cfunc(XrayIsolate *X, XrCFuncResult (*cfunc)(XrayIsolate*, XrValue*, int, XrValue*),
                                   XrValue *args, int argc, const char *name) {
    XrScheduler *sched = (XrScheduler *)X->vm.scheduler;
    if (sched && sched->coro_count >= XR_MAX_COROUTINES) {
        return NULL;
    }

    // Allocate: try pool first, then system heap
    XrCoroutine *coro = NULL;
    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (runtime) {
        coro = xr_coro_pool_get(runtime);
    }
    if (!coro) {
        if (X->sys_heap) {
            coro = xr_sysheap_alloc_coro(X->sys_heap);
            if (!coro) return NULL;
            coro->coro_gc = NULL;
        } else {
            coro = (XrCoroutine *)xr_malloc(sizeof(XrCoroutine));
            if (coro) {
                memset(coro, 0, sizeof(XrCoroutine));
                coro->gc.type = XR_TCOROUTINE;
            }
            if (!coro) return NULL;
            coro->vm_ctx.stack = NULL;
            coro->vm_ctx.frames = NULL;
            coro->coro_gc = NULL;
        }
    }

    // Common initialization (flags, coro_gc, stack/frames, field resets, timer, GC, ID)
    if (!coro_init_common(coro, X, name, true)) {
        return NULL;
    }

    // CFunc-specific: entry type
    coro->entry_type = XR_CORO_ENTRY_CFUNC;
    coro->entry.cfunc = cfunc;
    coro->source_file = NULL;
    coro->source_line = 0;

    // Copy args (no deep copy needed for C function args)
    coro->arg_count = argc;
    if (argc > 0 && args) {
        if (argc <= 4) {
            for (int i = 0; i < argc; i++) {
                coro->inline_args[i] = args[i];
            }
            coro->args = coro->inline_args;
        } else {
            coro->args = (XrValue *)xr_malloc(argc * sizeof(XrValue));
            if (coro->args) {
                memcpy(coro->args, args, argc * sizeof(XrValue));
            }
        }
    } else {
        coro->args = NULL;
    }

    return coro;
}

// Create Native coroutine (C function callback, no Yieldable support)
// For simple callbacks without I/O wait
XrCoroutine *xr_coro_create_native(XrayIsolate *X, void (*func)(void*), void *arg,
                                    const char *name) {
    XrScheduler *sched = (XrScheduler *)X->vm.scheduler;
    if (sched && sched->coro_count >= XR_MAX_COROUTINES) {
        return NULL;
    }

    // Allocate: try pool first, then system heap
    XrCoroutine *coro = NULL;
    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (runtime) {
        coro = xr_coro_pool_get(runtime);
    }
    if (!coro) {
        if (X->sys_heap) {
            coro = xr_sysheap_alloc_coro(X->sys_heap);
            if (!coro) return NULL;
            coro->coro_gc = NULL;
        } else {
            coro = (XrCoroutine *)xr_malloc(sizeof(XrCoroutine));
            if (coro) {
                memset(coro, 0, sizeof(XrCoroutine));
                coro->gc.type = XR_TCOROUTINE;
            }
            if (!coro) return NULL;
            coro->vm_ctx.stack = NULL;
            coro->vm_ctx.frames = NULL;
            coro->coro_gc = NULL;
        }
    }

    // Common initialization (flags, field resets, timer, GC, ID; no stack/frames)
    if (!coro_init_common(coro, X, name, false)) {
        return NULL;
    }

    // Native-specific: entry type
    coro->entry_type = XR_CORO_ENTRY_NATIVE;
    coro->entry.native.func = func;
    coro->entry.native.arg = arg;
    coro->source_file = NULL;
    coro->source_line = 0;
    coro->arg_count = 0;
    coro->args = NULL;

    return coro;
}

// xr_coro_create_stackful removed — fully stackless now

// Add coroutine to scheduler queue
void xr_coro_spawn(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro) return;

    // Use multi-core Runtime
    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (runtime) {
        xr_runtime_spawn(runtime, coro);
    }
}

// Release only the Immix heap (coro_gc) while preserving result/error.
// Called by await paths to free the largest memory consumer without
// invalidating task.result / task.error for later access.
// The coroutine struct itself is reclaimed by GC when no references remain.
void xr_coro_release_heap(XrCoroutine *coro) {
    if (!coro) return;
    XrCoroGC *gc = __atomic_exchange_n(&coro->coro_gc, NULL, __ATOMIC_ACQ_REL);
    if (gc) xr_coro_gc_destroy(gc);
}

// Release completed coroutine's heap resources (arena, stack, frames)
// Called after OP_AWAIT consumes the result, to reclaim memory.
// Optimization: try to recycle to Worker local pool (keep stack/frames allocated).
// Falls back to full release if no Worker context or pool is full.
void xr_coro_release_resources(XrCoroutine *coro) {
    if (!coro) return;

    // Destroy coro_gc (Immix heap) — use atomic exchange to prevent
    // double-free race with early release in xr_coro_run_on_worker
    {
        XrCoroGC *gc = __atomic_exchange_n(&coro->coro_gc, NULL, __ATOMIC_ACQ_REL);
        if (gc) xr_coro_gc_destroy(gc);
    }

    // Try to recycle: keep stack/frames, put into Worker local pool
    XrWorker *worker = xr_current_worker();
    if (worker && coro->vm_ctx.stack && coro->vm_ctx.frames &&
        worker->p.local_free_count < XR_CORO_LOCAL_FREE_MAX) {
        // Reset minimal state for reuse
        coro->vm_ctx.stack[0] = xr_null();
        coro->vm_ctx.stack_top = coro->vm_ctx.stack;
        coro->vm_ctx.frame_count = 0;
        coro->result = xr_null();
        coro->error = xr_null();
        coro->entry.closure = NULL;
        // Free non-inline args
        if (coro->args && coro->args != coro->inline_args) {
            xr_free(coro->args);
        }
        coro->args = NULL;
        coro->arg_count = 0;
        // Free optional resources
        if (coro->vm_ctx.handlers) {
            xr_free(coro->vm_ctx.handlers);
            coro->vm_ctx.handlers = NULL;
            coro->vm_ctx.handler_count = 0;
            coro->vm_ctx.handler_capacity = 0;
        }
        // Per-frame struct storage: free individual areas, keep arrays for reuse
        if (coro->vm_ctx.struct_areas) {
            for (int i = 0; i < coro->vm_ctx.struct_areas_cap; i++) {
                if (coro->vm_ctx.struct_areas[i]) {
                    xr_free(coro->vm_ctx.struct_areas[i]);
                    coro->vm_ctx.struct_areas[i] = NULL;
                    coro->vm_ctx.struct_area_caps[i] = 0;
                }
            }
        }
        // Struct return arena: reset usage (keep buffer for reuse)
        coro->vm_ctx.struct_ret_arena_used = 0;
        // ext->io_buf: keep alive for reuse across coro lifetimes (free only on full destroy)
        // Add to pool
        coro->next = worker->p.local_free_list;
        worker->p.local_free_list = coro;
        worker->p.local_free_count++;
        return;
    }

    // Release stack+frames
    if (coro->vm_ctx.stack) {
        if (coro->gc_flags & 0x0001) {
            // Slab-embedded stack: don't free, arena owns the memory.
            // But if stack was grown (realloc'd), it's a separate allocation now.
            if (coro->vm_ctx.stack_capacity != INITIAL_STACK_CAPACITY) {
                // Stack was grown — now independently malloc'd
                xr_free(coro->vm_ctx.stack);
            }
            // If capacity matches initial, stack is still in slab — skip free
        } else {
            // malloc'd stack — recycle to per-Worker slab free list or free
            char *stack_end = (char*)coro->vm_ctx.stack + sizeof(XrValue) * coro->vm_ctx.stack_capacity;
            bool combined = (coro->vm_ctx.frames && (char*)coro->vm_ctx.frames == stack_end);
            XrWorker *w = xr_current_worker();
            if (combined && w &&
                coro->vm_ctx.stack_capacity == INITIAL_STACK_CAPACITY &&
                w->p.stack_slab_count < 4096) {
                *(void**)coro->vm_ctx.stack = w->p.stack_slab_free;
                w->p.stack_slab_free = coro->vm_ctx.stack;
                w->p.stack_slab_count++;
            } else {
                xr_free(coro->vm_ctx.stack);
                if (!combined && coro->vm_ctx.frames) {
                    xr_free(coro->vm_ctx.frames);
                }
            }
        }
        coro->vm_ctx.stack = NULL;
        coro->vm_ctx.frames = NULL;
    } else if (coro->vm_ctx.frames) {
        xr_free(coro->vm_ctx.frames);
        coro->vm_ctx.frames = NULL;
    }
    if (coro->vm_ctx.handlers) {
        xr_free(coro->vm_ctx.handlers);
        coro->vm_ctx.handlers = NULL;
        coro->vm_ctx.handler_count = 0;
        coro->vm_ctx.handler_capacity = 0;
    }
    // Per-frame struct storage
    if (coro->vm_ctx.struct_areas) {
        for (int i = 0; i < coro->vm_ctx.struct_areas_cap; i++) {
            if (coro->vm_ctx.struct_areas[i]) xr_free(coro->vm_ctx.struct_areas[i]);
        }
        xr_free(coro->vm_ctx.struct_areas);
        xr_free(coro->vm_ctx.struct_area_caps);
        coro->vm_ctx.struct_areas = NULL;
        coro->vm_ctx.struct_area_caps = NULL;
        coro->vm_ctx.struct_areas_cap = 0;
    }
    // Struct return arena
    if (coro->vm_ctx.struct_ret_arena) {
        xr_free(coro->vm_ctx.struct_ret_arena);
        coro->vm_ctx.struct_ret_arena = NULL;
        coro->vm_ctx.struct_ret_arena_used = 0;
        coro->vm_ctx.struct_ret_arena_cap = 0;
    }
    if (coro->ext && coro->ext->io_buf) {
        xr_free(coro->ext->io_buf);
        coro->ext->io_buf = NULL;
        coro->ext->io_buf_cap = 0;
    }
    coro->result = xr_null();
}

// Free coroutine internal resources (GC destructor)
// Note: coroutine itself freed by GC, only free internally allocated arrays
void xr_coro_free(XrCoroutine *coro) {
    if (!coro) return;

    // Free GC context — atomic exchange to prevent double-free race
    {
        XrCoroGC *gc = __atomic_exchange_n(&coro->coro_gc, NULL, __ATOMIC_ACQ_REL);
        if (gc) xr_coro_gc_destroy(gc);
    }
    // Free stack+frames (skip slab-embedded stacks — arena owns them)
    if (coro->vm_ctx.stack) {
        if (coro->gc_flags & 0x0001) {
            // Slab stack: only free if grown beyond initial capacity
            if (coro->vm_ctx.stack_capacity != INITIAL_STACK_CAPACITY) {
                xr_free(coro->vm_ctx.stack);
            }
        } else {
            char *stack_end = (char*)coro->vm_ctx.stack + sizeof(XrValue) * coro->vm_ctx.stack_capacity;
            bool combined = (coro->vm_ctx.frames && (char*)coro->vm_ctx.frames == stack_end);
            xr_free(coro->vm_ctx.stack);
            if (!combined && coro->vm_ctx.frames) {
                xr_free(coro->vm_ctx.frames);
            }
        }
        coro->vm_ctx.stack = NULL;
        coro->vm_ctx.frames = NULL;
    } else if (coro->vm_ctx.frames) {
        xr_free(coro->vm_ctx.frames);
        coro->vm_ctx.frames = NULL;
    }
    // Exception handlers (lazily allocated)
    if (coro->vm_ctx.handlers) {
        xr_free(coro->vm_ctx.handlers);
        coro->vm_ctx.handlers = NULL;
    }
    // Per-frame struct storage
    if (coro->vm_ctx.struct_areas) {
        for (int i = 0; i < coro->vm_ctx.struct_areas_cap; i++) {
            if (coro->vm_ctx.struct_areas[i]) xr_free(coro->vm_ctx.struct_areas[i]);
        }
        xr_free(coro->vm_ctx.struct_areas);
        xr_free(coro->vm_ctx.struct_area_caps);
        coro->vm_ctx.struct_areas = NULL;
        coro->vm_ctx.struct_area_caps = NULL;
        coro->vm_ctx.struct_areas_cap = 0;
    }
    // Struct return arena
    if (coro->vm_ctx.struct_ret_arena) {
        xr_free(coro->vm_ctx.struct_ret_arena);
        coro->vm_ctx.struct_ret_arena = NULL;
        coro->vm_ctx.struct_ret_arena_used = 0;
        coro->vm_ctx.struct_ret_arena_cap = 0;
    }
    // Cold extension (io_buf, locals, watched_by)
    if (coro->ext) {
        if (coro->ext->io_buf) xr_free(coro->ext->io_buf);
        xr_free(coro->ext);
        coro->ext = NULL;
    }

    // Only free non-inline args (allocated when >2 args)
    if (coro->args && coro->args != coro->inline_args) {
        xr_free(coro->args);
        coro->args = NULL;
    }

    // Coroutine object itself freed by GC, no need for xr_free(coro)
}

// Recycle coroutine to Worker local pool (thread-safe, lock-free)
// Key optimization: keep memory, only reset state, avoid repeated malloc/free
void xr_coro_recycle_local(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;
    XR_DCHECK(xr_coro_flags_has(coro, XR_CORO_FLG_DONE), "recycle_local: coro not done");
    XR_DCHECK(!coro->coro_gc || !coro->coro_gc->in_gc, "recycle_local: GC active during recycle");

 // Cancel timer using cross-worker cancellation
    // This handles both local (direct) and cross-worker (async queue) cancellation
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        xr_worker_cancel_timer(worker, coro);
        // Note: ext->timer_active is set to false inside xr_worker_cancel_timer
    }

    // Reset GC context: finalize objects, bulk free Immix blocks, reset state.
    // Uses xr_coro_gc_reset which handles large objects, shared_refs, and
    // finalizers correctly (the previous partial reset skipped those).
    if (coro->coro_gc) {
        xr_coro_gc_reset(coro->coro_gc, coro);
    }
    // Stack: only zero return slot. GC scan uses stack_top boundary (reset below),
    // so slots beyond stack_top are never scanned. Full memset is unnecessary.
    if (coro->vm_ctx.stack) {
        coro->vm_ctx.stack[0] = xr_null();
    }

    // Free non-inline args
    if (coro->args && coro->args != coro->inline_args) {
        xr_free(coro->args);
    }
    coro->args = NULL;

    // Thorough reset: zero all fields that coro_init_common would memset.
    // This allows coro_init_common to skip the 640B memset for recycled coros.
    coro->entry_type = XR_CORO_ENTRY_CLOSURE;
    coro->entry.closure = NULL;
    coro->result = xr_null();
    coro->error = xr_null();
    coro->task = NULL;
    coro->wait_channel = NULL;
    coro->await_results = NULL;
    coro->current_scope = NULL;
    coro->vm_ctx.stack_top = coro->vm_ctx.stack;
    coro->vm_ctx.frame_count = 0;
    coro->vm_ctx.handler_count = 0;
    coro->arg_count = 0;
    coro->sched_link = NULL;
    coro->next = NULL;
    coro->prev = NULL;
    coro->jit_ctx = NULL;
    coro->wait_link = NULL;
    coro->wait_send = false;
    coro->send_value = xr_null();
    coro->recv_slot = NULL;
    coro->recv_slot_offset = -1;
    coro->channel_deadline = 0;
    coro->select_wait = NULL;
    coro->select_ready_case = 0;
    coro->pending_result_slot = -1;
    coro->pending_spawn = NULL;
    coro->parent_coro = NULL;
    coro->spawn_line = 0;
    coro->spawn_file = NULL;
    // ext fields (yield_info, lock_count, locked_worker, locals, watched_by)
    // are reset in coro_init_common dirty path; ext pointer preserved for io_buf reuse
    coro->name = NULL;
    coro->source_file = NULL;
    coro->source_line = 0;
    coro->inline_args[0] = xr_null();
    coro->inline_args[1] = xr_null();
    coro->inline_args[2] = xr_null();
    coro->inline_args[3] = xr_null();
    atomic_store_explicit(&coro->flags, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_NONE, memory_order_relaxed);
    atomic_store_explicit(&coro->resume_status, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->affinity_p, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->wait_count, 0, memory_order_relaxed);
    atomic_store_explicit(&coro->any_done, false, memory_order_relaxed);
    atomic_store_explicit(&coro->await_task, NULL, memory_order_relaxed);
    // timer fields live in ext; reset happens in coro_init_common dirty path
    // Mark as "clean" — coro_init_common can skip memset
    coro->gc_flags |= XR_CORO_GC_RECYCLED_CLEAN;
    XR_DCHECK(coro->coro_gc == NULL || coro->coro_gc->gcstate == XGC_PAUSE,
              "recycle_local: GC not in PAUSE state");

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
void xr_sched_init(XrScheduler *sched) {
    if (!sched) return;

    // Initialize multi-level priority queues
    for (int i = 0; i < XR_CORO_PRIORITY_COUNT; i++) {
        sched->ready_head[i] = NULL;
        sched->ready_tail[i] = NULL;
    }
    sched->current = NULL;
    sched->coro_count = 0;
    sched->next_id = 1;
    sched->total_created = 0;

    // Initialize scope
    sched->current_scope = NULL;

    // Initialize named coroutine registry
    sched->coro_registry = (XrCoroRegistry *)xr_malloc(sizeof(XrCoroRegistry));
    if (sched->coro_registry) {
        xr_coro_registry_init(sched->coro_registry);
    }
}

// Destroy scheduler
void xr_sched_destroy(XrScheduler *sched) {
    if (!sched) return;

    // Coroutines managed by GC, just clear list refs
    for (int i = 0; i < XR_CORO_PRIORITY_COUNT; i++) {
        sched->ready_head[i] = NULL;
        sched->ready_tail[i] = NULL;
    }

    sched->current = NULL;
    sched->coro_count = 0;

    // Destroy named coroutine registry
    if (sched->coro_registry) {
        xr_coro_registry_destroy(sched->coro_registry);
        xr_free(sched->coro_registry);
        sched->coro_registry = NULL;
    }
}

// Add coroutine to ready queue tail (select queue by priority)
void xr_sched_enqueue(XrScheduler *sched, XrCoroutine *coro) {
    if (!sched || !coro) return;

    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_FLG_RUNNING);
    xr_coro_flags_set(coro, XR_CORO_FLG_READY);
    coro->next = NULL;

    // Select queue by priority (from flags)
    int prio = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (prio < 0) prio = 0;
    if (prio >= XR_CORO_PRIORITY_COUNT) prio = XR_CORO_PRIORITY_COUNT - 1;

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
void xr_sched_remove(XrScheduler *sched, XrCoroutine *target) {
    if (!sched || !target) return;

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
XrCoroutine *xr_sched_dequeue(XrScheduler *sched) {
    if (!sched) return NULL;

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

// ========== Channel Wake (ownership-safe routing, Phase 0) ==========
//
// Design (Phase 0 rewrite):
//   - Local worker: direct wake via xr_worker_wake_one / xr_worker_wake_select
//   - Remote workers: dispatch command via MPSC chan_wake_queue
//   - Never directly access remote worker's blocked buckets or run queues
//   - Uses channel waiter_worker_mask to skip workers with no waiters

XrCoroutine *xr_runtime_wake_channel(XrayIsolate *X, void *channel, bool wake_sender) {
    if (!X || !channel) return NULL;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return NULL;

    XrWorker *current = xr_current_worker();
    int current_id = current ? current->p.id : -1;

    // Step 1: Local worker — direct wake (owner-safe)
    if (current) {
        XrCoroutine *coro = xr_worker_wake_one(current, channel, wake_sender);
        if (coro) return coro;
        coro = xr_worker_wake_select(current, channel);
        if (coro) return coro;
    }

    // Step 2: Remote workers — dispatch via command queue (mask-guided)
    XrChannel *ch = (XrChannel *)channel;
    uint64_t mask = atomic_load_explicit(&ch->waiter_worker_mask, memory_order_acquire);
    // Clear local worker bit (already handled)
    if (current_id >= 0) mask &= ~((uint64_t)1 << current_id);

    while (mask) {
        int wid = __builtin_ctzll(mask);
        mask &= mask - 1;  // Clear lowest set bit
        if (wid >= runtime->worker_count) break;

        xr_worker_dispatch_chan_wake(runtime, wid, channel, wake_sender, false);
        // Only need to wake one waiter; remote worker will handle locally.
        // We can't know synchronously if the remote wake succeeded, so
        // we dispatch to all masked workers.  In practice, at most one
        // worker will find a waiter and the rest are fast no-ops.
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
    if (!X || !channel) return;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return;

    XrWorker *current = xr_current_worker();
    int current_id = current ? current->p.id : -1;

    // Local worker: direct wake (owner-safe)
    if (current) {
        xr_worker_wake_all(current, channel);
        while (xr_worker_wake_select(current, channel)) {
            // Keep waking until no more select waiters
        }
    }

    // Remote workers: dispatch close commands via mask
    XrChannel *ch = (XrChannel *)channel;
    uint64_t mask = atomic_load_explicit(&ch->waiter_worker_mask, memory_order_acquire);
    if (current_id >= 0) mask &= ~((uint64_t)1 << current_id);

    while (mask) {
        int wid = __builtin_ctzll(mask);
        mask &= mask - 1;
        if (wid >= runtime->worker_count) break;

        xr_worker_dispatch_chan_wake(runtime, wid, channel, false, true);
    }

    // Clear the mask — channel is closed, no future waiters expected.
    atomic_store_explicit(&ch->waiter_worker_mask, 0, memory_order_relaxed);
}

// ========== Deadlock Diagnosis ==========

// Format coroutine identifier into caller-provided buffer
static __attribute__((unused)) const char *format_coro_id(XrCoroutine *coro, char *buf, size_t bufsz) {
    if (coro->name) {
        snprintf(buf, bufsz, "#%d \"%s\"", coro->id, coro->name);
    } else {
        snprintf(buf, bufsz, "#%d", coro->id);
    }
    return buf;
}

// Print deadlock diagnosis info (simplified: blocked queue managed by Runtime)
static __attribute__((unused)) void xr_sched_print_deadlock(XrScheduler *sched) {
    if (!sched) return;

    int ready_count = 0;
    for (int prio = 0; prio < XR_CORO_PRIORITY_COUNT; prio++) {
        XrCoroutine *r = sched->ready_head[prio];
        while (r) { ready_count++; r = r->next; }
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
    (void)owning_gc;
    xr_coro_free((XrCoroutine *)obj);
}


// Cancel coroutine
// Cancel logic:
// 1. Cancel timer if sleeping (must happen before flags change)
// 2. Set CANCELLED and DONE flags
// 3. Clear blocked state
void xr_coro_cancel(XrCoroutine *coro) {
    if (!coro || xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) return;

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
    coro->wait_channel = NULL;
    coro->result = xr_null();
}

// ========== Scope Structured Concurrency ==========

// Add coroutine to current scope
//
// Per-coroutine scope tracking: prefer parent->current_scope,
// fallback to runtime/sched globals for main thread.
void xr_scope_add_coro(XrScheduler *sched, XrCoroutine *coro, XrCoroutine *parent) {
    if (!coro) return;

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

    if (!scope) return;  // Not in scope

    // Record belonging scope (decrement count on complete)
    coro->parent_scope = scope;
    atomic_fetch_add(&scope->count, 1);

    // Set waiter on task (scope wake coordination lives on Task)
    if (parent && coro->task) {
        atomic_fetch_add(&parent->wait_count, 1);
        coro->task->waiter = parent;
        coro->task->waiter_index = -2;  // scope mode
    }
}

// ========== Multi-core Runtime Initialization ==========

// Initialize multi-core runtime
// @param X          Isolate instance
// @param num_workers Worker count (0 means auto-detect CPU cores)
//
// Multi-core parallel execution:
// - Each Worker thread executes coroutines with independent vm_ctx
// - Work stealing for load balancing across Workers
// - Coroutines have independent stacks/frames, no global VM lock
void xr_multicore_init(XrayIsolate *X, int num_workers) {
    if (!X) return;

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
    if (!X || !X->vm.runtime) return;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;

    // Stop Runtime (if started)
    xr_runtime_stop(runtime);

    // Free resources
    xr_runtime_destroy(runtime);

    X->vm.runtime = NULL;
    X->vm.multicore_enabled = false;
}

// xr_current_coro - Get current coroutine
XrCoroutine *xr_current_coro(XrayIsolate *X) {
    if (!X) return NULL;

    XrWorker *worker = xr_current_worker();
    if (worker && worker->m && worker->m->current_coro) {
        return worker->m->current_coro;
    }
    // Fallback: before VM starts, use main coroutine
    return X->main_coro;
}

// xr_coro_ready - Wake coroutine
//
// Put coroutine into run queue
// next=true puts into runnext for priority execution
void xr_coro_ready(XrayIsolate *X, XrCoroutine *gp, bool next) {
    if (!X || !gp) return;

    // CAS loop: atomically BLOCKED -> READY (prevents double-wake)
    uint32_t old_flags = xr_coro_flags_load(gp);
    for (;;) {
        if (!(old_flags & XR_CORO_FLG_BLOCKED)) {
            return;  // Already woken by another thread
        }
        uint32_t new_flags = (old_flags & ~(XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK))
                           | XR_CORO_FLG_READY;
        if (xr_coro_flags_cas(gp, &old_flags, new_flags)) {
            break;  // CAS succeeded, we own the wake
        }
        // CAS failed, old_flags updated, retry
    }

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return;

    XrWorker *worker = xr_current_worker();
    if (worker && next) {
        // LIFO slot: woken coroutine runs immediately on current worker (DFS style)
        xr_worker_push_lifo(worker, gp);
    } else {
        // No current worker or next=false: send to affinity worker's inbox
        int target_id = atomic_load_explicit(&gp->affinity_p, memory_order_relaxed);
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

// xr_coro_wake_waiter - Wake waiter when coroutine completes
//
// All await coordination now lives on XrTask (Phase 0B/0C).
// This function handles scope count decrement and delegates to xr_task_wake_waiter.
void xr_coro_wake_waiter(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro) return;

    // Decrement scope count (if created in scope)
    XrScopeContext *scope = coro->parent_scope;
    if (scope) {
        /* Check child failure for linked/supervisor scope BEFORE clearing parent_scope.
         * task->error is already set by xr_task_fail() in xworker.c error path. */
        bool child_failed = false;
        if (scope->mode != XR_SCOPE_WAIT && coro->task) {
            XrValue err = coro->task->error;
            if (!XR_IS_NULL(err) && XR_IS_STRING(err)) {
                child_failed = true;
                if (scope->mode == XR_SCOPE_LINKED) {
                    // linked scope: record first error only
                    if (XR_IS_NULL(scope->first_error)) {
                        scope->first_error = err;
                    }
                } else if (scope->mode == XR_SCOPE_SUPERVISOR) {
                    // supervisor scope: collect ALL errors into array
                    if (!scope->errors) {
                        scope->errors = xr_array_with_capacity(coro, 4);
                    }
                    if (scope->errors) {
                        xr_array_push(scope->errors, err);
                    }
                }
            }
        }

        /* Remove this coro from scope's child list BEFORE sibling cancel.
         * Critical: coro pool recycle can reuse memory, causing list corruption
         * if a completed coro remains in the list. */
        XrCoroutine **pp = &scope->first_child;
        while (*pp) {
            if (*pp == coro) {
                *pp = coro->scope_sibling;
                coro->scope_sibling = NULL;
                break;
            }
            pp = &(*pp)->scope_sibling;
        }

        /* linked scope: cancel all siblings on first child failure.
         * Must also decrement scope count + wake waiter for each cancelled sib,
         * because xr_coro_cancel() doesn't go through the xworker completion path. */
        if (child_failed && scope->mode == XR_SCOPE_LINKED &&
            !atomic_exchange(&scope->cancel_requested, true)) {
            for (XrCoroutine *sib = scope->first_child; sib; sib = sib->scope_sibling) {
                if (!xr_coro_flags_has(sib, XR_CORO_FLG_DONE)) {
                    xr_coro_cancel(sib);
                    if (sib->task) {
                        xr_task_cancel(sib->task);
                    }
                    sib->parent_scope = NULL;
                    atomic_fetch_sub(&scope->count, 1);
                    if (sib->task) {
                        xr_task_wake_waiter(X, sib->task);
                    }
                }
            }
        }

        atomic_fetch_sub(&scope->count, 1);
        coro->parent_scope = NULL;
    }

    // Delegate to xr_task_wake_waiter (all go creates Task)
    if (coro->task) {
        xr_task_wake_waiter(X, coro->task);
    }
}

/* ========== Stack Growth ========== */

bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots) {
    if (!coro || !coro->vm_ctx.stack) return false;
    XR_DCHECK(extra_slots > 0, "grow_stack: non-positive extra_slots");
    XR_DCHECK(coro->vm_ctx.stack_capacity > 0, "grow_stack: zero stack_capacity");

    int new_capacity = coro->vm_ctx.stack_capacity + extra_slots;
    if (new_capacity > 1024 * 1024) return false;

    // Check if stack and frames are in a combined allocation block.
    // If frames pointer is right after the stack, they share one malloc.
    char *stack_end = (char*)coro->vm_ctx.stack + sizeof(XrValue) * coro->vm_ctx.stack_capacity;
    bool combined = ((char*)coro->vm_ctx.frames == stack_end);

    // Check if stack is from arena slab (gc_flags bit 0)
    bool slab_stack = (coro->gc_flags & 0x0001) != 0;

    if (combined) {
        // Split: allocate new separate stack, copy data
        XrValue *new_stack = (XrValue*)xr_malloc(sizeof(XrValue) * new_capacity);
        if (!new_stack) return false;
        memcpy(new_stack, coro->vm_ctx.stack, sizeof(XrValue) * coro->vm_ctx.stack_capacity);
        memset(new_stack + coro->vm_ctx.stack_capacity, 0, sizeof(XrValue) * extra_slots);

        // Allocate separate frames, copy from old combined block
        XrBcCallFrame *new_frames = (XrBcCallFrame*)xr_malloc(
            sizeof(XrBcCallFrame) * coro->vm_ctx.frame_capacity);
        if (!new_frames) { xr_free(new_stack); return false; }
        memcpy(new_frames, coro->vm_ctx.frames, sizeof(XrBcCallFrame) * coro->vm_ctx.frame_count);

        // Free old block only if it was malloc'd (not from arena slab)
        if (!slab_stack) {
            xr_free(coro->vm_ctx.stack);
        }
        coro->vm_ctx.stack = new_stack;
        coro->vm_ctx.stack_capacity = new_capacity;
        coro->vm_ctx.frames = new_frames;
        // Clear slab flag: stack is now independently malloc'd
        coro->gc_flags &= ~0x0001;
    } else {
        // Already separate
        if (slab_stack) {
            // Slab stack (not combined): malloc new, copy, don't free old
            XrValue *new_stack = (XrValue*)xr_malloc(sizeof(XrValue) * new_capacity);
            if (!new_stack) return false;
            memcpy(new_stack, coro->vm_ctx.stack, sizeof(XrValue) * coro->vm_ctx.stack_capacity);
            memset(new_stack + coro->vm_ctx.stack_capacity, 0, sizeof(XrValue) * extra_slots);
            coro->vm_ctx.stack = new_stack;
            coro->vm_ctx.stack_capacity = new_capacity;
            coro->gc_flags &= ~0x0001;
        } else {
            XrValue *new_stack = (XrValue*)xr_realloc(coro->vm_ctx.stack, sizeof(XrValue) * new_capacity);
            if (!new_stack) return false;
            memset(new_stack + coro->vm_ctx.stack_capacity, 0, sizeof(XrValue) * extra_slots);
            coro->vm_ctx.stack = new_stack;
            coro->vm_ctx.stack_capacity = new_capacity;
        }
    }

    if (coro->vm_ctx.frame_count + 8 >= coro->vm_ctx.frame_capacity) {
        int new_frame_cap = coro->vm_ctx.frame_capacity * 2;
        // If frames were from slab (now split out in combined path above),
        // they may already be malloc'd. But if slab_stack was true and we
        // only grew stack (not combined path), frames are still in slab.
        // Check: after combined split, frames are always malloc'd.
        // After non-combined slab grow, frames pointer still points to slab.
        bool frames_in_slab = slab_stack && !combined;
        if (frames_in_slab) {
            XrBcCallFrame *new_frames = (XrBcCallFrame*)xr_malloc(
                sizeof(XrBcCallFrame) * new_frame_cap);
            if (!new_frames) return false;
            memcpy(new_frames, coro->vm_ctx.frames,
                   sizeof(XrBcCallFrame) * coro->vm_ctx.frame_count);
            coro->vm_ctx.frames = new_frames;
        } else {
            XrBcCallFrame *new_frames = (XrBcCallFrame*)xr_realloc(
                coro->vm_ctx.frames, sizeof(XrBcCallFrame) * new_frame_cap);
            if (!new_frames) return false;
            coro->vm_ctx.frames = new_frames;
        }
        coro->vm_ctx.frame_capacity = new_frame_cap;
    }

    return true;
}
