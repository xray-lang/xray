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
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/core/xr_script_info.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/mem/xobj_destroy_ops.h"
#include "xblock.h"
#include "xcoro_pool.h"
#include "xcoroutine.h"
#include "xscope_transfer.h"
#include "xworker.h"

typedef struct XrAotCoroState {
    const XrAotCoroDesc *desc;
    void *frame;
    XrAotRuntime *runtime;
} XrAotCoroState;

static XrAotCoroState *aot_state_from_coro(XrCoroutine *coro) {
    if (!coro || !coro->backend_state)
        return NULL;
    return (XrAotCoroState *) coro->backend_state;
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

static void aot_release_state(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return;

    aot_release_frame(state->desc, state->frame, coro ? coro->heap : NULL);
    xr_free(state);
    coro->backend_state = NULL;
    coro->backend = NULL;
    coro->gc_flags &= ~XR_CORO_GC_BACKEND_STATE_OWNED;
}

static void aot_clear_reusable_state(XrCoroutine *coro, XrAotCoroState *state) {
    if (!state)
        return;

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
                    XR_AOT_CAP_SEMAPHORE | XR_AOT_CAP_EVENT_COUNT)) != 0;
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
    if (cfg && cfg->configure_core)
        cfg->configure_core(runtime->core, runtime->caps, cfg->userdata);
}

void xr_aot_runtime_enable_transfer(XrAotRuntime *runtime) {
    (void) xr_aot_runtime_builtin_lazy(runtime, XR_GLOBAL_VAR_TASK_OUTCOME);
    xr_scope_transfer_enable_core(xr_aot_runtime_core(runtime));
}

static bool aot_coro_cancelled(const XrCoroutine *coro) {
    return coro && xr_coro_flags_has((XrCoroutine *) coro,
                                     XR_CORO_FLG_CANCEL_REQUESTED | XR_CORO_FLG_CANCELLED);
}

static XrCoroRunResult aot_map_result(XrCoroutine *coro, XrAotResult result) {
    switch (result.kind) {
        case XR_AOT_RUN_DONE:
            coro->result = result.value;
            return xr_coro_run_done(result.value);
        case XR_AOT_RUN_BLOCKED:
            (void) xr_coro_finalize_blocked_suspend(coro);
            return xr_coro_run_result(XR_CORO_RUN_BLOCKED);
        case XR_AOT_RUN_YIELD:
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_AOT_RUN_GEN_YIELD:
            coro->result = result.value;
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_AOT_RUN_SPAWN_CHILD:
            return xr_coro_run_spawn_child(result.child);
        case XR_AOT_RUN_CANCELLED:
            return xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        case XR_AOT_RUN_ERROR:
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
    max_align_t align;
} XrAotFrameHeader;

static _Thread_local XrAotFrameHeader *tls_aot_frame_cache[XR_AOT_FRAME_CACHE_BUCKETS];
static _Thread_local uint16_t tls_aot_frame_cache_count[XR_AOT_FRAME_CACHE_BUCKETS];
static _Thread_local unsigned char tls_aot_frame_owner_marker;

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
    if (runtime->scheduler) {
        xr_scheduler_runtime_delete(runtime->scheduler);
        runtime->scheduler = NULL;
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
    XrCoroutine *main_coro = xr_coro_create_aot(runtime, desc, frame, desc ? desc->name : "main");
    if (!main_coro)
        return XR_NULL_VAL;
    xr_runtime_main_thread_run(xr_aot_runtime_scheduler(runtime), main_coro);
    XrValue result = main_coro->result;
    xr_coro_destroy(main_coro);
    return result;
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
