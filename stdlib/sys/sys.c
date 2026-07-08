#include "sys.h"

#include "../common.h"
#include "../../src/base/xchecks.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/module/xmodule.h"
#include "../../src/os/os_dylib.h"
#include "../../src/os/os_pipe.h"
#include "../../src/os/os_proc.h"
#include "../../src/os/os_thread.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/mem/xsystem_heap.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xpanic_info.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/xshared.h"
#include "../../src/runtime/xvm_call.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_closure.h"

typedef struct XrSysMutexBody {
    xr_mutex_t mutex;
    bool initialized;
} XrSysMutexBody;

typedef struct XrSysRwLockBody {
    xr_rwlock_t rwlock;
    bool initialized;
} XrSysRwLockBody;

typedef struct XrSysCondvarBody {
    xr_cond_t cond;
    bool initialized;
} XrSysCondvarBody;

typedef struct XrSysBarrierBody {
    xr_mutex_t mutex;
    xr_cond_t cond;
    int64_t parties;
    int64_t arrived;
    int64_t generation;
    bool initialized;
} XrSysBarrierBody;

typedef struct XrSysOnceBody {
    xr_once_t once;
    bool initialized;
} XrSysOnceBody;

static XR_THREAD_LOCAL XrVMRuntime *g_sys_once_isolate = NULL;
static XR_THREAD_LOCAL XrClosure *g_sys_once_body = NULL;

static void sys_once_call_body(void) {
    if (g_sys_once_isolate && g_sys_once_body)
        (void) xr_vm_call_closure(g_sys_once_isolate, g_sys_once_body, NULL, 0);
}

static void sys_mutex_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysMutexBody *body = (XrSysMutexBody *) body_ptr;
    xr_mutex_init(&body->mutex);
    body->initialized = true;
}

static void sys_mutex_body_destroy(void *body_ptr) {
    XrSysMutexBody *body = (XrSysMutexBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_mutex_destroy(&body->mutex);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_mutex_body_desc = {
    .body_size = sizeof(XrSysMutexBody),
    .body_align = _Alignof(XrSysMutexBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_mutex_body_init,
    .destroy = sys_mutex_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_rwlock_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysRwLockBody *body = (XrSysRwLockBody *) body_ptr;
    xr_rwlock_init(&body->rwlock);
    body->initialized = true;
}

static void sys_rwlock_body_destroy(void *body_ptr) {
    XrSysRwLockBody *body = (XrSysRwLockBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_rwlock_destroy(&body->rwlock);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_rwlock_body_desc = {
    .body_size = sizeof(XrSysRwLockBody),
    .body_align = _Alignof(XrSysRwLockBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_rwlock_body_init,
    .destroy = sys_rwlock_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_condvar_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysCondvarBody *body = (XrSysCondvarBody *) body_ptr;
    xr_cond_init(&body->cond);
    body->initialized = true;
}

static void sys_condvar_body_destroy(void *body_ptr) {
    XrSysCondvarBody *body = (XrSysCondvarBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_cond_destroy(&body->cond);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_condvar_body_desc = {
    .body_size = sizeof(XrSysCondvarBody),
    .body_align = _Alignof(XrSysCondvarBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_condvar_body_init,
    .destroy = sys_condvar_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_barrier_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysBarrierBody *body = (XrSysBarrierBody *) body_ptr;
    xr_mutex_init(&body->mutex);
    xr_cond_init(&body->cond);
    body->parties = 1;
    body->arrived = 0;
    body->generation = 0;
    body->initialized = true;
}

static void sys_barrier_body_destroy(void *body_ptr) {
    XrSysBarrierBody *body = (XrSysBarrierBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_cond_destroy(&body->cond);
    xr_mutex_destroy(&body->mutex);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_barrier_body_desc = {
    .body_size = sizeof(XrSysBarrierBody),
    .body_align = _Alignof(XrSysBarrierBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_barrier_body_init,
    .destroy = sys_barrier_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_once_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysOnceBody *body = (XrSysOnceBody *) body_ptr;
    xr_once_t init = XR_ONCE_INITIALIZER;
    body->once = init;
    body->initialized = true;
}

static void sys_once_body_destroy(void *body_ptr) {
    XrSysOnceBody *body = (XrSysOnceBody *) body_ptr;
    if (body)
        body->initialized = false;
}

static XrNativeBodyDesc g_sys_once_body_desc = {
    .body_size = sizeof(XrSysOnceBody),
    .body_align = _Alignof(XrSysOnceBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_once_body_init,
    .destroy = sys_once_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static XrClass *sys_mutex_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysMutexClass != NULL, "sys.OsMutex class not registered");
    return core ? core->sysMutexClass : NULL;
}

static XrClass *sys_rwlock_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysRwLockClass != NULL, "sys.OsRwLock class not registered");
    return core ? core->sysRwLockClass : NULL;
}

static XrClass *sys_condvar_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysCondvarClass != NULL, "sys.OsCondvar class not registered");
    return core ? core->sysCondvarClass : NULL;
}

static XrClass *sys_barrier_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysBarrierClass != NULL, "sys.OsBarrier class not registered");
    return core ? core->sysBarrierClass : NULL;
}

static XrClass *sys_once_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysOnceClass != NULL, "sys.OsOnce class not registered");
    return core ? core->sysOnceClass : NULL;
}

static XrInstance *sys_shared_instance_new(XrVMRuntime *isolate, XrClass *klass) {
    XrSystemHeap *heap = xr_isolate_get_sys_heap(isolate);
    if (!heap)
        return xr_instance_new(isolate, klass);

    XrInstance *instance =
        (XrInstance *) xr_sysheap_alloc_shared(heap, xr_instance_size(klass), XR_TINSTANCE);
    if (!instance)
        return NULL;

    xr_instance_init_inplace(instance, klass);
    XR_OBJ_SET_STORAGE(&instance->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&instance->hdr, 1);

    XrNativeBodyDesc *desc = klass->native_body;
    if (desc && desc->init) {
        void *body = xr_instance_native_body(instance);
        XR_DCHECK(body != NULL, "sys shared native body pointer must not be NULL");
        desc->init(instance, body);
    }
    return instance;
}

static XrSysMutexBody *sys_mutex_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_mutex_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysMutexBody *) xr_instance_native_body(instance);
}

static XrSysRwLockBody *sys_rwlock_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_rwlock_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysRwLockBody *) xr_instance_native_body(instance);
}

static XrSysCondvarBody *sys_condvar_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_condvar_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysCondvarBody *) xr_instance_native_body(instance);
}

static XrSysBarrierBody *sys_barrier_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_barrier_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysBarrierBody *) xr_instance_native_body(instance);
}

static XrSysOnceBody *sys_once_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_once_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysOnceBody *) xr_instance_native_body(instance);
}

static XrValue sys_mutex_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.OsMutex method called with non-OsMutex receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_rwlock_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.OsRwLock method called with non-OsRwLock receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_condvar_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.OsCondvar method called with non-OsCondvar receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_condvar_invalid_mutex(XrVMRuntime *isolate) {
    XrValue exc =
        xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH, "sys.OsCondvar requires a sys.OsMutex");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_barrier_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.OsBarrier method called with non-OsBarrier receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_barrier_invalid_parties(XrVMRuntime *isolate) {
    XrValue exc =
        xr_panic_info_newf(isolate, XR_ERR_INVALID_ARG_TYPE, "sys.OsBarrier parties must be > 0");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_once_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.OsOnce method called with non-OsOnce receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_mutex_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrInstance *instance = sys_shared_instance_new(isolate, sys_mutex_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.OsMutex allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_rwlock_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrInstance *instance = sys_shared_instance_new(isolate, sys_rwlock_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.OsRwLock allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_condvar_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrInstance *instance = sys_shared_instance_new(isolate, sys_condvar_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.OsCondvar allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_barrier_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    if (!XR_IS_INT(args[0]) || XR_TO_INT(args[0]) <= 0)
        return sys_barrier_invalid_parties(isolate);

    XrInstance *instance = sys_shared_instance_new(isolate, sys_barrier_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.OsBarrier allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    XrValue value = xr_value_from_instance(instance);
    XrSysBarrierBody *body = sys_barrier_body(isolate, value);
    if (body)
        body->parties = XR_TO_INT(args[0]);
    return value;
}

static XrValue sys_once_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrInstance *instance = sys_shared_instance_new(isolate, sys_once_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.OsOnce allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_mutex_lock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysMutexBody *body = sys_mutex_body(isolate, self);
    if (!body)
        return sys_mutex_invalid_receiver(isolate);
    xr_mutex_lock(&body->mutex);
    return xr_null();
}

static XrValue sys_mutex_unlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysMutexBody *body = sys_mutex_body(isolate, self);
    if (!body)
        return sys_mutex_invalid_receiver(isolate);
    xr_mutex_unlock(&body->mutex);
    return xr_null();
}

static XrValue sys_mutex_try_lock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysMutexBody *body = sys_mutex_body(isolate, self);
    if (!body)
        return sys_mutex_invalid_receiver(isolate);
    return xr_bool(xr_mutex_trylock(&body->mutex));
}

static XrValue sys_rwlock_rdlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_rdlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_rwlock_rdunlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_rdunlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_rwlock_wrlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_wrlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_rwlock_wrunlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_wrunlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_condvar_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    XrSysMutexBody *mutex = sys_mutex_body(isolate, args[0]);
    if (!mutex)
        return sys_condvar_invalid_mutex(isolate);
    xr_cond_wait(&body->cond, &mutex->mutex);
    return xr_null();
}

static XrValue sys_condvar_wait_for(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    XrSysMutexBody *mutex = sys_mutex_body(isolate, args[0]);
    if (!mutex)
        return sys_condvar_invalid_mutex(isolate);
    uint64_t timeout_ns =
        XR_IS_INT(args[1]) && XR_TO_INT(args[1]) > 0 ? (uint64_t) XR_TO_INT(args[1]) : 0u;
    return xr_bool(xr_cond_wait_for_ns(&body->cond, &mutex->mutex, timeout_ns));
}

static XrValue sys_condvar_signal(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    xr_cond_signal(&body->cond);
    return xr_null();
}

static XrValue sys_condvar_broadcast(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    xr_cond_broadcast(&body->cond);
    return xr_null();
}

static XrValue sys_barrier_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysBarrierBody *body = sys_barrier_body(isolate, self);
    if (!body)
        return sys_barrier_invalid_receiver(isolate);

    xr_mutex_lock(&body->mutex);
    int64_t generation = body->generation;
    body->arrived++;
    if (body->arrived >= body->parties) {
        body->arrived = 0;
        body->generation++;
        xr_cond_broadcast(&body->cond);
        xr_mutex_unlock(&body->mutex);
        return xr_bool(true);
    }
    while (generation == body->generation)
        xr_cond_wait(&body->cond, &body->mutex);
    xr_mutex_unlock(&body->mutex);
    return xr_bool(true);
}

static XrValue sys_once_call(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    XrSysOnceBody *body = sys_once_body(isolate, self);
    if (!body)
        return sys_once_invalid_receiver(isolate);

    XrClosure *closure = xr_vm_closure_from_arg(isolate, args[0], "sys.OsOnce.call");
    if (!closure)
        return xr_null();

    XrVMRuntime *prev_isolate = g_sys_once_isolate;
    XrClosure *prev_body = g_sys_once_body;
    g_sys_once_isolate = isolate;
    g_sys_once_body = closure;
    xr_once_call(&body->once, sys_once_call_body);
    g_sys_once_isolate = prev_isolate;
    g_sys_once_body = prev_body;
    return xr_null();
}

static XrValue sys_cpu_count(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_int((int64_t) xr_os_cpu_count());
}

static XrValue sys_thread_yield(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    xr_thread_yield();
    return xr_null();
}

static XrValue sys_sleep_ms(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    int64_t ms = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    if (ms > 0)
        xr_thread_sleep_ms((unsigned int) ms);
    return xr_null();
}

static XrValue sys_pin_to_cpu(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    int64_t cpu = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    if (cpu < 0)
        return xr_bool(false);
    return xr_bool(xr_thread_pin_to_cpu((unsigned int) cpu) == 0);
}

static XrValue sys_thread_local_id(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_int((int64_t) xr_thread_current_id());
}

static XrValue sys_dylib_open(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    const char *path = (argc >= 1) ? xrs_string_arg(args[0], NULL) : NULL;
    if (!path || path[0] == '\0')
        return xr_int(0);
    XrDylib *lib = xr_dylib_open(path);
    return xr_int((int64_t) (intptr_t) lib);
}

static XrValue sys_dylib_symbol(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]))
        return xr_null();
    XrDylib *lib = (XrDylib *) (intptr_t) XR_TO_INT(args[0]);
    const char *name = xrs_string_arg(args[1], NULL);
    if (!lib || !name || name[0] == '\0')
        return xr_null();
    void *sym = xr_dylib_sym(lib, name);
    return sym ? xr_int((int64_t) (intptr_t) sym) : xr_null();
}

static XrValue sys_dylib_close(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    XrDylib *lib = (XrDylib *) (intptr_t) XR_TO_INT(args[0]);
    if (!lib)
        return xr_bool(true);
    xr_dylib_close(lib);
    return xr_bool(true);
}

static XrValue sys_dylib_last_error(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    const char *err = xr_dylib_last_error();
    return xrs_string_value_c(isolate, err ? err : "");
}

static bool sys_process_env_key_valid(const char *key) {
    if (!key || key[0] == '\0')
        return false;
    return strchr(key, '=') == NULL;
}

static void sys_process_env_free(const char **keys, const char **values) {
    xr_free((void *) keys);
    xr_free((void *) values);
}

static bool sys_process_env_from_arrays(XrValue keys_value, XrValue values_value,
                                        const char ***out_keys, const char ***out_values,
                                        size_t *out_count) {
    *out_keys = NULL;
    *out_values = NULL;
    *out_count = 0;

    if (XR_IS_NULL(keys_value) && XR_IS_NULL(values_value))
        return true;
    if (!XR_IS_ARRAY(keys_value) || !XR_IS_ARRAY(values_value))
        return false;

    XrArray *keys_arr = XR_TO_ARRAY(keys_value);
    XrArray *values_arr = XR_TO_ARRAY(values_value);
    int count = keys_arr ? keys_arr->length : -1;
    if (count < 0 || !values_arr || values_arr->length != count)
        return false;
    if (count == 0)
        return true;

    const char **keys = (const char **) xr_malloc(sizeof(char *) * (size_t) count);
    const char **values = (const char **) xr_malloc(sizeof(char *) * (size_t) count);
    if (!keys || !values) {
        sys_process_env_free(keys, values);
        return false;
    }

    for (int i = 0; i < count; i++) {
        const char *key = xrs_string_arg(xr_array_get(keys_arr, i), NULL);
        const char *value = xrs_string_arg(xr_array_get(values_arr, i), NULL);
        if (!sys_process_env_key_valid(key) || !value) {
            sys_process_env_free(keys, values);
            return false;
        }
        keys[i] = key;
        values[i] = value;
    }

    *out_keys = keys;
    *out_values = values;
    *out_count = (size_t) count;
    return true;
}

static bool sys_process_pipe_handle_from_optional(XrValue value, bool *out_has,
                                                  XrPipeHandle *out_handle) {
    *out_has = false;
    *out_handle = XR_PIPE_INVALID;
    if (XR_IS_NULL(value))
        return true;
    if (!XR_IS_INT(value))
        return false;
    *out_has = true;
    *out_handle = (XrPipeHandle) XR_TO_INT(value);
    return true;
}

static XrValue sys_process_spawn(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 8)
        return xr_int((int64_t) XR_PROC_INVALID);

    const char *program = xrs_string_arg(args[0], NULL);
    if (!program || program[0] == '\0' || !XR_IS_ARRAY(args[1]) ||
        (!XR_IS_NULL(args[2]) && !XR_IS_STRING(args[2])))
        return xr_int((int64_t) XR_PROC_INVALID);
    const char *cwd = XR_IS_STRING(args[2]) ? xrs_string_arg(args[2], NULL) : NULL;

    XrArray *arg_arr = XR_TO_ARRAY(args[1]);
    int extra = arg_arr ? arg_arr->length : 0;
    if (extra < 0 || (size_t) extra > (SIZE_MAX / sizeof(char *)) - 2)
        return xr_int((int64_t) XR_PROC_INVALID);

    const char **argv = (const char **) xr_malloc(sizeof(char *) * ((size_t) extra + 2));
    if (!argv)
        return xr_int((int64_t) XR_PROC_INVALID);

    argv[0] = program;
    for (int i = 0; i < extra; i++) {
        const char *s = xrs_string_arg(xr_array_get(arg_arr, i), NULL);
        if (!s) {
            xr_free(argv);
            return xr_int((int64_t) XR_PROC_INVALID);
        }
        argv[i + 1] = s;
    }
    argv[extra + 1] = NULL;

    const char **env_keys = NULL;
    const char **env_values = NULL;
    size_t env_count = 0;
    if (!sys_process_env_from_arrays(args[3], args[4], &env_keys, &env_values, &env_count)) {
        xr_free(argv);
        return xr_int((int64_t) XR_PROC_INVALID);
    }

    bool has_stdin = false;
    bool has_stdout = false;
    bool has_stderr = false;
    XrPipeHandle stdin_read = XR_PIPE_INVALID;
    XrPipeHandle stdout_write = XR_PIPE_INVALID;
    XrPipeHandle stderr_write = XR_PIPE_INVALID;
    if (!sys_process_pipe_handle_from_optional(args[5], &has_stdin, &stdin_read) ||
        !sys_process_pipe_handle_from_optional(args[6], &has_stdout, &stdout_write) ||
        !sys_process_pipe_handle_from_optional(args[7], &has_stderr, &stderr_write)) {
        sys_process_env_free(env_keys, env_values);
        xr_free(argv);
        return xr_int((int64_t) XR_PROC_INVALID);
    }

    XrProcSpawnOptions options = {
        .cwd = cwd,
        .env_keys = env_keys,
        .env_values = env_values,
        .env_count = env_count,
        .has_stdin = has_stdin,
        .stdin_read = stdin_read,
        .has_stdout = has_stdout,
        .stdout_write = stdout_write,
        .has_stderr = has_stderr,
        .stderr_write = stderr_write,
    };
    XrProcId pid = xr_proc_spawn_ex(program, argv, &options);
    sys_process_env_free(env_keys, env_values);
    xr_free(argv);
    return xr_int((int64_t) pid);
}

static XrValue sys_process_wait(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(-1);

    int code = -1;
    if (xr_proc_wait((XrProcId) XR_TO_INT(args[0]), &code) != 0)
        code = -1;
    return xr_int((int64_t) code);
}

static XrValue sys_process_try_wait(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(-1);

    int code = -1;
    XrProcWaitResult result = xr_proc_try_wait((XrProcId) XR_TO_INT(args[0]), &code);
    if (result == XR_PROC_WAIT_RUNNING)
        return xr_null();
    if (result == XR_PROC_WAIT_ERROR)
        code = -1;
    return xr_int((int64_t) code);
}

static XrValue sys_process_kill(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);

    return xr_bool(xr_proc_kill((XrProcId) XR_TO_INT(args[0]), (int) XR_TO_INT(args[1])) == 0);
}

static XrValue sys_pipe_open(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrPipe pipe;
    if (xr_pipe_create(&pipe, NULL) != 0)
        return xr_null();

    XrArray *ends = xr_array_with_capacity_typed(xr_current_coro(isolate), 2, XR_ELEM_I64);
    if (!ends) {
        xr_pipe_close(pipe.read);
        xr_pipe_close(pipe.write);
        return xr_null();
    }
    xr_array_push(ends, xr_int((int64_t) pipe.read));
    xr_array_push(ends, xr_int((int64_t) pipe.write));
    return xr_value_from_array(ends);
}

static XrValue sys_pipe_read(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();

    int64_t max_bytes = XR_TO_INT(args[1]);
    if (max_bytes < 0 || max_bytes > INT32_MAX)
        return xr_null();

    XrArray *bytes = xr_array_bytes_new(xr_current_coro(isolate), (int32_t) max_bytes);
    if (!bytes)
        return xr_null();

    int64_t n = xr_pipe_read((XrPipeHandle) XR_TO_INT(args[0]), bytes->data, (size_t) max_bytes);
    if (n < 0)
        return xr_null();
    bytes->length = (int32_t) n;
    return xr_value_from_array(bytes);
}

static XrValue sys_pipe_write(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !xr_value_is_array(args[1]))
        return xr_int(-1);

    XrArray *bytes = xr_value_to_array(args[1]);
    if (!bytes || bytes->elem_type != XR_ELEM_U8)
        return xr_int(-1);

    int64_t n =
        xr_pipe_write((XrPipeHandle) XR_TO_INT(args[0]), bytes->data, (size_t) bytes->length);
    return xr_int(n);
}

static XrValue sys_pipe_close(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    return xr_bool(xr_pipe_close((XrPipeHandle) XR_TO_INT(args[0])) == 0);
}

#define XR_STDLIB_VM_BIND_CLASS_OS_CONDVAR 1
#define XR_STDLIB_VM_BIND_CLASS_OS_BARRIER 1
#define XR_STDLIB_VM_BIND_CLASS_OS_ONCE 1
#define XR_STDLIB_VM_BIND_CLASS_OS_MUTEX 1
#define XR_STDLIB_VM_BIND_CLASS_OS_RW_LOCK 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_OS_RW_LOCK
#undef XR_STDLIB_VM_BIND_CLASS_OS_MUTEX
#undef XR_STDLIB_VM_BIND_CLASS_OS_ONCE
#undef XR_STDLIB_VM_BIND_CLASS_OS_BARRIER
#undef XR_STDLIB_VM_BIND_CLASS_OS_CONDVAR

void xr_sys_mutex_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_os_mutex_class_generated(isolate);
}

void xr_sys_rwlock_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_os_rw_lock_class_generated(isolate);
}

void xr_sys_condvar_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_os_condvar_class_generated(isolate);
}

void xr_sys_barrier_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_os_barrier_class_generated(isolate);
}

void xr_sys_once_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_os_once_class_generated(isolate);
}

#define XR_STDLIB_VM_BIND_MODULE_SYS 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_SYS

XrModule *xr_load_module_sys(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_sys: NULL isolate");
    XrModule *module = xr_module_create_native(isolate, "sys");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_sys_generated(isolate, module);
    module->loaded = true;
    return module;
}
