/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_runtime.c - Minimal AOT runtime owner and entry execution path
 */

#include "xaot_runtime_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../runtime/core/xr_exec_context.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/core/xr_script_info.h"
#include "../runtime/mem/xalloc_unified.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/mem/xobj_destroy_ops.h"
#include "../runtime/object/xstring.h"
#include "xblock.h"
#include "xcoro_pool.h"
#include "xcoroutine.h"
#include "xscope_transfer.h"
#include "xtask.h"
#include "xworker.h"

typedef struct XrAotCoroState {
    const XrAotCoroDesc *desc;
    void *frame;
    XrAotRuntime *runtime;
} XrAotCoroState;

static XrCoroHeap *aot_runtime_control_heap(const XrAotContext *ctx) {
    if (ctx && ctx->coro)
        return xr_coro_ensure_heap(ctx->coro);
    if (ctx && ctx->runtime && ctx->runtime->root_coro)
        return xr_coro_ensure_heap(ctx->runtime->root_coro);
    return NULL;
}

int64_t xr_aot_runtime_live_bytes(const XrAotContext *ctx) {
    XrCoroHeap *heap = aot_runtime_control_heap(ctx);
    return heap ? heap->totalbytes : 0;
}

int64_t xr_aot_runtime_live_objects(const XrAotContext *ctx) {
    XrCoroHeap *heap = aot_runtime_control_heap(ctx);
    return heap ? (int64_t) heap->object_count : 0;
}

/* Process-global by design (see xsystem_heap.h): the counters are the same
 * ones the VM natives read, so the two backends report identical semantics. */
int64_t xr_aot_runtime_shared_bytes(const XrAotContext *ctx) {
    (void) ctx;
    return (int64_t) xr_sysheap_shared_live_bytes_total();
}

int64_t xr_aot_runtime_static_bytes(const XrAotContext *ctx) {
    (void) ctx;
    return (int64_t) xr_sysheap_static_alloc_bytes_total();
}

XrAotRuntimeInfo xr_aot_runtime_info(const XrAotContext *ctx) {
    XrAotRuntimeInfo info = {0};
    XrCoroHeap *heap = aot_runtime_control_heap(ctx);
    if (!heap)
        return info;
    XrRegionStats stats = {0};
    xr_region_get_stats(&heap->region, &stats);
    info.live_bytes = heap->totalbytes;
    info.live_kb = (double) heap->totalbytes / 1024.0;
    info.live_objects = (int64_t) heap->object_count;
    info.finalizer_count = (int64_t) heap->finalizer_count;
    info.blocks = (int64_t) stats.total_blocks;
    info.free_blocks = (int64_t) stats.free_blocks;
    info.full_blocks = (int64_t) stats.full_blocks;
    return info;
}

static XrAotCoroState *aot_state_from_coro(XrCoroutine *coro) {
    if (!coro || !coro->backend_state)
        return NULL;
    return (XrAotCoroState *) coro->backend_state;
}

static void aot_drop_coro_locals(XrCoroutine *coro, XrAotRuntime *runtime) {
    if (!coro || !runtime || xr_coro_flags_has(coro, XR_CORO_FLG_MAIN) ||
        XR_IS_NULL(runtime->coro_locals) || !runtime->value_ops ||
        !runtime->value_ops->map_delete) {
        return;
    }
    XrValue owner_key = xr_int((int64_t) coro->id + 1);
    while (atomic_flag_test_and_set_explicit(&runtime->coro_locals_lock, memory_order_acquire))
        ;
    runtime->value_ops->map_delete(runtime->coro_locals, owner_key);
    atomic_flag_clear_explicit(&runtime->coro_locals_lock, memory_order_release);
}

static void aot_release_frame(const XrAotCoroDesc *desc, void *frame, XrCoroHeap *heap) {
    if (!frame)
        return;
    if (desc && desc->release_frame) {
        desc->release_frame(frame, heap);
        return;
    }
    xr_aot_frame_free(frame);
}

/* A cancelled coroutine stops between statements, so no return edge drains its
 * defer scope. Run it here, on the owner worker, before the task completes --
 * the frame stays intact and its release path re-runs the (now empty) scope
 * idempotently. */
static bool aot_has_cancellation_cleanup(const XrCoroutine *coro) {
    const XrAotCoroState *state = aot_state_from_coro((XrCoroutine *) coro);
    return state && state->frame && state->desc && state->desc->run_pending_defers;
}

static void aot_run_cancellation_cleanup(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state || !state->frame || !state->desc || !state->desc->run_pending_defers)
        return;
    XrExecutionContext *previous = xr_exec_context_enter(&coro->exec_ctx);
    state->desc->run_pending_defers(state->frame);
    xr_exec_context_restore(previous);
}

static void aot_release_state(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return;

    aot_drop_coro_locals(coro, state->runtime);
    aot_release_frame(state->desc, state->frame, coro ? coro->heap : NULL);
    xr_free(state);
    coro->backend_state = NULL;
    coro->backend = NULL;
    coro->gc_flags &= ~XR_CORO_GC_BACKEND_STATE_OWNED;
}

static void aot_clear_reusable_state(XrCoroutine *coro, XrAotCoroState *state) {
    if (!state)
        return;

    aot_drop_coro_locals(coro, state->runtime);
    aot_release_frame(state->desc, state->frame, coro ? coro->heap : NULL);
    state->desc = NULL;
    state->frame = NULL;
    state->runtime = NULL;
}

static bool aot_prepare_recycle(XrCoroutine *coro, XrWorker *worker) {
    (void) worker;
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return false;

    bool trim_backend_storage = (coro->gc_flags & XR_CORO_GC_TRIM_BACKEND_STORAGE) != 0;
    coro->gc_flags &= ~XR_CORO_GC_TRIM_BACKEND_STORAGE;
    aot_clear_reusable_state(coro, state);
    if (trim_backend_storage) {
        xr_free(state);
        coro->backend_state = NULL;
        coro->backend = NULL;
        coro->gc_flags &= ~XR_CORO_GC_BACKEND_STATE_OWNED;
    }
    return true;
}

static void aot_reset_reusable(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return;

    bool trim_backend_storage = (coro->gc_flags & XR_CORO_GC_TRIM_BACKEND_STORAGE) != 0;
    coro->gc_flags &= ~XR_CORO_GC_TRIM_BACKEND_STORAGE;
    aot_clear_reusable_state(coro, state);
    if (trim_backend_storage) {
        xr_free(state);
        coro->backend_state = NULL;
        coro->backend = NULL;
        coro->gc_flags &= ~XR_CORO_GC_BACKEND_STATE_OWNED;
    }
}

static const char *aot_backend_debug_name(const XrCoroutine *coro) {
    const XrAotCoroState *state = coro ? (const XrAotCoroState *) coro->backend_state : NULL;
    if (state && state->desc && state->desc->name)
        return state->desc->name;
    return "aot";
}

static void aot_backend_debug_snapshot(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot) {
    if (!snapshot)
        return;
    snapshot->backend_name = "aot";
    snapshot->function_name = aot_backend_debug_name(coro);
    snapshot->frame_count = coro && coro->backend_state ? 1 : 0;
    snapshot->in_c_frame = 0;
}

static void aot_backend_trace_roots(XrCoroutine *coro, void *visitor) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (state && state->desc && state->desc->trace_roots)
        state->desc->trace_roots(state->frame, visitor);
}

static void aot_mark_running(XrCoroutine *coro) {
    xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED,
                       XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);
}

static bool aot_caps_need_scheduler(uint32_t caps) {
    return (caps & (XR_AOT_CAP_CORO | XR_AOT_CAP_TIMER | XR_AOT_CAP_CHANNEL |
                    XR_AOT_CAP_WORK_QUEUE | XR_AOT_CAP_RESULT_GROUP | XR_AOT_CAP_COUNTDOWN_LATCH |
                    XR_AOT_CAP_SEMAPHORE | XR_AOT_CAP_EVENT_COUNT | XR_AOT_CAP_PARALLEL)) != 0;
}

static void *aot_host_backend_context(void *ctx) {
    return ctx;
}

static const XrSchedulerHostOps AOT_SCHEDULER_HOST_OPS = {
    .backend_context = aot_host_backend_context,
};

static void aot_runtime_configure_core(XrAotRuntime *runtime, const XrAotRuntimeConfig *cfg) {
    if (!runtime || !runtime->core)
        return;
    xr_runtime_core_enable_basic_destroy_ops(runtime->core);
    xr_runtime_core_set_aot_native_value_release(
        runtime->core, runtime->value_ops ? runtime->value_ops->release : NULL);
    if (cfg && cfg->configure_core)
        cfg->configure_core(runtime->core, runtime->caps, cfg->userdata);
}

void xr_aot_runtime_enable_transfer(XrAotRuntime *runtime) {
    xr_scope_transfer_enable_core(xr_aot_runtime_core(runtime));
}

#define XR_AOT_VALUE_TAG_STR 14
#define XR_AOT_VALUE_TAG_STR_ARC 19
typedef struct XrAotStringView {
    int64_t len;
    int64_t rune_len;
    uint32_t hash;
    uint32_t flags;
    char *data;
} XrAotStringView;

static bool aot_value_is_aot_string(XrValue value) {
    return value.tag == XR_AOT_VALUE_TAG_STR || value.tag == XR_AOT_VALUE_TAG_STR_ARC;
}

static XrValue aot_bridge_string_to_runtime(XrRuntimeCore *core, XrValue value) {
    if (!core || !aot_value_is_aot_string(value) || !value.ptr)
        return value;
    const XrAotStringView *src = (const XrAotStringView *) value.ptr;
    if (!src->data || src->len < 0)
        return XR_NULL_VAL;
    XrString *dst = xr_string_intern_core(core, src->data, (size_t) src->len, src->hash);
    return dst ? xr_string_value(dst) : XR_NULL_VAL;
}

static XrValue aot_bridge_result_value_to_runtime(XrRuntimeCore *core, XrValue value) {
    if (aot_value_is_aot_string(value))
        return aot_bridge_string_to_runtime(core, value);
    /* Header-bearing AOT values cross scheduler boundaries by identity.  The
     * canonical storage bits prove whether the root is transferable/shared;
     * runtime->value_ops supplies the matching native destructor if a holder
     * abandons it. */
    return value;
}

static XrValue aot_bridge_result_to_runtime(XrCoroutine *coro, XrAotRuntime *runtime,
                                            XrValue value) {
    XrRuntimeCore *core = coro && coro->core ? coro->core : xr_aot_runtime_core(runtime);
    return aot_bridge_result_value_to_runtime(core, value);
}

/* Uncaught value-return error diagnostic for a coroutine nothing is left to
 * observe (spec §8.1.1). Mirrors the VM's run_finalize in xvm_coro_backend.c:
 * an awaited or scope-linked coroutine's error is surfaced by the awaiting task
 * or the parent scope, so reprinting it here would double-report. The one shape
 * that would otherwise vanish silently is a statement-form fire-and-forget
 * `go f()` — no Task handle, no parent scope.
 *
 * The main coroutine is excluded on purpose: the generated entry reports the
 * top-level case itself after xr_aot_run_main returns. */
static void aot_report_dropped_error(XrCoroutine *coro, XrAotRuntime *runtime, XrValue error) {
    if (!coro || !runtime || XR_IS_NULL(error))
        return;
    if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN) || coro->task || xr_coro_parent_scope(coro))
        return;
    if (runtime->value_ops && runtime->value_ops->report_uncaught_error)
        runtime->value_ops->report_uncaught_error(error, /*in_go_coroutine=*/true);
}

static bool aot_coro_cancelled(const XrCoroutine *coro) {
    return coro && xr_coro_flags_has((XrCoroutine *) coro,
                                     XR_CORO_FLG_CANCEL_REQUESTED | XR_CORO_FLG_CANCELLED);
}

static XrCoroRunResult aot_map_result(XrCoroutine *coro, XrAotResult result) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    XrAotRuntime *runtime = state ? state->runtime : NULL;
    switch (result.kind) {
        case XR_AOT_RUN_DONE:
            result.value = aot_bridge_result_to_runtime(coro, runtime, result.value);
            coro->result = result.value;
            return xr_coro_run_done(result.value);
        case XR_AOT_RUN_BLOCKED:
            (void) xr_coro_finalize_blocked_suspend(coro);
            return xr_coro_run_result(XR_CORO_RUN_BLOCKED);
        case XR_AOT_RUN_YIELD:
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_AOT_RUN_GEN_YIELD:
            result.value = aot_bridge_result_to_runtime(coro, runtime, result.value);
            coro->result = result.value;
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_AOT_RUN_SPAWN_CHILD:
            return xr_coro_run_spawn_child(result.child);
        case XR_AOT_RUN_CANCELLED:
            return xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        case XR_AOT_RUN_ERROR:
            if (result.error_is_value) {
                /* Report before bridging: the reporter is the generated
                 * program's own formatter, so it must see the AOT-native value.
                 * Bridging rewrites an AOT string into a runtime XrString,
                 * which that formatter cannot read. */
                aot_report_dropped_error(coro, runtime, result.error);
                result.error = aot_bridge_result_to_runtime(coro, runtime, result.error);
            }
            return xr_coro_run_error(result.error, result.error_is_value);
        default:
            return xr_coro_run_error(result.error, result.error_is_value);
    }
}

static bool aot_resume_precheck(XrCoroutine *coro, const XrCoroEvent *event,
                                XrAotCoroState **state_out, XrCoroRunResult *result_out) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!coro || !state || !state->desc || !state->desc->resume) {
        if (result_out)
            *result_out = xr_coro_run_error(XR_NULL_VAL, false);
        return false;
    }
    if (event && event->kind == XR_CORO_EVENT_CANCEL) {
        if (result_out)
            *result_out = xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        return false;
    }
    if (aot_coro_cancelled(coro)) {
        if (result_out)
            *result_out = xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        return false;
    }

    aot_mark_running(coro);
    if (state_out)
        *state_out = state;
    return true;
}

static XrCoroRunResult aot_runtime_backend_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                                  const XrCoroRunContext *run_ctx) {
    XrAotCoroState *state = NULL;
    XrCoroRunResult precheck;
    if (!aot_resume_precheck(coro, event, &state, &precheck))
        return precheck;

    XrAotContext ctx;
    ctx.runtime = state->runtime;
    ctx.coro = coro;
    ctx.vm_host_ops = NULL;
    ctx.vm_host = NULL;
    ctx.worker = run_ctx ? (void *) run_ctx->worker : NULL;

    return aot_map_result(coro, state->desc->resume(state->frame, &ctx));
}

// Synchronous generator pull: run the (non-scheduled) AOT coroutine to its next
// `yield expr` or completion without touching the worker run queue.
static XrCoroRunKind aot_backend_gen_drive(XrCoroutine *coro, XrValue *out) {
    if (out)
        *out = XR_NULL_VAL;
    if (!coro)
        return XR_CORO_RUN_ERROR;
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE))
        return XR_CORO_RUN_DONE;

    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state || !state->desc || !state->desc->resume)
        return XR_CORO_RUN_ERROR;

    XrAotContext ctx;
    ctx.runtime = state->runtime;
    ctx.coro = coro;
    ctx.vm_host_ops = NULL;
    ctx.vm_host = NULL;
    ctx.worker = NULL;

    aot_mark_running(coro);
    XrAotResult result = state->desc->resume(state->frame, &ctx);

    switch (result.kind) {
        case XR_AOT_RUN_GEN_YIELD:
            coro->result = result.value;
            xr_coro_transition_to_ready(coro);
            if (out)
                *out = result.value;
            return XR_CORO_RUN_YIELD;
        case XR_AOT_RUN_DONE:
            coro->result = result.value;
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            if (out)
                *out = result.value;
            return XR_CORO_RUN_DONE;
        case XR_AOT_RUN_ERROR:
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            coro->error = result.error;
            coro->error_is_value = result.error_is_value;
            if (out)
                *out = result.error;
            return XR_CORO_RUN_ERROR;
        default:
            /* Generators are pure value producers; channel/await suspend is invalid. */
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            coro->error = XR_NULL_VAL;
            coro->error_is_value = false;
            return XR_CORO_RUN_ERROR;
    }
}

XR_FUNC XrAotGenDriveKind xr_aot_gen_drive(XrCoroutine *coro, XrValue *out,
                                           bool *out_error_is_value) {
    if (out)
        *out = XR_NULL_VAL;
    if (out_error_is_value)
        *out_error_is_value = false;
    if (!coro || !coro->backend || !coro->backend->gen_drive)
        return XR_AOT_GEN_DRIVE_ERROR;
    XrCoroRunKind kind = coro->backend->gen_drive(coro, out);
    switch (kind) {
        case XR_CORO_RUN_YIELD:
            return XR_AOT_GEN_DRIVE_YIELD;
        case XR_CORO_RUN_DONE:
            return XR_AOT_GEN_DRIVE_DONE;
        default:
            if (out_error_is_value)
                *out_error_is_value = coro->error_is_value;
            if (out && XR_IS_NULL(*out))
                *out = coro->error;
            return XR_AOT_GEN_DRIVE_ERROR;
    }
}

static const XrCoroBackendVTable aot_runtime_backend_vtable = {
    .kind = XR_CORO_BACKEND_AOT,
    .resume = aot_runtime_backend_resume,
    .gen_drive = aot_backend_gen_drive,
    .has_cancellation_cleanup = aot_has_cancellation_cleanup,
    .run_cancellation_cleanup = aot_run_cancellation_cleanup,
    .trace_roots = aot_backend_trace_roots,
    .prepare_recycle = aot_prepare_recycle,
    .reset_reusable = aot_reset_reusable,
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
    .release = aot_release_state,
    .destroy = aot_release_state,
    .debug_name = aot_backend_debug_name,
    .debug_snapshot = aot_backend_debug_snapshot,
};

/*
 * AOT frames are extremely short-lived in hot `go` fan-out/fan-in code. A
 * process-global cache was measured and rejected because the shared lock
 * becomes a cross-worker contention point. Keep the cache strictly
 * thread-local and record the size class in a small header so generated
 * release functions can keep the stable xr_aot_frame_free(frame) ABI.
 */
#define XR_AOT_FRAME_CACHE_BUCKETS 9u
#define XR_AOT_FRAME_CACHE_MAX_PER_BUCKET 64u
#define XR_AOT_FRAME_CACHE_MAX_SIZE 4096u

typedef union XrAotFrameHeader {
    struct {
        union XrAotFrameHeader *next;
        size_t size_class;
        uintptr_t owner_token;
        uint32_t bucket;
    } h;
#if defined(_MSC_VER)
    __declspec(align(16)) unsigned char align[16];
#else
    max_align_t align;
#endif
} XrAotFrameHeader;

static XR_THREAD_LOCAL XrAotFrameHeader *tls_aot_frame_cache[XR_AOT_FRAME_CACHE_BUCKETS];
static XR_THREAD_LOCAL uint16_t tls_aot_frame_cache_count[XR_AOT_FRAME_CACHE_BUCKETS];
static XR_THREAD_LOCAL unsigned char tls_aot_frame_owner_marker;

static uintptr_t aot_frame_owner_token(void) {
    return (uintptr_t) &tls_aot_frame_owner_marker;
}

static bool aot_frame_size_class(size_t size, size_t *out_size_class, uint32_t *out_bucket) {
    if (!out_size_class || !out_bucket || size == 0 || size > XR_AOT_FRAME_CACHE_MAX_SIZE)
        return false;
    size_t size_class = 16;
    uint32_t bucket = 0;
    while (size_class < size && bucket + 1 < XR_AOT_FRAME_CACHE_BUCKETS) {
        size_class <<= 1;
        bucket++;
    }
    if (size > size_class || size_class > XR_AOT_FRAME_CACHE_MAX_SIZE)
        return false;
    *out_size_class = size_class;
    *out_bucket = bucket;
    return true;
}

void *xr_aot_frame_alloc(size_t size) {
    if (size == 0)
        size = 1;

    uintptr_t owner_token = aot_frame_owner_token();
    size_t size_class = 0;
    uint32_t bucket = UINT32_MAX;
    bool cacheable = aot_frame_size_class(size, &size_class, &bucket);
    if (cacheable) {
        XrAotFrameHeader *header = tls_aot_frame_cache[bucket];
        if (header) {
            tls_aot_frame_cache[bucket] = header->h.next;
            tls_aot_frame_cache_count[bucket]--;
            header->h.next = NULL;
            header->h.owner_token = owner_token;
            return (void *) (header + 1);
        }
    } else {
        size_class = size;
    }

    if (size_class > SIZE_MAX - sizeof(XrAotFrameHeader))
        return NULL;
    XrAotFrameHeader *header =
        (XrAotFrameHeader *) xr_malloc(sizeof(XrAotFrameHeader) + size_class);
    if (!header)
        return NULL;
    header->h.next = NULL;
    header->h.size_class = size_class;
    header->h.owner_token = owner_token;
    header->h.bucket = cacheable ? bucket : UINT32_MAX;
    return (void *) (header + 1);
}

void xr_aot_frame_free(void *frame) {
    if (!frame)
        return;
    XrAotFrameHeader *header = ((XrAotFrameHeader *) frame) - 1;
    uint32_t bucket = header->h.bucket;
    if (header->h.owner_token == aot_frame_owner_token() && bucket < XR_AOT_FRAME_CACHE_BUCKETS &&
        tls_aot_frame_cache_count[bucket] < XR_AOT_FRAME_CACHE_MAX_PER_BUCKET) {
        header->h.next = tls_aot_frame_cache[bucket];
        tls_aot_frame_cache[bucket] = header;
        tls_aot_frame_cache_count[bucket]++;
        return;
    }
    xr_free(header);
}

void xr_aot_runtime_config_init(XrAotRuntimeConfig *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
}

/* Process-wide "current" standalone AOT runtime. Standalone binaries create
 * exactly one runtime in main() before any user code runs, so a relaxed
 * atomic pointer is sufficient; VM-hosted execution never registers one. */
static _Atomic(XrAotRuntime *) g_aot_runtime_current;

XrAotRuntime *xr_aot_runtime_current(void) {
    return atomic_load_explicit(&g_aot_runtime_current, memory_order_acquire);
}

XrAotRuntime *xr_aot_runtime_new(const XrAotRuntimeConfig *cfg) {
    XrAotRuntimeConfig local_cfg;
    if (cfg) {
        local_cfg = *cfg;
    } else {
        xr_aot_runtime_config_init(&local_cfg);
    }

    XrAotRuntime *runtime = (XrAotRuntime *) xr_calloc(1, sizeof(XrAotRuntime));
    if (!runtime)
        return NULL;
    runtime->caps = local_cfg.caps;
    runtime->value_ops = local_cfg.value_ops;
    runtime->coro_locals = XR_NULL_VAL;
    atomic_flag_clear_explicit(&runtime->coro_locals_lock, memory_order_relaxed);
    for (int i = 0; i < XR_USER_GLOBALS_START; i++)
        runtime->builtins[i] = XR_NULL_VAL;

    XrRuntimeCoreConfig core_cfg = {
        .owner_isolate = NULL,
        .userdata = local_cfg.userdata,
    };
    runtime->core = xr_runtime_core_new(&core_cfg);
    if (!runtime->core)
        goto fail;
    aot_runtime_configure_core(runtime, &local_cfg);
    xr_script_info_set(&runtime->core->script_info, local_cfg.file, local_cfg.argc, local_cfg.argv);

    if (aot_caps_need_scheduler(runtime->caps)) {
        runtime->scheduler = xr_scheduler_runtime_new(runtime->core, local_cfg.scheduler_workers);
        if (!runtime->scheduler)
            goto fail;
        XrSchedulerHost host = {
            .ops = &AOT_SCHEDULER_HOST_OPS,
            .ctx = runtime,
        };
        xr_scheduler_runtime_attach_host(runtime->scheduler, &host);
        xr_runtime_start(runtime->scheduler);
    }

    atomic_store_explicit(&g_aot_runtime_current, runtime, memory_order_release);
    return runtime;

fail:
    xr_aot_runtime_delete(runtime);
    return NULL;
}

void xr_aot_runtime_delete(XrAotRuntime *runtime) {
    if (!runtime)
        return;
    XrAotRuntime *expected = runtime;
    atomic_compare_exchange_strong(&g_aot_runtime_current, &expected, NULL);
    if (runtime->root_coro) {
        xr_coro_destroy(runtime->root_coro);
        runtime->root_coro = NULL;
    }
    if (runtime->scheduler) {
        xr_scheduler_runtime_delete(runtime->scheduler);
        runtime->scheduler = NULL;
    }
    if (runtime->root_scope) {
        xr_free(runtime->root_scope);
        runtime->root_scope = NULL;
    }
    if (!XR_IS_NULL(runtime->coro_locals) && runtime->value_ops && runtime->value_ops->release) {
        runtime->value_ops->release(runtime->coro_locals);
        runtime->coro_locals = XR_NULL_VAL;
    }
    if (runtime->core) {
        xr_runtime_core_delete(runtime->core);
        runtime->core = NULL;
    }
    xr_free(runtime);
}

uint32_t xr_aot_runtime_caps(const XrAotRuntime *runtime) {
    return runtime ? runtime->caps : XR_AOT_CAP_NONE;
}

XrRuntimeCore *xr_aot_runtime_core(XrAotRuntime *runtime) {
    return runtime ? runtime->core : NULL;
}

XrRuntime *xr_aot_runtime_scheduler(XrAotRuntime *runtime) {
    return runtime ? runtime->scheduler : NULL;
}

XrValue xr_aot_runtime_builtin(const XrAotRuntime *runtime, int32_t index) {
    if (!runtime || index < 0 || index >= XR_USER_GLOBALS_START)
        return XR_NULL_VAL;
    XrValue value = runtime->builtins[index];
    if (!XR_IS_NULL(value))
        return value;
    return xr_runtime_core_builtin(runtime->core, index);
}

void xr_aot_runtime_set_builtin(XrAotRuntime *runtime, int32_t index, XrValue value) {
    if (!runtime || index < 0 || index >= XR_USER_GLOBALS_START)
        return;
    runtime->builtins[index] = value;
    xr_runtime_core_set_builtin(runtime->core, index, value);
}

XrCoroutine *xr_coro_create_aot(XrAotRuntime *runtime, const XrAotCoroDesc *desc, void *frame,
                                const char *name) {
    if (!runtime || !desc || !desc->resume || !frame) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }

    XrRuntimeCore *core = xr_aot_runtime_core(runtime);
    XrRuntime *scheduler = xr_aot_runtime_scheduler(runtime);
    if (!core || !scheduler) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }

    XrCoroutine *coro = xr_coro_create_runtime_empty(core, scheduler, name ? name : desc->name);
    if (!coro) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }

    /* Task 250: an execution that runs language code owns an exec heap from
     * birth, so its EXEC_LOCAL allocation context never falls through to
     * storage that outlives the execution. The VM wires this inside
     * xr_coro_init_shell_owner behind need_storage, which requires a backend
     * prepare_execution_state hook the AOT backend does not have — so the
     * runtime-empty shell arrives heapless and the wiring happens here.
     * Generated code does reach this context: channel Recv/SendResult ADT
     * wrappers and friends allocate through coro->alloc_ctx. */
    if (!xr_coro_ensure_heap(coro)) {
        xr_coro_discard_runtime_empty(scheduler, coro);
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }

    if (coro->backend && coro->backend != &aot_runtime_backend_vtable) {
        if (coro->backend->destroy) {
            coro->backend->destroy(coro);
        } else {
            coro->backend = NULL;
            coro->backend_state = NULL;
            coro->gc_flags &= ~XR_CORO_GC_BACKEND_STATE_OWNED;
        }
    }

    XrAotCoroState *state = NULL;
    if (coro->backend == &aot_runtime_backend_vtable)
        state = aot_state_from_coro(coro);
    if (!state) {
        state = (XrAotCoroState *) xr_calloc(1, sizeof(XrAotCoroState));
        if (!state) {
            xr_coro_discard_runtime_empty(scheduler, coro);
            aot_release_frame(desc, frame, NULL);
            return NULL;
        }
    }
    state->desc = desc;
    state->frame = frame;
    state->runtime = runtime;

    xr_coro_attach_backend(coro, &aot_runtime_backend_vtable, state);
    coro->gc_flags |= XR_CORO_GC_BACKEND_STATE_OWNED;
    return coro;
}

XrValue xr_aot_run_main(XrAotRuntime *runtime, const XrAotCoroDesc *desc, void *frame) {
    XrCoroutine *main_coro = xr_coro_create_aot(runtime, desc, frame, "main");
    if (!main_coro)
        return XR_NULL_VAL;
    /* The language-level main coroutine has the stable cross-backend identity
     * 0.  Runtime-created children start at 1 in both VM and standalone AOT. */
    main_coro->id = 0;
    if (runtime->scheduler)
        atomic_store_explicit(&runtime->scheduler->next_coro_id, 1, memory_order_relaxed);
    (void) xr_coro_set_source(main_coro, runtime->core ? runtime->core->script_info.file : NULL, 0);
    xr_runtime_main_thread_run(xr_aot_runtime_scheduler(runtime), main_coro);
    XrValue result = main_coro->result;
    /* main_thread_run has joined every worker before returning, so the owner
     * timer wheel + cancel queue are quiescent. Detach the main coro's embedded
     * timer node now: destroying it here (before xr_aot_runtime_delete runs the
     * scheduler shutdown cancel-queue drain) would otherwise strand a dangling
     * ZOMBIE node in the owner cancel queue — a heap-use-after-free the drain
     * reads. xr_current_worker() is NULL at this point, so the owner-thread
     * detach in xr_coro_free cannot cover it. */
    xr_coro_detach_timer_quiescent(xr_aot_runtime_scheduler(runtime), main_coro);
    xr_coro_destroy(main_coro);
    return result;
}

static XrAotResult aot_root_descriptor_resume(void *raw_scope, const XrAotContext *ctx) {
    XrScopeContext *scope = (XrScopeContext *) raw_scope;
    (void) ctx;
    if (!scope)
        return xr_aot_error(XR_NULL_VAL, false);
    if (atomic_load_explicit(&scope->count, memory_order_acquire) != 0)
        return xr_aot_yielded();
    if (!XR_IS_NULL(scope->first_error))
        return xr_aot_error(scope->first_error, scope->first_error_is_value);
    return xr_aot_done(XR_NULL_VAL);
}

static void aot_root_descriptor_release(void *frame, XrCoroHeap *heap) {
    (void) frame;
    (void) heap;
}

static const XrAotCoroDesc AOT_ROOT_DESCRIPTOR = {
    .name = "root-descriptor",
    .frame_size = 0,
    .root_count = 0,
    .release_count = 0,
    .resume = aot_root_descriptor_resume,
    .trace_roots = NULL,
    .release_frame = aot_root_descriptor_release,
};

bool xr_aot_root_descriptor_begin(XrAotRuntime *runtime) {
    XrScopeContext *scope;
    XrCoroutine *root;
    if (!runtime || !runtime->scheduler || runtime->root_scope || runtime->root_coro)
        return false;
    scope = (XrScopeContext *) xr_calloc(1, sizeof(XrScopeContext));
    if (!scope)
        return false;
    atomic_init(&scope->count, 0);
    atomic_init(&scope->cancel_requested, false);
    atomic_init(&scope->child_lock, false);
    /* Root descriptor waits for top-level children without changing ordinary
     * `go` error isolation. Only an explicit `linked scope` carries policy. */
    scope->mode = XR_SCOPE_WAIT;
    scope->first_error = XR_NULL_VAL;
    root = xr_coro_create_aot(runtime, &AOT_ROOT_DESCRIPTOR, scope, "root-descriptor");
    if (!root) {
        xr_free(scope);
        return false;
    }
    scope->owner = root;
    atomic_store_explicit(&root->current_scope, scope, memory_order_release);
    runtime->root_scope = scope;
    runtime->root_coro = root;
    runtime->scheduler->current_scope = scope;
    return true;
}

bool xr_aot_root_descriptor_end(XrAotRuntime *runtime) {
    XrScopeContext *scope;
    XrCoroutine *root;
    bool ok;
    if (!runtime || !runtime->scheduler || !(scope = runtime->root_scope) ||
        !(root = runtime->root_coro))
        return false;
    ok =
        xr_runtime_main_thread_run(runtime->scheduler, root) == 0 && XR_IS_NULL(scope->first_error);
    runtime->scheduler->current_scope = NULL;
    atomic_store_explicit(&root->current_scope, NULL, memory_order_release);
    runtime->root_scope = NULL;
    runtime->root_coro = NULL;
    xr_coro_destroy(root);
    xr_free(scope);
    return ok;
}

XrAotResult xr_aot_sleep(const XrAotContext *ctx, int64_t milliseconds) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    if (aot_coro_cancelled(ctx->coro))
        return xr_aot_result(XR_AOT_RUN_CANCELLED);
    XrCoroBlockResult block = xr_coro_sleep(ctx->coro, milliseconds);
    if (aot_coro_cancelled(ctx->coro))
        return xr_aot_result(XR_AOT_RUN_CANCELLED);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_result(XR_AOT_RUN_DONE);
    return xr_aot_error(XR_NULL_VAL, false);
}
