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

#include <string.h>

#include "../base/xmalloc.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/core/xr_script_info.h"
#include "../runtime/gc/xcoro_heap.h"
#include "../runtime/gc/xobj_destroy_ops.h"
#include "xblock.h"
#include "xcoroutine.h"
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
                    XR_AOT_CAP_WORK_QUEUE | XR_AOT_CAP_RESULT_GROUP)) != 0;
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

static const XrCoroBackendVTable aot_runtime_backend_vtable = {
    .kind = XR_CORO_BACKEND_AOT,
    .resume = aot_runtime_backend_resume,
    .trace_roots = aot_backend_trace_roots,
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
    .release = aot_release_state,
    .destroy = aot_release_state,
    .debug_name = aot_backend_debug_name,
    .debug_snapshot = aot_backend_debug_snapshot,
};

void *xr_aot_frame_alloc(size_t size) {
    if (size == 0)
        size = 1;
    return xr_malloc(size);
}

void xr_aot_frame_free(void *frame) {
    xr_free(frame);
}

void xr_aot_runtime_config_init(XrAotRuntimeConfig *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
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

    return runtime;

fail:
    xr_aot_runtime_delete(runtime);
    return NULL;
}

void xr_aot_runtime_delete(XrAotRuntime *runtime) {
    if (!runtime)
        return;
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
    return runtime->builtins[index];
}

void xr_aot_runtime_set_builtin(XrAotRuntime *runtime, int32_t index, XrValue value) {
    if (!runtime || index < 0 || index >= XR_USER_GLOBALS_START)
        return;
    runtime->builtins[index] = value;
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

    XrAotCoroState *state = (XrAotCoroState *) xr_calloc(1, sizeof(XrAotCoroState));
    if (!state) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }
    state->desc = desc;
    state->frame = frame;
    state->runtime = runtime;

    XrCoroutine *coro = xr_coro_create_runtime_empty(core, scheduler, name ? name : desc->name);
    if (!coro) {
        aot_release_frame(desc, frame, NULL);
        xr_free(state);
        return NULL;
    }

    xr_coro_attach_backend(coro, &aot_runtime_backend_vtable, state);
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
