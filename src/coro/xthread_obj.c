/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xthread_obj.c - sys.Thread shared OS-thread handle.
 */

#include "xthread_obj.h"

#include "xcoroutine.h"
#include "xdeep_copy.h"
#include "xmachine.h"
#include "xworker_internal.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/xshared.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xisolate_internal.h"
#include "../vm/xvm.h"
#include "../vm/xvm_coro_api.h"
#include "../vm/xvm_worker_state.h"

#include <string.h>

static void thread_throw(XrVMRuntime *isolate, XrErrorCode code, const char *message) {
    if (!isolate || !message)
        return;
    XrValue exc = xr_panic_info_newf(isolate, code, "%s", message);
    xr_vm_throw_exception(isolate, exc);
}

static void thread_release_ref(XrThread *thread) {
    if (!thread)
        return;
    XrRuntimeCore *core = thread->core;
    if (xr_obj_drop_is_last(&thread->hdr))
        xr_shared_destroy_core(core, &thread->hdr);
}

static void thread_apply_affinity(const uint32_t *cpus, uint8_t count) {
    if (!cpus || count == 0)
        return;
    for (uint8_t i = 0; i < count; i++) {
        if (xr_thread_pin_to_cpu(cpus[i]) == 0)
            return;
    }
}

static void thread_runtime_enter(XrVMRuntime *isolate) {
    if (isolate)
        atomic_fetch_add_explicit(&isolate->sys_thread_count, 1, memory_order_acq_rel);
}

static void thread_runtime_leave(XrVMRuntime *isolate) {
    if (isolate)
        atomic_fetch_sub_explicit(&isolate->sys_thread_count, 1, memory_order_acq_rel);
}

void xr_thread_obj_drain_isolate(struct XrVMRuntime *isolate) {
    if (!isolate)
        return;
    while (atomic_load_explicit(&isolate->sys_thread_count, memory_order_acquire) != 0)
        xr_thread_yield();
}

static bool thread_bind_vm_tls(XrThread *thread, XrWorker *worker, XrMachine *machine) {
    if (!thread || !thread->isolate || !thread->coro || !worker || !machine)
        return false;

    XrRuntime *runtime = (XrRuntime *) thread->isolate->vm.scheduler;
    if (!runtime)
        return false;

    xr_machine_init(machine, -1, runtime);
    machine->thread = xr_thread_self();
    atomic_store_explicit(&machine->state, M_RUNNING, memory_order_release);
    atomic_store_explicit(&machine->current_coro, thread->coro, memory_order_relaxed);

    memset(worker, 0, sizeof(*worker));
    worker->p.id = -1;
    worker->p.runtime = runtime;
    worker->m = machine;

    XrVMContext *machine_ctx = xr_vm_machine_ctx(machine, thread->isolate);
    if (!machine_ctx)
        return false;
    machine_ctx->current_coro = thread->coro;

    tls_current_worker = worker;
    tls_current_machine = machine;
    return true;
}

static void thread_unbind_vm_tls(XrWorker *worker, XrMachine *machine) {
    if (machine) {
        XrVMContext *machine_ctx = NULL;
        if (machine->backend_storage)
            machine_ctx = xr_vm_machine_ctx(machine, NULL);
        if (machine_ctx)
            machine_ctx->current_coro = NULL;
        atomic_store_explicit(&machine->current_coro, (XrCoroutine *) NULL, memory_order_relaxed);
    }
    if (tls_current_worker == worker)
        tls_current_worker = NULL;
    if (tls_current_machine == machine)
        tls_current_machine = NULL;
    xr_machine_destroy(machine);
}

static void *thread_entry_vm(void *arg) {
    XrThread *thread = (XrThread *) arg;
    if (!thread)
        return NULL;

    if (thread->isolate)
        xray_vm_enter(thread->isolate);
    if (thread->name)
        xr_thread_set_name(xr_thread_self(), thread->name);
    thread_apply_affinity(thread->affinity_cpus, thread->affinity_count);

    XrWorker worker;
    XrMachine machine;
    bool bound_vm_tls = thread_bind_vm_tls(thread, &worker, &machine);

    XrValue out = xr_null();
    XrCoroRunKind kind =
        bound_vm_tls ? xr_vm_coro_run_to_completion(thread->coro, &out) : XR_CORO_RUN_ERROR;
    if (kind == XR_CORO_RUN_DONE) {
        thread->retval = out;
        atomic_store_explicit(&thread->failed, false, memory_order_release);
    } else {
        thread->retval = xr_null();
        thread->error = out;
        thread->error_is_value = thread->coro ? thread->coro->error_is_value : false;
        atomic_store_explicit(&thread->failed, true, memory_order_release);
    }
    atomic_store_explicit(&thread->finished, true, memory_order_release);

    XrVMRuntime *isolate = thread->isolate;
    if (bound_vm_tls)
        thread_unbind_vm_tls(&worker, &machine);
    if (isolate)
        xray_vm_exit();

    thread_release_ref(thread);
    thread_runtime_leave(isolate);
    return NULL;
}

XrThread *xr_thread_obj_spawn_vm(struct XrVMRuntime *isolate, struct XrCoroutine *coro,
                                 const char *name, size_t stack_size, const uint32_t *affinity_cpus,
                                 uint16_t affinity_count) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || !core->sys_heap || !coro) {
        if (coro)
            xr_coro_destroy(coro);
        thread_throw(isolate, XR_ERR_OUT_OF_MEMORY, "sys.Thread.spawn: unable to allocate handle");
        return NULL;
    }

    XrThread *thread =
        (XrThread *) xr_sysheap_alloc_shared(core->sys_heap, sizeof(XrThread), XR_TTHREAD);
    if (!thread) {
        xr_coro_destroy(coro);
        thread_throw(isolate, XR_ERR_OUT_OF_MEMORY, "sys.Thread.spawn: unable to allocate handle");
        return NULL;
    }

    thread->coro = coro;
    thread->isolate = isolate;
    thread->core = core;
    thread->name = name;
    thread->affinity_count = 0;
    if (affinity_cpus && affinity_count > 0) {
        uint16_t count =
            affinity_count < XR_THREAD_AFFINITY_MAX ? affinity_count : XR_THREAD_AFFINITY_MAX;
        thread->affinity_count = (uint8_t) count;
        memcpy(thread->affinity_cpus, affinity_cpus, (size_t) count * sizeof(uint32_t));
    }
    thread->retval = xr_null();
    thread->error = xr_null();
    thread->error_is_value = false;
    atomic_store_explicit(&thread->state, XR_THREAD_CREATED, memory_order_relaxed);
    atomic_store_explicit(&thread->finished, false, memory_order_relaxed);
    atomic_store_explicit(&thread->failed, false, memory_order_relaxed);

    xr_shared_retain(&thread->hdr); /* OS entry owns the handle until exit. */
    thread_runtime_enter(isolate);
    if (!xr_thread_create_ex(&thread->handle, thread_entry_vm, thread, stack_size)) {
        thread->coro = NULL;
        xr_coro_destroy(coro);
        thread_release_ref(thread); /* entry ref */
        thread_release_ref(thread); /* returned-handle ref */
        thread_runtime_leave(isolate);
        thread_throw(isolate, XR_ERR_RUNTIME, "sys.Thread.spawn: OS thread creation failed");
        return NULL;
    }
    if (name)
        xr_thread_set_name(thread->handle, name);
    return thread;
}

XrValue xr_thread_obj_join(XrThread *thread) {
    if (!thread)
        return xr_null();

    for (;;) {
        int state = atomic_load_explicit(&thread->state, memory_order_acquire);
        switch ((XrThreadState) state) {
            case XR_THREAD_JOINED:
                return thread->retval;
            case XR_THREAD_DETACHED:
                return xr_null();
            case XR_THREAD_JOINING:
                xr_thread_yield();
                break;
            case XR_THREAD_CREATED: {
                int expected = XR_THREAD_CREATED;
                if (!atomic_compare_exchange_strong_explicit(
                        &thread->state, &expected, XR_THREAD_JOINING, memory_order_acq_rel,
                        memory_order_acquire)) {
                    break;
                }
                if (xr_thread_is_valid(thread->handle))
                    (void) xr_thread_join(thread->handle, NULL);
                atomic_store_explicit(&thread->finished, true, memory_order_release);
                atomic_store_explicit(&thread->state, XR_THREAD_JOINED, memory_order_release);
                return thread->retval;
            }
        }
    }
}

void xr_thread_obj_detach(XrThread *thread) {
    if (!thread)
        return;
    int expected = XR_THREAD_CREATED;
    if (atomic_compare_exchange_strong_explicit(&thread->state, &expected, XR_THREAD_DETACHED,
                                                memory_order_acq_rel, memory_order_acquire)) {
        if (xr_thread_is_valid(thread->handle))
            xr_thread_detach(thread->handle);
    }
}

void xr_obj_destroy_thread(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    XrThread *thread = (XrThread *) obj;
    xr_thread_obj_detach(thread);
    if (thread->coro) {
        xr_coro_destroy(thread->coro);
        thread->coro = NULL;
    }
    thread->isolate = NULL;
    thread->core = NULL;
    thread->name = NULL;
}

static XrValue m_join(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    XrThread *thread = xr_value_to_thread(self);
    if (!thread) {
        thread_throw(isolate, XR_ERR_TYPE_MISMATCH, "Thread.join: receiver is not a Thread");
        return xr_null();
    }
    if (atomic_load_explicit(&thread->state, memory_order_acquire) == XR_THREAD_DETACHED) {
        thread_throw(isolate, XR_ERR_RUNTIME, "Thread.join: thread is detached");
        return xr_null();
    }
    XrValue result = xr_thread_obj_join(thread);
    XrCoroutine *caller = xr_current_coro(isolate);
    if (atomic_load_explicit(&thread->failed, memory_order_acquire)) {
        XrValue err = thread->error;
        if (XR_IS_PTR(err))
            err = xr_deep_copy_to_coro(isolate, err, caller);
        if (XR_IS_NULL(err)) {
            thread_throw(isolate, XR_ERR_RUNTIME, "Thread.join: thread body failed");
        } else if (thread->error_is_value) {
            xr_vm_set_pending_error(isolate, err);
        } else {
            xr_vm_throw_exception(isolate, err);
        }
        return xr_null();
    }
    if (XR_IS_PTR(result))
        result = xr_deep_copy_to_coro(isolate, result, caller);
    return result;
}

static XrValue m_detach(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    XrThread *thread = xr_value_to_thread(self);
    if (!thread) {
        thread_throw(isolate, XR_ERR_TYPE_MISMATCH, "Thread.detach: receiver is not a Thread");
        return xr_null();
    }
    xr_thread_obj_detach(thread);
    return xr_null();
}

static XrValue g_done(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    XrThread *thread = xr_value_to_thread(self);
    if (!thread)
        return xr_bool(false);
    return xr_bool(atomic_load_explicit(&thread->finished, memory_order_acquire));
}

void xr_thread_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod thread_methods[] = {
        {"join", m_join, 0},
        {"detach", m_detach, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod thread_getters[] = {
        {"done", g_done, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo info = {
        .name = TYPE_NAME_THREAD,
        .gc_type = XR_TTHREAD,
        .methods = (XrNativeMethod *) thread_methods,
        .getters = (XrNativeMethod *) thread_getters,
    };
    xr_register_native_type(isolate, &info);
}
