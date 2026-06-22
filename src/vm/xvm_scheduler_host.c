/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_scheduler_host.c - VM host callbacks for the scheduler runtime
 */

#include "../coro/xworker.h"
#include "../coro/xcoro_registry.h"
#include "../coro/xcoroutine.h"
#include "../coro/xtask.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"

static XrayIsolate *vm_host_isolate(void *ctx) {
    return (XrayIsolate *) ctx;
}

static void *vm_host_backend_context(void *ctx) {
    return vm_host_isolate(ctx);
}

static void vm_host_notify_coro(void *ctx, XrCoroutine *coro, const char *reason) {
    XrayIsolate *X = vm_host_isolate(ctx);
    if (!X || !coro)
        return;
    XrCoroState *state = (XrCoroState *) X->vm.coro_state;
    xr_coro_notify_monitors(X, state ? state->coro_registry : NULL, coro, reason);
}

static void vm_host_unregister_named_coro(XrayIsolate *X, XrCoroutine *coro) {
    if (!X || !coro)
        return;
    const char *name = xr_coro_name(coro);
    if (!name)
        return;

    XrCoroState *state = (XrCoroState *) X->vm.coro_state;
    if (state && state->coro_registry)
        xr_coro_registry_unregister(state->coro_registry, name);
}

static void vm_host_coro_on_exit(void *ctx, XrCoroutine *coro) {
    XrayIsolate *X = vm_host_isolate(ctx);
    vm_host_unregister_named_coro(X, coro);
}

static void vm_host_wake_scope_waiter(void *ctx, XrCoroutine *coro) {
    XrayIsolate *X = vm_host_isolate(ctx);
    if (X && coro)
        xr_coro_wake_scope_waiter(X, coro);
}

static void vm_host_wake_coro_waiter(void *ctx, XrCoroutine *coro) {
    XrayIsolate *X = vm_host_isolate(ctx);
    if (X && coro)
        xr_coro_wake_waiter(X, coro);
}

static void vm_host_wake_task_waiter(void *ctx, XrTask *task) {
    XrayIsolate *X = vm_host_isolate(ctx);
    if (X && task)
        xr_task_wake_waiter(X, task);
}

static void vm_host_adopt_deferred_tasks(void *ctx, XrTask *tasks, size_t count) {
    xr_task_isolate_adopt_deferred(vm_host_isolate(ctx), tasks, count);
}

static const XrSchedulerHostOps VM_SCHEDULER_HOST_OPS = {
    .backend_context = vm_host_backend_context,
    .notify_coro = vm_host_notify_coro,
    .coro_on_exit = vm_host_coro_on_exit,
    .wake_scope_waiter = vm_host_wake_scope_waiter,
    .wake_coro_waiter = vm_host_wake_coro_waiter,
    .wake_task_waiter = vm_host_wake_task_waiter,
    .adopt_deferred_tasks = vm_host_adopt_deferred_tasks,
};

void xr_scheduler_runtime_attach_isolate(XrSchedulerRuntime *runtime, XrayIsolate *isolate) {
    if (!runtime)
        return;
    if (isolate) {
        XR_DCHECK(xr_isolate_get_runtime_core(isolate) == runtime->core,
                  "scheduler_runtime_attach_isolate: isolate core mismatch");
    }

    XrSchedulerHost host = {
        .ops = isolate ? &VM_SCHEDULER_HOST_OPS : NULL,
        .ctx = isolate,
    };
    xr_scheduler_runtime_attach_host(runtime, &host);
}
