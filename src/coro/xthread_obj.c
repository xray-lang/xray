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

#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"

static void thread_throw(XrVMRuntime *isolate, XrErrorCode code, const char *message) {
    if (!isolate || !message)
        return;
    XrValue exc = xr_panic_info_newf(isolate, code, "%s", message);
    xr_vm_throw_exception(isolate, exc);
}

XrThread *xr_thread_obj_spawn(struct XrVMRuntime *isolate, struct XrClosure *body, const char *name,
                              size_t stack_size) {
    (void) body;
    (void) name;
    (void) stack_size;
    thread_throw(isolate, XR_ERR_RUNTIME,
                 "sys.Thread.spawn backend is not wired; XI_THREAD_SPAWN needs a backend "
                 "trampoline");
    return NULL;
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
    thread->body = NULL;
    thread->isolate = NULL;
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
    return xr_thread_obj_join(thread);
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
